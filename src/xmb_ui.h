#pragma once

#include <cstdint>
#include <vita2d.h>

struct XmbUiState {
    float phase = 0.0f;
};

void xmb_ui_update(XmbUiState* state, bool ambience_enabled);
void xmb_draw_background(const XmbUiState& state, bool ambience_enabled);
void xmb_draw_glass_panel(float x, float y, float w, float h, unsigned alpha = 120);
void xmb_draw_selection(float x, float y, float w, float h, float pulse);
void xmb_draw_separator(float x, float y, float w);
