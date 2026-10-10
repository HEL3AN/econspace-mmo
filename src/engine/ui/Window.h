#pragma once

#include "raylib.h"
#include "ui/Input.h"
#include <functional>
#include <string>

// A window's frame: background, title bar, close button, and the content inside them.
// Where it is, whether it is open and which one is in front belong to the desk (#297);
// this only knows what it looks like, so it is drawn wherever the desk says it is.
class Window
{
public:
    using Content = std::function<void(Ui::Frame&)>;

    Window(std::string title, Content content);

    // `owner`: this window owns the mouse this frame. Its content is told so through the
    // frame, and nothing in it highlights or reacts otherwise.
    void Draw(Rectangle bounds, bool owner) const;

    static Rectangle TitleBar(Rectangle bounds);
    static Rectangle CloseButton(Rectangle bounds);
    static Rectangle ContentArea(Rectangle bounds);

private:
    std::string title_;
    Content     content_;
};
