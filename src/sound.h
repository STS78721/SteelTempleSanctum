// Procedural console-style UI sounds & optional Home ambient audio, synthesized with SDL3 audio (zero external assets).
#pragma once
#include <string>

namespace sanctum {

enum Sfx {
    SFX_MOVE,        // crisp tactile mechanical click + glassy-wood tick (focus move)
    SFX_EDGE,        // soft low muted wooden bump (boundary hit, throttled)
    SFX_OK,          // crisp click + bright two-note upward pop (confirm / open)
    SFX_BACK,        // soft reverse click + gentle descending two-tone (back / cancel)
    SFX_TAB,         // airy lateral card-flip click (L/R tab / page / package switch)
    SFX_TOGGLE_ON,   // crisp switch snap + upward ping (toggle ON / tick / choose)
    SFX_TOGGLE_OFF,  // crisp switch release + downward click (toggle OFF / untick)
    SFX_KEY,         // feather-light keycap click (on-screen keyboard / slider tick)
    SFX_HOME,        // tactile latch click + warm 3-note rising console chord (Home button)
    SFX_LAUNCH,      // lock-in snap + resonant ascending game-start chime (Play)
    SFX_ERROR,       // soft muted wooden double-knock (locked / cannot act)
    SFX_NOTIFY,      // delicate 2-note chime (download / update ready)
    SFX_SHUTTER,     // crisp camera shutter snap (screenshot capture)
    SFX_COUNT
};

void sound_init();                  // non-fatal if no audio device exists; pre-synthesizes all buffers
void sound_shutdown();              // closes stream/device and shuts down SDL_INIT_AUDIO
void sound_play(Sfx s);             // no-op if disabled, paused, or not initialized
void sound_play_move(int dir, float pan = 0.0f); // directional move click with optional dynamic pitch step & stereo pan
void sound_play_oneshot(Sfx s);     // plays even while paused behind a running game (auto-re-pauses after SFX finishes)
void sound_poll();                  // checks if an in-game oneshot SFX finished so audio device can sleep again
void sound_set_enabled(bool on);    // toggles UI SFX
void sound_set_volume(int level);   // 0 = Low (0.35), 1 = Medium (0.65), 2 = High (1.0)
void sound_set_theme(int theme);    // 0 = Tactile Click, 1 = Bubble Pop, 2 = Warm Retro, 3 = Soft Minimal
void sound_set_ambient(int mode, const std::string &wav_path = {}); // 0 = Off, 1 = Built-in hum, 2 = Custom WAV
void sound_set_paused(bool paused); // pauses audio device while a launched game is running, resumes on return

}  // namespace sanctum
