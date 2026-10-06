#include "vpk_install.h"

#include "miniz.h"

#include <mbedtls/sha1.h>
#include <mbedtls/sha256.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/promoterutil.h>
#include <psp2/sysmodule.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <vector>

namespace steamvita {
namespace {

constexpr const char* LOG_PATH = "ux0:data/SteamVita/updater.log";

bool is_dot_entry(const char* name) {
    return name &&
           (std::strcmp(name, ".") == 0 ||
            std::strcmp(name, "..") == 0);
}

bool safe_archive_path(const std::string& path) {
    if (path.empty() || path[0] == '/' || path[0] == '\\') return false;
    if (path.find(':') != std::string::npos) return false;
    if (path.find("\\") != std::string::npos) return false;

    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t slash = path.find('/', start);
        const std::size_t end =
            slash == std::string::npos ? path.size() : slash;
        const std::string part = path.substr(start, end - start);
        if (part == "..") return false;
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return true;
}

std::string parent_path(const std::string& path) {
    const std::size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return {};
    return path.substr(0, slash);
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}


std::uint32_t read_be32(const unsigned char* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24u) |
           (static_cast<std::uint32_t>(p[1]) << 16u) |
           (static_cast<std::uint32_t>(p[2]) << 8u) |
           static_cast<std::uint32_t>(p[3]);
}

bool sha1_digest(const unsigned char* data,
                 std::size_t size,
                 unsigned char out[20]) {
    mbedtls_sha1_context ctx;
    mbedtls_sha1_init(&ctx);
    if (mbedtls_sha1_starts(&ctx) != 0) {
        mbedtls_sha1_free(&ctx);
        return false;
    }
    if (size > 0 && mbedtls_sha1_update(&ctx, data, size) != 0) {
        mbedtls_sha1_free(&ctx);
        return false;
    }
    const int rc = mbedtls_sha1_finish(&ctx, out);
    mbedtls_sha1_free(&ctx);
    return rc == 0;
}

bool fpkg_hmac(const unsigned char* data,
               std::size_t size,
               unsigned char out[16]) {
    unsigned char first[20]{};
    if (!sha1_digest(data, size, first)) return false;

    unsigned char block[64]{};
    std::memcpy(&block[0], &first[4], 8);
    std::memcpy(&block[8], &first[4], 8);
    std::memcpy(&block[16], &first[12], 4);
    block[20] = first[16];
    block[21] = first[1];
    block[22] = first[2];
    block[23] = first[3];
    std::memcpy(&block[24], &block[16], 8);

    unsigned char second[20]{};
    if (!sha1_digest(block, sizeof(block), second)) return false;
    std::memcpy(out, second, 16);
    return true;
}

bool load_promoter_modules(bool* loaded_paf, bool* loaded_promoter) {
    *loaded_paf = false;
    *loaded_promoter = false;

    if (sceSysmoduleIsLoadedInternal(SCE_SYSMODULE_INTERNAL_PAF) < 0) {
        uint32_t ptr[0x100] = {0};
        ptr[1] = static_cast<uint32_t>(
            reinterpret_cast<uintptr_t>(&ptr[0]));
        uint32_t args[] = {0x400000u, 0xEA60u, 0x40000u, 0u, 0u};

        const int result = sceSysmoduleLoadModuleInternalWithArg(
            SCE_SYSMODULE_INTERNAL_PAF,
            sizeof(args),
            args,
            reinterpret_cast<SceSysmoduleOpt*>(ptr));
        if (result < 0) return false;
        *loaded_paf = true;
    }

    if (sceSysmoduleIsLoadedInternal(
            SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL) < 0) {
        const int result = sceSysmoduleLoadModuleInternal(
            SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL);
        if (result < 0) {
            if (*loaded_paf) {
                SceSysmoduleOpt opt{};
                sceSysmoduleUnloadModuleInternalWithArg(
                    SCE_SYSMODULE_INTERNAL_PAF, 0, nullptr, &opt);
            }
            return false;
        }
        *loaded_promoter = true;
    }

    return true;
}

void unload_promoter_modules(bool loaded_paf, bool loaded_promoter) {
    if (loaded_promoter) {
        sceSysmoduleUnloadModuleInternal(
            SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL);
    }

    if (loaded_paf) {
        SceSysmoduleOpt opt{};
        sceSysmoduleUnloadModuleInternalWithArg(
            SCE_SYSMODULE_INTERNAL_PAF, 0, nullptr, &opt);
    }
}

} // namespace

