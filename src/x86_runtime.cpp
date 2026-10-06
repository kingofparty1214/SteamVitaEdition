#include "x86_runtime.h"

#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>

namespace {

std::uint16_t read_u16_le(const unsigned char* data) {
    return static_cast<std::uint16_t>(data[0]) |
           (static_cast<std::uint16_t>(data[1]) << 8u);
}

std::uint32_t read_u32_le(const unsigned char* data) {
    return static_cast<std::uint32_t>(data[0]) |
           (static_cast<std::uint32_t>(data[1]) << 8u) |
           (static_cast<std::uint32_t>(data[2]) << 16u) |
           (static_cast<std::uint32_t>(data[3]) << 24u);
}

std::uint64_t read_u64_le(const unsigned char* data) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(data[i]) << (i * 8u);
    }
    return value;
}

} // namespace

PeImageInfo probe_pe_image(const std::string& path) {
    PeImageInfo info;

    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) {
        info.detail = "Could not open executable.";
        return info;
    }

    unsigned char dos[64]{};
    if (std::fread(dos, 1, sizeof(dos), file) != sizeof(dos) ||
        dos[0] != 'M' || dos[1] != 'Z') {
        std::fclose(file);
        info.detail = "Not a valid DOS/PE executable.";
        return info;
    }

    const std::uint32_t pe_offset = read_u32_le(dos + 0x3c);
    if (std::fseek(file, static_cast<long>(pe_offset), SEEK_SET) != 0) {
        std::fclose(file);
        info.detail = "Invalid PE header offset.";
        return info;
    }

    unsigned char coff[24]{};
    if (std::fread(coff, 1, sizeof(coff), file) != sizeof(coff) ||
        std::memcmp(coff, "PE\0\0", 4) != 0) {
        std::fclose(file);
        info.detail = "Invalid PE signature.";
        return info;
    }

    info.machine = read_u16_le(coff + 4);
    const std::uint16_t section_count = read_u16_le(coff + 6);
    const std::uint16_t optional_size = read_u16_le(coff + 20);

    if (optional_size < 64u || optional_size > 4096u) {
        std::fclose(file);
        info.detail = "Invalid PE optional header size.";
        return info;
    }

    std::vector<unsigned char> optional(optional_size);
    if (std::fread(optional.data(), 1, optional.size(), file) != optional.size()) {
        std::fclose(file);
        info.detail = "Incomplete PE optional header.";
        return info;
    }

    info.optional_magic = read_u16_le(optional.data());
    info.entry_rva = read_u32_le(optional.data() + 16u);
    info.size_of_image = read_u32_le(optional.data() + 56u);
    info.size_of_headers = read_u32_le(optional.data() + 60u);

    constexpr std::uint16_t IMAGE_FILE_MACHINE_I386 = 0x014c;
    constexpr std::uint16_t IMAGE_FILE_MACHINE_AMD64 = 0x8664;
    constexpr std::uint16_t PE32_MAGIC = 0x10b;
    constexpr std::uint16_t PE32_PLUS_MAGIC = 0x20b;

    std::size_t data_directory_offset = 0;
    if (info.optional_magic == PE32_MAGIC) {
        if (optional.size() < 112u) {
            std::fclose(file);
            info.detail = "Incomplete PE32 optional header.";
            return info;
        }
        info.image_base = read_u32_le(optional.data() + 28u);
        data_directory_offset = 96u;
    } else if (info.optional_magic == PE32_PLUS_MAGIC) {
        if (optional.size() < 128u) {
            std::fclose(file);
            info.detail = "Incomplete PE32+ optional header.";
            return info;
        }
        info.image_base = read_u64_le(optional.data() + 24u);
        data_directory_offset = 112u;
    }

    std::vector<PeSectionInfo> sections;
    sections.reserve(section_count);

    for (std::uint16_t i = 0; i < section_count; ++i) {
        unsigned char sh[40]{};
        if (std::fread(sh, 1, sizeof(sh), file) != sizeof(sh)) break;

        PeSectionInfo section;
        for (unsigned name_index = 0; name_index < 8u && sh[name_index] != 0u;
             ++name_index) {
            section.name.push_back(static_cast<char>(sh[name_index]));
        }
        section.virtual_size = read_u32_le(sh + 8u);
        section.virtual_address = read_u32_le(sh + 12u);
        section.raw_size = read_u32_le(sh + 16u);
        section.raw_offset = read_u32_le(sh + 20u);
        section.characteristics = read_u32_le(sh + 36u);
        sections.push_back(section);
    }

    auto rva_to_file = [&](std::uint32_t rva, std::uint32_t* out) -> bool {
        if (!out) return false;
        for (const PeSectionInfo& section : sections) {
            const std::uint32_t span =
                std::max(section.virtual_size, section.raw_size);
            if (rva >= section.virtual_address &&
                rva - section.virtual_address < span) {
                *out = section.raw_offset + (rva - section.virtual_address);
                return true;
            }
        }
        return false;
    };

    if (data_directory_offset != 0 &&
        optional.size() >= data_directory_offset + 16u) {
        const std::uint32_t import_rva =
            read_u32_le(optional.data() + data_directory_offset + 8u);

        std::uint32_t import_offset = 0;
        if (import_rva != 0 && rva_to_file(import_rva, &import_offset) &&
            std::fseek(file, static_cast<long>(import_offset), SEEK_SET) == 0) {
            for (unsigned descriptor_index = 0;
                 descriptor_index < 256u;
                 ++descriptor_index) {
                unsigned char descriptor[20]{};
                if (std::fread(descriptor, 1, sizeof(descriptor), file) !=
                    sizeof(descriptor)) {
                    break;
                }

                bool all_zero = true;
                for (unsigned char byte : descriptor) {
                    if (byte != 0u) {
                        all_zero = false;
                        break;
                    }
                }
                if (all_zero) break;

                const std::uint32_t name_rva =
                    read_u32_le(descriptor + 12u);
                std::uint32_t name_offset = 0;
                if (!rva_to_file(name_rva, &name_offset)) continue;

                const long resume = std::ftell(file);
                if (resume < 0 ||
                    std::fseek(file, static_cast<long>(name_offset), SEEK_SET) != 0) {
                    continue;
                }

                std::string dll;
                for (std::size_t n = 0; n < 260u; ++n) {
                    const int ch = std::fgetc(file);
                    if (ch <= 0) break;
                    dll.push_back(static_cast<char>(ch));
                }

                std::fseek(file, resume, SEEK_SET);

                if (!dll.empty()) {
                    std::transform(dll.begin(), dll.end(), dll.begin(),
                        [](unsigned char ch) {
                            if (ch >= 'A' && ch <= 'Z') {
                                return static_cast<char>(ch - 'A' + 'a');
                            }
                            return static_cast<char>(ch);
                        });
                    if (std::find(info.imported_dlls.begin(),
                                  info.imported_dlls.end(),
                                  dll) == info.imported_dlls.end()) {
                        info.imported_dlls.push_back(dll);
                    }
                }
            }
        }
    }

    info.sections = sections;
    std::fclose(file);

    info.valid = true;
    if (info.machine == IMAGE_FILE_MACHINE_I386 &&
        info.optional_magic == PE32_MAGIC) {
        info.architecture = GuestArchitecture::X86_32;
        info.detail = "PE32 x86 executable.";
    } else if (info.machine == IMAGE_FILE_MACHINE_AMD64 &&
               info.optional_magic == PE32_PLUS_MAGIC) {
        info.architecture = GuestArchitecture::X86_64;
        info.detail = "PE32+ x86-64 executable.";
    } else {
        info.architecture = GuestArchitecture::Other;
        info.detail = "PE executable uses an unsupported architecture.";
    }

    return info;
}


