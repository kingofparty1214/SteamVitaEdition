#include "win32_shims.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace {

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
    return value;
}

bool name_in(const std::string& value,
             std::initializer_list<const char*> names) {
    for (const char* name : names) {
        if (value == name) return true;
    }
    return false;
}

void write_u32_le(std::uint8_t* data, std::uint32_t value) {
    for (unsigned i = 0; i < 4u; ++i) {
        data[i] = static_cast<std::uint8_t>((value >> (i * 8u)) & 0xffu);
    }
}

void write_u64_le(std::uint8_t* data, std::uint64_t value) {
    for (unsigned i = 0; i < 8u; ++i) {
        data[i] = static_cast<std::uint8_t>((value >> (i * 8u)) & 0xffu);
    }
}

bool kernel32_symbol(const std::string& name) {
    static const std::set<std::string> names = {
        "getlasterror", "setlasterror",
        "getcurrentprocess", "getcurrentprocessid",
        "getcurrentthread", "getcurrentthreadid",
        "queryperformancecounter", "queryperformancefrequency",
        "getsystemtimeasfiletime", "getsystemtime", "getlocaltime",
        "gettickcount", "sleep", "sleepex", "switchtothread",
        "getprocessheap", "heapalloc", "heapfree", "heapsize", "heaprealloc",
        "virtualalloc", "virtualfree", "virtualprotect", "virtualquery",
        "initializecriticalsection", "initializecriticalsectionandspincount",
        "initializecriticalsectionex", "entercriticalsection",
        "tryentercriticalsection", "leavecriticalsection",
        "deletecriticalsection", "initializeslisthead",
        "tlsalloc", "tlsfree", "tlsgetvalue", "tlssetvalue",
        "createthread", "exitthread",
        "createeventa", "createeventw", "createeventexw",
        "setevent", "resetevent",
        "waitforsingleobject", "waitforsingleobjectex",
        "waitformultipleobjects", "waitformultipleobjectsex",
        "createfilea", "createfilew", "readfile", "writefile",
        "closehandle", "flushfilebuffers",
        "setfilepointer", "setfilepointerex", "setendoffile",
        "getfilesize", "getfiletype",
        "getfileattributesa", "getfileattributesw", "getfileattributesexw",
        "createDirectoryw", "deletefilew", "copyfilew", "movefileexw",
        "getfullpathnamew", "gettemppathw", "gettempfilenamew",
        "findfirstfilew", "findfirstfileexw", "findnextfilew", "findclose",
        "getmodulehandlea", "getmodulehandlew", "getmodulehandleexw",
        "getmodulefilenamea", "getmodulefilenamew",
        "loadlibrarya", "loadlibraryw", "loadlibraryexw",
        "freelibrary", "getprocaddress",
        "getcommandlinea", "getcommandlinew",
        "getenvironmentstringsw", "freeenvironmentstringsw",
        "getenvironmentvariablea", "setenvironmentvariablew",
        "multibytetowidechar", "widechartomultibyte",
        "getacp", "getoemcp", "getcpinfo", "isvalidcodepage",
        "getstdhandle", "setstdhandle", "writeconsolew", "readconsolew",
        "getconsolemode", "setconsolemode", "getconsolecp",
        "outputdebugstringa", "isdebuggerpresent",
        "exitprocess", "terminateprocess"
    };
    return names.find(lower_ascii(name)) != names.end();
}

bool user32_symbol(const std::string& name) {
    static const std::set<std::string> names = {
        "createwindowexw", "destroywindow", "showwindow", "updatewindow",
        "defwindowprocw", "registerclassw", "registerclassexw",
        "unregisterclassw", "getclientrect", "getwindowrect",
        "setwindowpos", "getdesktopwindow",
        "peekmessagea", "getmessagea", "translatemessage",
        "dispatchmessagea", "postquitmessage",
        "getkeystate", "getasynckeystate", "mapvirtualkeya",
        "getcursorpos", "setcursorpos", "showcursor", "clipcursor",
        "setfocus", "getfocus", "getactivewindow",
        "getsystemmetrics", "messageboxa", "messageboxw"
    };
    return names.find(lower_ascii(name)) != names.end();
}

bool winmm_symbol(const std::string& name) {
    const std::string n = lower_ascii(name);
    return n == "timegettime" || n == "timebeginperiod" ||
           n == "timeendperiod" || n.find("waveout") == 0 ||
           n.find("wavein") == 0;
}

bool winsock_symbol(const PeImportSymbol& symbol) {
    if (symbol.by_ordinal) return true;
    const std::string n = lower_ascii(symbol.name);
    return n.find("wsa") == 0 ||
           name_in(n, {"getaddrinfo", "freeaddrinfo", "getnameinfo"});
}

bool opengl_symbol(const std::string& name) {
    const std::string n = lower_ascii(name);
    return n.find("wgl") == 0;
}

} // namespace

