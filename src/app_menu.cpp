#include "app_menu.h"

#include "xmb_ui.h"

#include <algorithm>
#include <string>

namespace {

unsigned color(unsigned r, unsigned g, unsigned b) {
    return RGBA8(r, g, b, 255);
}

void draw_text(vita2d_pgf* font,
               float x,
               float y,
               float scale,
               unsigned c,
               const std::string& value) {
    vita2d_pgf_draw_text(font, x, y, c, scale, value.c_str());
}

const char* on_off(bool value) {
    return value ? "On" : "Off";
}

} // namespace

void AppMenu::open() {
    active = true;
    page = AppMenuPage::Main;
    selected = 0;
}

void AppMenu::close() {
    active = false;
    page = AppMenuPage::Main;
    selected = 0;
}

void AppMenu::back() {
    if (page == AppMenuPage::Main) {
        close();
        return;
    }
    page = AppMenuPage::Main;
    selected = 0;
}

int AppMenu::item_count(const SteamVitaSettings& settings) const {
    switch (page) {
        case AppMenuPage::Main:
            return settings.developer_mode ? 5 : 4;
        case AppMenuPage::Settings:
            return 5;
        case AppMenuPage::Developer:
            return 6;
        case AppMenuPage::Console:
            return 0;
    }
    return 0;
}

void AppMenu::move(int direction, const SteamVitaSettings& settings) {
    const int count = item_count(settings);
    if (count <= 0) return;
    selected = (selected + direction) % count;
    if (selected < 0) selected += count;
}

AppMenuAction AppMenu::activate(const SteamVitaSettings& settings) {
    if (page == AppMenuPage::Console) return AppMenuAction::Back;

    if (page == AppMenuPage::Main) {
        if (selected == 0) return AppMenuAction::Search;
        if (selected == 1) return AppMenuAction::CheckUpdates;
        if (selected == 2) {
            page = AppMenuPage::Settings;
            selected = 0;
            return AppMenuAction::OpenSettings;
        }
        if (settings.developer_mode && selected == 3) {
            page = AppMenuPage::Developer;
            selected = 0;
            return AppMenuAction::OpenDeveloper;
        }
        return AppMenuAction::About;
    }

    if (page == AppMenuPage::Settings) {
        switch (selected) {
            case 0: return AppMenuAction::ToggleAmbience;
            case 1: return AppMenuAction::ToggleMenuSounds;
            case 2: return AppMenuAction::ToggleBackgroundMusic;
            case 3: return AppMenuAction::ToggleDeveloperMode;
            case 4: return AppMenuAction::ToggleVerboseLogging;
        }
    }

    if (page == AppMenuPage::Developer) {
        switch (selected) {
            case 0:
                page = AppMenuPage::Console;
                selected = 0;
                return AppMenuAction::OpenConsole;
            case 1: return AppMenuAction::RuntimeReport;
            case 2: return AppMenuAction::RuntimePack;
            case 3: return AppMenuAction::ClearLogs;
            case 4: return AppMenuAction::RefreshLibrary;
            case 5: return AppMenuAction::ForceUpdateCheck;
        }
    }

    return AppMenuAction::None;
}

void draw_app_menu(vita2d_pgf* font,
                   const AppMenu& menu,
                   const SteamVitaSettings& settings,
                   const std::vector<std::string>& console_lines,
                   float pulse) {
    if (!menu.active) return;

    if (menu.page == AppMenuPage::Console) {
        xmb_draw_glass_panel(42, 76, 876, 410, 210);
        draw_text(font, 68, 112, .86f, color(242, 247, 255), "Developer Console");
        draw_text(font, 690, 112, .52f, color(160, 190, 220), "Circle: back");

        const int max_lines = 15;
        const int count = std::min(max_lines, static_cast<int>(console_lines.size()));
        const int start = static_cast<int>(console_lines.size()) - count;
        for (int i = 0; i < count; ++i) {
            std::string line = console_lines[start + i];
            if (line.size() > 116) line = line.substr(0, 113) + "...";
            draw_text(font, 68, 145 + i * 21, .46f,
                      color(190, 211, 232), line);
        }
        if (count == 0) {
            draw_text(font, 68, 165, .58f, color(160, 190, 220),
                      "No diagnostic lines recorded yet.");
        }
        return;
    }

    xmb_draw_glass_panel(535, 78, 385, 395, 205);

    std::string title = "SteamVita";
    if (menu.page == AppMenuPage::Settings) title = "Settings";
    else if (menu.page == AppMenuPage::Developer) title = "Developer Tools";

    draw_text(font, 566, 119, .90f, color(244, 248, 255), title);
    xmb_draw_separator(566, 134, 320);

    std::vector<std::string> labels;
    if (menu.page == AppMenuPage::Main) {
        labels = {"Search Games", "Check for Updates", "Settings"};
        if (settings.developer_mode) labels.push_back("Developer Tools");
        labels.push_back("About");
    } else if (menu.page == AppMenuPage::Settings) {
        labels = {
            std::string("UI Ambience          ") + on_off(settings.ui_ambience),
            std::string("Menu Sounds          ") + on_off(settings.menu_sounds),
            std::string("Background Music     ") + on_off(settings.background_music),
            std::string("Developer Mode       ") + on_off(settings.developer_mode),
            std::string("Verbose Logging      ") + on_off(settings.verbose_logging),
        };
    } else {
        labels = {
            "Console",
            "Selected Game Runtime Report",
            "Install / Update Runtime Pack",
            "Clear Diagnostic Logs",
            "Force Library Refresh",
            "Force Update Check",
        };
    }

    for (int i = 0; i < static_cast<int>(labels.size()); ++i) {
        const float y = 166.0f + i * 52.0f;
        if (i == menu.selected) {
            xmb_draw_selection(557, y - 26, 338, 40, pulse);
        }
        draw_text(font, 575, y, .67f, color(239, 244, 252), labels[i]);
    }

    draw_text(font, 566, 449, .50f, color(155, 183, 211),
              "X: select    Circle: back");
}
