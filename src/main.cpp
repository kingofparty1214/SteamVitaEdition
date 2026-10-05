#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <vita2d.h>

#include <array>
#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "qrcodegen.h"
#include "steam_client.h"

namespace {

constexpr unsigned SCREEN_W = 960;
constexpr unsigned SCREEN_H = 544;
constexpr std::uint32_t LEGO_BATMAN_APP_ID = 21000;

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
    text(font, 300, 44, .68f, color(155, 164, 181), "your real Steam library");

    if (!account_name.empty()) {
        text(font, 700, 44, .68f, color(155, 164, 181),
             shorten(account_name, 24));
    }
}

void draw_status_bar(vita2d_pgf* font, const std::string& status) {
    vita2d_draw_rectangle(0, 492, SCREEN_W, 52, color(29, 33, 43));
    text(font, 24, 525, .66f, color(180, 188, 203), shorten(status, 115));
}

void draw_signed_out(vita2d_pgf* font, SteamState state, const std::string& status) {
    vita2d_draw_rectangle(105, 115, 750, 300, color(29, 33, 43));
    text(font, 145, 166, 1.08f, color(240, 242, 247), "Your real Steam library");
    text(font, 145, 208, .74f, color(170, 179, 195),
         "SteamVita does not add demo or fake game entries.");
    text(font, 145, 238, .74f, color(170, 179, 195),
         "Sign in and the list comes directly from your Steam account.");

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

void draw_library(vita2d_pgf* font,
                  const std::vector<SteamGame>& games,
                  int selected) {
    vita2d_draw_rectangle(24, 94, 590, 378, color(29, 33, 43));
    vita2d_draw_rectangle(632, 94, 304, 378, color(29, 33, 43));

    text(font, 44, 128, .78f, color(115, 164, 255),
         "Library - " + std::to_string(games.size()) + " owned games");

    if (games.empty()) {
        text(font, 44, 182, .82f, color(240, 242, 247),
             "Steam returned an empty library.");
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

        if (game.app_id == LEGO_BATMAN_APP_ID) {
            text(font, 535, y + 24, .55f, color(235, 201, 112), "TARGET");
        }
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
    text(font, 650, 362, .70f, color(132, 206, 144), "Owned on this account");

    if (game.app_id == LEGO_BATMAN_APP_ID) {
        text(font, 650, 402, .62f, color(235, 201, 112),
             "LEGO Batman PC target");
        text(font, 650, 426, .56f, color(155, 164, 181),
             "Win32 runtime is next.");
    } else {
        text(font, 650, 414, .56f, color(155, 164, 181),
             "Runtime support not added yet.");
    }

    text(font, 44, 463, .58f, color(155, 164, 181),
         "Up/Down: browse   Triangle: refresh   Square: sign out   Circle: exit");
}

} // namespace

int main() {
    mkdir("ux0:data/SteamVita", 0777);

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

    std::vector<SteamGame> games;
    int selected = 0;
    unsigned previous_buttons = 0;
    bool running = true;
    SteamState previous_state = steam.state();
    std::string local_status = startup_error;
    QrImage qr;

    while (running) {
        steam.update();

        const SteamState current_state = steam.state();
        if (current_state == SteamState::Ready &&
            previous_state != SteamState::Ready) {
            games = steam.games_snapshot();
            selected = 0;
            local_status.clear();
        }
        previous_state = current_state;

        SceCtrlData pad{};
        sceCtrlPeekBufferPositive(0, &pad, 1);
        const unsigned pressed = pad.buttons & ~previous_buttons;
        previous_buttons = pad.buttons;

        if (current_state == SteamState::Ready) {
            if ((pressed & SCE_CTRL_UP) && !games.empty()) {
                selected =
                    (selected - 1 + static_cast<int>(games.size())) %
                    static_cast<int>(games.size());
            }

            if ((pressed & SCE_CTRL_DOWN) && !games.empty()) {
                selected =
                    (selected + 1) % static_cast<int>(games.size());
            }

            if (pressed & SCE_CTRL_TRIANGLE) {
                steam.refresh_library();
                local_status = "Refreshing your Steam library...";
            }

            if (pressed & SCE_CTRL_SQUARE) {
                steam.sign_out();
                games.clear();
                selected = 0;
                local_status.clear();
            }

            if ((pressed & SCE_CTRL_CROSS) && !games.empty()) {
                if (games[selected].app_id == LEGO_BATMAN_APP_ID) {
                    local_status =
                        "LEGO Batman is owned. The Win32 compatibility runtime is not ready yet.";
                } else {
                    local_status =
                        "This is a real owned game, but SteamVita has no runtime for it yet.";
                }
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

        draw_header(font, steam.account_name());

        const SteamState draw_state = steam.state();
        const std::string steam_status = steam.status();

        if (draw_state == SteamState::Ready) {
            draw_library(font, games, selected);
        } else if (draw_state == SteamState::SignedOut ||
                   draw_state == SteamState::Error) {
            draw_signed_out(font, draw_state, steam_status);
        } else {
            draw_login(font, draw_state, steam_status, qr, steam.qr_url());
        }

        draw_status_bar(font, local_status.empty() ? steam_status : local_status);

        vita2d_end_drawing();
        vita2d_swap_buffers();
    }

    steam.sign_out();

    vita2d_free_pgf(font);
    vita2d_fini();
    sceKernelExitProcess(0);
    return 0;
}
