#include "xmb_ui.h"

#include <cmath>

namespace {

unsigned rgba(unsigned r, unsigned g, unsigned b, unsigned a) {
    return RGBA8(r, g, b, a);
}

constexpr float SCREEN_W = 960.0f;
constexpr float SCREEN_H = 544.0f;

} // namespace

void xmb_ui_update(XmbUiState* state, bool ambience_enabled) {
    if (!state || !ambience_enabled) return;
    state->phase += 0.0125f;
    if (state->phase > 10000.0f) state->phase = 0.0f;
}

void xmb_draw_background(const XmbUiState& state, bool ambience_enabled) {
    vita2d_draw_rectangle(0, 0, SCREEN_W, SCREEN_H, rgba(10, 20, 39, 255));
    vita2d_draw_rectangle(0, 0, SCREEN_W, 150, rgba(20, 42, 72, 255));
    vita2d_draw_rectangle(0, 150, SCREEN_W, 210, rgba(14, 31, 56, 255));
    vita2d_draw_rectangle(0, 360, SCREEN_W, 184, rgba(8, 17, 33, 255));

    const float phase = ambience_enabled ? state.phase : 0.0f;

    for (int band = 0; band < 4; ++band) {
        const float base_y = 240.0f + static_cast<float>(band) * 31.0f;
        const float amplitude = 22.0f + static_cast<float>(band) * 6.0f;
        const float speed = 0.7f + static_cast<float>(band) * 0.17f;
        const unsigned alpha = static_cast<unsigned>(28 - band * 4);

        float previous_x = 0.0f;
        float previous_y =
            base_y + std::sin(phase * speed + band) * amplitude;

        for (int x = 16; x <= 960; x += 16) {
            const float fx = static_cast<float>(x);
            const float y =
                base_y +
                std::sin(fx * 0.009f + phase * speed + band * 0.9f) * amplitude +
                std::sin(fx * 0.0035f - phase * 0.6f) * 10.0f;

            const float min_y = previous_y < y ? previous_y : y;
            const float height = std::fabs(y - previous_y) + 2.0f;
            vita2d_draw_rectangle(
                previous_x,
                min_y,
                18.0f,
                height,
                rgba(105, 175, 255, alpha));

            previous_x = fx;
            previous_y = y;
        }
    }

    for (int i = 0; i < 14; ++i) {
        const float drift = ambience_enabled
            ? std::sin(phase * (0.35f + i * 0.015f) + i * 1.7f)
            : 0.0f;
        const float x = 42.0f + static_cast<float>((i * 71) % 900);
        const float y = 90.0f + static_cast<float>((i * 97) % 380) + drift * 8.0f;
        const float size = (i % 3 == 0) ? 2.0f : 1.0f;
        vita2d_draw_rectangle(x, y, size, size, rgba(220, 236, 255, 90));
    }

    vita2d_draw_rectangle(0, 0, SCREEN_W, 544, rgba(3, 8, 18, 32));
}

void xmb_draw_glass_panel(float x, float y, float w, float h, unsigned alpha) {
    vita2d_draw_rectangle(x, y, w, h, rgba(9, 17, 31, alpha));
    vita2d_draw_rectangle(x, y, w, 1.0f, rgba(196, 225, 255, 34));
    vita2d_draw_rectangle(x, y + h - 1.0f, w, 1.0f, rgba(0, 0, 0, 42));
}

void xmb_draw_selection(float x, float y, float w, float h, float pulse) {
    const float glow = 0.5f + 0.5f * std::sin(pulse);
    const unsigned outer_alpha =
        static_cast<unsigned>(28.0f + glow * 24.0f);
    const unsigned inner_alpha =
        static_cast<unsigned>(70.0f + glow * 34.0f);

    vita2d_draw_rectangle(
        x - 4.0f, y - 3.0f, w + 8.0f, h + 6.0f,
        rgba(105, 175, 255, outer_alpha));
    vita2d_draw_rectangle(
        x, y, w, h,
        rgba(126, 188, 255, inner_alpha));
    vita2d_draw_rectangle(
        x, y, 3.0f, h,
        rgba(225, 242, 255, 190));
}

void xmb_draw_separator(float x, float y, float w) {
    vita2d_draw_rectangle(x, y, w, 1.0f, rgba(185, 215, 245, 30));
}
