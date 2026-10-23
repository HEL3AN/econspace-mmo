#pragma once

#include "raylib.h"

#include <string>
#include <string_view>

// Drag and drop between windows (#297, #472): a mechanic of the window framework, so any
// window can be a source or a target without inventing its own.
//
// A drag carries a value, not a pointer: the window it came from may not be drawn on the frame
// it lands. A press on a source *arms* a drag; it starts only once the cursor has moved a few
// pixels with the button held, so a click is still a click. While it is dragged the mouse
// belongs to whatever is under the cursor rather than to the window pressed in (the desk lifts
// its capture), so a target can light up. On release the drop is offered for that one frame:
// the first target under the cursor, in a window that owns the mouse, that accepts the kind
// takes it. Released where nothing takes it, nothing happens.
//
// Immediate mode, as everything in this UI is: sources and targets declare themselves while
// they are drawn (Layout::DragSource, TableSpec::drag, Layout::DropHover, Layout::Dropped). The
// state below is one per process, because there is one mouse.
namespace Ui
{

struct DragPayload
{
    std::string kind;           // what is carried: "goods"; later "module", "ship", "entity"
    std::string source;         // where from, as the source names it: "hold", "storage", "can:812"
    int         item = -1;      // which one, in the kind's own terms: a good's index
    int         amount = 0;     // how many this drag carries
    int         available = 0;  // how many there were where it came from
    std::string label;          // what the cursor shows while it is dragged
};

namespace Drag
{

// How far, in pixels, the cursor moves with the button held before a press becomes a drag.
constexpr float THRESHOLD = 6.0f;

// Once a frame, before anything is drawn: where the mouse is and whether the left button is
// down. The desk calls it (Desk::BeginFrame); a test hands it in. A drop not taken on its frame
// is gone on the next.
void Update(Vector2 mouse, bool down);

// A press on a source: the payload a drag would carry from here, and where the press was.
// Ignored while something is already being dragged.
void Arm(const DragPayload& payload, Vector2 at);
void Cancel();  // Esc, or a source that is gone

bool Armed();   // pressed on a source, not yet pulled far enough
bool Active();  // being dragged
// Released this frame, and not yet taken.
bool Dropping();
// What is being dragged, or what has just been dropped; null when neither.
const DragPayload* Current();
Vector2            Position();  // the cursor, as the last Update saw it

// The drop of this frame, if its kind is `kind` (empty: any kind): taken, so no other target
// sees it. Null when there is none. Whether the drop landed *on* the caller is the caller's
// question (Layout::Dropped asks it); this is for one that has already answered it -- the
// world, when no window was under the cursor.
const DragPayload* Take(std::string_view kind = {});

// Whether a payload of `kind` is accepted by a target that asks for `accepts` (empty: any).
bool Accepts(std::string_view accepts, std::string_view kind);

// The payload's label beside the cursor, over everything: drawn last in a frame.
void Draw();

}  // namespace Drag
}  // namespace Ui
