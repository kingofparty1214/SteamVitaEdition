#include "dev_tools.h"

#include <psp2/kernel/threadmgr.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <sys/stat.h>
#include <vector>

namespace {

constexpr const char* DATA_DIR = "ux0:data/SteamVita";
constexpr const char* LOG_DIR = "ux0:data/SteamVita/logs";
constexpr const char* SETTINGS_PATH = "ux0:data/SteamVita/settings.ini";
constexpr std::size_t MAX_LOG_SIZE = 256u * 1024u;
constexpr int MAX_ARCHIVES = 4;
constexpr std::size_t MAX_MEMORY_LINES = 200;

std::mutex g_log_mutex;
FILE* g_log_file = nullptr;
std::vector<std::string> g_recent;

bool ensure_dir(const char* path) {
    struct stat info {};
    if (stat(path, &info) == 0) return S_ISDIR(info.st_mode);
    return mkdir(path, 0777) == 0;
}

std::string archive_path(int index) {
    std::ostringstream out;
    out << LOG_DIR << "/steamvita." << index << ".log";
    return out.str();
}

const char* current_log_path() {
    return "ux0:data/SteamVita/logs/steamvita.log";
}

long file_size(FILE* file) {
    if (!file) return 0;
    const long current = std::ftell(file);
    if (current < 0) return 0;
    if (std::fseek(file, 0, SEEK_END) != 0) return 0;
    const long size = std::ftell(file);
    std::fseek(file, current, SEEK_SET);
    return size < 0 ? 0 : size;
}

void rotate_locked() {
    if (g_log_file) {
        std::fclose(g_log_file);
        g_log_file = nullptr;
    }

    std::remove(archive_path(MAX_ARCHIVES).c_str());
    for (int i = MAX_ARCHIVES - 1; i >= 1; --i) {
        const std::string from = archive_path(i);
        const std::string to = archive_path(i + 1);
        std::rename(from.c_str(), to.c_str());
    }
    std::rename(current_log_path(), archive_path(1).c_str());
    g_log_file = std::fopen(current_log_path(), "ab");
}

void push_recent_locked(const std::string& line) {
    g_recent.push_back(line);
    if (g_recent.size() > MAX_MEMORY_LINES) {
        g_recent.erase(
            g_recent.begin(),
            g_recent.begin() +
                static_cast<std::ptrdiff_t>(
                    g_recent.size() - MAX_MEMORY_LINES));
    }
}

} // namespace

bool load_steamvita_settings(SteamVitaSettings* settings) {
    if (!settings) return false;
    *settings = SteamVitaSettings{};

    std::ifstream input(SETTINGS_PATH);
    if (!input) return false;

    std::string line;
    while (std::getline(input, line)) {
        if (line == "developer_mode=1") settings->developer_mode = true;
        else if (line == "verbose_logging=1") settings->verbose_logging = true;
        else if (line == "ui_ambience=0") settings->ui_ambience = false;
        else if (line == "menu_sounds=0") settings->menu_sounds = false;
        else if (line == "background_music=1") settings->background_music = true;
    }
    return true;
}

bool save_steamvita_settings(const SteamVitaSettings& settings) {
    ensure_dir(DATA_DIR);
    std::ofstream output(SETTINGS_PATH, std::ios::trunc);
    if (!output) return false;
    output << "developer_mode=" << (settings.developer_mode ? 1 : 0) << "\n";
    output << "verbose_logging=" << (settings.verbose_logging ? 1 : 0) << "\n";
    output << "ui_ambience=" << (settings.ui_ambience ? 1 : 0) << "\n";
    output << "menu_sounds=" << (settings.menu_sounds ? 1 : 0) << "\n";
    output << "background_music=" << (settings.background_music ? 1 : 0) << "\n";
    return true;
}

void devlog_initialize() {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    ensure_dir(DATA_DIR);
    ensure_dir(LOG_DIR);

    if (!g_log_file) {
        g_log_file = std::fopen(current_log_path(), "ab+");
    }
    if (g_log_file && file_size(g_log_file) >=
            static_cast<long>(MAX_LOG_SIZE)) {
        rotate_locked();
    }
}

void devlog_shutdown() {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (g_log_file) {
        std::fflush(g_log_file);
        std::fclose(g_log_file);
        g_log_file = nullptr;
    }
}

void devlog_write(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (!g_log_file) {
        ensure_dir(DATA_DIR);
        ensure_dir(LOG_DIR);
        g_log_file = std::fopen(current_log_path(), "ab+");
    }

    std::ostringstream stamped;
    stamped << "[" << sceKernelGetSystemTimeWide() / 1000ull << " ms] "
            << line;
    const std::string output = stamped.str();

    push_recent_locked(output);

    if (g_log_file) {
        std::fprintf(g_log_file, "%s\n", output.c_str());
        std::fflush(g_log_file);
        if (file_size(g_log_file) >= static_cast<long>(MAX_LOG_SIZE)) {
            rotate_locked();
        }
    }
}

void devlog_writef(const char* format, ...) {
    if (!format) return;
    char buffer[768]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    devlog_write(buffer);
}

std::vector<std::string> devlog_recent_lines(std::size_t max_lines) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (max_lines == 0 || g_recent.empty()) return {};
    const std::size_t count = std::min(max_lines, g_recent.size());
    return std::vector<std::string>(
        g_recent.end() - static_cast<std::ptrdiff_t>(count),
        g_recent.end());
}

std::string devlog_directory() {
    return LOG_DIR;
}
