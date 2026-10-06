#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

enum class GuestArchitecture {
    Unknown,
    X86_32,
    X86_64,
    Other,
};

struct PeImageInfo {
    bool valid = false;
    GuestArchitecture architecture = GuestArchitecture::Unknown;
    std::uint16_t machine = 0;
    std::uint16_t optional_magic = 0;
    std::uint32_t entry_rva = 0;
    std::uint64_t image_base = 0;
    std::vector<std::string> imported_dlls;
    std::string detail;
};

PeImageInfo probe_pe_image(const std::string& path);

enum class X86IrOp {
    Nop,
    MovRegImm32,
    Ret,
    Unsupported,
};

struct X86IrInstruction {
    X86IrOp op = X86IrOp::Unsupported;
    std::uint8_t reg = 0;
    std::uint32_t immediate = 0;
    std::size_t length = 0;
};

bool decode_x86_basic(const std::uint8_t* code,
                      std::size_t size,
                      X86IrInstruction* out);


enum class X64IrOp {
    Nop,
    Ret,
    MovRegImm64,
    Unsupported,
};

struct X64IrInstruction {
    X64IrOp op = X64IrOp::Unsupported;
    std::uint8_t reg = 0;
    std::uint64_t immediate = 0;
    std::size_t length = 0;
};

bool decode_x64_basic(const std::uint8_t* code,
                      std::size_t size,
                      X64IrInstruction* out);
