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

    // Input: runs the selected item and closes the menu.
    // Returns true if the cursor is currently over the menu (mouse is busy with UI).
    bool Update();
    void Draw() const;

private:
    Rectangle Bounds() const;

    bool              open_ = false;
    Vector2           pos_ = { 0.0f, 0.0f };
    std::vector<Item> items_;
};
