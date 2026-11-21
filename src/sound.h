// Procedural Switch 2-style UI sounds, synthesized at start-up with SDL3 audio (zero external assets).
#pragma once

namespace sanctum {

enum Sfx {
    SFX_MOVE,        // crisp rounded glassy-wood tick (focus move)
    SFX_EDGE,        // soft low muted wooden bump (boundary hit, throttled)
    SFX_OK,          // bright two-note upward glassy pop (confirm / open)
    SFX_BACK,        // gentle descending two-tone (back / cancel)
    SFX_TAB,         // airy lateral flip-pluck (L/R tab / page / package switch)
    SFX_TOGGLE_ON,   // crisp upward double-tap (toggle ON / tick / choose)
    SFX_TOGGLE_OFF,  // soft downward click (toggle OFF / untick)
    SFX_KEY,         // feather-light key pop (on-screen keyboard / slider tick)
    SFX_HOME,        // warm 3-note rising console chord (Home button)
    SFX_LAUNCH,      // resonant ascending game-start chime (Play)
    SFX_ERROR,       // soft muted double low-tap (locked / cannot act)
    SFX_NOTIFY,      // delicate 2-note chime (download / update ready)
    SFX_COUNT
};

void sound_init();                  // non-fatal if no audio device exists; pre-synthesizes all buffers
void sound_shutdown();              // closes stream/device and shuts down SDL_INIT_AUDIO
void sound_play(Sfx s);             // no-op if disabled, paused, or not initialized
void sound_set_enabled(bool on);    // pauses device & clears active voices when false; resumes when true (unless externally paused)
void sound_set_volume(int level);   // 0 = Low (0.35), 1 = Medium (0.65), 2 = High (1.0)
void sound_set_paused(bool paused); // pauses audio device while a launched game is running, resumes on return

}  // namespace sanctum