void log_line(const std::string& message) {
    ensure_directory("ux0:data/SteamVita");
    std::ofstream output(LOG_PATH, std::ios::app);
    if (output) output << message << "\n";
}

bool path_exists(const std::string& path) {
    SceIoStat stat{};
    return sceIoGetstat(path.c_str(), &stat) >= 0;
}

bool ensure_directory(const std::string& path) {
    if (path.empty()) return false;
    if (path_exists(path)) return true;

    std::size_t start = 0;
    const std::size_t colon = path.find(':');
    if (colon != std::string::npos) start = colon + 1;

    for (std::size_t i = start; i < path.size(); ++i) {
        if (path[i] != '/') continue;
        const std::string part = path.substr(0, i);
        if (!part.empty() && !path_exists(part)) {
            sceIoMkdir(part.c_str(), 0777);
        }
    }

    if (!path_exists(path)) sceIoMkdir(path.c_str(), 0777);
    return path_exists(path);
}

bool remove_tree(const std::string& path) {
    SceIoStat stat{};
    if (sceIoGetstat(path.c_str(), &stat) < 0) return true;

    if ((stat.st_mode & SCE_S_IFDIR) == 0) {
        return sceIoRemove(path.c_str()) >= 0;
    }

    SceUID dir = sceIoDopen(path.c_str());
    if (dir < 0) return false;

    bool ok = true;
    SceIoDirent entry{};
    while (sceIoDread(dir, &entry) > 0) {
        if (is_dot_entry(entry.d_name)) {
            std::memset(&entry, 0, sizeof(entry));
            continue;
        }

        const std::string child = path + "/" + entry.d_name;
        if (!remove_tree(child)) ok = false;
        std::memset(&entry, 0, sizeof(entry));
    }

    sceIoDclose(dir);
    if (sceIoRmdir(path.c_str()) < 0) ok = false;
    return ok;
}

bool extract_zip(const std::string& zip_path,
                 const std::string& destination,
                 std::string* error_message) {
    remove_tree(destination);
    if (!ensure_directory(destination)) {
        if (error_message) *error_message = "Could not create update staging directory.";
        return false;
    }

    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, zip_path.c_str(), 0)) {
        if (error_message) *error_message = "Could not open ZIP archive.";
        return false;
    }

    const mz_uint files = mz_zip_reader_get_num_files(&zip);
    if (files == 0 || files > 4096u) {
        mz_zip_reader_end(&zip);
        if (error_message) *error_message = "ZIP archive has an invalid file count.";
        return false;
    }

    bool ok = true;
    for (mz_uint i = 0; i < files; ++i) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, i, &stat) ||
            !stat.m_filename) {
            ok = false;
            if (error_message) *error_message = "Could not read ZIP archive contents.";
            break;
        }

        const std::string relative = stat.m_filename;
        if (!safe_archive_path(relative)) {
            ok = false;
            if (error_message) *error_message = "ZIP archive contains an unsafe path.";
            break;
        }

        const std::string target = destination + "/" + relative;
        if (mz_zip_reader_is_file_a_directory(&zip, i)) {
            if (!ensure_directory(target)) {
                ok = false;
                if (error_message) *error_message = "Could not create an update directory.";
                break;
            }
            continue;
        }

        const std::string parent = parent_path(target);
        if (!parent.empty() && !ensure_directory(parent)) {
            ok = false;
            if (error_message) *error_message = "Could not create update file directories.";
            break;
        }

        if (!mz_zip_reader_extract_to_file(
                &zip, i, target.c_str(), 0)) {
            ok = false;
            if (error_message) *error_message = "Could not extract ZIP archive.";
            break;
        }
    }

    mz_zip_reader_end(&zip);

    if (!ok) {
        remove_tree(destination);
        return false;
    }

    return true;
}

