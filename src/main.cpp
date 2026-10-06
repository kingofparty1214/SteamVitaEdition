#include <psp2/ctrl.h>
#include <psp2/ime_dialog.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/sysmodule.h>
#include <vita2d.h>

#include <array>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "qrcodegen.h"
#include "compat_runtime.h"
#include "game_installer.h"
#include "steam_client.h"
#include "update_manager.h"
#include "runtime_pack.h"
#include "xmb_ui.h"
#include "ui_audio.h"
#include "dev_tools.h"
#include "app_menu.h"

#ifndef STEAMVITA_VERSION
#define STEAMVITA_VERSION "0.13.0"
#endif

namespace {

constexpr unsigned SCREEN_W = 960;
constexpr unsigned SCREEN_H = 544;

unsigned color(unsigned r, unsigned g, unsigned b) {
    return RGBA8(r, g, b, 255);
}

void text(vita2d_pgf* font,
          float x,
          float y,
          float scale,
          unsigned text_color,
          const std::string& value) {
    vita2d_pgf_draw_text(font, x, y, text_color, scale, value.c_str());
}

std::string shorten(const std::string& value, std::size_t max_chars) {
    if (value.size() <= max_chars) return value;
    if (max_chars <= 3) return value.substr(0, max_chars);
    return value.substr(0, max_chars - 3) + "...";
}

std::string playtime_text(std::uint32_t minutes) {
    if (minutes < 60) {
        return std::to_string(minutes) + " min";
    }

    std::ostringstream out;
    out << std::fixed << std::setprecision(1)
        << (static_cast<double>(minutes) / 60.0) << " hours";
    return out.str();
}


std::string lowercase_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return value;
}

std::vector<SteamGame> filter_games(const std::vector<SteamGame>& games,
                                    const std::string& query) {
    if (query.empty()) return games;

    const std::string needle = lowercase_ascii(query);
    std::vector<SteamGame> matches;
    matches.reserve(games.size());

    for (const SteamGame& game : games) {
        if (lowercase_ascii(game.name).find(needle) != std::string::npos ||
            std::to_string(game.app_id).find(needle) != std::string::npos) {
            matches.push_back(game);
        }
    }
    return matches;
}

enum class LibraryView {
    Library,
    Installed,
};

std::vector<SteamGame> installed_games_from_library(
        const std::vector<SteamGame>& games) {
    std::vector<SteamGame> installed;
    installed.reserve(games.size());
    for (const SteamGame& game : games) {
        if (is_compat_game_installed(game.app_id)) {
            installed.push_back(game);
        }
    }
    return installed;
}

int wrap_index(int value, int count) {
    if (count <= 0) return 0;
    value %= count;
    if (value < 0) value += count;
    return value;
}

void utf8_to_utf16(const std::string& input,
                   SceWChar16* output,
                   std::size_t capacity) {
    if (!output || capacity == 0) return;
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < input.size() && j + 1 < capacity) {
        const unsigned char c = static_cast<unsigned char>(input[i]);
        if (c < 0x80) {
            output[j++] = static_cast<SceWChar16>(c);
            ++i;
        } else if ((c & 0xe0u) == 0xc0u && i + 1 < input.size()) {
            output[j++] = static_cast<SceWChar16>(
                ((c & 0x1fu) << 6u) |
                (static_cast<unsigned char>(input[i + 1]) & 0x3fu));
            i += 2;
        } else if ((c & 0xf0u) == 0xe0u && i + 2 < input.size()) {
            output[j++] = static_cast<SceWChar16>(
                ((c & 0x0fu) << 12u) |
                ((static_cast<unsigned char>(input[i + 1]) & 0x3fu) << 6u) |
                (static_cast<unsigned char>(input[i + 2]) & 0x3fu));
            i += 3;
        } else {
            ++i;
        }
    }
    output[j] = 0;
}

std::string utf16_to_utf8(const SceWChar16* input) {
    std::string output;
    if (!input) return output;

    for (std::size_t i = 0; input[i] != 0; ++i) {
        const unsigned value = input[i];
        if (value < 0x80u) {
            output.push_back(static_cast<char>(value));
        } else if (value < 0x800u) {
            output.push_back(static_cast<char>(0xc0u | (value >> 6u)));
            output.push_back(static_cast<char>(0x80u | (value & 0x3fu)));
        } else {
            output.push_back(static_cast<char>(0xe0u | (value >> 12u)));
            output.push_back(static_cast<char>(0x80u | ((value >> 6u) & 0x3fu)));
            output.push_back(static_cast<char>(0x80u | (value & 0x3fu)));
        }
    }
    return output;
}

struct SearchIme {
    std::array<SceWChar16, 128> input{};
    std::array<SceWChar16, 64> title{};
    bool active = false;

