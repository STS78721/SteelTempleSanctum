#include "sound.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <vector>

#include "app.h"

namespace sanctum {

static constexpr int kSampleRate = 48000;
static constexpr float kPi = 3.14159265358979323846f;
static constexpr int kMaxVoices = 16;

struct Voice {
    int sfx = 0;
    size_t pos = 0;   // in stereo frames
    bool active = false;
};

static std::vector<float> g_bufs[SFX_COUNT];
static Voice g_voices[kMaxVoices];
static Sint64 g_last_ms[SFX_COUNT];
static std::mutex g_lock;
static SDL_AudioStream *g_stream = nullptr;
static bool g_audio_subsys = false;
static bool g_enabled = true;
static bool g_paused = false;
static float g_gain = 0.65f;

// ---- procedural synthesis helpers (interleaved stereo f32 @ 48 kHz)

static std::vector<float> make_buf(float dur_ms) {
    size_t frames = (size_t)std::max(1.0f, dur_ms * 0.001f * kSampleRate + 0.5f);
    return std::vector<float>(frames * 2, 0.0f);
}

static void add_tone(std::vector<float> &buf, float start_ms, float dur_ms, float f0, float f1, float glide_ms,
                     float tau_ms, float amp, float h2_amp, float h3_amp, float pan = 0.0f) {
    size_t total = buf.size() / 2;
    size_t s0 = (size_t)std::max(0.0f, start_ms * 0.001f * kSampleRate);
    size_t n = (size_t)std::max(1.0f, dur_ms * 0.001f * kSampleRate);
    size_t att = std::min(n / 2, (size_t)(0.0012f * kSampleRate));   // 1.2 ms raised-cosine attack
    size_t rel = std::min(n / 2, (size_t)(0.0015f * kSampleRate));   // 1.5 ms end-of-note fade

    float angle = (SDL_clamp(pan, -1.0f, 1.0f) + 1.0f) * 0.25f * kPi;
    float lg = std::cos(angle) * 1.41421356f;
    float rg = std::sin(angle) * 1.41421356f;

    double phase = 0.0;
    for (size_t i = 0; i < n && s0 + i < total; ++i) {
        float t_ms = (float)i * 1000.0f / kSampleRate;
        float f = glide_ms > 0.0f ? (f1 + (f0 - f1) * std::exp(-t_ms / glide_ms)) : f1;
        float env_att = att > 0 && i < att ? 0.5f * (1.0f - std::cos(kPi * (float)i / (float)att)) : 1.0f;
        float env_rel = rel > 0 && i >= n - rel ? 0.5f * (1.0f - std::cos(kPi * (float)(n - i) / (float)rel)) : 1.0f;
        float env_dec = std::exp(-t_ms / tau_ms);
        float h_dec = std::exp(-t_ms / (tau_ms * 0.4f));   // harmonics decay 2.5x faster than fundamental

        float s = (float)std::sin(phase);
        if (h2_amp != 0.0f) s += h2_amp * h_dec * (float)std::sin(2.0 * phase);
        if (h3_amp != 0.0f) s += h3_amp * h_dec * (float)std::sin(3.0 * phase);
        float sample = amp * env_att * env_rel * env_dec * s;

        buf[(s0 + i) * 2 + 0] += sample * lg;
        buf[(s0 + i) * 2 + 1] += sample * rg;
        phase += 2.0 * (double)kPi * (double)f / (double)kSampleRate;
    }
}

static void add_tap(std::vector<float> &buf, float start_ms, float dur_ms, float amp, float cutoff_hz, float pan = 0.0f) {
    size_t total = buf.size() / 2;
    size_t s0 = (size_t)std::max(0.0f, start_ms * 0.001f * kSampleRate);
    size_t n = (size_t)std::max(1.0f, dur_ms * 0.001f * kSampleRate);
    size_t att = std::min(n / 2, (size_t)(0.0004f * kSampleRate));
    float tau_ms = std::max(0.6f, dur_ms * 0.4f);

    float dt = 1.0f / kSampleRate;
    float rc_lp = 1.0f / (2.0f * kPi * cutoff_hz);
    float rc_hp = 1.0f / (2.0f * kPi * (cutoff_hz * 0.35f));
    float a_lp = dt / (rc_lp + dt);
    float a_hp = dt / (rc_hp + dt);

    float angle = (SDL_clamp(pan, -1.0f, 1.0f) + 1.0f) * 0.25f * kPi;
    float lg = std::cos(angle) * 1.41421356f;
    float rg = std::sin(angle) * 1.41421356f;

    uint32_t rng = 0x9E3779B9u;
    float lp = 0.0f, hp_state = 0.0f;
    for (size_t i = 0; i < n && s0 + i < total; ++i) {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        float white = ((float)(rng & 0xFFFFu) / 32768.0f) - 1.0f;
        lp += a_lp * (white - lp);
        float bp = lp - hp_state;
        hp_state += a_hp * bp;

        float t_ms = (float)i * 1000.0f / kSampleRate;
        float env_att = att > 0 && i < att ? (float)i / (float)att : 1.0f;
        float env_rel = i >= n - att && att > 0 ? (float)(n - i) / (float)att : 1.0f;
        float env = env_att * env_rel * std::exp(-t_ms / tau_ms);
        float sample = amp * env * bp;
        buf[(s0 + i) * 2 + 0] += sample * lg;
        buf[(s0 + i) * 2 + 1] += sample * rg;
    }
}

static void finish_buf(std::vector<float> &buf) {
    size_t frames = buf.size() / 2;
    size_t tail = std::min(frames, (size_t)(0.002f * kSampleRate));   // 2 ms click-free tail fade
    for (size_t i = 0; i < tail; ++i) {
        float g = 0.5f * (1.0f - std::cos(kPi * (float)(tail - 1 - i) / (float)tail));
        size_t idx = (frames - tail + i) * 2;
        buf[idx + 0] *= g;
        buf[idx + 1] *= g;
    }
}

static void build_sfx() {
    // SFX_MOVE: crisp Switch 2 rounded glassy-wood tick
    {
        auto &b = g_bufs[SFX_MOVE] = make_buf(36);
        add_tone(b, 0, 36, 920.0f, 680.0f, 9.0f, 8.5f, 0.34f, 0.20f, 0.06f);
        add_tap(b, 0, 3.5f, 0.05f, 2400.0f);
        finish_buf(b);
    }
    // SFX_EDGE: soft low muted wooden bump
    {
        auto &b = g_bufs[SFX_EDGE] = make_buf(48);
        add_tone(b, 0, 48, 220.0f, 145.0f, 12.0f, 12.0f, 0.24f, 0.14f, 0.0f);
        finish_buf(b);
    }
    // SFX_OK: bright two-note upward glassy pop
    {
        auto &b = g_bufs[SFX_OK] = make_buf(105);
        add_tone(b, 0, 56, 784.0f, 830.0f, 10.0f, 16.0f, 0.28f, 0.16f, 0.0f);
        add_tap(b, 0, 3.0f, 0.04f, 2600.0f);
        add_tone(b, 24, 81, 1174.66f, 1174.66f, 0.0f, 26.0f, 0.34f, 0.12f, 0.04f, 0.06f);
        finish_buf(b);
    }
    // SFX_BACK: gentle descending two-tone
    {
        auto &b = g_bufs[SFX_BACK] = make_buf(95);
        add_tone(b, 0, 50, 659.25f, 659.25f, 0.0f, 15.0f, 0.25f, 0.10f, 0.0f);
        add_tone(b, 22, 73, 440.00f, 440.00f, 0.0f, 24.0f, 0.28f, 0.08f, 0.0f, -0.05f);
        finish_buf(b);
    }
    // SFX_TAB: airy lateral flip-pluck
    {
        auto &b = g_bufs[SFX_TAB] = make_buf(65);
        add_tap(b, 0, 6.0f, 0.05f, 3200.0f);
        add_tone(b, 0, 65, 987.77f, 1318.51f, 14.0f, 16.0f, 0.26f, 0.14f, 0.05f);
        finish_buf(b);
    }
    // SFX_TOGGLE_ON: crisp upward double-tap
    {
        auto &b = g_bufs[SFX_TOGGLE_ON] = make_buf(95);
        add_tone(b, 0, 48, 698.46f, 698.46f, 0.0f, 13.0f, 0.25f, 0.12f, 0.0f);
        add_tone(b, 26, 69, 1046.50f, 1046.50f, 0.0f, 24.0f, 0.32f, 0.12f, 0.05f);
        finish_buf(b);
    }
    // SFX_TOGGLE_OFF: soft downward click
    {
        auto &b = g_bufs[SFX_TOGGLE_OFF] = make_buf(85);
        add_tone(b, 0, 45, 698.46f, 698.46f, 0.0f, 12.0f, 0.23f, 0.08f, 0.0f);
        add_tone(b, 24, 61, 523.25f, 523.25f, 0.0f, 20.0f, 0.24f, 0.06f, 0.0f);
        finish_buf(b);
    }
    // SFX_KEY: feather-light key pop
    {
        auto &b = g_bufs[SFX_KEY] = make_buf(26);
        add_tone(b, 0, 26, 1180.0f, 900.0f, 6.0f, 6.0f, 0.24f, 0.12f, 0.0f);
        add_tap(b, 0, 3.0f, 0.04f, 2800.0f);
        finish_buf(b);
    }
    // SFX_HOME: warm 3-note rising console chord
    {
        auto &b = g_bufs[SFX_HOME] = make_buf(195);
        add_tone(b, 0, 160, 523.25f, 523.25f, 0.0f, 45.0f, 0.22f, 0.08f, 0.0f, -0.14f);
        add_tone(b, 28, 165, 783.99f, 783.99f, 0.0f, 50.0f, 0.24f, 0.08f, 0.0f, 0.0f);
        add_tone(b, 56, 139, 1046.50f, 1046.50f, 0.0f, 60.0f, 0.26f, 0.12f, 0.0f, 0.14f);
        finish_buf(b);
    }
    // SFX_LAUNCH: resonant ascending game-start chime
    {
        auto &b = g_bufs[SFX_LAUNCH] = make_buf(270);
        add_tap(b, 0, 4.0f, 0.05f, 2800.0f);
        add_tone(b, 0, 220, 523.25f, 523.25f, 0.0f, 65.0f, 0.22f, 0.08f, 0.0f, -0.16f);
        add_tone(b, 24, 220, 659.25f, 659.25f, 0.0f, 70.0f, 0.22f, 0.08f, 0.0f, -0.06f);
        add_tone(b, 48, 210, 783.99f, 783.99f, 0.0f, 75.0f, 0.24f, 0.08f, 0.0f, 0.06f);
        add_tone(b, 74, 196, 1046.50f, 1046.50f, 0.0f, 85.0f, 0.26f, 0.12f, 0.0f, 0.16f);
        add_tone(b, 98, 172, 1567.98f, 1567.98f, 0.0f, 75.0f, 0.12f, 0.05f, 0.0f, 0.0f);
        finish_buf(b);
    }
    // SFX_ERROR: soft muted double low-tap
    {
        auto &b = g_bufs[SFX_ERROR] = make_buf(125);
        add_tone(b, 0, 55, 260.0f, 230.0f, 12.0f, 18.0f, 0.25f, 0.10f, 0.0f);
        add_tone(b, 48, 77, 215.0f, 185.0f, 12.0f, 22.0f, 0.23f, 0.08f, 0.0f);
        finish_buf(b);
    }
    // SFX_NOTIFY: delicate 2-note chime
    {
        auto &b = g_bufs[SFX_NOTIFY] = make_buf(180);
        add_tone(b, 0, 120, 880.00f, 880.00f, 0.0f, 35.0f, 0.22f, 0.08f, 0.0f, -0.08f);
        add_tone(b, 45, 135, 1318.51f, 1318.51f, 0.0f, 55.0f, 0.25f, 0.10f, 0.04f, 0.08f);
        finish_buf(b);
    }
}

// ---- SDL3 audio stream mixer

static void SDLCALL audio_cb(void *, SDL_AudioStream *stream, int additional_amount, int) {
    if (additional_amount <= 0) return;
    float mix[512];   // up to 256 stereo frames per chunk
    int rem = additional_amount / (int)(sizeof(float) * 2);
    while (rem > 0) {
        int frames = std::min(rem, 256);
        for (int i = 0; i < frames * 2; ++i) mix[i] = 0.0f;

        {
            std::lock_guard<std::mutex> l(g_lock);
            float gain = g_enabled && !g_paused ? g_gain : 0.0f;
            if (gain > 0.0f) {
                for (int v = 0; v < kMaxVoices; ++v) {
                    Voice &vc = g_voices[v];
                    if (!vc.active) continue;
                    const std::vector<float> &buf = g_bufs[vc.sfx];
                    size_t total = buf.size() / 2;
                    size_t avail = vc.pos < total ? std::min((size_t)frames, total - vc.pos) : 0;
                    for (size_t i = 0; i < avail; ++i) {
                        mix[i * 2 + 0] += buf[(vc.pos + i) * 2 + 0] * gain;
                        mix[i * 2 + 1] += buf[(vc.pos + i) * 2 + 1] * gain;
                    }
                    vc.pos += avail;
                    if (vc.pos >= total) vc.active = false;
                }
            }
        }

        for (int i = 0; i < frames * 2; ++i) mix[i] = std::tanh(mix[i]);
        SDL_PutAudioStreamData(stream, mix, frames * (int)(sizeof(float) * 2));
        rem -= frames;
    }
}

static void update_device_state() {
    if (!g_stream) return;
    SDL_AudioDeviceID dev = SDL_GetAudioStreamDevice(g_stream);
    if (!dev) return;
    bool run = g_enabled && !g_paused;
    if (run) {
        SDL_ResumeAudioDevice(dev);
    } else {
        SDL_PauseAudioDevice(dev);
        SDL_ClearAudioStream(g_stream);
    }
}

void sound_init() {
    build_sfx();
    for (int i = 0; i < SFX_COUNT; ++i) g_last_ms[i] = -1000;

    if (getenv("SANCTUM_SHOT") || getenv("SANCTUM_NO_AUDIO")) {
        sanctum_log("sound: skipped (SANCTUM_SHOT/SANCTUM_NO_AUDIO)");
        return;
    }
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        sanctum_log("sound: SDL_InitSubSystem(AUDIO) failed (%s)", SDL_GetError());
        return;
    }
    g_audio_subsys = true;

    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_F32;
    spec.channels = 2;
    spec.freq = kSampleRate;
    g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_cb, nullptr);
    if (!g_stream) {
        sanctum_log("sound: disabled (%s)", SDL_GetError());
        return;
    }
    sanctum_log("sound: initialized (%d Hz stereo)", kSampleRate);
    std::lock_guard<std::mutex> l(g_lock);
    update_device_state();
}

