#pragma once

#include "raylib.h"

#include <functional>
#include <string>

// A distance of the player's own choosing for orbit, keep-at-range and follow (#298): a
// small panel with a slider and one button per behaviour, opened from the context menu
// beside the presets.
//
// Deliberately on its own and knowing nothing about Game. A real window system is coming
// (#297); this is the piece that moves into it, and until then it is drawn and fed input
// like the context menu it is opened from.
class RangePicker
{
public:
    // `navMode` is the command's: 3 orbit, 4 keep at range, 5 follow.
    using Choose = std::function<void(int navMode, float range)>;

    // Opens beside `at` (screen pixels) for an object of radius `targetSize`. The range runs
    // from just outside the object to far enough to stand off from a planet, and starts at
    // `initial`.
    void Open(Vector2 at, const std::string& targetName, float targetSize, float initial,
              Choose choose);
    void Close() { open_ = false; }
    bool IsOpen() const { return open_; }

    // True while the cursor is over it, so a click there is not also a click in space.
    bool Over() const;
    // Draws it and acts on what the mouse did; closes after a choice.
    void Draw();

private:
    Rectangle Bounds() const;

    bool        open_ = false;
    bool        justOpened_ = false;
    Vector2     pos_ = { 0.0f, 0.0f };
    std::string name_;
    float       lo_ = 1.0f, hi_ = 1.0f, value_ = 1.0f;
    Choose      choose_;
};
