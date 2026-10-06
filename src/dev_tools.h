#pragma once

#include <cstddef>
#include <string>
#include <vector>

struct SteamVitaSettings {
    bool developer_mode = false;
    bool verbose_logging = false;
};

bool load_steamvita_settings(SteamVitaSettings* settings);
bool save_steamvita_settings(const SteamVitaSettings& settings);

void devlog_initialize();
void devlog_shutdown();
void devlog_write(const std::string& line);
void devlog_writef(const char* format, ...);

std::vector<std::string> devlog_recent_lines(std::size_t max_lines);
std::string devlog_directory();
