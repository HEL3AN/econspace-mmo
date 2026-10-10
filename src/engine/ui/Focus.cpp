#include "ui/Focus.h"

#include "raylib.h"

#include <algorithm>

namespace Ui
{

void Focus::Take(std::string_view owner, std::string_view field)
{
    if (Holds(owner, field))
        return;
    if (Taken())
        Release(Blur::Elsewhere);
    // Taking back a focus lost earlier this frame -- the press that released it landed on
    // the field itself -- is not a loss.
    lost_.erase(std::remove_if(lost_.begin(), lost_.end(),
                               [&](const Lost& l) { return l.owner == owner && l.field == field; }),
                lost_.end());
    owner_.assign(owner);
    field_.assign(field);
    seen_ = true;
}

bool Focus::Holds(std::string_view owner, std::string_view field) const
{
    return Taken() && owner_ == owner && field_ == field;
}

void Focus::Release(Blur why)
{
    if (!Taken())
        return;
    lost_.push_back({ owner_, field_, why });
    owner_.clear();
    field_.clear();
}

void Focus::ReleaseOwner(std::string_view owner, Blur why)
{
    if (Taken() && owner_ == owner)
        Release(why);
}

void Focus::Seen(std::string_view owner, std::string_view field)
{
    if (Holds(owner, field))
        seen_ = true;
}

void Focus::EndFrame()
{
    // A field hears why it lost the focus in the frame it lost it; one that has not asked
    // by now is not being drawn.
    lost_.clear();
    if (Taken() && !seen_)
        Release(Blur::Elsewhere);
    seen_ = false;
}

Focus::Blur Focus::TakeBlur(std::string_view owner, std::string_view field)
{
    for (auto it = lost_.begin(); it != lost_.end(); ++it)
        if (it->owner == owner && it->field == field)
        {
            const Blur why = it->why;
            lost_.erase(it);
            return why;
        }
    return Blur::None;
}

EditResult EditText(std::string& text, const KeyInput& in, size_t maxLength, bool (*accept)(char))
{
    EditResult r;
    for (int i = 0; i < in.backspace && !text.empty(); i++)
    {
        text.pop_back();
        r.changed = true;
    }
    for (char c : in.chars)
    {
        if (c < 32 || c > 126 || text.size() >= maxLength || (accept && !accept(c)))
            continue;
        text += c;
        r.changed = true;
    }
    r.submitted = in.enter;
    return r;
}

KeyInput ReadKeys()
{
    KeyInput k;
    for (int c = GetCharPressed(); c != 0; c = GetCharPressed())
        if (c >= 32 && c < 127)
            k.chars += (char)c;
    // Held, it repeats, as every text box does.
    if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE))
        k.backspace = 1;
    k.enter = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER);
    return k;
}

}  // namespace Ui
