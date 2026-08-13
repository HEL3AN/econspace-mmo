#pragma once

#include "raylib.h"
#include <string>
#include <functional>

// Draggable UI window: frame, title bar, close button, and content area.
// The content is drawn by the supplied callback (which receives the rectangle
// of the inner area).
class Window
{
public:
    Window(std::string title, Rectangle bounds, bool open);

    void SetContent(std::function<void(Rectangle)> content) { content_ = std::move(content); }

    void Draw();        // frame, title, and content (if the window is open)
    void UpdateDrag();  // continue an in-progress drag

    bool IsOpen() const { return open_; }
    void SetOpen(bool open) { open_ = open; }
    void Toggle() { open_ = !open_; }

    void SetBounds(Rectangle bounds) { bounds_ = bounds; }  // reset position/size

    bool ContainsMouse() const;  // mouse over the (open) window
    bool TitleBarHit(Vector2 m) const;
    bool CloseButtonHit(Vector2 m) const;
    void StartDrag(Vector2 m);

private:
    Rectangle TitleBarRect() const;
    Rectangle CloseButtonRect() const;
    Rectangle ContentArea() const;

    std::string                    title_;
    Rectangle                      bounds_;
    bool                           open_;
    bool                           dragging_ = false;
    Vector2                        dragOffset_ = { 0.0f, 0.0f };
    std::function<void(Rectangle)> content_;
};
