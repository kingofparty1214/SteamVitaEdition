#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <vita2d.h>

#include <algorithm>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "dosbox_backend.h"
#include "library.h"

static const char* LIBRARY_PATH = "ux0:data/SteamVita/games";

static unsigned C(unsigned r, unsigned g, unsigned b) {
    return RGBA8(r, g, b, 255);
}

static void txt(vita2d_pgf* font,
                float x,
                float y,
                float size,
                unsigned color,
                const std::string& value) {
    vita2d_pgf_draw_text(font, x, y, color, size, value.c_str());
}

static std::string content_path_for(const GameEntry& game) {
    if (game.entry.empty()) return game.path;
    return game.path + "/" + game.entry;
}

int main() {
    mkdir("ux0:data/SteamVita", 0777);
    mkdir(LIBRARY_PATH, 0777);

    vita2d_init();
    vita2d_set_vblank_wait(1);
    vita2d_set_clear_color(C(18, 20, 27));

    vita2d_pgf* font = vita2d_load_default_pgf();
    if (!font) return 1;

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);

    auto games = scan_game_library(LIBRARY_PATH);
    int selected = 0;
    unsigned previous_buttons = 0;
    bool running = true;
    std::string status = games.empty()
        ? "Add a game folder, then press Triangle."
        : "SteamVita v0.2 - DOSBox backend enabled.";

    while (running) {
        SceCtrlData pad {};
        sceCtrlPeekBufferPositive(0, &pad, 1);
        const unsigned pressed = pad.buttons & ~previous_buttons;
        previous_buttons = pad.buttons;

        if ((pressed & SCE_CTRL_UP) && !games.empty()) {
            selected = (selected - 1 + static_cast<int>(games.size()))
                     % static_cast<int>(games.size());
        }

        if ((pressed & SCE_CTRL_DOWN) && !games.empty()) {
            selected = (selected + 1) % static_cast<int>(games.size());
        }

        if (pressed & SCE_CTRL_TRIANGLE) {
            games = scan_game_library(LIBRARY_PATH);
            selected = 0;
            status = games.empty() ? "No game folders found." : "Library rescanned.";
        }

        if ((pressed & SCE_CTRL_CROSS) && !games.empty()) {
            const GameEntry game = games[selected];

            if (game.backend == "dosbox") {
                const std::string content = content_path_for(game);
                const std::string save_dir = game.path + "/saves";
                std::string error;

                status = "Launching " + game.name + "...";
                if (run_dosbox_game(content, save_dir, &error)) {
                    status = "Returned from " + game.name + ".";
                } else {
                    status = error;
                }

                // The backend polls the pad too. Reset edge detection so a held
                // button does not immediately trigger an action in the launcher.
                previous_buttons = 0;
            } else if (game.backend == "x86") {
                status = "x86/Win32 backend is not implemented yet.";
            } else {
                status = "Set backend=dosbox in game.ini for DOS games.";
            }
        }

        if (pressed & SCE_CTRL_CIRCLE) running = false;

        vita2d_start_drawing();
        vita2d_clear_screen();

        vita2d_draw_rectangle(0, 0, 960, 72, C(29, 33, 43));
        vita2d_draw_rectangle(0, 70, 960, 2, C(115, 164, 255));
        txt(font, 28, 46, 1.25f, C(240, 242, 247), "SteamVita");
        txt(font, 285, 43, .70f, C(155, 164, 181), "experimental PC game launcher");

        vita2d_draw_rectangle(28, 98, 560, 368, C(29, 33, 43));
        if (games.empty()) {
            txt(font, 52, 148, .95f, C(240, 242, 247), "No games found");
            txt(font, 52, 182, .72f, C(155, 164, 181), "Create folders in:");
            txt(font, 52, 210, .72f, C(115, 164, 255), LIBRARY_PATH);
        } else {
            const int start = std::max(0, selected - 6);
            for (int row = 0; row < 7 && start + row < static_cast<int>(games.size()); ++row) {
                const int index = start + row;
                const int y = 107 + row * 50;
                if (index == selected) {
                    vita2d_draw_rectangle(38, y, 540, 46, C(65, 83, 125));
                }
                txt(font, 52, y + 30, .86f, C(240, 242, 247), games[index].name);
                txt(font, 418, y + 30, .64f, C(155, 164, 181), games[index].backend);
            }
        }

        vita2d_draw_rectangle(612, 98, 320, 368, C(29, 33, 43));
        txt(font, 632, 134, .85f, C(115, 164, 255), "Selected game");

        if (!games.empty()) {
            const GameEntry& game = games[selected];
            txt(font, 632, 174, .92f, C(240, 242, 247), game.name);
            txt(font, 632, 220, .70f, C(155, 164, 181), "Backend");
            txt(font, 632, 248, .78f, C(240, 242, 247), game.backend);
            txt(font, 632, 292, .70f, C(155, 164, 181), "Entry");
            txt(font, 632, 320, .70f, C(240, 242, 247),
                game.entry.empty() ? "(folder)" : game.entry);
            txt(font, 632, 372, .66f, C(155, 164, 181), "X launch");
            txt(font, 632, 398, .66f, C(155, 164, 181), "Triangle rescan");
            txt(font, 632, 424, .66f, C(155, 164, 181), "Circle exit");
            txt(font, 632, 450, .60f, C(115, 164, 255), "DOS: Start+Select returns");
        }

        vita2d_draw_rectangle(0, 490, 960, 54, C(29, 33, 43));
        txt(font, 26, 524, .68f, C(155, 164, 181), status);

        vita2d_end_drawing();
        vita2d_swap_buffers();
    }

    vita2d_free_pgf(font);
    vita2d_fini();
    sceKernelExitProcess(0);
    return 0;
}
