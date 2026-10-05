#include "dosbox_backend.h"

#include <psp2/audioout.h>
#include <psp2/ctrl.h>
#include <vita2d.h>
#include <libretro.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <sys/stat.h>

namespace {

constexpr unsigned SCREEN_W = 960;
constexpr unsigned SCREEN_H = 544;
constexpr int AUDIO_GRAIN = 256;

std::string g_system_dir = "ux0:data/SteamVita/system";
std::string g_save_dir;
std::string g_content_dir;
std::map<std::string, std::string> g_variables;

vita2d_texture* g_texture = nullptr;
unsigned g_texture_w = 0;
unsigned g_texture_h = 0;

SceCtrlData g_pad {};
bool g_quit_requested = false;

int g_audio_port = -1;
int16_t g_audio_buffer[AUDIO_GRAIN * 2] {};
size_t g_audio_fill = 0;

void log_cb(enum retro_log_level level, const char* fmt, ...) {
    const char* prefix = "INFO";
    if (level == RETRO_LOG_ERROR) prefix = "ERROR";
    else if (level == RETRO_LOG_WARN) prefix = "WARN";
    else if (level == RETRO_LOG_DEBUG) prefix = "DEBUG";

    std::printf("[DOSBox:%s] ", prefix);
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
}

void parse_legacy_variables(const retro_variable* vars) {
    g_variables.clear();
    if (!vars) return;

    for (const retro_variable* v = vars; v->key; ++v) {
        if (!v->value) continue;
        const char* semi = std::strchr(v->value, ';');
        const char* options = semi ? semi + 1 : v->value;
        while (*options == ' ') ++options;

        std::string value(options);
        const size_t bar = value.find('|');
        if (bar != std::string::npos) value.resize(bar);
        g_variables[v->key] = value;
    }
}

bool environment_cb(unsigned cmd, void* data) {
    switch (cmd) {
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
            const auto fmt = *static_cast<retro_pixel_format*>(data);
            return fmt == RETRO_PIXEL_FORMAT_XRGB8888;
        }

        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
            *static_cast<const char**>(data) = g_system_dir.c_str();
            return true;

        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
            *static_cast<const char**>(data) = g_save_dir.c_str();
            return true;

#ifdef RETRO_ENVIRONMENT_GET_CONTENT_DIRECTORY
        case RETRO_ENVIRONMENT_GET_CONTENT_DIRECTORY:
            *static_cast<const char**>(data) = g_content_dir.c_str();
            return true;
#endif

        case RETRO_ENVIRONMENT_GET_CAN_DUPE:
            *static_cast<bool*>(data) = true;
            return true;

        case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
            // Ask cores to fall back to the original SET_VARIABLES interface.
            *static_cast<unsigned*>(data) = 0;
            return true;

        case RETRO_ENVIRONMENT_SET_VARIABLES:
            parse_legacy_variables(static_cast<const retro_variable*>(data));
            return true;

        case RETRO_ENVIRONMENT_GET_VARIABLE: {
            auto* var = static_cast<retro_variable*>(data);
            if (!var || !var->key) return false;
            auto it = g_variables.find(var->key);
            if (it == g_variables.end()) {
                var->value = nullptr;
                return false;
            }
            var->value = it->second.c_str();
            return true;
        }

        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
            *static_cast<bool*>(data) = false;
            return true;

        case RETRO_ENVIRONMENT_GET_LANGUAGE:
            *static_cast<unsigned*>(data) = RETRO_LANGUAGE_ENGLISH;
            return true;

        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: {
            auto* cb = static_cast<retro_log_callback*>(data);
            cb->log = log_cb;
            return true;
        }

        case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
        case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
        case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
        case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
        case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
        case RETRO_ENVIRONMENT_SET_MESSAGE:
            return true;

#ifdef RETRO_ENVIRONMENT_GET_INPUT_BITMASKS
        case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
            return true;
#endif

#ifdef RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE
        case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
            *static_cast<int*>(data) = 3; // video + audio enabled
            return true;
#endif

#ifdef RETRO_ENVIRONMENT_GET_FASTFORWARDING
        case RETRO_ENVIRONMENT_GET_FASTFORWARDING:
            *static_cast<bool*>(data) = false;
            return true;
#endif

        default:
            return false;
    }
}

