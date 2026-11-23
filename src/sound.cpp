#include "sound.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <vector>

#include "app.h"
#include "config.h"
#include "input.h"

namespace sanctum {

static constexpr int kSampleRate = 48000;
static constexpr float kPi = 3.14159265358979323846f;
static constexpr int kMaxVoices = 16;

struct Voice {
    int sfx = 0;
    double pos = 0.0;     // fractional stereo frame index
    double rate = 1.0;    // pitch ratio (1.0 = normal)
    float pan_l = 1.0f;   // left channel scale
    float pan_r = 1.0f;   // right channel scale
    bool active = false;
};

static std::vector<float> g_bufs[SFX_COUNT];
static std::vector<float> g_bgm_hum;     // seamless 8 s built-in ambient console hum (interleaved f32 stereo)
static std::vector<float> g_bgm_custom;  // optional user WAV converted to 48 kHz f32 stereo
static std::string g_bgm_loaded_path;
static size_t g_bgm_pos = 0;
static int g_bgm_mode = 0;               // 0 Off, 1 Hum, 2 Custom WAV
static float g_bgm_env = 0.0f;           // smooth fade envelope 0..1

static Voice g_voices[kMaxVoices];
static Sint64 g_last_ms[SFX_COUNT];
static Sint64 g_last_move_ms = -1000;
static int g_move_step = 0;
static std::mutex g_lock;
static SDL_AudioStream *g_stream = nullptr;
static bool g_audio_subsys = false;
static bool g_enabled = true;
static bool g_paused = false;
static Uint64 g_oneshot_until = 0;
static float g_gain = 0.65f;
static int g_theme = 0;

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
    size_t att = std::min(n / 2, (size_t)(0.0012f * kSampleRate));
    size_t rel = std::min(n / 2, (size_t)(0.0015f * kSampleRate));

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
        float h_dec = std::exp(-t_ms / (tau_ms * 0.4f));

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

// Crisp mechanical-wood/acrylic micro-click: ultra-short high-Q resonant burst + sub-ms tactile snap
static void add_click(std::vector<float> &buf, float start_ms, float freq_hz, float tau_ms, float amp, float pan = 0.0f) {
    size_t total = buf.size() / 2;
    size_t s0 = (size_t)std::max(0.0f, start_ms * 0.001f * kSampleRate);
    size_t n = (size_t)std::max(8.0f, (tau_ms * 5.5f + 2.0f) * 0.001f * kSampleRate);
    float angle = (SDL_clamp(pan, -1.0f, 1.0f) + 1.0f) * 0.25f * kPi;
    float lg = std::cos(angle) * 1.41421356f;
    float rg = std::sin(angle) * 1.41421356f;

    // 2-pole resonant bandpass excited by a razor-short 0.25 ms woody impulse + tiny pitched snap
    float w0 = 2.0f * kPi * SDL_clamp(freq_hz, 400.0f, 9500.0f) / (float)kSampleRate;
    float r = std::exp(-1.0f / std::max(1.0f, tau_ms * 0.001f * kSampleRate));
    float a1 = -2.0f * r * std::cos(w0);
    float a2 = r * r;
    float y1 = 0.0f, y2 = 0.0f;
    uint32_t rng = 0x51A7F3C9u ^ (uint32_t)(freq_hz * 17.0f);
    size_t burst = std::max<size_t>(3, (size_t)(0.00032f * kSampleRate));
    double snap_ph = 0.0;

    for (size_t i = 0; i < n && s0 + i < total; ++i) {
        float x = 0.0f;
        if (i == 0) x = 1.0f;
        else if (i < burst) {
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            float wn = ((float)(rng & 0xFFFFu) / 32768.0f) - 1.0f;
            x = wn * 0.55f * (1.0f - (float)i / (float)burst);
        }
        float y = (1.0f - r) * x - a1 * y1 - a2 * y2;
        y2 = y1;
        y1 = y;

        // Add subtle physical switch-body knock (rapid downward drop in first 2.5 ms)
        float t_ms = (float)i * 1000.0f / (float)kSampleRate;
        float snap_f = (freq_hz * 0.28f) + (freq_hz * 0.52f) * std::exp(-t_ms / 0.85f);
        float snap_env = std::exp(-t_ms / std::max(0.7f, tau_ms * 0.65f));
        float snap = 0.34f * snap_env * (float)std::sin(snap_ph);
        snap_ph += 2.0 * (double)kPi * (double)snap_f / (double)kSampleRate;

        float env_in = i < 4 ? (float)(i + 1) / 4.0f : 1.0f;
        float sample = amp * env_in * (y * 1.9f + snap);
        buf[(s0 + i) * 2 + 0] += sample * lg;
        buf[(s0 + i) * 2 + 1] += sample * rg;
    }
}

static void finish_buf(std::vector<float> &buf) {
    size_t frames = buf.size() / 2;
    size_t tail = std::min(frames, (size_t)(0.002f * kSampleRate));
    for (size_t i = 0; i < tail; ++i) {
        float g = 0.5f * (1.0f - std::cos(kPi * (float)(tail - 1 - i) / (float)tail));
        size_t idx = (frames - tail + i) * 2;
        buf[idx + 0] *= g;
        buf[idx + 1] *= g;
    }
}

static void build_sfx(int theme) {
    // Profiles:
    //   0 = Tactile Click (crisp mechanical micro-clicks + tight woody-glass pops)
    //   1 = Bubble Pop    (playful rising aquatic/marimba bubbly blips)
    //   2 = Warm Retro    (lower pitch, fuller woody harmonics)
    //   3 = Soft Minimal  (airy pure sine ticks, whisper-light transient)
    bool is_clicky = (theme == 0);
    bool is_bubbly = (theme == 1);
    float pm = theme == 2 ? 0.78f : theme == 3 ? 1.12f : 1.0f;
    float hm = theme == 2 ? 1.55f : theme == 3 ? 0.25f : is_bubbly ? 0.65f : 0.95f;
    float tm = theme == 2 ? 1.25f : theme == 3 ? 0.88f : is_clicky ? 0.84f : 1.05f;
    float am = theme == 3 ? 0.72f : 1.0f;
    float clkm = is_clicky ? 1.0f : is_bubbly ? 0.28f : theme == 2 ? 0.62f : 0.14f;
    float tapm = is_clicky ? 1.0f : is_bubbly ? 0.35f : theme == 2 ? 0.70f : 0.18f;

    auto tone = [&](std::vector<float> &b, float s, float d, float f0, float f1, float gl, float tau, float amp,
                    float h2, float h3, float pan = 0.0f) {
        add_tone(b, s, d, f0 * pm, f1 * pm, gl, tau * tm, amp * am, h2 * hm, h3 * hm, pan);
    };
    auto click = [&](std::vector<float> &b, float s, float fhz, float tau, float amp, float pan = 0.0f) {
        if (clkm > 0.01f) add_click(b, s, fhz * pm, tau, amp * clkm * am, pan);
    };

    // ---- SFX_MOVE: signature menu navigation sound
    {
        if (is_clicky) {
            auto &b = g_bufs[SFX_MOVE] = make_buf(32);
            // Crisp mechanical-acrylic micro-click + tight woody glass tick
            click(b, 0.0f, 3550.0f, 1.35f, 0.34f);
            add_tap(b, 0.0f, 3.2f, 0.14f, 4200.0f);
            tone(b, 0.6f, 28.0f, 1180.0f, 840.0f, 5.5f, 6.2f, 0.27f, 0.18f, 0.05f);
            finish_buf(b);
        } else if (is_bubbly) {
            auto &b = g_bufs[SFX_MOVE] = make_buf(44);
            // Playful upward water-bubble blip
            tone(b, 0.0f, 44.0f, 460.0f, 940.0f, 11.0f, 11.5f, 0.34f, 0.10f, 0.02f);
            click(b, 0.0f, 2200.0f, 1.0f, 0.12f);
            finish_buf(b);
        } else {
            auto &b = g_bufs[SFX_MOVE] = make_buf(36);
            tone(b, 0, 36, 920.0f, 680.0f, 9.0f, 8.5f, 0.32f, 0.20f, 0.06f);
            click(b, 0.0f, 2600.0f, 1.2f, 0.18f);
            finish_buf(b);
        }
    }
    // ---- SFX_EDGE: soft muted rubbery wood bump
    {
        auto &b = g_bufs[SFX_EDGE] = make_buf(48);
        click(b, 0.0f, 1150.0f, 1.6f, 0.22f);
        tone(b, 0.0f, 48.0f, 235.0f, 140.0f, 10.0f, 11.5f, 0.26f, 0.12f, 0.0f);
        finish_buf(b);
    }
    // ---- SFX_OK: crisp tactile click + bright ascending glassy-marimba chime
    {
        auto &b = g_bufs[SFX_OK] = make_buf(112);
        click(b, 0.0f, 3800.0f, 1.4f, 0.32f, -0.04f);
        add_tap(b, 0.0f, 3.0f, 0.11f * tapm, 3800.0f);
        if (is_bubbly) {
            tone(b, 0.0f, 52.0f, 587.33f, 783.99f, 9.0f, 14.0f, 0.28f, 0.10f, 0.0f, -0.05f);
            tone(b, 22.0f, 90.0f, 880.0f, 1174.66f, 10.0f, 24.0f, 0.34f, 0.08f, 0.03f, 0.06f);
        } else {
            tone(b, 0.5f, 52.0f, 783.99f, 830.61f, 7.0f, 13.0f, 0.26f, 0.14f, 0.03f, -0.04f);
            click(b, 20.0f, 4400.0f, 1.1f, 0.18f, 0.05f);
            tone(b, 20.5f, 90.0f, 1174.66f, 1174.66f, 0.0f, 24.0f, 0.33f, 0.11f, 0.04f, 0.06f);
            tone(b, 21.0f, 68.0f, 1760.0f, 1760.0f, 0.0f, 14.0f, 0.08f, 0.0f, 0.0f, 0.08f);
        }
        finish_buf(b);
    }
    // ---- SFX_BACK: soft reverse woody click + smooth descending fifth
    {
        auto &b = g_bufs[SFX_BACK] = make_buf(96);
        click(b, 0.0f, 2450.0f, 1.3f, 0.25f, 0.04f);
        tone(b, 0.5f, 46.0f, 740.0f, 659.25f, 9.0f, 13.0f, 0.24f, 0.10f, 0.0f, 0.04f);
        click(b, 20.0f, 1850.0f, 1.2f, 0.16f, -0.05f);
        tone(b, 20.5f, 75.0f, 493.88f, 440.00f, 12.0f, 22.0f, 0.27f, 0.08f, 0.0f, -0.06f);
        finish_buf(b);
    }
    // ---- SFX_TAB: breezy crisp card-flip snap
    {
        auto &b = g_bufs[SFX_TAB] = make_buf(68);
        click(b, 0.0f, 4300.0f, 1.2f, 0.30f);
        add_tap(b, 0.0f, 5.0f, 0.13f * tapm, 4100.0f);
        tone(b, 0.5f, 66.0f, 987.77f, 1318.51f, 12.0f, 15.0f, 0.25f, 0.12f, 0.04f);
        finish_buf(b);
    }
    // ---- SFX_TOGGLE_ON: crisp tactile switch snap ON + bright upward ping
    {
        auto &b = g_bufs[SFX_TOGGLE_ON] = make_buf(98);
        click(b, 0.0f, 3950.0f, 1.2f, 0.32f, -0.04f);
        tone(b, 0.5f, 44.0f, 698.46f, 740.0f, 7.0f, 12.0f, 0.24f, 0.12f, 0.0f, -0.04f);
        click(b, 24.0f, 4600.0f, 1.1f, 0.20f, 0.05f);
        tone(b, 24.5f, 72.0f, 1046.50f, 1046.50f, 0.0f, 22.0f, 0.31f, 0.12f, 0.05f, 0.05f);
        finish_buf(b);
    }
    // ---- SFX_TOGGLE_OFF: crisp tactile switch release OFF
    {
        auto &b = g_bufs[SFX_TOGGLE_OFF] = make_buf(86);
        click(b, 0.0f, 2700.0f, 1.2f, 0.26f, 0.04f);
        tone(b, 0.5f, 42.0f, 698.46f, 659.25f, 8.0f, 11.0f, 0.22f, 0.08f, 0.0f, 0.04f);
        click(b, 22.0f, 1950.0f, 1.2f, 0.18f, -0.04f);
        tone(b, 22.5f, 62.0f, 523.25f, 493.88f, 10.0f, 18.0f, 0.23f, 0.06f, 0.0f, -0.04f);
        finish_buf(b);
    }
    // ---- SFX_KEY: ultra-light keycap / slider tick
    {
        auto &b = g_bufs[SFX_KEY] = make_buf(24);
        click(b, 0.0f, 4500.0f, 1.0f, 0.28f);
        tone(b, 0.4f, 22.0f, 1320.0f, 960.0f, 4.5f, 5.0f, 0.21f, 0.10f, 0.0f);
        finish_buf(b);
    }
    // ---- SFX_HOME: crisp magnetic latch click + luminous 3-note console chord
    {
        auto &b = g_bufs[SFX_HOME] = make_buf(200);
        click(b, 0.0f, 3600.0f, 1.4f, 0.30f);
        tone(b, 1.0f, 165.0f, 523.25f, 523.25f, 0.0f, 45.0f, 0.22f, 0.08f, 0.0f, -0.16f);
        tone(b, 26.0f, 165.0f, 783.99f, 783.99f, 0.0f, 50.0f, 0.24f, 0.08f, 0.0f, 0.0f);
        tone(b, 52.0f, 146.0f, 1046.50f, 1046.50f, 0.0f, 60.0f, 0.26f, 0.12f, 0.0f, 0.16f);
        tone(b, 74.0f, 120.0f, 1567.98f, 1567.98f, 0.0f, 48.0f, 0.09f, 0.04f, 0.0f, 0.08f);
        finish_buf(b);
    }
    // ---- SFX_LAUNCH: tactile double-click lock-in + sparkling console start fanfare
    {
        auto &b = g_bufs[SFX_LAUNCH] = make_buf(275);
        click(b, 0.0f, 3850.0f, 1.4f, 0.34f, -0.08f);
        click(b, 22.0f, 4650.0f, 1.2f, 0.24f, 0.08f);
        tone(b, 1.0f, 220.0f, 523.25f, 523.25f, 0.0f, 65.0f, 0.22f, 0.08f, 0.0f, -0.18f);
        tone(b, 24.0f, 220.0f, 659.25f, 659.25f, 0.0f, 70.0f, 0.22f, 0.08f, 0.0f, -0.07f);
        tone(b, 48.0f, 215.0f, 783.99f, 783.99f, 0.0f, 75.0f, 0.24f, 0.08f, 0.0f, 0.07f);
        tone(b, 74.0f, 200.0f, 1046.50f, 1046.50f, 0.0f, 85.0f, 0.26f, 0.12f, 0.0f, 0.18f);
        tone(b, 98.0f, 175.0f, 1567.98f, 1567.98f, 0.0f, 75.0f, 0.13f, 0.05f, 0.0f, 0.0f);
        finish_buf(b);
    }
    // ---- SFX_ERROR: muted woody double-knock
    {
        auto &b = g_bufs[SFX_ERROR] = make_buf(128);
        click(b, 0.0f, 1300.0f, 1.6f, 0.24f);
        tone(b, 0.0f, 55.0f, 260.0f, 220.0f, 12.0f, 17.0f, 0.25f, 0.10f, 0.0f);
        click(b, 48.0f, 1100.0f, 1.6f, 0.22f);
        tone(b, 48.0f, 78.0f, 215.0f, 175.0f, 12.0f, 21.0f, 0.23f, 0.08f, 0.0f);
        finish_buf(b);
    }
    // ---- SFX_NOTIFY: delicate crystalline 2-bell chime
    {
        auto &b = g_bufs[SFX_NOTIFY] = make_buf(185);
        click(b, 0.0f, 4200.0f, 1.0f, 0.16f, -0.08f);
        tone(b, 0.5f, 120.0f, 880.00f, 880.00f, 0.0f, 35.0f, 0.22f, 0.08f, 0.0f, -0.08f);
        tone(b, 45.0f, 138.0f, 1318.51f, 1318.51f, 0.0f, 55.0f, 0.25f, 0.10f, 0.04f, 0.08f);
        finish_buf(b);
    }
    // ---- SFX_SHUTTER: crisp double-blade camera shutter snap + confirmation ping
    {
        auto &b = g_bufs[SFX_SHUTTER] = make_buf(145);
        add_tap(b, 0, 9.0f, 0.34f, 4400.0f, -0.10f);
        add_click(b, 0.0f, 4100.0f, 1.4f, 0.28f, -0.10f);
        tone(b, 0, 22, 1650.0f, 920.0f, 5.0f, 6.0f, 0.20f, 0.14f, 0.0f, -0.08f);
        add_tap(b, 26, 12.0f, 0.38f, 3400.0f, 0.10f);
        tone(b, 28, 112, 1760.0f, 1760.0f, 0.0f, 26.0f, 0.22f, 0.10f, 0.04f, 0.08f);
        finish_buf(b);
    }
}

static void build_ambient_hum() {
    // Seamless 8-second gentle console ambient pad: every tone is an integer multiple of 1/8 Hz -> exact phase wrap
    const size_t frames = (size_t)kSampleRate * 8;
    g_bgm_hum.assign(frames * 2, 0.0f);
    // Warm Cmaj9 airy pad (C3 ~131Hz, G3 ~196Hz, D4 ~294Hz, E4 ~330Hz), quantized to 0.125 Hz grid
    struct Partial { double hz, amp, lfo_hz, pan; };
    static const Partial ps[] = {
        {131.0,  0.032, 0.125, -0.25},
        {196.0,  0.024, 0.250,  0.25},
        {293.75, 0.014, 0.125, -0.45},
        {329.75, 0.012, 0.250,  0.45},
        {261.75, 0.010, 0.125,  0.00},
    };
    for (size_t i = 0; i < frames; ++i) {
        double t = (double)i / (double)kSampleRate;
        float L = 0.0f, R = 0.0f;
        for (const auto &p : ps) {
            double breath = 0.72 + 0.28 * std::sin(2.0 * kPi * p.lfo_hz * t);
            float v = (float)(p.amp * breath * std::sin(2.0 * kPi * p.hz * t));
            float ang = (float)((p.pan + 1.0) * 0.25 * kPi);
            L += v * std::cos(ang) * 1.4142f;
            R += v * std::sin(ang) * 1.4142f;
        }
        g_bgm_hum[i * 2 + 0] = L;
        g_bgm_hum[i * 2 + 1] = R;
    }
}

static void load_custom_wav(const std::string &path) {
    if (path == g_bgm_loaded_path) return;
    g_bgm_loaded_path = path;
    g_bgm_custom.clear();
    g_bgm_pos = 0;
    if (path.empty()) return;
    SDL_AudioSpec src_spec{};
    Uint8 *src_buf = nullptr;
    Uint32 src_len = 0;
    if (!SDL_LoadWAV(path.c_str(), &src_spec, &src_buf, &src_len)) {
        sanctum_log("sound: could not load custom WAV %s (%s)", path.c_str(), SDL_GetError());
        return;
    }
    SDL_AudioSpec dst_spec{SDL_AUDIO_F32, 2, kSampleRate};
    Uint8 *dst_buf = nullptr;
    int dst_len = 0;
    if (SDL_ConvertAudioSamples(&src_spec, src_buf, (int)src_len, &dst_spec, &dst_buf, &dst_len) && dst_buf && dst_len > 0) {
        size_t floats = (size_t)dst_len / sizeof(float);
        g_bgm_custom.assign((const float *)dst_buf, (const float *)dst_buf + floats);
        SDL_free(dst_buf);
        sanctum_log("sound: loaded custom WAV %s (%.1f s)", path.c_str(), (double)(floats / 2) / kSampleRate);
    }
    SDL_free(src_buf);
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
            // Ambient BGM (gentle fade in/out)
            const std::vector<float> *bgm = nullptr;
            if (!g_paused) {
                if (g_bgm_mode == 1 && !g_bgm_hum.empty()) bgm = &g_bgm_hum;
                else if (g_bgm_mode == 2 && !g_bgm_custom.empty()) bgm = &g_bgm_custom;
            }
            float bgm_target = bgm ? (g_bgm_mode == 2 ? 0.22f * g_gain : 0.85f * g_gain) : 0.0f;
            if (bgm && bgm->size() >= 2) {
                size_t total = bgm->size() / 2;
                for (int i = 0; i < frames; ++i) {
                    g_bgm_env += (bgm_target - g_bgm_env) * 0.0008f;
                    size_t idx = (g_bgm_pos % total) * 2;
                    mix[i * 2 + 0] += (*bgm)[idx + 0] * g_bgm_env;
                    mix[i * 2 + 1] += (*bgm)[idx + 1] * g_bgm_env;
                    g_bgm_pos = (g_bgm_pos + 1) % total;
                }
            } else {
                g_bgm_env = 0.0f;
            }

            // UI SFX voices (with smooth fractional-rate pitch shift & subtle stereo pan)
            bool oneshot_active = g_oneshot_until != 0 && SDL_GetTicks() < g_oneshot_until;
            float gain = g_enabled && (!g_paused || oneshot_active) ? g_gain : 0.0f;
            if (gain > 0.0f) {
                for (int v = 0; v < kMaxVoices; ++v) {
                    Voice &vc = g_voices[v];
                    if (!vc.active) continue;
                    const std::vector<float> &buf = g_bufs[vc.sfx];
                    size_t total = buf.size() / 2;
                    if (total == 0) { vc.active = false; continue; }
                    float gl = gain * vc.pan_l;
                    float gr = gain * vc.pan_r;
                    for (int i = 0; i < frames; ++i) {
                        size_t idx = (size_t)vc.pos;
                        if (idx >= total) { vc.active = false; break; }
                        float frac = (float)(vc.pos - (double)idx);
                        size_t idx2 = idx + 1 < total ? idx + 1 : idx;
                        float l = buf[idx * 2 + 0] + (buf[idx2 * 2 + 0] - buf[idx * 2 + 0]) * frac;
                        float r = buf[idx * 2 + 1] + (buf[idx2 * 2 + 1] - buf[idx * 2 + 1]) * frac;
                        mix[i * 2 + 0] += l * gl;
                        mix[i * 2 + 1] += r * gr;
                        vc.pos += vc.rate;
                    }
                    if ((size_t)vc.pos >= total) vc.active = false;
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
    bool oneshot = g_oneshot_until != 0 && SDL_GetTicks() < g_oneshot_until;
    bool run = ((g_enabled || g_bgm_mode > 0) && !g_paused) || (g_enabled && oneshot);
    if (run) {
        SDL_ResumeAudioDevice(dev);
    } else {
        SDL_PauseAudioDevice(dev);
        SDL_ClearAudioStream(g_stream);
    }
}

void sound_init() {
    build_sfx(g_theme);
    build_ambient_hum();
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
    g_bgm_hum.clear();
    g_bgm_custom.clear();
}

static void enqueue_voice_locked(Sfx s, double rate, float pan) {
    if (!g_stream || !g_enabled) return;
    int slot = -1;
    double max_pos = -1.0;
    int oldest = 0;
    for (int i = 0; i < kMaxVoices; ++i) {
        if (!g_voices[i].active) { slot = i; break; }
        if (g_voices[i].pos >= max_pos) { max_pos = g_voices[i].pos; oldest = i; }
    }
    if (slot < 0) slot = oldest;
    float p = SDL_clamp(pan, -0.45f, 0.45f);
    float ang = (p + 1.0f) * 0.25f * kPi;
    float pl = std::cos(ang) * 1.41421356f;
    float pr = std::sin(ang) * 1.41421356f;
    g_voices[slot] = {s, 0.0, SDL_clamp(rate, 0.72, 1.38), pl, pr, true};
}

void sound_play_move(int dir, float pan) {
    bool fire = false;
    {
        std::lock_guard<std::mutex> l(g_lock);
        if (g_paused) return;
        Sint64 now = (Sint64)SDL_GetTicks();
        if (now - g_last_ms[SFX_MOVE] < 25) return;
        g_last_ms[SFX_MOVE] = now;
        fire = true;

        double rate = 1.0;
        float use_pan = 0.0f;
        if (cfg().sound_pitch_pan) {
            if (now - g_last_move_ms > 420) g_move_step = 0;
            else if (dir > 0) g_move_step = std::min(5, g_move_step + 1);
            else if (dir < 0) g_move_step = std::max(-4, g_move_step - 1);
            else g_move_step = (g_move_step >= 2) ? 0 : g_move_step + 1;
            g_last_move_ms = now;
            // Subtle half-semitone musical micro-ladder prevents ear fatigue during rapid scrolling
            rate = std::pow(2.0, (double)g_move_step * 0.55 / 12.0);
            use_pan = SDL_clamp(pan, -0.34f, 0.34f);
        }
        enqueue_voice_locked(SFX_MOVE, rate, use_pan);
    }
    if (fire) input_rumble(HAP_TICK);
}

void sound_play(Sfx s) {
    if (s < 0 || s >= SFX_COUNT) return;
    if (s == SFX_MOVE) {
        sound_play_move(0, 0.0f);
        return;
    }
    Sint64 cooldown = (s == SFX_KEY) ? 25 : (s == SFX_EDGE ? 150 : 20);
    bool fire = false;
    {
        std::lock_guard<std::mutex> l(g_lock);
        if (g_paused) return;
        Sint64 now = (Sint64)SDL_GetTicks();
        if (now - g_last_ms[s] < cooldown) return;
        g_last_ms[s] = now;
        fire = true;
        enqueue_voice_locked(s, 1.0, 0.0f);
    }
    if (fire) {
        Haptic h = HAP_TICK;
        switch (s) {
        case SFX_OK: case SFX_TOGGLE_ON: case SFX_HOME: case SFX_NOTIFY: case SFX_SHUTTER: h = HAP_OK; break;
        case SFX_BACK: case SFX_TOGGLE_OFF: h = HAP_BACK; break;
        case SFX_LAUNCH: h = HAP_LAUNCH; break;
        case SFX_ERROR: case SFX_EDGE: h = HAP_ERROR; break;
        default: h = HAP_TICK; break;
        }
        input_rumble(h);
    }
}

void sound_play_oneshot(Sfx s) {
    if (s < 0 || s >= SFX_COUNT) return;
    bool fire = false;
    {
        std::lock_guard<std::mutex> l(g_lock);
        Sint64 now = (Sint64)SDL_GetTicks();
        if (now - g_last_ms[s] < 120) return;
        g_last_ms[s] = now;
        fire = true;
        if (g_stream && g_enabled) {
            if (g_paused) {
                g_oneshot_until = (Uint64)now + 320;
                update_device_state();
            }
            enqueue_voice_locked(s, 1.0, 0.0f);
        }
    }
    if (fire) input_rumble(HAP_OK, true);
}

void sound_poll() {
    std::lock_guard<std::mutex> l(g_lock);
    if (g_oneshot_until != 0 && SDL_GetTicks() >= g_oneshot_until) {
        g_oneshot_until = 0;
        update_device_state();
    }
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

void sound_set_theme(int theme) {
    int t = SDL_clamp(theme, 0, 3);
    std::lock_guard<std::mutex> l(g_lock);
    if (g_theme == t) return;
    g_theme = t;
    for (Voice &v : g_voices) v.active = false;
    build_sfx(g_theme);
}

void sound_set_ambient(int mode, const std::string &wav_path) {
    int m = SDL_clamp(mode, 0, 2);
    std::vector<float> loaded;
    if (m == 2 && !wav_path.empty()) {
        std::lock_guard<std::mutex> l(g_lock);
        load_custom_wav(wav_path);
    }
    std::lock_guard<std::mutex> l(g_lock);
    g_bgm_mode = m;
    g_bgm_pos = 0;
    g_bgm_env = 0.0f;
    update_device_state();
}

void sound_set_paused(bool paused) {
    std::lock_guard<std::mutex> l(g_lock);
    if (g_paused == paused) return;
    g_paused = paused;
    if (paused) {
        for (Voice &v : g_voices) v.active = false;
        g_bgm_env = 0.0f;
    }
    update_device_state();
}

}  // namespace sanctum
