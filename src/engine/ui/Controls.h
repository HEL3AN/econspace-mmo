#pragma once

#include "raylib.h"

// Controls that more than one screen wants: a number on a track, and a thing that is on
// or off.
//
// These began inside the gallery (#118) with a note saying they would move here when a
// second screen wanted them. The treatment settings (#120) are that second screen, and it
// is shown in two places at once -- in the game and in the editor -- so a copy per screen
// would have been three.
//
// Immediate mode, like the rest of this UI: they draw and report what the mouse did in the
// same call, and own no state.
namespace Ui
{

// Returns true on the frames the value changed. The whole row is the grab target, not the
// handle: a five-pixel target is a fight, and these are meant to be swept back and forth
// while watching what they do.
bool Slider(Rectangle r, const char* label, float& value, float lo, float hi,
            const char* fmt = "%.2f");

// Returns true on the frame it was clicked.
bool Toggle(Rectangle r, const char* label, bool& value);

// A momentary button, for the things that are not a state: reset, move up, save.
bool SmallButton(Rectangle r, const char* label, bool highlighted = false);

}  // namespace Ui
