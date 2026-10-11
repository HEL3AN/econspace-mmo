#include "ui/Paint.h"

#include "rlgl.h"

#include <algorithm>
#include <cmath>

namespace Ui
{

namespace
{
Backdrop g_backdrop;

// The soft shadow: a dark square blurred out to nothing, SHADOW_EDGE pixels each side of it,
// made the first time a window casts one.
constexpr int SHADOW_EDGE = 24;
Texture2D     g_shadow{};

const Texture2D& ShadowTexture()
{
    if (g_shadow.id == 0)
    {
        const int size = SHADOW_EDGE * 4;
        Image     image = GenImageColor(size, size, BLANK);
        ImageDrawRectangle(&image, SHADOW_EDGE, SHADOW_EDGE, size - 2 * SHADOW_EDGE,
                           size - 2 * SHADOW_EDGE, Color{ 0, 0, 0, 110 });
        ImageBlurGaussian(&image, SHADOW_EDGE / 2);
        g_shadow = LoadTextureFromImage(image);
        SetTextureFilter(g_shadow, TEXTURE_FILTER_BILINEAR);
        UnloadImage(image);
    }
    return g_shadow;
}

// The outline of a cut rectangle, counter-clockwise on the screen as raylib wants a fan:
// the top-left and bottom-right corners cut by `c`.
int ChamferPoints(Rectangle r, float c, Vector2 out[6])
{
    const float x0 = r.x, y0 = r.y, x1 = r.x + r.width, y1 = r.y + r.height;
    out[0] = { x0 + c, y0 };
    out[1] = { x0, y0 + c };
    out[2] = { x0, y1 };
    out[3] = { x1 - c, y1 };
    out[4] = { x1, y1 - c };
    out[5] = { x1, y0 };
    return 6;
}

float Clamp(Rectangle r, float radius)
{
    return std::max(0.0f, std::min(radius, 0.5f * std::min(r.width, r.height)));
}

Rectangle Grow(Rectangle r, float d)
{
    return { r.x - d, r.y - d, r.width + 2.0f * d, r.height + 2.0f * d };
}

// A convex outline as triangles from its first point, each sent as a quad with its last
// corner twice, as raylib's own fans are: a change of rlBegin's mode resets the batch's
// texture, so triangles would lose the one set for them.
template <typename V> void Fan(const Vector2* p, int n, unsigned int texture, V&& vertex)
{
    rlSetTexture(texture);
    rlBegin(RL_QUADS);
    for (int i = 1; i + 1 < n; i++)
    {
        vertex(p[0]);
        vertex(p[i]);
        vertex(p[i + 1]);
        vertex(p[i + 1]);
    }
    rlEnd();
    rlSetTexture(0);
}

void Fan(const Vector2* p, int n, Color color)
{
    const Texture2D white = GetShapesTexture();
    const Rectangle at = GetShapesTextureRectangle();
    Fan(p, n, white.id,
        [&](Vector2 v)
        {
            rlColor4ub(color.r, color.g, color.b, color.a);
            rlTexCoord2f(at.x / (float)white.width, at.y / (float)white.height);
            rlVertex2f(v.x, v.y);
        });
}
}  // namespace

Color Faded(Color c, float alpha)
{
    c.a = (unsigned char)std::clamp((float)c.a * alpha, 0.0f, 255.0f);
    return c;
}

void DrawMark(Vector2 centre, float radius, Mark mark, Color color)
{
    if (color.a == 0 || radius <= 0.0f)
        return;
    switch (mark)
    {
        case Mark::None: break;
        case Mark::Ring:
            DrawCircleV(centre, radius * 0.45f, color);
            DrawRing(centre, radius * 0.8f, radius, 0.0f, 360.0f, 24, color);
            break;
        case Mark::Circle: DrawCircleV(centre, radius * 0.85f, color); break;
        // A diamond and a triangle have less area than a circle the same size across; a
        // little larger, they read as the same weight.
        case Mark::Diamond: DrawPoly(centre, 4, radius, 0.0f, color); break;
        case Mark::Triangle:
            DrawPoly({ centre.x, centre.y + radius * 0.15f }, 3, radius * 1.1f, -90.0f, color);
            break;
        case Mark::Square:
        {
            const float h = radius * 0.75f;
            DrawRectangleV({ centre.x - h, centre.y - h }, { 2.0f * h, 2.0f * h }, color);
            break;
        }
    }
}

void DrawNotch(Vector2 centre, float r, float size, Color color)
{
    if (color.a == 0 || size <= 0.0f)
        return;
    // Its base on the bracket's top edge, its point into the bracket: counter-clockwise, as
    // raylib wants a triangle, with y down.
    const float y = centre.y - r;
    DrawTriangle({ centre.x - size, y }, { centre.x, y + size }, { centre.x + size, y }, color);
}

void FillShape(Rectangle r, Corner corner, float radius, Color color)
{
    if (color.a == 0 || r.width <= 0.0f || r.height <= 0.0f)
        return;
    const float c = Clamp(r, radius);
    if (corner == Corner::Square || c <= 0.0f)
        DrawRectangleRec(r, color);
    else if (corner == Corner::Rounded)
        DrawRectangleRounded(r, std::min(1.0f, 2.0f * c / std::min(r.width, r.height)), 6, color);
    else
    {
        Vector2 p[6];
        Fan(p, ChamferPoints(r, c, p), color);
    }
}

void OutlineShape(Rectangle r, Corner corner, float radius, float width, Color color)
{
    if (color.a == 0 || r.width <= 0.0f || r.height <= 0.0f)
        return;
    const float c = Clamp(r, radius);
    if (corner == Corner::Square || c <= 0.0f)
        DrawRectangleLinesEx(r, width, color);
    else if (corner == Corner::Rounded)
    {
        // raylib draws a rounded outline outside its rectangle; inset, it stays inside.
        const Rectangle in = Grow(r, -width);
        const float     s = std::max(1.0f, std::min(in.width, in.height));
        DrawRectangleRoundedLinesEx(in, std::min(1.0f, 2.0f * c / s), 6, width, color);
    }
    else
    {
        // Each side as a band inside the shape: the straight ones as rectangles, the cuts as
        // quads along the diagonal, so the corners meet without gaps or overlaps.
        Vector2 p[6];
        ChamferPoints(r, c, p);
        const float d = width * 0.41421356f;  // how far a 45-degree band's inner edge moves along
        // Top, right, bottom, left, in from the cut to the far corner.
        DrawRectangleRec({ r.x + c, r.y, r.width - c, width }, color);
        DrawRectangleRec({ r.x + r.width - width, r.y + width, width, r.height - c - width },
                         color);
        DrawRectangleRec({ r.x, r.y + r.height - width, r.width - c, width }, color);
        DrawRectangleRec({ r.x, r.y + c, width, r.height - c - width }, color);
        // The two cuts: the outer diagonal and the same one moved in by the width.
        const Vector2 tl[4] = {
            p[0], p[1], { p[1].x + width, p[1].y + d }, { p[0].x + d, p[0].y + width }
        };
        const Vector2 br[4] = {
            p[3], p[4], { p[4].x - width, p[4].y - d }, { p[3].x - d, p[3].y - width }
        };
        Fan(tl, 4, color);
        Fan(br, 4, color);
    }
}

void FillShapeFromScreen(Rectangle r, Corner corner, float radius, const Texture2D& texture,
                         Vector2 screen, Color tint)
{
    if (texture.id == 0 || r.width <= 0.0f || r.height <= 0.0f || screen.x <= 0.0f ||
        screen.y <= 0.0f)
        return;
    const float c = corner == Corner::Chamfer ? Clamp(r, radius) : 0.0f;
    Vector2     p[6];
    int         n = 0;
    if (c > 0.0f)
        n = ChamferPoints(r, c, p);
    else
    {
        // Rounded corners are left square here: the panel drawn over it is rounded, and the
        // few pixels of blur in each corner are the world's own colours, blurred.
        p[0] = { r.x, r.y };
        p[1] = { r.x, r.y + r.height };
        p[2] = { r.x + r.width, r.y + r.height };
        p[3] = { r.x + r.width, r.y };
        n = 4;
    }
    auto uv = [&](Vector2 v)
    {
        // A render texture is stored upside down.
        rlColor4ub(tint.r, tint.g, tint.b, tint.a);
        rlTexCoord2f(v.x / screen.x, 1.0f - v.y / screen.y);
        rlVertex2f(v.x, v.y);
    };
    Fan(p, n, texture.id, uv);
}

void FillTopBand(Rectangle r, float cut, Color from, Color to, bool across)
{
    if ((from.a == 0 && to.a == 0) || r.width <= 0.0f || r.height <= 0.0f)
        return;
    const float     c = Clamp(r, cut);
    const Vector2   p[5] = { { r.x + c, r.y },
                             { r.x, r.y + c },
                             { r.x, r.y + r.height },
                             { r.x + r.width, r.y + r.height },
                             { r.x + r.width, r.y } };
    const Texture2D shapes = GetShapesTexture();
    const Rectangle white = GetShapesTextureRectangle();
    auto            vertex = [&](Vector2 v)
    {
        // A linear shade is exact across a fan: each triangle interpolates it as it should.
        const float f = across ? (v.x - r.x) / r.width : (v.y - r.y) / r.height;
        auto        mix = [f](unsigned char a, unsigned char b)
        { return (unsigned char)std::lround((float)a + ((float)b - (float)a) * f); };
        rlColor4ub(mix(from.r, to.r), mix(from.g, to.g), mix(from.b, to.b), mix(from.a, to.a));
        rlTexCoord2f(white.x / (float)shapes.width, white.y / (float)shapes.height);
        rlVertex2f(v.x, v.y);
    };
    Fan(p, 5, GetShapesTexture().id, vertex);
}

void FillFadeRight(Rectangle r, Color color)
{
    DrawRectangleGradientH((int)r.x, (int)r.y, (int)r.width, (int)r.height, color,
                           Faded(color, 0.0f));
}

void FillFadeUp(Rectangle r, Color color)
{
    DrawRectangleGradientV((int)r.x, (int)r.y, (int)r.width, (int)r.height, Faded(color, 0.0f),
                           color);
}

void DrawGlow(Rectangle r, Corner corner, float radius, Color color, float spread)
{
    if (color.a == 0 || spread < 1.0f)
        return;
    // Rings of the shape, wider and fainter outwards; laid over each other they fall off
    // smoothly enough at the few pixels a glow is wide.
    constexpr int RINGS = 4;
    for (int i = RINGS; i >= 1; i--)
    {
        const float f = (float)i / (float)RINGS;
        FillShape(Grow(r, spread * f), corner, radius + spread * f,
                  Faded(color, 0.35f * (1.0f - f) + 0.08f));
    }
}

void DrawShadow(Rectangle r, Corner corner, float radius, Shadow shadow, float scale)
{
    if (shadow == Shadow::None)
        return;
    if (shadow == Shadow::Offset)
    {
        // A hard copy of the window, down and to the right: a plate standing off the screen.
        const float d = std::round(4.0f * scale);
        FillShape({ r.x + d, r.y + d, r.width, r.height }, corner, radius, Color{ 0, 0, 0, 128 });
        return;
    }
    // Soft: wide, faint and low, as light from above would cast it. One blurred square,
    // stretched by its middle: drawn as layers of the window's own shape, it was six fills the
    // size of the window, which a laptop's GPU paid two milliseconds a frame for at 1080p.
    const Texture2D& t = ShadowTexture();
    const float      spread = (float)SHADOW_EDGE * scale, drop = 8.0f * scale;
    const NPatchInfo n{ { 0.0f, 0.0f, (float)t.width, (float)t.height },
                        SHADOW_EDGE * 3 / 2,
                        SHADOW_EDGE * 3 / 2,
                        SHADOW_EDGE * 3 / 2,
                        SHADOW_EDGE * 3 / 2,
                        NPATCH_NINE_PATCH };
    (void)radius;  // a blur this wide hides the corner it came from
    DrawTextureNPatch(t, n, Grow({ r.x, r.y + drop, r.width, r.height }, spread), { 0.0f, 0.0f },
                      0.0f, WHITE);
}

void UnloadPaint()
{
    if (g_shadow.id != 0)
        UnloadTexture(g_shadow);
    g_shadow = {};
}

void DrawDashes(Rectangle r, Color color, float dash, float gap)
{
    if (color.a == 0 || dash < 1.0f)
        return;
    const bool  across = r.width >= r.height;
    const float length = across ? r.width : r.height;
    for (float at = 0.0f; at < length; at += dash + gap)
    {
        const float n = std::min(dash, length - at);
        DrawRectangleRec(across ? Rectangle{ r.x + at, r.y, n, r.height }
                                : Rectangle{ r.x, r.y + at, r.width, n },
                         color);
    }
}

void DrawStripes(Rectangle r, Color color, float on, float off)
{
    if (color.a == 0 || on < 1.0f)
        return;
    for (float at = 0.0f; at < r.width; at += on + off)
        DrawRectangleRec({ r.x + at, r.y, std::min(on, r.width - at), r.height }, color);
}

void DrawGlowString(Face face, std::string_view text, Vector2 pos, float px, Color color,
                    Color glow)
{
    if (glow.a > 0)
    {
        // The text itself, faint, a pixel or two out in eight directions: a halo the shape of
        // its letters, which a blurred rectangle would not be.
        const Color   halo = Faded(glow, 0.3f);
        const float   d = std::max(1.0f, std::round(px / 9.0f));
        const Vector2 at[8] = { { -d, 0 },  { d, 0 }, { 0, -d }, { 0, d },
                                { -d, -d }, { d, d }, { -d, d }, { d, -d } };
        for (const Vector2& o : at)
            DrawString(face, text, { pos.x + o.x, pos.y + o.y }, px, halo);
    }
    DrawString(face, text, pos, px, color);
}

void SetBackdrop(const Backdrop& b)
{
    g_backdrop = b;
}

const Backdrop& CurrentBackdrop()
{
    return g_backdrop;
}

}  // namespace Ui
