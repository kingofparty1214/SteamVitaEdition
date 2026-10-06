#include "x86_runtime.h"

#include <cstdio>
#include <cstring>

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

    unsigned char dos[64] {};
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

    unsigned char header[64] {};
    if (std::fread(header, 1, sizeof(header), file) < 48) {
        std::fclose(file);
        info.detail = "Incomplete PE header.";
        return info;
    }
    std::fclose(file);

    if (std::memcmp(header, "PE\0\0", 4) != 0) {
        info.detail = "Invalid PE signature.";
        return info;
    }

    info.machine = read_u16_le(header + 4);
    info.optional_magic = read_u16_le(header + 24);
    info.entry_rva = read_u32_le(header + 40);

    constexpr std::uint16_t IMAGE_FILE_MACHINE_I386 = 0x014c;
    constexpr std::uint16_t IMAGE_FILE_MACHINE_AMD64 = 0x8664;
    constexpr std::uint16_t PE32_MAGIC = 0x10b;
    constexpr std::uint16_t PE32_PLUS_MAGIC = 0x20b;

    if (info.optional_magic == PE32_MAGIC) {
        info.image_base = read_u32_le(header + 52);
    } else if (info.optional_magic == PE32_PLUS_MAGIC) {
        info.image_base = read_u64_le(header + 48);
    }

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
