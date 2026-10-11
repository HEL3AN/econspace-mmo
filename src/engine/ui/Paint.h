#pragma once

#include "raylib.h"
#include "ui/Fonts.h"
#include "ui/Theme.h"

#include <cstdint>
#include <string_view>

// The shapes a theme's renderer tokens draw (#297): cut and rounded corners, a glow, a shadow,
// dashes and stripes. Written once, here, and used by the window frame (Window.cpp) and the
// layout's widgets (Layout.cpp), so a theme that says "chamfer" gets the same chamfer on a
// window, a button and a tab. Everything is in screen pixels; the caller scales.
namespace Ui
{

// What a layout box asks of its painter beyond a fill and an outline (Box::Paint). Bits,
// because a lit tab in a glass theme is both cut and glowing.
enum PaintFlag : uint8_t
{
    PAINT_CHAMFER = 1 << 0,     // corners cut rather than rounded, by the box's radius
    PAINT_DASHED = 1 << 1,      // a fill drawn as dashes along its long side: a divider
    PAINT_STRIPED = 1 << 2,     // a fill drawn as segments across it: a bar
    PAINT_GLOW = 1 << 3,        // a soft halo in the fill's colour, by the theme's glow
    PAINT_FADE_RIGHT = 1 << 4,  // the fill fading out to the right: a lit row
    PAINT_FADE_UP = 1 << 5,     // the fill fading out upwards: a lit tab
    PAINT_RAISED = 1 << 6,      // a small shadow under it: a segmented tab's chosen plate
};

// A rectangle with its corners rounded, cut or left square. A cut takes the top-left and the
// bottom-right corner, as a stencilled plate does; a radius too large for the box is clamped.
void FillShape(Rectangle r, Corner corner, float radius, Color color);
void OutlineShape(Rectangle r, Corner corner, float radius, float width, Color color);
// The same shape filled from a texture that covers the whole screen, of `screen` pixels, at
// whatever size it is, upside down as a render texture is stored: the blurred world under a
// window.
void FillShapeFromScreen(Rectangle r, Corner corner, float radius, const Texture2D& texture,
                         Vector2 screen, Color tint);

// The top band of a window -- its title bar, its sheen -- with only its top-left corner cut
// (by `cut`; 0 for none), shaded from `from` to `to` across it (left to right) or down it.
void FillTopBand(Rectangle r, float cut, Color from, Color to, bool across);

// A fill faded out in one direction, from `color` to its own colour at no alpha.
void FillFadeRight(Rectangle r, Color color);
void FillFadeUp(Rectangle r, Color color);

// A halo around a shape in `color`, `spread` pixels wide, fading out. Drawn before the shape.
void DrawGlow(Rectangle r, Corner corner, float radius, Color color, float spread);
// A window's shadow, in the theme's style. Drawn before the window.
void DrawShadow(Rectangle r, Corner corner, float radius, Shadow shadow, float scale);

// Dashes along the long side of `r`, `dash` pixels on and `gap` off.
void DrawDashes(Rectangle r, Color color, float dash, float gap);
// Segments across `r`, left to right, `on` pixels wide with `off` between.
void DrawStripes(Rectangle r, Color color, float on, float off);

// Text with a soft halo of `glow` around it: a title in a glowing theme. With no glow (alpha
// 0) it is DrawString.
void DrawGlowString(Face face, std::string_view text, Vector2 pos, float px, Color color,
                    Color glow);

// What drawing made and kept (the soft shadow's texture). Before CloseWindow.
void UnloadPaint();

// A colour at a fraction of its alpha.
Color Faded(Color c, float alpha);

// --- The blurred world behind the windows -------------------------------------------------
// Set by whoever drew the world (the game, when the player has the blur on and the theme
// offers it), cleared at the end of the frame. While it is set, a window's panel is that
// texture under the theme's blurPanel instead of its panel colour.
struct Backdrop
{
    const Texture2D* texture = nullptr;     // the world, blurred, stretched over the screen
    Vector2          screen{ 0.0f, 0.0f };  // the screen's size in pixels
};
void            SetBackdrop(const Backdrop& b);
const Backdrop& CurrentBackdrop();

}  // namespace Ui