    bool begin(const std::string& initial, std::string* error) {
        if (active) return false;
        input.fill(0);
        title.fill(0);
        utf8_to_utf16(initial, input.data(), input.size());
        utf8_to_utf16("Search Steam library", title.data(), title.size());

        SceImeDialogParam param;
        sceImeDialogParamInit(&param);
        param.inputMethod = 0;
        param.supportedLanguages = 0;
        param.languagesForced = SCE_FALSE;
        param.type = SCE_IME_TYPE_DEFAULT;
        param.option = SCE_IME_OPTION_NO_AUTO_CAPITALIZATION;
        param.dialogMode = SCE_IME_DIALOG_DIALOG_MODE_WITH_CANCEL;
        param.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_WITH_CLEAR;
        param.title = title.data();
        param.maxTextLength = static_cast<SceUInt32>(input.size() - 1);
        param.initialText = input.data();
        param.inputTextBuffer = input.data();
        param.enterLabel = SCE_IME_ENTER_LABEL_SEARCH;

        const int result = sceImeDialogInit(&param);
        if (result < 0) {
            if (error) {
                std::ostringstream message;
                message << "Could not open search keyboard (0x"
                        << std::hex << static_cast<unsigned>(result) << ").";
                *error = message.str();
            }
            return false;
        }

        active = true;
        if (error) error->clear();
        return true;
    }

    int update(std::string* result_text) {
        if (!active) return 0;

        const SceCommonDialogStatus status = sceImeDialogGetStatus();
        if (status == SCE_COMMON_DIALOG_STATUS_RUNNING) return 0;

        if (status == SCE_COMMON_DIALOG_STATUS_FINISHED) {
            SceImeDialogResult result{};
            sceImeDialogGetResult(&result);
            sceImeDialogTerm();
            active = false;
            if (result.button == SCE_IME_DIALOG_BUTTON_ENTER) {
                if (result_text) *result_text = utf16_to_utf8(input.data());
                return 1;
            }
            return -1;
        }

        if (status == SCE_COMMON_DIALOG_STATUS_NONE) {
            active = false;
            return -1;
        }

        return 0;
    }
};

struct GameMenu {
    bool active = false;
    int selected = 0;
};

struct QrImage {
    std::string source;
    std::array<std::uint8_t, qrcodegen_BUFFER_LEN_MAX> temp{};
    std::array<std::uint8_t, qrcodegen_BUFFER_LEN_MAX> qr{};
    bool valid = false;

    void set(const std::string& url) {
        if (url == source) return;
        source = url;
        valid = false;
        if (source.empty()) return;

        valid = qrcodegen_encodeText(
            source.c_str(),
            temp.data(),
            qr.data(),
            qrcodegen_Ecc_MEDIUM,
            qrcodegen_VERSION_MIN,
            qrcodegen_VERSION_MAX,
            qrcodegen_Mask_AUTO,
            true);
    }

    void draw(float center_x, float top_y, float max_size) const {
        if (!valid) return;

        const int qr_size = qrcodegen_getSize(qr.data());
        if (qr_size <= 0) return;

        constexpr int quiet = 4;
        const int total_modules = qr_size + quiet * 2;
        int module = static_cast<int>(max_size) / total_modules;
        if (module < 1) module = 1;

        const float actual = static_cast<float>(module * total_modules);
        const float left = center_x - actual * 0.5f;

        vita2d_draw_rectangle(left, top_y, actual, actual, color(255, 255, 255));

        for (int y = 0; y < qr_size; ++y) {
            for (int x = 0; x < qr_size; ++x) {
                if (!qrcodegen_getModule(qr.data(), x, y)) continue;

                const float px = left + static_cast<float>((x + quiet) * module);
                const float py = top_y + static_cast<float>((y + quiet) * module);
                vita2d_draw_rectangle(
                    px,
                    py,
                    static_cast<float>(module),
                    static_cast<float>(module),
                    color(0, 0, 0));
            }
        }
    }
};

void draw_header(vita2d_pgf* font, const std::string& account_name) {
    vita2d_draw_rectangle(0, 0, SCREEN_W, 76, color(29, 33, 43));
    vita2d_draw_rectangle(0, 74, SCREEN_W, 2, color(115, 164, 255));

    text(font, 28, 48, 1.25f, color(240, 242, 247), "SteamVita");
    text(font, 300, 44, .68f, color(155, 164, 181), "Steam on your PlayStation Vita");

    if (!account_name.empty()) {
        text(font, 700, 44, .68f, color(155, 164, 181),
             shorten(account_name, 24));
    }
}

void draw_status_bar(vita2d_pgf* font, const std::string& status) {
    vita2d_draw_rectangle(0, 492, SCREEN_W, 52, color(29, 33, 43));
    text(font, 24, 525, .66f, color(180, 188, 203), shorten(status, 92));

    const std::string version = std::string("v") + STEAMVITA_VERSION;
    text(font, 842, 525, .56f, color(115, 164, 255), version);
}
std::string format_bytes(std::uint64_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 3) {
        value /= 1024.0;
        ++unit;
    }

    std::ostringstream out;
    if (unit == 0) {
        out << static_cast<std::uint64_t>(value) << " " << units[unit];
    } else {
        out << std::fixed << std::setprecision(1)
            << value << " " << units[unit];
    }
    return out.str();
}