PeLoadedImage load_pe_image(const std::string& path) {
    PeLoadedImage loaded;
    const PeImageInfo info = probe_pe_image(path);

    loaded.architecture = info.architecture;
    loaded.preferred_image_base = info.image_base;
    loaded.entry_rva = info.entry_rva;

    if (!info.valid) {
        loaded.detail = info.detail;
        return loaded;
    }

    constexpr std::uint32_t MAX_MAPPED_IMAGE_SIZE = 64u * 1024u * 1024u;
    if (info.size_of_image == 0u ||
        info.size_of_image > MAX_MAPPED_IMAGE_SIZE) {
        loaded.detail = "PE image is too large to map safely on Vita.";
        return loaded;
    }

    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) {
        loaded.detail = "Could not reopen executable for image mapping.";
        return loaded;
    }

    try {
        loaded.image.assign(info.size_of_image, 0u);
    } catch (...) {
        std::fclose(file);
        loaded.detail = "Not enough memory to map PE image.";
        return loaded;
    }

    const std::size_t header_bytes =
        std::min<std::size_t>(info.size_of_headers, loaded.image.size());
    if (header_bytes > 0u) {
        if (std::fseek(file, 0, SEEK_SET) != 0 ||
            std::fread(loaded.image.data(), 1, header_bytes, file) != header_bytes) {
            std::fclose(file);
            loaded.image.clear();
            loaded.detail = "Could not map PE headers.";
            return loaded;
        }
    }

    for (const PeSectionInfo& section : info.sections) {
        if (section.raw_size == 0u) continue;
        if (section.virtual_address >= loaded.image.size()) {
            std::fclose(file);
            loaded.image.clear();
            loaded.detail = "PE section virtual address is outside image.";
            return loaded;
        }

        const std::size_t available =
            loaded.image.size() - section.virtual_address;
        const std::size_t copy_size =
            std::min<std::size_t>(section.raw_size, available);

        if (std::fseek(file, static_cast<long>(section.raw_offset), SEEK_SET) != 0 ||
            std::fread(loaded.image.data() + section.virtual_address,
                       1, copy_size, file) != copy_size) {
            std::fclose(file);
            loaded.image.clear();
            loaded.detail = "Could not map PE section " + section.name + ".";
            return loaded;
        }
    }

    std::fclose(file);

    if (loaded.entry_rva >= loaded.image.size()) {
        loaded.image.clear();
        loaded.detail = "PE entry point is outside mapped image.";
        return loaded;
    }

    loaded.valid = true;
    loaded.detail = "PE image mapped and entry point located.";
    return loaded;
}

