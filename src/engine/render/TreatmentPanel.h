#pragma once

#include "render/Treatment.h"
#include "raylib.h"

namespace Render
{

// The settings surface for the screen treatment (#120).
//
// One panel, drawn by both the game and the editor's gallery. The direction asks for this
// to be adjustable *in the game* rather than in a file, and the gallery is where a look is
// actually judged -- two implementations of the same panel would have drifted by the
// second issue that touched it.
//
// It edits the Treatment's config in place. Returns true on a frame something changed, so
// the caller can decide whether to write the file; it never writes one itself, because
// when to persist is a policy the caller owns.
bool DrawTreatmentPanel(Rectangle area, Treatment& t);

// How tall the panel wants to be for the chain it is showing. Callers size a window or a
// column from it rather than guessing and clipping.
float TreatmentPanelHeight(const Treatment& t);

}  // namespace Render
