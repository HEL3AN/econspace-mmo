#pragma once

#include "raylib.h"
#include "ui/Input.h"
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// A window's frame: background, title bar (or a strip of tabs), its buttons, and the content
// inside them. Where it is, whether it is open, pinned, collapsed or stacked, and which one
// is in front belong to the desk (#297); this only knows what it looks like, so it is drawn
// however the desk says it is. Its metrics come from the theme at the current UI scale.
class Window
{
public:
    using Content = std::function<void(Ui::Frame&)>;

    // How the desk wants the frame drawn this frame.
    struct Chrome
    {
        // This window owns the mouse: its content is told so through the frame, and
        // nothing in it highlights or reacts otherwise.
        bool owner = false;
        bool resizable = false;  // the grip in the bottom right corner
        bool pinned = false;     // the pin is lit, and there is no grip
        bool collapsed = false;  // the title bar only
        // The titles of a stack's tabs, in order, and which is in front; with fewer than
        // two, the title bar shows the window's own title.
        std::vector<std::string> tabs;
        int                      activeTab = 0;
        bool dropTarget = false;  // what is being dragged would become a tab of this
    };

    Window(std::string title, Content content);

    const std::string& Title() const { return title_; }

    // `focus` and `id` are handed to the content's frame, so a text field in it can take the
    // keyboard.
    void Draw(Rectangle bounds, const Chrome& chrome, Ui::Focus* focus = nullptr,
              std::string_view id = {}) const;

    static Rectangle TitleBar(Rectangle bounds);
    static Rectangle CloseButton(Rectangle bounds);
    static Rectangle CollapseButton(Rectangle bounds);  // left of the close button
    static Rectangle PinButton(Rectangle bounds);       // left of the collapse button
    static Rectangle ContentArea(Rectangle bounds);
    static Rectangle ResizeGrip(Rectangle bounds);
    // Tab i of n in the title bar: as wide as the theme's tabWidth, narrower when they would
    // not fit before the buttons. What is left of the bar beside them still drags the stack.
    static Rectangle TabRect(Rectangle bounds, int i, int n);

private:
    std::string title_;
    Content     content_;
};