std::string format_eta(std::uint64_t seconds) {
    if (seconds == 0) return "--:--";
    const std::uint64_t hours = seconds / 3600;
    const std::uint64_t minutes = (seconds % 3600) / 60;
    const std::uint64_t secs = seconds % 60;

    std::ostringstream out;
    if (hours > 0) {
        out << hours << ":"
            << std::setw(2) << std::setfill('0') << minutes << ":"
            << std::setw(2) << std::setfill('0') << secs;
    } else {
        out << minutes << ":"
            << std::setw(2) << std::setfill('0') << secs;
    }
    return out.str();
}

void draw_install_progress(vita2d_pgf* font,
                           const InstallSnapshot& install) {
    if (!install.active()) return;

    vita2d_draw_rectangle(190, 432, 580, 52, color(18, 20, 27));
    vita2d_draw_rectangle(192, 434, 576, 48, color(37, 41, 52));

    const float bar_x = 212.0f;
    const float bar_y = 447.0f;
    const float bar_w = 360.0f;
    const float bar_h = 12.0f;
    vita2d_draw_rectangle(bar_x, bar_y, bar_w, bar_h, color(18, 20, 27));

    double fraction = 0.0;
    if (install.total_bytes > 0) {
        fraction = static_cast<double>(install.downloaded_bytes) /
                   static_cast<double>(install.total_bytes);
        if (fraction < 0.0) fraction = 0.0;
        if (fraction > 1.0) fraction = 1.0;
        vita2d_draw_rectangle(
            bar_x, bar_y,
            static_cast<float>(bar_w * fraction),
            bar_h, color(115, 164, 255));
    }

    std::ostringstream left;
    if (install.total_bytes > 0) {
        left << static_cast<int>(fraction * 100.0) << "%  "
             << format_bytes(install.downloaded_bytes) << " / "
             << format_bytes(install.total_bytes);
    } else {
        left << "Preparing download...";
    }

    std::ostringstream right;
    if (install.bytes_per_second > 0) {
        right << format_bytes(install.bytes_per_second)
              << "/s  ETA " << format_eta(install.eta_seconds);
    } else {
        right << "ETA --:--";
    }

    text(font, 212, 477, .52f, color(200, 205, 216), left.str());
    text(font, 585, 477, .52f, color(200, 205, 216), right.str());
}


void draw_signed_out(vita2d_pgf* font, SteamState state, const std::string& status) {
    vita2d_draw_rectangle(105, 115, 750, 300, color(29, 33, 43));
    text(font, 145, 166, 1.08f, color(240, 242, 247), "Your Steam library. On Vita.");
    text(font, 145, 208, .74f, color(170, 179, 195),
         "Browse your Steam library from your PlayStation Vita.");
    text(font, 145, 238, .74f, color(170, 179, 195),
         "Sign in securely with Steam to sync your owned game library.");

    if (state == SteamState::Error) {
        text(font, 145, 292, .72f, color(232, 125, 125), shorten(status, 82));
        text(font, 145, 352, .78f, color(115, 164, 255),
             "Press X to try Steam QR sign-in again");
    } else {
        text(font, 145, 320, .86f, color(115, 164, 255),
             "Press X to sign in with Steam");
        text(font, 145, 355, .68f, color(155, 164, 181),
             "You will approve the login from the Steam mobile app.");
    }

    text(font, 145, 395, .62f, color(155, 164, 181), "Circle: exit");
}

void draw_login(vita2d_pgf* font,
                SteamState state,
                const std::string& status,
                QrImage& qr,
                const std::string& qr_url) {
    qr.set(qr_url);

    if (qr.valid) {
        qr.draw(480.0f, 98.0f, 330.0f);
        text(font, 273, 458, .72f, color(240, 242, 247),
             "Scan this with the Steam mobile app");
        text(font, 318, 482, .60f, color(155, 164, 181),
             "Circle cancels the sign-in");
    } else {
        vita2d_draw_rectangle(150, 145, 660, 220, color(29, 33, 43));
        const char* heading =
            state == SteamState::LoadingLibrary ? "Loading your Steam library" :
            state == SteamState::Authorizing ? "Waiting for Steam approval" :
            "Connecting to Steam";
        text(font, 220, 218, 1.0f, color(240, 242, 247), heading);
        text(font, 220, 267, .72f, color(170, 179, 195), shorten(status, 70));
        text(font, 220, 326, .62f, color(155, 164, 181), "Circle: cancel");
    }
}

