// Copyright (c) Prevail Verifier contributors.
// SPDX-License-Identifier: MIT
#include <cassert>
#include <iostream>
#include <string>
#include <vector>
#include <sys/stat.h>

#include "asm_files.hpp"
#include "platform.hpp"

#include "elfio/elfio.hpp"

using std::cout;
using std::string;
using std::vector;

template <typename T>
static vector<T> vector_of(ELFIO::section* sec) {
    if (!sec)
        return {};
    auto data = sec->get_data();
    auto size = sec->get_size();
    assert(size % sizeof(T) == 0);
    return {(T*)data, (T*)(data + size)};
}

int create_map_crab(uint32_t map_type, uint32_t key_size, uint32_t value_size, uint32_t max_entries, ebpf_verifier_options_t options) {
    // For now we just make up a number as if the map were created,
    // without actually creating anything.
    return (value_size << 14) + (key_size << 6); // + i;
}

EbpfMapDescriptor* find_map_descriptor(int map_fd) {
    for (EbpfMapDescriptor& map : global_program_info.map_descriptors) {
        if (map.original_fd == map_fd) {
            return &map;
        }
    }
    return nullptr;
}

vector<raw_program> read_elf(const std::string& path, const std::string& desired_section, const ebpf_verifier_options_t* options, const ebpf_platform_t* platform) {
    if (options == nullptr)
        options = &ebpf_verifier_default_options;
    ELFIO::elfio reader;
    if (!reader.load(path)) {
        struct stat st;
        if (stat(path.c_str(), &st)) {
            throw std::runtime_error(string(strerror(errno)) + " opening " + path);
        }
        throw std::runtime_error(string("Can't process ELF file ") + path);
    }

    program_info info{platform};

    ELFIO::section* maps_section = reader.sections["maps"];
    if (maps_section) {
        platform->parse_maps_section(info.map_descriptors, maps_section->get_data(), maps_section->get_size(), platform->create_map, *options);
    }

    ELFIO::const_symbol_section_accessor symbols{reader, reader.sections[".symtab"]};
    auto read_reloc_value = [&symbols,platform](int symbol) -> size_t {
        string symbol_name;
        ELFIO::Elf64_Addr value{};
        ELFIO::Elf_Xword size{};
        unsigned char bind{};
        unsigned char type{};
        ELFIO::Elf_Half section_index{};
        unsigned char other{};
        symbols.get_symbol(symbol, symbol_name, value, size, bind, type, section_index, other);

        return value / platform->map_record_size;
    };

    vector<raw_program> res;

    for (const auto section : reader.sections) {
        const string name = section->get_name();
        if (!desired_section.empty() && name != desired_section)
            continue;
        if (name == "license" || name == "version" || name == "maps")
            continue;
        if (name != ".text" && name.find('.') == 0) {
            continue;
        }
        info.type = platform->get_program_type(name, path);
        if (section->get_size() == 0)
            continue;
        raw_program prog{path, name, vector_of<ebpf_inst>(section), info};
        auto prelocs = reader.sections[string(".rel") + name];
        if (!prelocs)
            prelocs = reader.sections[string(".rela") + name];

        if (prelocs) {
            ELFIO::const_relocation_section_accessor reloc{reader, prelocs};
            ELFIO::Elf64_Addr offset;
            ELFIO::Elf_Word symbol{};
            ELFIO::Elf_Word type;
            ELFIO::Elf_Sxword addend;
            for (ELFIO::Elf_Xword i = 0; i < reloc.get_entries_num(); i++) {
                if (reloc.get_entry(i, offset, symbol, type, addend)) {
                    ebpf_inst& inst = prog.prog[offset / sizeof(ebpf_inst)];
                    inst.src = 1; // magic number for LoadFd

                    size_t reloc_value = read_reloc_value(symbol);
                    if (reloc_value >= info.map_descriptors.size()) {
                        throw std::runtime_error(string("Bad reloc value (") + std::to_string(reloc_value) + "). "
                                                 + "Make sure to compile with -O2.");
                    }
                    inst.imm = info.map_descriptors.at(reloc_value).original_fd;
                }
            }
        }
        res.push_back(prog);
    }
    if (res.empty()) {
        if (desired_section.empty()) {
            throw std::runtime_error(string("Can't find any non-empty TEXT sections in file ") + path);
        }
        throw std::runtime_error(string("Can't find section ") + desired_section + " in file " + path);
    }
    return res;
}
