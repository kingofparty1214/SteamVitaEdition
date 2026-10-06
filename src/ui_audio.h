#pragma once

enum class UiSound {
    Navigate,
    Select,
    Back,
};

bool ui_audio_initialize();
void ui_audio_shutdown();
void ui_audio_play(UiSound sound, bool enabled);