void sound_shutdown() {
    if (g_stream) {
        SDL_DestroyAudioStream(g_stream);
        g_stream = nullptr;
    }
    if (g_audio_subsys) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        g_audio_subsys = false;
    }
    std::lock_guard<std::mutex> l(g_lock);
    for (Voice &v : g_voices) v = {};
    for (auto &b : g_bufs) b.clear();
}

void sound_play(Sfx s) {
    if (s < 0 || s >= SFX_COUNT || !g_stream) return;
    std::lock_guard<std::mutex> l(g_lock);
    if (!g_enabled || g_paused) return;

    Sint64 now = (Sint64)SDL_GetTicks();
    Sint64 cooldown = (s == SFX_MOVE || s == SFX_KEY) ? 25 : (s == SFX_EDGE ? 150 : 20);
    if (now - g_last_ms[s] < cooldown) return;
    g_last_ms[s] = now;

    // find an idle slot, or steal the furthest-along voice if all 16 are busy
    int slot = -1;
    size_t max_pos = 0;
    int oldest = 0;
    for (int i = 0; i < kMaxVoices; ++i) {
        if (!g_voices[i].active) { slot = i; break; }
        if (g_voices[i].pos >= max_pos) { max_pos = g_voices[i].pos; oldest = i; }
    }
    if (slot < 0) slot = oldest;
    g_voices[slot] = {s, 0, true};
}

void sound_set_enabled(bool on) {
    std::lock_guard<std::mutex> l(g_lock);
    g_enabled = on;
    if (!on) {
        for (Voice &v : g_voices) v.active = false;
    }
    update_device_state();
}

void sound_set_volume(int level) {
    static constexpr float kLevels[] = {0.35f, 0.65f, 1.0f};
    std::lock_guard<std::mutex> l(g_lock);
    g_gain = kLevels[SDL_clamp(level, 0, 2)];
}

void sound_set_paused(bool paused) {
    std::lock_guard<std::mutex> l(g_lock);
    if (g_paused == paused) return;
    g_paused = paused;
    if (paused) {
        for (Voice &v : g_voices) v.active = false;
    }
    update_device_state();
}

}  // namespace sanctum
