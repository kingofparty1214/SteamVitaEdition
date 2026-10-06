#pragma once

#include <string>
#include <vector>
#include <vita2d.h>

#include "dev_tools.h"

enum class AppMenuPage {
    Main,
    Settings,
    Developer,
    Console,
};

enum class AppMenuAction {
    None,
    Search,
    CheckUpdates,
    OpenSettings,
    OpenDeveloper,
    About,
    ToggleAmbience,
    ToggleMenuSounds,
    ToggleBackgroundMusic,
    ToggleDeveloperMode,
    ToggleVerboseLogging,
    OpenConsole,
    RuntimeReport,
    ClearLogs,
    RefreshLibrary,
    ForceUpdateCheck,
    Back,
};

struct AppMenu {
    bool active = false;
    AppMenuPage page = AppMenuPage::Main;
    int selected = 0;

    void open();
    void close();
    void back();
    int item_count(const SteamVitaSettings& settings) const;
    void move(int direction, const SteamVitaSettings& settings);
    AppMenuAction activate(const SteamVitaSettings& settings);
};

void draw_app_menu(vita2d_pgf* font,
                   const AppMenu& menu,
                   const SteamVitaSettings& settings,
                   const std::vector<std::string>& console_lines,
                   float pulse);
