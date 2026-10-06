#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "x86_runtime.h"

enum class Win32ShimModule {
    Unknown,
    Kernel32,
    User32,
    WinMM,
    Winsock,
    OpenGL,
    Advapi32,
    Gdi32,
    Shell32,
    Ole32,
    Version,
    Imm32,
    SetupApi,
    Bcrypt,
    Crypt32,
    WinHttp,
    Hid,
};

struct Win32ShimSymbol {
    std::string dll;
    std::string name;
    std::uint16_t ordinal = 0;
    bool by_ordinal = false;
    Win32ShimModule module = Win32ShimModule::Unknown;
    std::uint64_t guest_address = 0;
};

struct Win32ImportBindResult {
    bool success = false;
    std::vector<Win32ShimSymbol> bound;
    std::vector<PeImportSymbol> unresolved;
    std::string detail;
};

Win32ShimModule win32_shim_module_for_dll(const std::string& dll);
bool win32_shim_has_symbol(const PeImportSymbol& symbol);

Win32ImportBindResult bind_win32_imports(PeLoadedImage* image);

const char* win32_shim_module_label(Win32ShimModule module);
