#pragma once

#include "raylib.h"

#include <functional>
#include <string>

// A distance of the player's own choosing for orbit, keep-at-range and follow (#298): a
// small panel with a slider and one button per behaviour, opened from the context menu
// beside the presets.
//
// Deliberately on its own and knowing nothing about Game. It sits on the desk's popup
// layer (#297) beside the context menu it is opened from, which decides whether it owns
// the mouse; its controls ask Ui::MouseOver, so they react only when it does.
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

    // Draws it and acts on what the mouse did; closes after a choice.
    void Draw();

    Rectangle Bounds() const;

private:
    bool        open_ = false;
    bool        justOpened_ = false;
    Vector2     pos_ = { 0.0f, 0.0f };
    std::string name_;
    float       lo_ = 1.0f, hi_ = 1.0f, value_ = 1.0f;
    Choose      choose_;
};
