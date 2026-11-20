// Every device becomes one Nav per frame: pads (SDL3 gamepads, by the label printed on the button, so A confirms on
// Xbox, Nintendo and PlayStation pads alike), the keyboard, and the mouse (hover moves focus, click is A, the
// right button is B). Held directions and sticks repeat like a console menu: a pause, then a steady step.
#pragma once
#include <SDL3/SDL.h>

namespace sanctum {

void input_event(const SDL_Event &e);
void input_frame();   // after the frame's events: picks this frame's Nav (with repeat)

}  // namespace sanctum