void draw_game_menu(vita2d_pgf* font,
                    const SteamGame& game,
                    bool installed,
                    bool session_ready,
                    int selected) {
    vita2d_draw_rectangle(250, 145, 460, 245, color(18, 20, 27));
    vita2d_draw_rectangle(252, 147, 456, 241, color(37, 41, 52));

    text(font, 280, 185, .88f, color(240, 242, 247),
         shorten(game.name, 38));
    text(font, 280, 215, .58f, color(155, 164, 181), "Game Actions");

    const char* actions[2] = {
        installed
            ? "Run / Inspect"
            : (session_ready ? "Install" : "Sign in to download"),
        installed ? "Uninstall" : "Cancel"
    };

    for (int i = 0; i < 2; ++i) {
        const int y = 245 + i * 52;
        if (selected == i) {
            vita2d_draw_rectangle(272, y - 24, 416, 40, color(65, 83, 125));
        }
        text(font, 292, y, .72f, color(240, 242, 247), actions[i]);
    }

    text(font, 280, 360, .56f, color(155, 164, 181),
         "X: select   Circle: close");
}

void draw_library(vita2d_pgf* font,
                  const std::vector<SteamGame>& games,
                  std::size_t total_games,
                  int selected,
                  bool offline,
                  bool update_available,
                  const std::string& search_query,
                  LibraryView view) {
    vita2d_draw_rectangle(24, 94, 590, 378, color(29, 33, 43));
    vita2d_draw_rectangle(632, 94, 304, 378, color(29, 33, 43));

    vita2d_draw_rectangle(36, 103, 122, 27,
        view == LibraryView::Library ? color(65, 83, 125) : color(37, 41, 52));
    vita2d_draw_rectangle(164, 103, 132, 27,
        view == LibraryView::Installed ? color(65, 83, 125) : color(37, 41, 52));
    text(font, 59, 122, .58f, color(240, 242, 247), "Library");
    text(font, 184, 122, .58f, color(240, 242, 247), "Installed");

    std::string library_title;
    if (search_query.empty()) {
        if (view == LibraryView::Installed) {
            library_title =
                "Installed - " + std::to_string(total_games) + " games";
        } else {
            library_title =
                "Library - " + std::to_string(total_games) + " games";
        }
    } else {
        library_title =
            "Search - " + std::to_string(games.size()) + " of " +
            std::to_string(total_games) + " games";
    }
    if (offline) library_title += "  [OFFLINE]";
    text(font, 320, 123, .68f, color(115, 164, 255), library_title);

    if (!search_query.empty()) {
        text(font, 650, 128, .60f, color(155, 164, 181), "Search");
        text(font, 650, 156, .72f, color(240, 242, 247),
             shorten(search_query, 28));
    }

    if (games.empty()) {
        text(font, 44, 182, .82f, color(240, 242, 247),
             search_query.empty()
                 ? (view == LibraryView::Installed
                        ? "No SteamVita games are installed yet."
                        : "Steam returned an empty library.")
                 : "No games match your search.");
        const std::string empty_footer =
            "Square: tab   SELECT: menu   Triangle: refresh   Circle: exit";
        text(font, 44, 463, .56f, color(155, 164, 181), empty_footer);
        return;
    }

    const int visible_rows = 8;
    int start = selected - visible_rows / 2;
    if (start < 0) start = 0;
    if (start + visible_rows > static_cast<int>(games.size())) {
        start = std::max(0, static_cast<int>(games.size()) - visible_rows);
    }

    for (int row = 0; row < visible_rows; ++row) {
        const int index = start + row;
        if (index >= static_cast<int>(games.size())) break;

        const int y = 142 + row * 39;
        if (index == selected) {
            vita2d_draw_rectangle(36, y, 566, 36, color(65, 83, 125));
        }

        const SteamGame& game = games[index];
        text(font, 48, y + 25, .72f, color(240, 242, 247),
             shorten(game.name, 47));

    }

    const SteamGame& game = games[selected];

    text(font, 650, 128, .78f, color(115, 164, 255), "Selected");
    text(font, 650, 170, .82f, color(240, 242, 247), shorten(game.name, 28));

    text(font, 650, 211, .60f, color(155, 164, 181), "Steam AppID");
    text(font, 650, 236, .70f, color(240, 242, 247),
         std::to_string(game.app_id));

    text(font, 650, 273, .60f, color(155, 164, 181), "Playtime");
    text(font, 650, 298, .70f, color(240, 242, 247),
         playtime_text(game.playtime_minutes));

    text(font, 650, 337, .60f, color(155, 164, 181), "Ownership");
    const char* ownership_text =
        game.ownership == SteamOwnership::FamilyShared
            ? "Family Shared"
            : "Owned on this account";
    text(font, 650, 362, .70f, color(132, 206, 144), ownership_text);

    text(font, 650, 402, .60f, color(155, 164, 181), "Runtime");
    text(font, 650, 428, .56f, color(155, 164, 181),
         "Generic Windows x86 compatibility layer");

    std::string footer = update_available
        ? "Square: tab   X: actions   L/R: page   SELECT: menu"
        : "Square: tab   X: actions   L/R: page   SELECT: menu";
    if (!search_query.empty()) footer += "   L: clear";
    text(font, 44, 463, .54f, color(155, 164, 181), footer);
}

} // namespace