bool decode_x86_basic(const std::uint8_t* code,
                      std::size_t size,
                      X86IrInstruction* out) {
    if (!code || !out || size == 0) return false;

    X86IrInstruction instruction;
    const std::uint8_t opcode = code[0];

    if (opcode == 0x90) {
        instruction.op = X86IrOp::Nop;
        instruction.length = 1;
    } else if (opcode == 0xc3) {
        instruction.op = X86IrOp::Ret;
        instruction.length = 1;
    } else if (opcode >= 0xb8 && opcode <= 0xbf && size >= 5) {
        instruction.op = X86IrOp::MovRegImm32;
        instruction.reg = static_cast<std::uint8_t>(opcode - 0xb8);
        instruction.immediate =
            static_cast<std::uint32_t>(code[1]) |
            (static_cast<std::uint32_t>(code[2]) << 8u) |
            (static_cast<std::uint32_t>(code[3]) << 16u) |
            (static_cast<std::uint32_t>(code[4]) << 24u);
        instruction.length = 5;
    } else {
        instruction.op = X86IrOp::Unsupported;
        instruction.length = 1;
    }

    *out = instruction;
    return instruction.op != X86IrOp::Unsupported;
}


bool decode_x64_basic(const std::uint8_t* code,
                      std::size_t size,
                      X64IrInstruction* out) {
    if (!code || !out || size == 0) return false;

    X64IrInstruction instruction;

    std::size_t index = 0;
    std::uint8_t rex = 0;
    if (code[index] >= 0x40 && code[index] <= 0x4f) {
        rex = code[index++];
        if (index >= size) {
            instruction.op = X64IrOp::Unsupported;
            instruction.length = index;
            *out = instruction;
            return false;
        }
    }

    const std::uint8_t opcode = code[index];

    if (opcode == 0x90) {
        instruction.op = X64IrOp::Nop;
        instruction.length = index + 1u;
    } else if (opcode == 0xc3) {
        instruction.op = X64IrOp::Ret;
        instruction.length = index + 1u;
    } else if (opcode >= 0xb8 && opcode <= 0xbf &&
               (rex & 0x08u) != 0u &&
               size >= index + 9u) {
        instruction.op = X64IrOp::MovRegImm64;
        instruction.reg = static_cast<std::uint8_t>(
            (opcode - 0xb8u) | ((rex & 0x01u) ? 8u : 0u));

        std::uint64_t value = 0;
        for (unsigned i = 0; i < 8u; ++i) {
            value |= static_cast<std::uint64_t>(code[index + 1u + i])
                     << (i * 8u);
        }
        instruction.immediate = value;
        instruction.length = index + 9u;
    } else {
        instruction.op = X64IrOp::Unsupported;
        instruction.length = index + 1u;
    }

    *out = instruction;
    return instruction.op != X64IrOp::Unsupported;
}
