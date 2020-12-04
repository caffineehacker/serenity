/*
 * Copyright (c) 2020, Itamar S. <itamar8910@gmail.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/LexicalPath.h>
#include <AK/LogStream.h>
#include <AK/ScopeGuard.h>
#include <LibC/mman.h>
#include <LibC/stdio.h>
#include <LibC/sys/internals.h>
#include <LibC/unistd.h>
#include <LibCore/File.h>
#include <LibELF/AuxiliaryVector.h>
#include <LibELF/DynamicLoader.h>
#include <LibELF/DynamicObject.h>
#include <LibELF/Image.h>
#include <LibELF/Loader.h>
#include <LibELF/exec_elf.h>
#include <dlfcn.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

// #define DYNAMIC_LOAD_VERBOSE

#ifdef DYNAMIC_LOAD_VERBOSE
#    define VERBOSE(fmt, ...) dbgprintf(fmt, ##__VA_ARGS__)
#else
#    define VERBOSE(fmt, ...) \
        do {                  \
        } while (0)
#endif
#define TLS_VERBOSE(fmt, ...) dbgprintf(fmt, ##__VA_ARGS__)

char* __static_environ[] = { nullptr }; // We don't get the environment without some libc workarounds..

static HashMap<String, NonnullRefPtr<ELF::DynamicLoader>> g_loaders;
static HashMap<String, NonnullRefPtr<ELF::DynamicObject>> g_loaded_objects;

static size_t g_current_tls_offset = 0;
static size_t g_total_tls_size = 0;

static void init_libc()
{
    environ = __static_environ;
    __environ_is_malloced = false;
    __stdio_is_initialized = false;
    __malloc_init();
}

static void perform_self_relocations()
{
    // We need to relocate ourselves.
    // (these relocations seem to be generated because of our vtables)

    // TODO: Pass this address in the Auxiliary Vector
    u32 base = 0x08000000;
    Elf32_Ehdr* header = (Elf32_Ehdr*)(base);
    Elf32_Phdr* pheader = (Elf32_Phdr*)(base + header->e_phoff);
    u32 dynamic_section_addr = 0;
    for (size_t i = 0; i < (size_t)header->e_phnum; ++i, ++pheader) {
        if (pheader->p_type != PT_DYNAMIC)
            continue;
        dynamic_section_addr = pheader->p_vaddr + base;
    }
    if (!dynamic_section_addr)
        exit(1);

    auto dynamic_object = ELF::DynamicObject::construct((VirtualAddress(base)), (VirtualAddress(dynamic_section_addr)));

    dynamic_object->relocation_section().for_each_relocation([base](auto& reloc) {
        if (reloc.type() != R_386_RELATIVE)
            return IterationDecision::Continue;
        *(u32*)reloc.address().as_ptr() += base;
        return IterationDecision::Continue;
    });
}

static ELF::DynamicObject::SymbolLookupResult global_symbol_lookup(const char* symbol_name)
{
    VERBOSE("global symbol lookup: %s\n", symbol_name);
    for (auto& lib : g_loaded_objects) {
        VERBOSE("looking up in object: %s\n", lib.key.characters());
        auto res = lib.value->lookup_symbol(symbol_name);
        if (!res.has_value())
            continue;
        return res.value();
    }
    ASSERT_NOT_REACHED();
    return {};
}

static void map_library(const String& name, int fd)
{
    struct stat lib_stat;
    int rc = fstat(fd, &lib_stat);
    ASSERT(!rc);

    auto loader = ELF::DynamicLoader::construct(name.characters(), fd, lib_stat.st_size);
    loader->set_tls_offset(g_current_tls_offset);
    loader->m_global_symbol_lookup_func = global_symbol_lookup;

    g_loaders.set(name, loader);

    g_current_tls_offset += loader->tls_size();
}

static void map_library(const String& name)
{
    // TODO: Do we want to also look for libs in other paths too?
    String path = String::format("/usr/lib/%s", name.characters());
    int fd = open(path.characters(), O_RDONLY);
    ASSERT(fd >= 0);
    map_library(name, fd);
}

static String get_library_name(const StringView& path)
{
    return LexicalPath(path).basename();
}

static void map_dependencies(const String& name)
{
    dbg() << "mapping dependencies for: " << name;
    auto lib = g_loaders.get(name).value();
    lib->for_each_needed_library([](auto needed_name) {
        dbg() << "needed library: " << needed_name;
        String library_name = get_library_name(needed_name);

        if (!g_loaders.contains(library_name)) {
            map_library(library_name);
            map_dependencies(library_name);
        }
        return IterationDecision::Continue;
    });
}

static void allocate_tls()
{
    size_t total_tls_size = 0;
    for (const auto& data : g_loaders) {
        total_tls_size += data.value->tls_size();
    }
    if (total_tls_size) {
        void* tls_address = allocate_tls(total_tls_size);
        dbg() << "from userspace, tls_address: " << tls_address;
    }
    g_total_tls_size = total_tls_size;
}

static void load_elf(const String& name)
{
    dbg() << "load_elf: " << name;
    auto loader = g_loaders.get(name).value();
    loader->for_each_needed_library([](auto needed_name) {
        dbg() << "needed library: " << needed_name;
        String library_name = get_library_name(needed_name);
        if (!g_loaded_objects.contains(library_name)) {
            load_elf(library_name);
        }
        return IterationDecision::Continue;
    });

    auto dynamic_object = loader->load_from_image(RTLD_GLOBAL, g_total_tls_size);
    ASSERT(!dynamic_object.is_null());
    g_loaded_objects.set(name, dynamic_object.release_nonnull());
}

static FlatPtr loader_main(auxv_t* auxvp)
{
    int main_program_fd = -1;
    for (; auxvp->a_type != AT_NULL; ++auxvp) {
        if (auxvp->a_type == AuxiliaryValue::ExecFileDescriptor) {
            main_program_fd = auxvp->a_un.a_val;
        }
    }
    ASSERT(main_program_fd >= 0);

    // TODO: Pass this in the auxiliary vector
    const String main_program_name = "MainProgram";
    map_library(main_program_name, main_program_fd);
    map_dependencies(main_program_name);

    dbg() << "loaded all dependencies";
    for (auto& lib : g_loaders) {
        dbg() << lib.key << "- tls size: " << lib.value->tls_size() << ", tls offset: " << lib.value->tls_offset();
    }

    allocate_tls();

    load_elf(main_program_name);

    auto main_program_lib = g_loaders.get(main_program_name).value();
    FlatPtr entry_point = reinterpret_cast<FlatPtr>(main_program_lib->image().entry().as_ptr() + (FlatPtr)main_program_lib->text_segment_load_address().as_ptr());
    dbg() << "entry point: " << (void*)entry_point;

    // This will unmap the temporary memory maps we had for loading the libraries
    g_loaders.clear();

    return entry_point;
}

extern "C" {

// The compiler expects a previous declaration
void _start(int, char**, char**);

using MainFunction = int (*)(int, char**, char**);

void _start(int argc, char** argv, char** envp)
{
    perform_self_relocations();
    init_libc();

    char** env;
    for (env = envp; *env; ++env) {
    }

    auxv_t* auxvp = (auxv_t*)++env;
    FlatPtr entry = loader_main(auxvp);
    MainFunction main_function = (MainFunction)(entry);
    dbg() << "jumping to main program entry point: " << (void*)main_function;
    int rc = main_function(argc, argv, envp);
    dbg() << "rc: " << rc;
    sleep(100);
    _exit(rc);
}
}