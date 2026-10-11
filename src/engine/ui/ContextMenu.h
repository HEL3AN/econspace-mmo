#pragma once

#include "raylib.h"
#include <string>
#include <vector>
#include <functional>

// Right-click context menu on an object: a vertical list of actions.
// Opens at the cursor, closes on selecting an item or clicking elsewhere. It knows nothing
// about what the actions are: the game builds the list from the same model its other
// windows and the agent's tools use (Actions::For, #297).
class ContextMenu
{
public:
    // Menu item: a label and the action run when it's selected.
    struct Item
    {
        std::string           label;
        std::function<void()> action;
    };

    // Opens at the cursor position. A header, when given, is a line above the actions that
    // names what they are about -- "Aurora Hub  12.5k" (#365) -- and is not itself one.
    void Open(Vector2 pos, std::vector<Item> items, std::string header = std::string());
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
    float             width_ = 0.0f;  // the longest label, set on Open
    std::vector<Item> items_;
    std::string       header_;

    float HeaderHeight() const;  // 0 without one
};
