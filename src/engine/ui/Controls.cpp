#include "ui/Controls.h"

#include "ui/Input.h"
#include "ui/UiTheme.h"
#include "raymath.h"
#include <cmath>

namespace Ui
{

bool Slider(Rectangle r, const char* label, float& value, float lo, float hi, const char* fmt)
{
    Text(label, (int)r.x, (int)r.y, 12, TEXT_DIM);
    Text(TextFormat(fmt, value), (int)(r.x + r.width - 54), (int)r.y, 12, TEXT);

    Rectangle track{ r.x, r.y + 16.0f, r.width, 8.0f };
    DrawRectangleRec(track, Fade(TITLE_BG, 0.9f));
    DrawRectangleLinesEx(track, 1.0f, PANEL_BORDER);

    const float t = (hi > lo) ? Clamp((value - lo) / (hi - lo), 0.0f, 1.0f) : 0.0f;
    DrawRectangleRec({ track.x, track.y, track.width * t, track.height }, Fade(ACCENT, 0.55f));
    DrawCircleV({ track.x + track.width * t, track.y + track.height / 2.0f }, 5.0f, ACCENT);

    Rectangle grab{ r.x, r.y + 8.0f, r.width, 20.0f };
    if (MouseDown(MOUSE_BUTTON_LEFT) && MouseOver(grab))
    {
        const float nt = Clamp((GetMousePosition().x - track.x) / track.width, 0.0f, 1.0f);
        const float nv = lo + nt * (hi - lo);
        if (nv != value)
        {
            value = nv;
            return true;
        }
    }
    return false;
}

bool LogSlider(Rectangle r, const char* label, float& value, float lo, float hi, const char* fmt)
{
    // Slider moves the logarithm along the track and prints nothing (an empty format); the
    // number printed is the value itself, never its logarithm.
    float       t = std::log(fmaxf(value, lo));
    const float tlo = std::log(lo), thi = std::log(hi);
    const bool  changed = Slider(r, label, t, tlo, thi, "");
    if (changed)
        value = Clamp(std::exp(t), lo, hi);
    Text(TextFormat(fmt, value), (int)(r.x + r.width - 54), (int)r.y, 12, TEXT);
    return changed;
}

bool Toggle(Rectangle r, const char* label, bool& value)
{
    const bool over = MouseOver(r);
    DrawRectangleRec(r, value ? Fade(ACCENT, 0.22f)
                              : (over ? Fade(ACCENT, 0.10f) : Fade(TITLE_BG, 0.8f)));
    DrawRectangleLinesEx(r, 1.0f, value ? ACCENT : PANEL_BORDER);
    Text(label, (int)r.x + 8, (int)r.y + 5, 13, value ? ACCENT : TEXT);
    if (over && MousePressed(MOUSE_BUTTON_LEFT))
    {
        value = !value;
        return true;
    }
    return false;
}

bool SmallButton(Rectangle r, const char* label, bool highlighted)
{
    const bool over = MouseOver(r);
    DrawRectangleRec(r, over ? Fade(ACCENT, 0.20f) : Fade(TITLE_BG, 0.8f));
    DrawRectangleLinesEx(r, 1.0f, highlighted ? ACCENT : PANEL_BORDER);
    Text(label, (int)r.x + 6, (int)r.y + 4, 12, highlighted ? ACCENT : TEXT);
    return over && MousePressed(MOUSE_BUTTON_LEFT);
}

}  // namespace Ui
