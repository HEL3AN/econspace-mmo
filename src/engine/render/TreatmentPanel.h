#pragma once

#include "render/MaterialLibrary.h"
#include "render/Treatment.h"
#include "ui/Input.h"
#include "ui/Layout.h"

namespace Render
{

// The settings surface for the screen treatment (#120).
//
// One panel, drawn by both the game and the editor's gallery. The direction asks for this
// to be adjustable *in the game* rather than in a file, and the gallery is where a look is
// actually judged -- two implementations of the same panel would have drifted by the
// second issue that touched it. The game shows it as a window on its desk (#297); the
// editor, which has no desk, in a box of its own. Either way it is laid out by Ui::Layout,
// so it follows the UI scale and the size it is given.
//
// It edits the Treatment's config in place. Draw returns true on a frame something
// changed, so the caller can decide whether to write the file; it never writes one itself,
// because when to persist is a policy the caller owns.
//
// `materials`, when given, has its problems listed under the passes' (#190). A material
// whose shader failed is the same kind of fact as a pass whose shader failed -- a
// property of this machine's driver -- and this is the one screen that says so.
class TreatmentPanel
{
public:
    bool Draw(const Ui::Frame& f, Treatment& t, const MaterialLibrary* materials = nullptr);

private:
    Ui::Layout layout_;  // remembers hover, scroll and a slider's drag between frames
};

}  // namespace Render
