#pragma once

#include <algorithm>
#include <cmath>

namespace Render
{

// How built objects are drawn (#618): a setting of this machine, not of the world.
//
// 0 is the 2D path every object always had; 1 and up are solids (Built3D) with more and more
// of the light worked out: lit solids only, then shadows, then occlusion, then more steps. A
// number and not an enum so the setting file, look.json and the UI share it without a header
// that needs a graphics library; the tests cover this half.
namespace BuiltLook
{
constexpr int PLAIN = 0;    // the 2D path
constexpr int BASIC = 1;    // lit solids, no shadows, no height pass
constexpr int SHADOWS = 2;  // + shadows from the star
constexpr int FULL = 3;     // + occlusion where parts meet
constexpr int HIGH = 4;     // + longer, softer shadows
constexpr int COUNT = 5;

inline int Clamp(int look)
{
    return std::clamp(look, PLAIN, HIGH);
}

inline const char* Name(int look)
{
    static const char* names[COUNT] = { "2D", "Basic", "Shadows", "Full", "High" };
    return names[Clamp(look)];
}
}  // namespace BuiltLook

// Takes the look down a step when drawing built objects is what the frame cannot afford, and
// never puts it back up: only the player raises it, by choosing (Reset).
//
// What it watches is the frame, not the solids: a frame that is slow with solids on screen is
// the one a cheaper look might fix, and the GPU timers that would say whose time it was are not
// on every driver (and cost a stall on the ones that matter most). So it is deliberately slow
// and cautious: a window of frames with solids in view, hitches clamped so a load does not
// count, one step per window, and nothing at all when the frame is fine.
class BuiltGovernor
{
public:
    // The average frame time, in seconds, over a window above which the look goes down a step.
    static constexpr float LIMIT_SECONDS = 0.026f;  // under ~38 frames a second
    static constexpr int   WINDOW = 120;            // frames with solids in view
    // A frame longer than this is a hitch (loading, a window dragged) and counts as this much.
    static constexpr float HITCH_SECONDS = 0.08f;

    // One frame: its time, and whether any solid was drawn in it. Returns true when this frame
    // lowered the cap.
    bool Frame(float seconds, bool solidsDrawn)
    {
        if (!solidsDrawn || cap_ <= BuiltLook::PLAIN)
            return false;
        sum_ += std::min(seconds, HITCH_SECONDS);
        if (++frames_ < WINDOW)
            return false;
        const bool slow = sum_ / (float)frames_ > LIMIT_SECONDS;
        sum_ = 0.0f;
        frames_ = 0;
        if (!slow)
            return false;
        cap_--;
        return true;
    }

    // The look is never above this.
    int Cap() const { return cap_; }
    // The player chose (or the setting changed): start again from what they asked for.
    void Reset()
    {
        cap_ = BuiltLook::HIGH;
        sum_ = 0.0f;
        frames_ = 0;
    }
    // What is drawn for a requested look.
    int Effective(int requested) const { return std::min(BuiltLook::Clamp(requested), cap_); }

private:
    int   cap_ = BuiltLook::HIGH;
    float sum_ = 0.0f;
    int   frames_ = 0;
};

}  // namespace Render
