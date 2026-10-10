#pragma once

#include "raylib.h"

#include <string_view>

// Who the mouse belongs to while something is being drawn (#297).
//
// Every widget in this UI draws and reads the mouse in the same call, so a row in a window
// that sits under another one used to react to a click on the one in front. The desk picks
// one owner of the mouse per frame and, while it draws anything else, says so here: the
// functions below then report no hover and no clicks. Outside any scope the mouse is
// everyone's, which is what the editor and the tests want -- they have no desk.
namespace Ui
{

// Sets whether what is drawn inside it owns the mouse, and restores the previous answer on
// the way out, so scopes nest.
class MouseScope
{
public:
    explicit MouseScope(bool owned);
    ~MouseScope();
    MouseScope(const MouseScope&) = delete;
    MouseScope& operator=(const MouseScope&) = delete;

private:
    bool previous_;
};

bool  MouseOwned();
bool  MouseOver(Rectangle r);    // the cursor is over r, and the mouse is ours
bool  MousePressed(int button);  // pressed this frame, and the mouse is ours
bool  MouseDown(int button);
float MouseWheel();

class Focus;

// What a window's content is handed: where it may draw, and the mouse as far as it is
// concerned. Pressed() and Hovered() are false unless the window owns the mouse. A frame
// from the desk also carries the keyboard focus and the window's id, which is what a text
// field inside it needs to take the keyboard (#297); without them a field cannot be typed in.
class Frame
{
public:
    Frame(Rectangle area, bool owner, Focus* focus = nullptr, std::string_view id = {})
        : area_(area), owner_(owner), focus_(focus), id_(id)
    {
    }

    Rectangle        Area() const { return area_; }
    bool             Owner() const { return owner_; }
    Focus*           KeyboardFocus() const { return focus_; }
    std::string_view Id() const { return id_; }
    Vector2 Mouse() const { return GetMousePosition(); }  // for coordinates, not for clicks
    bool    Hovered(Rectangle r) const;
    bool    Pressed(int button = MOUSE_BUTTON_LEFT) const;
    bool    Down(int button = MOUSE_BUTTON_LEFT) const;
    float   Wheel() const;

private:
    Rectangle        area_;
    bool             owner_;
    Focus*           focus_;
    std::string_view id_;
};

}  // namespace Ui
