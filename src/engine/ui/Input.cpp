#include "ui/Input.h"

namespace Ui
{

namespace
{
bool owned = true;  // no desk, no scope: the mouse is everyone's
}  // namespace

MouseScope::MouseScope(bool own) : previous_(owned)
{
    owned = own;
}

MouseScope::~MouseScope()
{
    owned = previous_;
}

bool MouseOwned()
{
    return owned;
}

bool MouseOver(Rectangle r)
{
    return owned && CheckCollisionPointRec(GetMousePosition(), r);
}

bool MousePressed(int button)
{
    return owned && IsMouseButtonPressed(button);
}

bool MouseDown(int button)
{
    return owned && IsMouseButtonDown(button);
}

float MouseWheel()
{
    return owned ? GetMouseWheelMove() : 0.0f;
}

bool Frame::Hovered(Rectangle r) const
{
    return owner_ && CheckCollisionPointRec(GetMousePosition(), r);
}

bool Frame::Pressed(int button) const
{
    return owner_ && IsMouseButtonPressed(button);
}

bool Frame::Down(int button) const
{
    return owner_ && IsMouseButtonDown(button);
}

float Frame::Wheel() const
{
    return owner_ ? GetMouseWheelMove() : 0.0f;
}

}  // namespace Ui