void ensure_texture(unsigned width, unsigned height) {
    if (g_texture && width == g_texture_w && height == g_texture_h) return;

    if (g_texture) {
        vita2d_free_texture(g_texture);
        g_texture = nullptr;
    }

    g_texture = vita2d_create_empty_texture_format(
        width,
        height,
        SCE_GXM_TEXTURE_FORMAT_A8B8G8R8);

    g_texture_w = width;
    g_texture_h = height;
}

void draw_current_texture() {
    vita2d_start_drawing();
    vita2d_clear_screen();

    if (g_texture && g_texture_w && g_texture_h) {
        const float sx = static_cast<float>(SCREEN_W) / static_cast<float>(g_texture_w);
        const float sy = static_cast<float>(SCREEN_H) / static_cast<float>(g_texture_h);
        const float scale = std::min(sx, sy);
        const float draw_w = g_texture_w * scale;
        const float draw_h = g_texture_h * scale;
        const float x = (SCREEN_W - draw_w) * 0.5f;
        const float y = (SCREEN_H - draw_h) * 0.5f;
        vita2d_draw_texture_scale(g_texture, x, y, scale, scale);
    }

    vita2d_end_drawing();
    vita2d_swap_buffers();
}

void video_cb(const void* data, unsigned width, unsigned height, size_t pitch) {
    if (!width || !height) return;
    ensure_texture(width, height);
    if (!g_texture) return;

    if (data) {
        const auto* src_base = static_cast<const uint8_t*>(data);
        auto* dst_base = static_cast<uint8_t*>(vita2d_texture_get_datap(g_texture));
        const unsigned dst_stride = vita2d_texture_get_stride(g_texture);

        for (unsigned y = 0; y < height; ++y) {
            const auto* src = reinterpret_cast<const uint32_t*>(src_base + y * pitch);
            auto* dst = reinterpret_cast<uint32_t*>(dst_base + y * dst_stride);

            for (unsigned x = 0; x < width; ++x) {
                const uint32_t p = src[x]; // XRGB8888 = 0x00RRGGBB
                const unsigned r = (p >> 16) & 0xFF;
                const unsigned g = (p >> 8) & 0xFF;
                const unsigned b = p & 0xFF;
                dst[x] = RGBA8(r, g, b, 255);
            }
        }
    }

    draw_current_texture();
}

void flush_audio() {
    if (g_audio_port >= 0 && g_audio_fill == AUDIO_GRAIN) {
        sceAudioOutOutput(g_audio_port, g_audio_buffer);
        g_audio_fill = 0;
    }
}

size_t audio_batch_cb(const int16_t* data, size_t frames) {
    if (!data || g_audio_port < 0) return frames;

    size_t consumed = 0;
    while (consumed < frames) {
        const size_t room = AUDIO_GRAIN - g_audio_fill;
        const size_t take = std::min(room, frames - consumed);

        std::memcpy(
            &g_audio_buffer[g_audio_fill * 2],
            &data[consumed * 2],
            take * 2 * sizeof(int16_t));

        g_audio_fill += take;
        consumed += take;
        flush_audio();
    }

    return frames;
}

void audio_sample_cb(int16_t left, int16_t right) {
    const int16_t pair[2] = {left, right};
    audio_batch_cb(pair, 1);
}

void input_poll_cb() {
    sceCtrlPeekBufferPositive(0, &g_pad, 1);
    if ((g_pad.buttons & SCE_CTRL_START) && (g_pad.buttons & SCE_CTRL_SELECT)) {
        g_quit_requested = true;
    }
}

int16_t analog_axis(uint8_t value) {
    int v = (static_cast<int>(value) - 128) * 256;
    v = std::max(-32768, std::min(32767, v));
    return static_cast<int16_t>(v);
}

