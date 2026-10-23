#include "ui/Drag.h"

#include "ui/Fonts.h"
#include "ui/Theme.h"

#include "raymath.h"

namespace Ui
{
namespace Drag
{

namespace
{
enum class State
{
    Idle,
    Armed,
    Active,
    Dropped,
};

State       state = State::Idle;
DragPayload payload;
Vector2     pressAt{ 0.0f, 0.0f };
Vector2     mouseAt{ 0.0f, 0.0f };
}  // namespace

void Update(Vector2 mouse, bool down)
{
    mouseAt = mouse;
    switch (state)
    {
        case State::Dropped: state = State::Idle; break;  // nobody took it on its frame
        case State::Armed:
            if (!down)
                state = State::Idle;  // a click, not a drag
            else if (Vector2Distance(mouse, pressAt) >= THRESHOLD)
                state = State::Active;
            break;
        case State::Active:
            if (!down)
                state = State::Dropped;
            break;
        case State::Idle: break;
    }
}

void Arm(const DragPayload& p, Vector2 at)
{
    if (state == State::Active || state == State::Dropped)
        return;
    payload = p;
    pressAt = at;
    mouseAt = at;
    state = State::Armed;
}

void Cancel()
{
    state = State::Idle;
}

bool Armed()
{
    return state == State::Armed;
}

bool Active()
{
    return state == State::Active;
}

bool Dropping()
{
    return state == State::Dropped;
}

const DragPayload* Current()
{
    return state == State::Active || state == State::Dropped ? &payload : nullptr;
}

Vector2 Position()
{
    return mouseAt;
}

bool Accepts(std::string_view accepts, std::string_view kind)
{
    return accepts.empty() || accepts == kind;
}

const DragPayload* Take(std::string_view kind)
{
    if (state != State::Dropped || !Accepts(kind, payload.kind))
        return nullptr;
    state = State::Idle;
    return &payload;  // still valid until the next Arm
}

void Draw()
{
    if (state != State::Active || payload.label.empty())
        return;
    const Theme&  t = CurrentTheme();
    const float   px = Px(t.fontSize.body);
    const Vector2 size = MeasureString(Face::Regular, px, payload.label);
    const float   pad = Px(6.0f);
    // Below and right of the cursor, as a tooltip sits, so the target under it stays visible.
    const Rectangle box{ mouseAt.x + Px(14.0f), mouseAt.y + Px(10.0f), size.x + 2.0f * pad,
                         size.y + pad };
    DrawRectangleRec(box, Fade(t.colors.panel, 0.92f));
    DrawRectangleLinesEx(box, 1.0f, t.colors.accent);
    DrawString(Face::Regular, payload.label, { box.x + pad, box.y + pad * 0.5f }, px,
               t.colors.text);
}

}  // namespace Drag
}  // namespace Ui