int main() {
    mkdir("ux0:data/SteamVita", 0777);

    SteamVitaSettings settings;
    load_steamvita_settings(&settings);
    devlog_initialize();
    devlog_write("SteamVita starting.");
    ui_audio_initialize();
    XmbUiState xmb_ui;
    AppMenu app_menu;

    vita2d_init();
    vita2d_set_vblank_wait(1);
    vita2d_set_clear_color(color(18, 20, 27));

    vita2d_pgf* font = vita2d_load_default_pgf();
    if (!font) {
        vita2d_fini();
        return 1;
    }

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);

    SteamClient steam;
    std::string startup_error;
    steam.initialize(&startup_error);

    UpdateManager updater;
    updater.initialize(steam.network_ready());
    bool updater_waiting_for_network =
        updater.state() == UpdateState::Disabled;

    RuntimePackManager runtime_pack;
    runtime_pack.initialize(steam.network_ready());
    bool runtime_waiting_for_network =
        runtime_pack.state() == RuntimePackState::Disabled ||
        runtime_pack.state() == RuntimePackState::Missing;

    GameInstaller installer;

    std::vector<SteamGame> all_games;
    std::vector<SteamGame> installed_games;
    std::vector<SteamGame> games;
    LibraryView library_view = LibraryView::Library;
    std::string search_query;
    SearchIme search_ime;
    const bool ime_module_loaded =
        sceSysmoduleLoadModule(SCE_SYSMODULE_IME) >= 0;
    int selected = 0;
    unsigned previous_buttons = 0;
    unsigned nav_repeat_frames = 0;
    int nav_repeat_direction = 0;
    bool running = true;
    SteamState previous_state = steam.state();
    if (previous_state == SteamState::Ready) {
        all_games = steam.games_snapshot();
        installed_games = installed_games_from_library(all_games);
        games = filter_games(all_games, search_query);
    }
    std::string local_status = startup_error;
    QrImage qr;
    GameMenu game_menu;

    while (running) {
        xmb_ui_update(&xmb_ui, settings.ui_ambience);
        steam.update();

        if (updater_waiting_for_network && steam.network_ready()) {
            updater.initialize(true);
            updater_waiting_for_network = false;
        }

        if (runtime_waiting_for_network && steam.network_ready()) {
            runtime_pack.force_check(true);
            runtime_waiting_for_network = false;
        }

        updater.update();
        installer.update();

        const SteamState current_state = steam.state();
        if (current_state == SteamState::Ready &&
            previous_state != SteamState::Ready) {
            all_games = steam.games_snapshot();
            installed_games = installed_games_from_library(all_games);
            const std::vector<SteamGame>& source =
                library_view == LibraryView::Installed
                    ? installed_games
                    : all_games;
            games = filter_games(source, search_query);
            selected = 0;
            local_status.clear();
        }
        previous_state = current_state;

        if (search_ime.active) {
            vita2d_start_drawing();
            vita2d_clear_screen();
            xmb_draw_background(xmb_ui, settings.ui_ambience);
            draw_header(font, steam.account_name());
            const std::vector<SteamGame>& search_source =
                library_view == LibraryView::Installed
                    ? installed_games
                    : all_games;
            draw_library(font, games, search_source.size(), selected,
                         steam.offline_mode(), updater.update_available(),
                         search_query, library_view);
            draw_status_bar(font, "Type a game name or AppID, then press Search.");
            vita2d_end_drawing();
            vita2d_common_dialog_update();

            std::string entered_search;
            const int ime_result = search_ime.update(&entered_search);
            if (ime_result == 1) {
                search_query = entered_search;
                const std::vector<SteamGame>& search_source =
                    library_view == LibraryView::Installed
                        ? installed_games
                        : all_games;
                games = filter_games(search_source, search_query);
                selected = 0;
                if (search_query.empty()) {
                    local_status = "Search cleared.";
                } else {
                    std::ostringstream message;
                    message << "Found " << games.size()
                            << " game" << (games.size() == 1 ? "" : "s")
                            << " matching \"" << shorten(search_query, 32) << "\".";
                    local_status = message.str();
                }
            } else if (ime_result < 0) {
                local_status = "Search cancelled.";
            }
            if (ime_result != 0) {
                previous_buttons = 0;
            }

            vita2d_swap_buffers();
            continue;
        }

        if (game_menu.active && current_state == SteamState::Ready) {
            SceCtrlData pad{};
            sceCtrlPeekBufferPositive(0, &pad, 1);
            const unsigned pressed = pad.buttons & ~previous_buttons;
            previous_buttons = pad.buttons;

            if ((pressed & SCE_CTRL_UP) || (pressed & SCE_CTRL_DOWN)) {
                game_menu.selected = 1 - game_menu.selected;
                ui_audio_play(UiSound::Navigate, settings.menu_sounds);
            }

            if (pressed & SCE_CTRL_CIRCLE) {
                ui_audio_play(UiSound::Back, settings.menu_sounds);
                game_menu.active = false;
                game_menu.selected = 0;
                previous_buttons = pad.buttons;
            } else if ((pressed & SCE_CTRL_CROSS) && !games.empty()) {
                const SteamGame selected_game = games[selected];
                const bool installed =
                    is_compat_game_installed(selected_game.app_id);

                if (game_menu.selected == 0) {
                    if (!installed) {
                        if (!steam.has_session()) {
                            game_menu.active = false;
                            game_menu.selected = 0;
                            local_status.clear();
                            previous_buttons = pad.buttons;
                            steam.start_qr_login();
                        } else {
                            const InstallSnapshot install = installer.snapshot();
                            if (install.active()) {
                                local_status =
                                    "Another game install is already running.";
                            } else if (installer.start_install(
                                           selected_game.app_id,
                                           selected_game.name,
                                           steam.session_credentials_snapshot())) {
                                local_status =
                                    "Starting install for " + selected_game.name + "...";
                            } else {
                                local_status = installer.snapshot().status;
                            }
                        }
                    } else {
                        const CompatReport report =
                            inspect_compat_game(
                                selected_game.app_id,
                                selected_game.name);
                        if (report.state == CompatState::ReadyForTranslator) {
                            local_status =
                                "PE32 x86 found: " + report.executable_path +
                                ". Translator handoff is the next step.";
                        } else {
                            local_status =
                                compat_state_label(report.state) +
                                ": " + report.detail;
                        }
                    }
                    game_menu.active = false;
                    game_menu.selected = 0;
                    previous_buttons = pad.buttons;
                } else if (installed) {
                    std::string uninstall_error;
                    if (uninstall_compat_game(
                            selected_game.app_id,
                            &uninstall_error)) {
                        installed_games =
                            installed_games_from_library(all_games);
                        const std::vector<SteamGame>& source =
                            library_view == LibraryView::Installed
                                ? installed_games
                                : all_games;
                        games = filter_games(source, search_query);
                        if (selected >= static_cast<int>(games.size())) {
                            selected = games.empty()
                                ? 0
                                : static_cast<int>(games.size()) - 1;
                        }
                        local_status =
                            "Uninstalled " + selected_game.name + ".";
                    } else {
                        local_status = uninstall_error;
                    }
                    game_menu.active = false;
                    game_menu.selected = 0;
                    previous_buttons = 0;
                } else {
                    game_menu.active = false;
                    game_menu.selected = 0;
                    previous_buttons = 0;
                }
            }

            vita2d_start_drawing();
            vita2d_clear_screen();
            xmb_draw_background(xmb_ui, settings.ui_ambience);
            draw_header(font, steam.account_name());

            const std::vector<SteamGame>& draw_source =
                library_view == LibraryView::Installed
                    ? installed_games
                    : all_games;
            draw_library(font, games, draw_source.size(), selected,
                         steam.offline_mode(),
                         updater.update_available(),
                         search_query, library_view);
            if (!games.empty()) {
                const SteamGame& selected_game = games[selected];
                draw_game_menu(
                    font,
                    selected_game,
                    is_compat_game_installed(selected_game.app_id),
                    steam.has_session(),
                    game_menu.selected);
            }
            const InstallSnapshot draw_install = installer.snapshot();
            draw_install_progress(font, draw_install);
            draw_status_bar(font, local_status.empty()
                ? "Game actions"
                : local_status);

            vita2d_end_drawing();
            vita2d_swap_buffers();
            continue;
        }

        SceCtrlData pad{};
        sceCtrlPeekBufferPositive(0, &pad, 1);
        const unsigned pressed = pad.buttons & ~previous_buttons;
        previous_buttons = pad.buttons;

        const bool console_combo =
            settings.developer_mode &&
            (pressed & SCE_CTRL_START) &&
            (pad.buttons & SCE_CTRL_LTRIGGER) &&
            (pad.buttons & SCE_CTRL_RTRIGGER);
        if (console_combo) {
            app_menu.active = true;
            app_menu.page = AppMenuPage::Console;
            app_menu.selected = 0;
            ui_audio_play(UiSound::Select, settings.menu_sounds);
            devlog_write("Developer console opened with L+R+START.");
        }

        if (app_menu.active && current_state == SteamState::Ready) {
            if ((pressed & SCE_CTRL_UP) || (pressed & SCE_CTRL_DOWN)) {
                app_menu.move((pressed & SCE_CTRL_UP) ? -1 : 1, settings);
                ui_audio_play(UiSound::Navigate, settings.menu_sounds);
            }

            if (pressed & SCE_CTRL_CIRCLE) {
                app_menu.back();
                ui_audio_play(UiSound::Back, settings.menu_sounds);
            } else if (pressed & SCE_CTRL_CROSS) {
                const AppMenuAction action = app_menu.activate(settings);
                ui_audio_play(UiSound::Select, settings.menu_sounds);

                if (action == AppMenuAction::Search) {
                    app_menu.close();
                    if (!ime_module_loaded) {
                        local_status = "The Vita keyboard module is unavailable.";
                    } else {
                        std::string error;
                        if (search_ime.begin(search_query, &error)) {
                            previous_buttons = 0;
                            local_status = "Search keyboard opened.";
                        } else if (!error.empty()) {
                            local_status = error;
                        }
                    }
                } else if (action == AppMenuAction::CheckUpdates ||
                           action == AppMenuAction::ForceUpdateCheck) {
                    updater.force_check(steam.network_ready());
                    local_status = updater.status();
                    devlog_write("Manual update check requested.");
                } else if (action == AppMenuAction::ToggleAmbience) {
                    settings.ui_ambience = !settings.ui_ambience;
                    save_steamvita_settings(settings);
                } else if (action == AppMenuAction::ToggleMenuSounds) {
                    settings.menu_sounds = !settings.menu_sounds;
                    save_steamvita_settings(settings);
                } else if (action == AppMenuAction::ToggleBackgroundMusic) {
                    settings.background_music = !settings.background_music;
                    save_steamvita_settings(settings);
                    local_status = settings.background_music
                        ? "Background music enabled. Add custom music under ux0:data/SteamVita/music/."
                        : "Background music disabled.";
                } else if (action == AppMenuAction::ToggleDeveloperMode) {
                    settings.developer_mode = !settings.developer_mode;
                    save_steamvita_settings(settings);
                    devlog_write(settings.developer_mode
                        ? "Developer mode enabled."
                        : "Developer mode disabled.");
                } else if (action == AppMenuAction::ToggleVerboseLogging) {
                    settings.verbose_logging = !settings.verbose_logging;
                    save_steamvita_settings(settings);
                    devlog_write(settings.verbose_logging
                        ? "Verbose logging enabled."
                        : "Verbose logging disabled.");
                } else if (action == AppMenuAction::RuntimeReport) {
                    if (!games.empty() &&
                        is_compat_game_installed(games[selected].app_id)) {
                        const CompatReport report =
                            inspect_compat_game(games[selected].app_id,
                                                games[selected].name);
                        local_status =
                            compat_state_label(report.state) + ": " + report.detail;
                        devlog_write("Runtime report: " + local_status);
                    } else {
                        local_status = "Select an installed game first.";
                    }
                } else if (action == AppMenuAction::RuntimePack) {
                    if (runtime_pack.update_available()) {
                        runtime_pack.start_install();
                    } else {
                        runtime_pack.force_check(steam.network_ready());
                    }
                    local_status = runtime_pack.status();
                    devlog_write("Runtime pack action: " + local_status);
                } else if (action == AppMenuAction::ClearLogs) {
                    local_status = devlog_clear()
                        ? "Diagnostic logs cleared."
                        : "Could not clear diagnostic logs.";
                } else if (action == AppMenuAction::RefreshLibrary) {
                    steam.refresh_library();
                    local_status = "Refreshing your Steam library...";
                    devlog_write("Manual library refresh requested.");
                } else if (action == AppMenuAction::About) {
                    local_status =
                        std::string("SteamVita ") + STEAMVITA_VERSION +
                        " - Steam library, installer, and Vita Proton runtime.";
                }
            }

            vita2d_start_drawing();
            vita2d_clear_screen();
            xmb_draw_background(xmb_ui, settings.ui_ambience);
            draw_header(font, steam.account_name());

            const std::vector<SteamGame>& draw_source =
                library_view == LibraryView::Installed
                    ? installed_games
                    : all_games;
            draw_library(font, games, draw_source.size(), selected,
                         steam.offline_mode(),
                         updater.update_available(),
                         search_query, library_view);

            draw_app_menu(font, app_menu, settings,
                          devlog_recent_lines(18), xmb_ui.phase);

            draw_status_bar(font, local_status.empty()
                ? "SELECT menu"
                : local_status);
            vita2d_end_drawing();
            vita2d_swap_buffers();
            continue;
        }

        if ((pressed & SCE_CTRL_START) && updater.update_available()) {
            updater.start_update();
            local_status = updater.status();
        }

        if (updater.state() == UpdateState::ReadyToInstall) {
            std::string update_error;
            local_status = updater.status();
            if (updater.launch_installer(&update_error)) {
                sceKernelExitProcess(0);
            } else if (!update_error.empty()) {
                local_status = update_error;
            }
        }

        if (current_state == SteamState::Ready) {
            if (pressed & SCE_CTRL_SELECT) {
                app_menu.open();
                ui_audio_play(UiSound::Select, settings.menu_sounds);
                previous_buttons = pad.buttons;
            }

            if (pressed & SCE_CTRL_LTRIGGER) {
                if (!search_query.empty()) {
                    search_query.clear();
                    const std::vector<SteamGame>& source =
                        library_view == LibraryView::Installed
                            ? installed_games
                            : all_games;
                    games = source;
                    selected = 0;
                    local_status = "Search cleared.";
                } else if (!games.empty()) {
                    selected = wrap_index(
                        selected - 8, static_cast<int>(games.size()));
                    local_status.clear();
                }
            }

            if (pressed & SCE_CTRL_RTRIGGER) {
                const InstallSnapshot active_install = installer.snapshot();
                if (active_install.active()) {
                    installer.cancel();
                    local_status = "Cancelling game install...";
                } else if (!games.empty()) {
                    selected = wrap_index(
                        selected + 8, static_cast<int>(games.size()));
                    local_status.clear();
                }
            }

            int held_direction = 0;
            if (pad.buttons & SCE_CTRL_UP) held_direction = -1;
            else if (pad.buttons & SCE_CTRL_DOWN) held_direction = 1;
            else if (pad.ly < 80) held_direction = -1;
            else if (pad.ly > 175) held_direction = 1;

            bool move_now = false;
            if (held_direction == 0) {
                nav_repeat_direction = 0;
                nav_repeat_frames = 0;
            } else if (held_direction != nav_repeat_direction) {
                nav_repeat_direction = held_direction;
                nav_repeat_frames = 0;
                move_now = true;
            } else {
                ++nav_repeat_frames;
                if (nav_repeat_frames >= 18 &&
                    ((nav_repeat_frames - 18) % 3u) == 0u) {
                    move_now = true;
                }
            }

            if (move_now && !games.empty()) {
                selected = wrap_index(
                    selected + held_direction,
                    static_cast<int>(games.size()));
                ui_audio_play(UiSound::Navigate, settings.menu_sounds);
            }

            if (pressed & SCE_CTRL_TRIANGLE) {
                steam.refresh_library();
                local_status = "Refreshing your Steam library...";
            }

            if (pressed & SCE_CTRL_SQUARE) {
                library_view =
                    library_view == LibraryView::Library
                        ? LibraryView::Installed
                        : LibraryView::Library;
                if (library_view == LibraryView::Installed) {
                    installed_games = installed_games_from_library(all_games);
                }
                const std::vector<SteamGame>& source =
                    library_view == LibraryView::Installed
                        ? installed_games
                        : all_games;
                games = filter_games(source, search_query);
                selected = 0;
                local_status.clear();
            }

            if ((pressed & SCE_CTRL_CROSS) && !games.empty()) {
                ui_audio_play(UiSound::Select, settings.menu_sounds);
                game_menu.active = true;
                game_menu.selected = 0;
                local_status.clear();
                previous_buttons = pad.buttons;
            }

            if (pressed & SCE_CTRL_CIRCLE) running = false;
        } else if (current_state == SteamState::SignedOut ||
                   current_state == SteamState::Error) {
            if (pressed & SCE_CTRL_CROSS) {
                local_status.clear();
                steam.start_qr_login();
            }
            if (pressed & SCE_CTRL_CIRCLE) running = false;
        } else {
            if (pressed & SCE_CTRL_CIRCLE) {
                steam.sign_out();
                local_status.clear();
            }
        }

        vita2d_start_drawing();
        vita2d_clear_screen();
        xmb_draw_background(xmb_ui, settings.ui_ambience);

        draw_header(font, steam.account_name());

        const SteamState draw_state = steam.state();
        const std::string steam_status = steam.status();

        if (draw_state == SteamState::Ready) {
            const std::vector<SteamGame>& draw_source =
                library_view == LibraryView::Installed
                    ? installed_games
                    : all_games;
            draw_library(font, games, draw_source.size(), selected,
                         steam.offline_mode(),
                         updater.update_available(),
                         search_query, library_view);
        } else if (draw_state == SteamState::SignedOut ||
                   draw_state == SteamState::Error) {
            draw_signed_out(font, draw_state, steam_status);
        } else {
            draw_login(font, draw_state, steam_status, qr, steam.qr_url());
        }

        std::string display_status =
            local_status.empty() ? steam_status : local_status;

        const UpdateState update_state = updater.state();
        if (update_state == UpdateState::Available ||
            update_state == UpdateState::Downloading ||
            update_state == UpdateState::ReadyToInstall) {
            display_status = updater.status();
        }

        const InstallSnapshot install = installer.snapshot();
        if (install.active() || install.state == InstallState::Error ||
            install.state == InstallState::Installed) {
            display_status = install.status;
        }

        const RuntimePackState runtime_state = runtime_pack.state();
        if (runtime_state == RuntimePackState::Downloading ||
            runtime_state == RuntimePackState::Installing ||
            runtime_state == RuntimePackState::Error) {
            display_status = runtime_pack.status();
        }

        const InstallSnapshot draw_install = installer.snapshot();
        draw_install_progress(font, draw_install);
        draw_status_bar(font, display_status);

        vita2d_end_drawing();
        vita2d_swap_buffers();
    }

    steam.sign_out();
    devlog_write("SteamVita shutting down.");
    ui_audio_shutdown();
    devlog_shutdown();

    if (search_ime.active) {
        sceImeDialogAbort();
        sceImeDialogTerm();
    }
    if (ime_module_loaded) {
        sceSysmoduleUnloadModule(SCE_SYSMODULE_IME);
    }

    vita2d_free_pgf(font);
    vita2d_fini();
    sceKernelExitProcess(0);
    return 0;
}