bool extract_vpk(const std::string& vpk_path,
                 const std::string& destination,
                 std::string* error_message) {
    if (!extract_zip(vpk_path, destination, error_message)) {
        return false;
    }

    if (!path_exists(destination + "/eboot.bin") ||
        !path_exists(destination + "/sce_sys/param.sfo")) {
        remove_tree(destination);
        if (error_message) {
            *error_message = "Extracted update package is missing required files.";
        }
        return false;
    }

    return true;
}

bool verify_sha256_file(const std::string& path,
                        const std::string& expected_hex,
                        std::string* error_message) {
    if (expected_hex.size() != 64u) {
        if (error_message) *error_message = "Update checksum is malformed.";
        return false;
    }

    unsigned char expected[32]{};
    for (std::size_t i = 0; i < 32u; ++i) {
        const int hi = hex_value(expected_hex[i * 2]);
        const int lo = hex_value(expected_hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            if (error_message) *error_message = "Update checksum is malformed.";
            return false;
        }
        expected[i] = static_cast<unsigned char>((hi << 4) | lo);
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        if (error_message) *error_message = "Downloaded update could not be opened.";
        return false;
    }

    mbedtls_sha256_context context;
    mbedtls_sha256_init(&context);
    if (mbedtls_sha256_starts(&context, 0) != 0) {
        mbedtls_sha256_free(&context);
        if (error_message) *error_message = "Could not initialize SHA-256.";
        return false;
    }

    char buffer[16 * 1024];
    while (input.good()) {
        input.read(buffer, sizeof(buffer));
        const std::streamsize bytes = input.gcount();
        if (bytes <= 0) break;

        if (mbedtls_sha256_update(
                &context,
                reinterpret_cast<const unsigned char*>(buffer),
                static_cast<std::size_t>(bytes)) != 0) {
            mbedtls_sha256_free(&context);
            if (error_message) *error_message = "Could not hash the downloaded update.";
            return false;
        }
    }

    unsigned char actual[32]{};
    const int finish = mbedtls_sha256_finish(&context, actual);
    mbedtls_sha256_free(&context);
    if (finish != 0) {
        if (error_message) *error_message = "Could not finish update verification.";
        return false;
    }

    if (std::memcmp(actual, expected, sizeof(actual)) != 0) {
        if (error_message) *error_message = "Downloaded update failed SHA-256 verification.";
        return false;
    }

    return true;
}


