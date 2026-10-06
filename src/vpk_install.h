#pragma once

#include <string>

namespace steamvita {

bool path_exists(const std::string& path);
bool ensure_directory(const std::string& path);
bool remove_tree(const std::string& path);
bool extract_vpk(const std::string& vpk_path,
                 const std::string& destination,
                 std::string* error_message);
bool verify_sha256_file(const std::string& path,
                        const std::string& expected_hex,
                        std::string* error_message);
bool prepare_package_head(const std::string& directory,
                          const std::string& title_id,
                          std::string* error_message);
bool promote_directory(const std::string& directory,
                       std::string* error_message);
void log_line(const std::string& message);

} // namespace steamvita
