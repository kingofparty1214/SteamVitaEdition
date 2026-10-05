#pragma once
#include <string>
#include <vector>
struct GameEntry { std::string name,path,backend,entry; };
std::vector<GameEntry> scan_game_library(const std::string& root);