bool prepare_package_head(const std::string& directory,
                          const std::string& title_id,
                          std::string* error_message) {
    if (title_id.size() != 9u) {
        if (error_message) *error_message = "Updater title ID is invalid.";
        return false;
    }

    std::ifstream input("app0:/head.bin", std::ios::binary);
    if (!input) {
        if (error_message) *error_message = "Vita package header template is missing.";
        return false;
    }

    std::vector<unsigned char> head(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    if (head.size() != 1072u) {
        if (error_message) *error_message = "Vita package header template is invalid.";
        return false;
    }

    std::string content_id =
        "EP9000-" + title_id + "_00-0000000000000000";
    if (content_id.size() > 48u) {
        if (error_message) *error_message = "Updater content ID is invalid.";
        return false;
    }

    std::memset(&head[0x30], 0, 48);
    std::memcpy(&head[0x30], content_id.data(), content_id.size());

    const std::uint32_t header_len = read_be32(&head[0xD0]);
    const std::uint32_t info_off = read_be32(&head[0x08]);
    const std::uint32_t info_len = read_be32(&head[0x10]);
    const std::uint32_t info_out = read_be32(&head[0xD4]);
    const std::uint32_t all_len = read_be32(&head[0xE8]);

    if (header_len + 16u > head.size() ||
        info_len < 64u ||
        static_cast<std::size_t>(info_off) + info_len > head.size() ||
        info_out + 16u > head.size() ||
        all_len + 16u > head.size()) {
        if (error_message) *error_message = "Vita package header template is malformed.";
        return false;
    }

    unsigned char mac[16]{};
    if (!fpkg_hmac(head.data(), header_len, mac)) {
        if (error_message) *error_message = "Could not prepare Vita package header.";
        return false;
    }
    std::memcpy(&head[header_len], mac, sizeof(mac));

    if (!fpkg_hmac(&head[info_off], info_len - 64u, mac)) {
        if (error_message) *error_message = "Could not prepare Vita package metadata.";
        return false;
    }
    std::memcpy(&head[info_out], mac, sizeof(mac));

    if (!fpkg_hmac(head.data(), all_len, mac)) {
        if (error_message) *error_message = "Could not finalize Vita package header.";
        return false;
    }
    std::memcpy(&head[all_len], mac, sizeof(mac));

    const std::string package_dir = directory + "/sce_sys/package";
    if (!ensure_directory(package_dir)) {
        if (error_message) *error_message = "Could not create Vita package metadata directory.";
        return false;
    }

    std::ofstream output(package_dir + "/head.bin",
                         std::ios::binary | std::ios::trunc);
    if (!output) {
        if (error_message) *error_message = "Could not write Vita package header.";
        return false;
    }
    output.write(reinterpret_cast<const char*>(head.data()),
                 static_cast<std::streamsize>(head.size()));
    output.flush();
    if (!output.good()) {
        if (error_message) *error_message = "Could not save Vita package header.";
        return false;
    }
    return true;
}

bool promote_directory(const std::string& directory,
                       std::string* error_message) {
    if (!path_exists(directory + "/eboot.bin") ||
        !path_exists(directory + "/sce_sys/param.sfo")) {
        if (error_message) *error_message = "Update package is incomplete.";
        return false;
    }

    bool loaded_paf = false;
    bool loaded_promoter = false;
    if (!load_promoter_modules(&loaded_paf, &loaded_promoter)) {
        if (error_message) *error_message = "Could not load the Vita installer service.";
        return false;
    }

    const int init = scePromoterUtilityInit();
    if (init < 0) {
        unload_promoter_modules(loaded_paf, loaded_promoter);
        if (error_message) *error_message = "Could not initialize the Vita installer.";
        return false;
    }

    const int promote = scePromoterUtilityPromotePkgWithRif(
        directory.c_str(), 1);
    if (promote < 0) {
        scePromoterUtilityExit();
        unload_promoter_modules(loaded_paf, loaded_promoter);
        if (error_message) {
            std::ostringstream message;
            message << "Vita installer rejected the package: 0x"
                    << std::hex << static_cast<unsigned>(promote);
            *error_message = message.str();
        }
        return false;
    }

    int state = 1;
    int polls = 0;
    int state_call = 0;
    while (polls < 12000) {
        state = 0;
        state_call = scePromoterUtilityGetState(&state);
        if (state_call < 0) break;
        if (state == 0) break;
        sceKernelDelayThread(10 * 1000);
        ++polls;
    }

    int operation_result = 0;
    const int result_call =
        scePromoterUtilityGetResult(&operation_result);
    scePromoterUtilityExit();
    unload_promoter_modules(loaded_paf, loaded_promoter);

    if (state != 0 || state_call < 0 ||
        result_call < 0 || operation_result < 0) {
        if (error_message) {
            std::ostringstream message;
            message << "Vita install failed"
                    << " state=" << state
                    << " state_call=0x" << std::hex
                    << static_cast<unsigned>(state_call)
                    << " result_call=0x"
                    << static_cast<unsigned>(result_call)
                    << " operation=0x"
                    << static_cast<unsigned>(operation_result);
            *error_message = message.str();
        }
        return false;
    }

    return true;
}

} // namespace steamvita
