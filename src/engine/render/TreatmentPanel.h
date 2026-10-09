#pragma once

#include "render/MaterialLibrary.h"
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
//
// `materials`, when given, has its problems listed under the passes' (#190). A material
// whose shader failed is the same kind of fact as a pass whose shader failed -- a
// property of this machine's driver -- and this is the one screen that says so.
bool DrawTreatmentPanel(Rectangle area, Treatment& t, const MaterialLibrary* materials = nullptr);

// How tall the panel wants to be for the chain it is showing. Callers size a window or a
// column from it rather than guessing and clipping.
float TreatmentPanelHeight(const Treatment& t, const MaterialLibrary* materials = nullptr);

}  // namespace Render