bool joy_button(unsigned id) {
    switch (id) {
        case RETRO_DEVICE_ID_JOYPAD_UP:     return g_pad.buttons & SCE_CTRL_UP;
        case RETRO_DEVICE_ID_JOYPAD_DOWN:   return g_pad.buttons & SCE_CTRL_DOWN;
        case RETRO_DEVICE_ID_JOYPAD_LEFT:   return g_pad.buttons & SCE_CTRL_LEFT;
        case RETRO_DEVICE_ID_JOYPAD_RIGHT:  return g_pad.buttons & SCE_CTRL_RIGHT;
        case RETRO_DEVICE_ID_JOYPAD_START:  return g_pad.buttons & SCE_CTRL_START;
        case RETRO_DEVICE_ID_JOYPAD_SELECT: return g_pad.buttons & SCE_CTRL_SELECT;
        case RETRO_DEVICE_ID_JOYPAD_L:      return g_pad.buttons & SCE_CTRL_LTRIGGER;
        case RETRO_DEVICE_ID_JOYPAD_R:      return g_pad.buttons & SCE_CTRL_RTRIGGER;
        // RetroPad layout: south=B, east=A, west=Y, north=X.
        case RETRO_DEVICE_ID_JOYPAD_B:      return g_pad.buttons & SCE_CTRL_CROSS;
        case RETRO_DEVICE_ID_JOYPAD_A:      return g_pad.buttons & SCE_CTRL_CIRCLE;
        case RETRO_DEVICE_ID_JOYPAD_Y:      return g_pad.buttons & SCE_CTRL_SQUARE;
        case RETRO_DEVICE_ID_JOYPAD_X:      return g_pad.buttons & SCE_CTRL_TRIANGLE;
        default: return false;
    }
}

int16_t input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id) {
    if (port != 0) return 0;
    device &= RETRO_DEVICE_MASK;

    if (device == RETRO_DEVICE_JOYPAD) {
#ifdef RETRO_DEVICE_ID_JOYPAD_MASK
        if (id == RETRO_DEVICE_ID_JOYPAD_MASK) {
            uint16_t mask = 0;
            for (unsigned i = 0; i < 16; ++i) {
                if (joy_button(i)) mask |= static_cast<uint16_t>(1u << i);
            }
            return static_cast<int16_t>(mask);
        }
#endif
        return joy_button(id) ? 1 : 0;
    }

    if (device == RETRO_DEVICE_ANALOG) {
        if (index == RETRO_DEVICE_INDEX_ANALOG_LEFT) {
            if (id == RETRO_DEVICE_ID_ANALOG_X) return analog_axis(g_pad.lx);
            if (id == RETRO_DEVICE_ID_ANALOG_Y) return analog_axis(g_pad.ly);
        }
        if (index == RETRO_DEVICE_INDEX_ANALOG_RIGHT) {
            if (id == RETRO_DEVICE_ID_ANALOG_X) return analog_axis(g_pad.rx);
            if (id == RETRO_DEVICE_ID_ANALOG_Y) return analog_axis(g_pad.ry);
        }
    }

    return 0;
}

void cleanup_frontend() {
    if (g_texture) {
        vita2d_free_texture(g_texture);
        g_texture = nullptr;
    }
    g_texture_w = 0;
    g_texture_h = 0;

    if (g_audio_port >= 0) {
        sceAudioOutReleasePort(g_audio_port);
        g_audio_port = -1;
    }
    g_audio_fill = 0;
}

std::string parent_directory(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return ".";
    return path.substr(0, slash);
}

} // namespace

bool run_dosbox_game(const std::string& content_path,
                     const std::string& save_directory,
                     std::string* error_message) {
    g_save_dir = save_directory;
    g_content_dir = parent_directory(content_path);
    g_quit_requested = false;
    g_variables.clear();
    g_pad = {};

    mkdir(g_system_dir.c_str(), 0777);
    mkdir(g_save_dir.c_str(), 0777);

    retro_set_environment(environment_cb);
    retro_set_video_refresh(video_cb);
    retro_set_audio_sample(audio_sample_cb);
    retro_set_audio_sample_batch(audio_batch_cb);
    retro_set_input_poll(input_poll_cb);
    retro_set_input_state(input_state_cb);

    retro_init();

    g_audio_port = sceAudioOutOpenPort(
        SCE_AUDIO_OUT_PORT_TYPE_MAIN,
        AUDIO_GRAIN,
        48000,
        SCE_AUDIO_OUT_MODE_STEREO);

    retro_game_info game {};
    game.path = content_path.c_str();

    if (!retro_load_game(&game)) {
        if (error_message) *error_message = "DOSBox Pure could not load: " + content_path;
        retro_deinit();
        cleanup_frontend();
        return false;
    }

    retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);

    while (!g_quit_requested) {
        retro_run();
    }

    retro_unload_game();
    retro_deinit();
    cleanup_frontend();

    if (error_message) error_message->clear();
    return true;
}
