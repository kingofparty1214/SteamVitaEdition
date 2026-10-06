#include "ui_audio.h"

#include <psp2/audioout.h>

#include <array>
#include <cstdint>

namespace {

constexpr int GRAIN = 256;
constexpr int SAMPLE_RATE = 48000;
int g_port = -1;

std::array<std::int16_t, GRAIN * 2> make_tone(UiSound sound) {
    std::array<std::int16_t, GRAIN * 2> pcm{};

    int period = 56;
    int amplitude = 2100;
    if (sound == UiSound::Select) {
        period = 42;
        amplitude = 2800;
    } else if (sound == UiSound::Back) {
        period = 72;
        amplitude = 1800;
    }

    for (int i = 0; i < GRAIN; ++i) {
        const int phase = i % period;
        int sample = phase < period / 2 ? amplitude : -amplitude;

        const int fade = GRAIN - i;
        sample = (sample * fade) / GRAIN;

        pcm[i * 2] = static_cast<std::int16_t>(sample);
        pcm[i * 2 + 1] = static_cast<std::int16_t>(sample);
    }

    return pcm;
}

} // namespace

bool ui_audio_initialize() {
    if (g_port >= 0) return true;

    g_port = sceAudioOutOpenPort(
        SCE_AUDIO_OUT_PORT_TYPE_MAIN,
        GRAIN,
        SAMPLE_RATE,
        SCE_AUDIO_OUT_MODE_STEREO);

    if (g_port < 0) return false;

    int volume[2] = {9000, 9000};
    sceAudioOutSetVolume(
        g_port,
        SCE_AUDIO_VOLUME_FLAG_L_CH |
            SCE_AUDIO_VOLUME_FLAG_R_CH,
        volume);

    return true;
}

void ui_audio_shutdown() {
    if (g_port >= 0) {
        sceAudioOutReleasePort(g_port);
        g_port = -1;
    }
}

void ui_audio_play(UiSound sound, bool enabled) {
    if (!enabled || g_port < 0) return;
    const auto pcm = make_tone(sound);
    sceAudioOutOutput(g_port, pcm.data());
}