Win32ShimModule win32_shim_module_for_dll(const std::string& raw_dll) {
    const std::string dll = lower_ascii(raw_dll);
    if (dll == "kernel32.dll") return Win32ShimModule::Kernel32;
    if (dll == "user32.dll") return Win32ShimModule::User32;
    if (dll == "winmm.dll") return Win32ShimModule::WinMM;
    if (dll == "ws2_32.dll") return Win32ShimModule::Winsock;
    if (dll == "opengl32.dll") return Win32ShimModule::OpenGL;
    if (dll == "advapi32.dll") return Win32ShimModule::Advapi32;
    if (dll == "gdi32.dll") return Win32ShimModule::Gdi32;
    if (dll == "shell32.dll") return Win32ShimModule::Shell32;
    if (dll == "ole32.dll" || dll == "oleaut32.dll") return Win32ShimModule::Ole32;
    if (dll == "version.dll") return Win32ShimModule::Version;
    if (dll == "imm32.dll") return Win32ShimModule::Imm32;
    if (dll == "setupapi.dll") return Win32ShimModule::SetupApi;
    if (dll == "bcrypt.dll") return Win32ShimModule::Bcrypt;
    if (dll == "crypt32.dll") return Win32ShimModule::Crypt32;
    if (dll == "winhttp.dll") return Win32ShimModule::WinHttp;
    if (dll == "hid.dll") return Win32ShimModule::Hid;
    return Win32ShimModule::Unknown;
}

bool win32_shim_has_symbol(const PeImportSymbol& symbol) {
    switch (win32_shim_module_for_dll(symbol.dll)) {
        case Win32ShimModule::Kernel32:
            return !symbol.by_ordinal && kernel32_symbol(symbol.name);
        case Win32ShimModule::User32:
            return !symbol.by_ordinal && user32_symbol(symbol.name);
        case Win32ShimModule::WinMM:
            return !symbol.by_ordinal && winmm_symbol(symbol.name);
        case Win32ShimModule::Winsock:
            return winsock_symbol(symbol);
        case Win32ShimModule::OpenGL:
            return !symbol.by_ordinal && opengl_symbol(symbol.name);
        default:
            return false;
    }
}

Win32ImportBindResult bind_win32_imports(PeLoadedImage* image) {
    Win32ImportBindResult result;
    if (!image || !image->valid || image->image.empty()) {
        result.detail = "No loaded PE image is available for import binding.";
        return result;
    }

    const bool is_x64 = image->architecture == GuestArchitecture::X86_64;
    const std::size_t pointer_size = is_x64 ? 8u : 4u;
    std::uint64_t next_guest_address =
        is_x64 ? 0x0000000071000000ull : 0x71000000ull;

    for (const PeImportSymbol& import : image->imports) {
        if (!win32_shim_has_symbol(import)) {
            result.unresolved.push_back(import);
            continue;
        }

        if (import.iat_rva >= image->image.size() ||
            image->image.size() - import.iat_rva < pointer_size) {
            result.unresolved.push_back(import);
            continue;
        }

        Win32ShimSymbol shim;
        shim.dll = import.dll;
        shim.name = import.name;
        shim.ordinal = import.ordinal;
        shim.by_ordinal = import.by_ordinal;
        shim.module = win32_shim_module_for_dll(import.dll);
        shim.guest_address = next_guest_address;
        next_guest_address += 0x20u;

        if (is_x64) {
            write_u64_le(image->image.data() + import.iat_rva,
                         shim.guest_address);
        } else {
            write_u32_le(image->image.data() + import.iat_rva,
                         static_cast<std::uint32_t>(shim.guest_address));
        }

        result.bound.push_back(shim);
    }

    result.success = result.unresolved.empty();
    if (result.success) {
        result.detail =
            "All recognized Win32 imports were bound to Vita Proton trampolines.";
    } else {
        result.detail =
            std::to_string(result.bound.size()) + " imports bound, " +
            std::to_string(result.unresolved.size()) + " still unresolved.";
    }
    return result;
}

const char* win32_shim_module_label(Win32ShimModule module) {
    switch (module) {
        case Win32ShimModule::Kernel32: return "Kernel32";
        case Win32ShimModule::User32: return "User32";
        case Win32ShimModule::WinMM: return "WinMM";
        case Win32ShimModule::Winsock: return "Winsock";
        case Win32ShimModule::OpenGL: return "OpenGL";
        case Win32ShimModule::Advapi32: return "Advapi32";
        case Win32ShimModule::Gdi32: return "GDI32";
        case Win32ShimModule::Shell32: return "Shell32";
        case Win32ShimModule::Ole32: return "OLE";
        case Win32ShimModule::Version: return "Version";
        case Win32ShimModule::Imm32: return "IMM32";
        case Win32ShimModule::SetupApi: return "SetupAPI";
        case Win32ShimModule::Bcrypt: return "BCrypt";
        case Win32ShimModule::Crypt32: return "Crypt32";
        case Win32ShimModule::WinHttp: return "WinHTTP";
        case Win32ShimModule::Hid: return "HID";
        case Win32ShimModule::Unknown: break;
    }
    return "Unknown";
}
