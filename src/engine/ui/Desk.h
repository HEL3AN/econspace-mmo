#pragma once

#include "ui/DeskLayout.h"
#include "ui/Input.h"
#include "ui/Window.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Ui
{

// The window manager (#297): every window, screen and popup the client shows, on layers,
// with one owner of the mouse per frame and one rule for Esc.
//
// Two kinds of thing live on it. A *window* is the desk's own: it keeps where it is,
// whether it is open and which is in front, draws its frame, drags it and closes it, and
// the window's content is handed a `Frame`. A *surface* is drawn and kept by someone else
// -- the map, the sensor screen, the station, the context menu -- and is here so that it
// takes part in the same z-order, mouse ownership and Esc as the windows.
//
// Every frame: BeginFrame, then Escape if Esc was pressed, then HandleMouse; draw with
// Draw(layer) or, for a surface drawn elsewhere, inside a MouseScope of Owns(id); Persist
// at the end.
class Desk
{
public:
    struct Surface
    {
        std::function<Rectangle()> bounds;  // where it is this frame (required)
        // When its open state is kept elsewhere (the context menu knows when it is open);
        // otherwise the desk keeps it.
        std::function<bool()> isOpen;
        // What closing it means besides the desk forgetting it: F10's panel writes its file.
        std::function<void()> onClose;
        // Drawn by Draw(layer) in z-order; without one, its owner draws it.
        std::function<void()> draw;
    };

    void AddWindow(const WindowSpec& spec, const std::string& title, bool open,
                   Window::Content content);
    void AddSurface(const WindowSpec& spec, bool open, Surface surface);

    // The screen size, where the surfaces are and whether they are open, and who owns the
    // mouse -- hit-tested against where things were drawn last.
    void BeginFrame();
    // Window frames: a press raises the window it lands on, the close button closes it, the
    // title bar drags it, and a resizable window's corner grip resizes it.
    void HandleMouse();
    // Closes the top thing Esc closes; true if something was.
    bool Escape();
    // Draws the open windows and surfaces of one layer that the desk draws, bottom to top.
    void Draw(Layer layer) const;
    // Writes the layout if a window moved, opened or closed since the last call.
    void Persist();

    bool IsOpen(const std::string& id) const;
    void SetOpen(const std::string& id, bool open);  // opening one raises it
    void Toggle(const std::string& id) { SetOpen(id, !IsOpen(id)); }
    bool Owns(const std::string& id) const;  // owns the mouse this frame
    bool WorldOwnsMouse() const { return layout_.Owner() == DeskLayout::NONE; }
    void ResetLayout() { layout_.Reset(); }

    // The menu bar's buttons, in the order they were registered.
    struct MenuSlot
    {
        std::string id;
        std::string label;
        bool        open;
    };
    std::vector<MenuSlot> MenuSlots() const;

    // Reads one account's layout from a file shared by every account on this machine, and
    // keeps it there from now on. A file from a newer build is neither read nor written.
    void UseLayoutFile(const std::string& path, const std::string& account);

    const DeskLayout& Layout() const { return layout_; }

private:
    struct Item
    {
        std::unique_ptr<Window> window;  // null for a surface
        Surface                 surface;
    };

    void Close(int h);

    DeskLayout        layout_;
    std::vector<Item> items_;  // by handle
    int               dragging_ = DeskLayout::NONE;
    bool              resizing_ = false;  // the drag is of the corner, not the title bar
    Vector2           dragOffset_{ 0.0f, 0.0f };
    std::string       file_;
    std::string       account_;
    bool              canSave_ = false;
};

}  // namespace Ui
