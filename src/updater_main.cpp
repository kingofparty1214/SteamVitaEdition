#include "vpk_install.h"

#include <psp2/appmgr.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <vita2d.h>

#include <cstdio>
#include <fstream>
#include <string>

namespace {

#ifndef STEAMVITA_UPDATER_VERSION
#define STEAMVITA_UPDATER_VERSION "0.13.0"
#endif

constexpr const char* UPDATER_VERSION_FILE =
    "ux0:data/SteamVita/updater.version";

constexpr const char* UPDATE_VPK =
    "ux0:data/SteamVita/update/SteamVita.vpk";
constexpr const char* EXPECTED_SHA =
    "ux0:data/SteamVita/update/expected.sha256";
constexpr const char* STAGE_DIR =
    "ux0:data/SteamVita/update_pkg";

unsigned color(unsigned r, unsigned g, unsigned b) {
    return RGBA8(r, g, b, 255);
}

void draw(vita2d_pgf* font,
          const std::string& heading,
          const std::string& line) {
    vita2d_start_drawing();
    vita2d_clear_screen();
    vita2d_draw_rectangle(
        0, 0, 960, 544, color(18, 20, 27));
    vita2d_draw_rectangle(
        0, 0, 960, 76, color(29, 33, 43));
    vita2d_pgf_draw_text(
        font, 36, 48, color(240, 242, 247),
        1.15f, "SteamVita Updater");
    vita2d_pgf_draw_text(
        font, 80, 225, color(115, 164, 255),
        1.0f, heading.c_str());
    vita2d_pgf_draw_text(
        font, 80, 275, color(180, 188, 203),
        .72f, line.c_str());
    vita2d_end_drawing();
    vita2d_swap_buffers();
}

std::string read_expected_sha() {
    std::ifstream input(EXPECTED_SHA);
    std::string value;
    std::getline(input, value);
    while (!value.empty() &&
           (value.back() == '\r' ||
            value.back() == '\n' ||
            value.back() == ' ' ||
            value.back() == '\t')) {
        value.pop_back();
    }
    return value;
}

void wait_for_exit(vita2d_pgf* font,
                   const std::string& error) {
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
    for (;;) {
        draw(font, "Update failed", error + "  Circle: exit");
        SceCtrlData pad{};
        sceCtrlPeekBufferPositive(0, &pad, 1);
        if (pad.buttons & SCE_CTRL_CIRCLE) break;
        sceKernelDelayThread(16 * 1000);
    }
}

} // namespace

int main() {
    steamvita::ensure_directory("ux0:data/SteamVita");
    {
        std::ofstream version_file(UPDATER_VERSION_FILE, std::ios::trunc);
        if (version_file) {
            version_file << STEAMVITA_UPDATER_VERSION << "\n";
        }
    }
    steamvita::log_line(
        std::string("SteamVita updater started, version ") +
        STEAMVITA_UPDATER_VERSION + ".");

    vita2d_init();
    vita2d_set_vblank_wait(1);
    vita2d_pgf* font = vita2d_load_default_pgf();
    if (!font) {
        vita2d_fini();
        return 1;
    }

    draw(font, "Preparing update",
         "Closing SteamVita...");
    sceAppMgrDestroyOtherApp();
    sceKernelDelayThread(600 * 1000);

    const std::string sha = read_expected_sha();
    std::string error;

    draw(font, "Verifying update",
         "Checking SHA-256...");
    if (!steamvita::verify_sha256_file(
            UPDATE_VPK, sha, &error)) {
        steamvita::remove_tree(STAGE_DIR);
        steamvita::log_line("Verification failed: " + error);
        wait_for_exit(font, error);
        vita2d_free_pgf(font);
        vita2d_fini();
        return 1;
    }

    draw(font, "Preparing update",
         "Extracting verified VPK...");
    if (!steamvita::extract_vpk(
            UPDATE_VPK, STAGE_DIR, &error)) {
        steamvita::remove_tree(STAGE_DIR);
        steamvita::log_line("Extraction failed: " + error);
        wait_for_exit(font, error);
        vita2d_free_pgf(font);
        vita2d_fini();
        return 1;
    }

    draw(font, "Installing update",
         "Do not power off the Vita.");
    if (!steamvita::promote_directory(
            STAGE_DIR, &error)) {
        // Staging is disposable. Keep the verified VPK + SHA so the
        // updater can retry without downloading the whole file again.
        steamvita::remove_tree(STAGE_DIR);
        steamvita::log_line("Install failed: " + error);
        wait_for_exit(font, error);
        vita2d_free_pgf(font);
        vita2d_fini();
        return 1;
    }

    steamvita::remove_tree(STAGE_DIR);
    std::remove(UPDATE_VPK);
    std::remove(EXPECTED_SHA);

    draw(font, "Update complete",
         "Starting SteamVita...");
    sceKernelDelayThread(500 * 1000);

    const int launch = sceAppMgrLaunchAppByUri(
        0xFFFFF, "psgm:play?titleid=STMVITA01");
    if (launch < 0) {
        wait_for_exit(font,
                      "Update installed. Open SteamVita from LiveArea.");
    }

    vita2d_free_pgf(font);
    vita2d_fini();
    sceKernelExitProcess(0);
    return 0;
}
