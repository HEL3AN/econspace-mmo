#pragma once

#include "raylib.h"
#include <string>
#include <vector>
#include <functional>

// Right-click context menu on an object: a vertical list of actions.
// Opens at the cursor, closes on selecting an item or clicking elsewhere.
class ContextMenu
{
public:
    // Menu item: a label and the action run when it's selected.
    struct Item
    {
        std::string           label;
        std::function<void()> action;
    };

    void Open(Vector2 pos, std::vector<Item> items);  // open at the cursor position
    void Close();
    bool IsOpen() const { return open_; }

    // Input: runs the selected item and closes the menu; a click anywhere else closes it
    // too. Whether the cursor is over it is asked of Ui::MouseOver, so it is only "over" when
    // the desk says the menu owns the mouse (#297). Returns that.
    bool Update();
    void Draw() const;

    Rectangle Bounds() const;

private:
    bool              open_ = false;
    Vector2           pos_ = { 0.0f, 0.0f };
    std::vector<Item> items_;
};
