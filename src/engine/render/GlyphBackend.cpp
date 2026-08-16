#include "render/GlyphBackend.h"

#include "render/Textures.h"
#include "ui/UiTheme.h"
#include <cmath>

namespace Render
{

namespace
{
// Draws one glyph centred on a world point.
void GlyphAt(const char* glyph, Vector2 centre, float height, Color c)
{
    Font    font = Ui::GetFont();
    Vector2 extent = MeasureTextEx(font, glyph, height, 0.0f);
    DrawTextEx(font, glyph, { centre.x - extent.x * 0.5f, centre.y - extent.y * 0.5f }, height,
               0.0f, c);
}
}  // namespace

void GlyphBackend::Draw(const Item& item)
{
    Color c = item.color;
    if (item.intensity < 1.0f)
        c = Fade(c, 0.35f + 0.65f * item.intensity);

    // A planet's orbit is a property of where it is, not of what it is, so it stays a
    // line even in a character presentation — without it the map loses its structure.
    if (item.ring > 0.0f)
        DrawCircleLines(0.0f, 0.0f, item.ring, Fade(c, 0.18f));

    switch (item.style)
    {
        case GlyphStyle::Region: DrawRegion(item, c); return;
        case GlyphStyle::Directional: DrawDirectional(item, c); return;
        case GlyphStyle::Point: DrawPoint(item, c); return;
    }
}

void GlyphBackend::DrawPoint(const Item& item, Color c)
{
    const float height = item.size * 2.0f * SCALE;
    if (height < MIN_PIXELS)
    {
        DrawPixelV(item.pos, c);
        return;
    }
    GlyphAt(item.glyph.empty() ? "?" : item.glyph.c_str(), item.pos, height, c);
}

// A nebula or a belt is an area, not an object. Drawing it as one character scaled to
// three thousand units would put a `~` across the whole screen and hide everything
// inside it; a ring of small marks says "this extends to here" and stays see-through.
void GlyphBackend::DrawRegion(const Item& item, Color c)
{
    const char* glyph = item.glyph.empty() ? "?" : item.glyph.c_str();
    const float height = item.size * REGION_GLYPH;
    if (height < MIN_PIXELS)
    {
        DrawPixelV(item.pos, c);
        return;
    }

    for (int i = 0; i < REGION_MARKS; i++)
    {
        const float angle = (float)i * (2.0f * PI / (float)REGION_MARKS);
        GlyphAt(
            glyph,
            { item.pos.x + std::cos(angle) * item.size, item.pos.y + std::sin(angle) * item.size },
            height, c);
    }
    // A sparse interior, so a region reads as filled rather than as a ring of debris.
    // The arrangement is a fixed function of the index: a belt that shimmered between
    // frames would look like motion where there is none.
    for (int i = 0; i < 5; i++)
    {
        const float angle = (float)i * 2.39996f;
        const float r = item.size * (0.25f + 0.5f * (float)((i * 7) % 11) / 11.0f);
        GlyphAt(glyph, { item.pos.x + std::cos(angle) * r, item.pos.y + std::sin(angle) * r },
                height, Fade(c, 0.55f));
    }
}

// A ship's heading is the most useful thing about it at a glance — whether it is coming
// at you. The glyph turns rather than a separate marker being drawn beside it.
void GlyphBackend::DrawDirectional(const Item& item, Color c)
{
    const float height = item.size * 2.0f * SCALE;
    if (height < MIN_PIXELS)
    {
        DrawPixelV(item.pos, c);
        return;
    }

    const char* glyph = item.glyph.empty() ? "?" : item.glyph.c_str();
    Font        font = Ui::GetFont();
    Vector2     extent = MeasureTextEx(font, glyph, height, 0.0f);
    // The glyph is drawn nose-up, as the ship sprites are, so the heading gets a quarter
    // turn added. Origin at the glyph's centre so it spins in place.
    DrawTextPro(font, glyph, item.pos, { extent.x * 0.5f, extent.y * 0.5f },
                item.heading * RAD2DEG + 90.0f, height, 0.0f, c);

    // Hull bar over a damaged ship, unrotated so it stays readable.
    if (item.intensity < 1.0f)
    {
        const float barW = item.size * 2.0f;
        Vector2     barPos = { item.pos.x - barW / 2.0f, item.pos.y - item.size - 8.0f };
        DrawRectangleV(barPos, { barW, 3.0f }, Fade(GRAY, 0.5f));
        DrawRectangleV(barPos, { barW * item.intensity, 3.0f }, RED);
    }
}

void ShapeBackend::Draw(const Item& item)
{
    Color c = item.color;
    if (item.intensity < 1.0f)
        c = Fade(c, 0.35f + 0.65f * item.intensity);

    if (item.ring > 0.0f)
        DrawCircleLines(0.0f, 0.0f, item.ring, DARKGRAY);

    switch (item.kind)
    {
        case EntityKind::Star:
            DrawCircleV(item.pos, item.size * 1.8f, Fade(c, 0.15f));  // glow
            if (!Tex::DrawSprite(item.sprite.c_str(), item.pos, item.size, 0.0f, c))
                DrawCircleV(item.pos, item.size, c);
            return;

        case EntityKind::Planet:
            if (!Tex::DrawSprite(item.sprite.c_str(), item.pos, item.size, 0.0f, c))
                DrawCircleV(item.pos, item.size, c);
            return;

        case EntityKind::Station:
            if (Tex::DrawSprite(item.sprite.c_str(), item.pos, item.size, 0.0f, WHITE))
                return;
            DrawPolyLines(item.pos, 6, item.size, 0.0f, c);
            DrawPolyLines(item.pos, 6, item.size * 0.6f, 30.0f, Fade(c, 0.6f));
            DrawCircleV(item.pos, 3.0f, c);
            return;

        case EntityKind::Field:
        {
            DrawCircleLines(item.pos.x, item.pos.y, item.size, Fade(GRAY, 0.35f));
            if (Tex::DrawSprite(item.sprite.c_str(), item.pos, item.size, 0.0f,
                                Fade(WHITE, 0.35f + 0.65f * item.intensity)))
                return;
            // A scatter of rocks, thinning out as the belt is worked. The arrangement is
            // a fixed function of the index so a belt does not shimmer between frames.
            for (int i = 0; i < 16; i++)
            {
                float   angle = i * 2.39996f;
                float   radius = item.size * (0.2f + 0.65f * ((i * 7 % 11) / 11.0f));
                Vector2 p = { item.pos.x + std::cos(angle) * radius,
                              item.pos.y + std::sin(angle) * radius };
                DrawCircleV(p, 3.0f + (i % 3), c);
            }
            return;
        }

        case EntityKind::Gate:
            if (Tex::DrawSprite(item.sprite.c_str(), item.pos, item.size, 0.0f, WHITE))
                return;
            DrawCircleV(item.pos, item.size * 0.6f, Fade(c, 0.15f));
            DrawCircleLines(item.pos.x, item.pos.y, item.size, c);
            DrawCircleLines(item.pos.x, item.pos.y, item.size * 0.85f, Fade(c, 0.6f));
            return;

        case EntityKind::Nebula:
            DrawCircleV(item.pos, item.size, Fade(c, 0.08f));
            DrawCircleV(item.pos, item.size * 0.7f, Fade(c, 0.08f));
            DrawCircleV(item.pos, item.size * 0.4f, Fade(c, 0.10f));
            DrawCircleLines(item.pos.x, item.pos.y, item.size, Fade(c, 0.35f));
            return;

        case EntityKind::Derelict:
        {
            if (Tex::DrawSprite(item.sprite.c_str(), item.pos, item.size, 0.0f, c))
                return;
            // A diamond hull with one side broken open.
            Vector2 top{ item.pos.x, item.pos.y - item.size };
            Vector2 bot{ item.pos.x, item.pos.y + item.size };
            Vector2 left{ item.pos.x - item.size * 0.7f, item.pos.y };
            Vector2 right{ item.pos.x + item.size * 0.7f, item.pos.y };
            DrawLineEx(top, right, 2.0f, c);
            DrawLineEx(right, bot, 2.0f, c);
            DrawLineEx(bot, left, 2.0f, Fade(c, 0.4f));
            DrawLineEx(left, top, 2.0f, c);
            return;
        }

        case EntityKind::Npc:
        case EntityKind::PlayerShip:
        {
            const float cs = std::cos(item.heading);
            const float sn = std::sin(item.heading);
            auto        toWorld = [&](float x, float y) -> Vector2
            { return { item.pos.x + x * cs - y * sn, item.pos.y + x * sn + y * cs }; };

            if (item.thrusting)
                DrawLineEx(toWorld(-item.size * 0.7f, 0.0f), toWorld(-item.size * 1.5f, 0.0f), 4.0f,
                           ORANGE);

            // The sprite is drawn nose-up, so the heading gets a quarter turn added.
            if (!Tex::DrawSprite(item.sprite.c_str(), item.pos, item.size,
                                 item.heading * RAD2DEG + 90.0f, item.color))
            {
                DrawTriangle(toWorld(item.size, 0.0f),
                             toWorld(-item.size * 0.7f, -item.size * 0.6f),
                             toWorld(-item.size * 0.7f, item.size * 0.6f), item.color);
            }

            // Hull bar over a damaged ship. Not drawn for an intact one — a bar over
            // every ship in the system is noise.
            if (item.intensity < 1.0f)
            {
                float   barW = item.size * 2.0f;
                Vector2 barPos = { item.pos.x - barW / 2.0f, item.pos.y - item.size - 8.0f };
                DrawRectangleV(barPos, { barW, 3.0f }, Fade(GRAY, 0.5f));
                DrawRectangleV(barPos, { barW * item.intensity, 3.0f }, RED);
            }
            return;
        }

        case EntityKind::Unknown: DrawCircleV(item.pos, item.size, c); return;
    }
}

}  // namespace Render
