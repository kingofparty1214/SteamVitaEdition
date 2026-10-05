#pragma once

#include <string>

// Run a DOS title through the embedded DOSBox Pure libretro core.
// Returns true if the core loaded the content successfully.
bool run_dosbox_game(const std::string& content_path,
                     const std::string& save_directory,
                     std::string* error_message);
