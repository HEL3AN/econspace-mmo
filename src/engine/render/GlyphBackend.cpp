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

// A surface's colour under a light sample: its own colour dimmed to the ambient floor and
// lifted back by however much light reaches it, then tinted by the colour of that light.
//
// The object keeps its own hue when unlit, which is the point of the ambient floor -- a
// game that is honestly black is a game nobody can read, and this is a space sim where
// most of the screen is far from any star.
Color Lit(Color base, const Lighting& lg, const Lighting::Sample& s)
{
    if (lg.Empty())
        return base;

    const float k = lg.ambient + (1.0f - lg.ambient) * s.strength;
    float       r = base.r * k, g = base.g * k, b = base.b * k;

    // The tint multiplies rather than replaces, so a red star reddens what it lights
    // without turning a blue hull red, and it fades out with the light itself.
    const float m = 0.6f * s.strength;
    r = r * (1.0f - m) + r * (s.tint.r / 255.0f) * m;
    g = g * (1.0f - m) + g * (s.tint.g / 255.0f) * m;
    b = b * (1.0f - m) + b * (s.tint.b / 255.0f) * m;

    return { (unsigned char)r, (unsigned char)g, (unsigned char)b, base.a };
}

bool HasDirection(const Lighting::Sample& s)
{
    return s.dir.x != 0.0f || s.dir.y != 0.0f;
}

// A thing that is itself a light is never shaded by one. Shading a star by the light it
// emits is how a sun ends up looking like a moon, and reading it off the item rather than
// off EntityKind means a beacon a player builds gets the same exemption for free.
bool Emissive(const Item& it)
{
    return it.lightRadius > 0.0f && it.lightIntensity > 0.0f;
}

// The arc where the light grazes a round silhouette. Cheap, and it does more for reading
// an object as solid than anything else here.
//
// Only for silhouettes that actually are round. A rim traced at the radius of a triangle
// is a crescent floating in empty space beside the ship, which is what it looked like the
// first time this was drawn. Shapes that are not discs get their rim when they get real
// silhouettes (#122).
void RimArc(Vector2 pos, float size, const Lighting& lg, const Lighting::Sample& s,
            float arcDegrees, float thickness)
{
    if (lg.Empty() || s.strength <= 0.01f || !HasDirection(s))
        return;
    const float a = std::atan2(s.dir.y, s.dir.x) * RAD2DEG;
    const Color rim{ s.tint.r, s.tint.g, s.tint.b,
                     (unsigned char)(210.0f * std::min(1.0f, s.strength)) };
    DrawRing(pos, size * (1.0f - thickness), size, a - arcDegrees, a + arcDegrees, 24, rim);
}

// A round body with a lit side. Three offset discs and a rim rather than a shader: #121
// replaces the drawing, and what #119 is for is knowing where the light is.
void ShadeDisc(Vector2 pos, float size, Color base, const Lighting& lg, const Lighting::Sample& s)
{
    if (lg.Empty() || !HasDirection(s) || s.strength <= 0.01f)
    {
        DrawCircleV(pos, size, Lit(base, lg, s));
        return;
    }

    Lighting::Sample unlit = s;
    unlit.strength = 0.0f;
    DrawCircleV(pos, size, Lit(base, lg, unlit));

    // Each step is smaller, brighter and more opaque than the last, so the lit side
    // arrives at the full brightness the light actually delivers and the terminator is a
    // ramp rather than an edge. Faint steps here were the first attempt, and they made a
    // lit planet darker overall than an unlit one -- which is worse than no lighting.
    for (int i = 1; i <= 3; i++)
    {
        const float      t = i / 4.0f;
        Lighting::Sample step = s;
        step.strength = s.strength * (0.45f + 0.55f * t);
        DrawCircleV({ pos.x + s.dir.x * size * 0.42f * t, pos.y + s.dir.y * size * 0.42f * t },
                    size * (1.0f - 0.55f * t), Fade(Lit(base, lg, step), 0.55f + 0.4f * t));
    }
}
}  // namespace

void GlyphBackend::Draw(const Item& item)
{
    Color c = Emissive(item) ? item.color : Lit(item.color, lighting_, lighting_.At(item.pos));
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

bool ShapeBackend::BeginMaterial(const Item& item, const Lighting::Sample& light)
{
    return BeginMaterialAt(item, light, item.pos, item.size);
}

bool ShapeBackend::BeginMaterialAt(const Item& item, const Lighting::Sample& light, Vector2 at,
                                   float size, Vector2 axis)
{
    if (materials_ == nullptr || item.material.empty())
        return false;
    const Material* m = Materials::Find(item.material);
    if (m == nullptr)
        return false;

    MaterialInputs in;
    in.item = &item;
    in.light = light;
    in.ambient = lighting_.ambient;
    in.time = (float)GetTime();
    in.screenPos = GetWorldToScreen2D(at, view_);
    // A radius in pixels, taken from the camera rather than assumed: the same object is
    // eight pixels across on the system map and four hundred in the gallery, and a shader
    // that guessed would be right in exactly one of those.
    in.screenSize = size * view_.zoom;
    // OpenGL counts gl_FragCoord from the bottom; raylib counts screen y from the top, and
    // so does everything in Lighting. Both conversions happen here, once, rather than in
    // every shader -- and they have to happen together: flipping the position without the
    // light direction lights the object from the mirror image of where the star is, which
    // is exactly what it did the first time this ran.
    in.screenPos.y = (float)GetScreenHeight() - in.screenPos.y;
    in.light.dir.y = -in.light.dir.y;
    in.axis = { axis.x, -axis.y };

    return materials_->Begin(*m, in);
}

void ShapeBackend::EndMaterial()
{
    if (materials_ != nullptr)
        materials_->End();
}

void ShapeBackend::Draw(const Item& item)
{
    // What the light does to this object, asked once. An object is small next to the
    // distance to its star, so one sample at its centre is the whole of the difference.
    const Lighting::Sample light = lighting_.At(item.pos);

    // A composition is shaded a part at a time (#135), so it never begins an object-level
    // material: one sphere at the object's centre is the truth about a planet and a lie
    // about a station, where an arm two radii out was being shaded by whatever slice of
    // that sphere it happened to be standing in.
    if (item.shape != nullptr && !item.shape->Empty())
    {
        if (item.ring > 0.0f)
            DrawCircleLines(0.0f, 0.0f, item.ring, DARKGRAY);
        DrawComposition(item, item.color, light);
        return;
    }

    // A material shades the fragments itself, so the colour handed to the primitive stays
    // the object's own and the CPU-side dimming is left off: doing both would darken twice.
    const bool shaded = BeginMaterial(item, light);

    Color c = (shaded || Emissive(item)) ? item.color : Lit(item.color, lighting_, light);
    if (!shaded && item.intensity < 1.0f)
        c = Fade(c, 0.35f + 0.65f * item.intensity);

    if (item.ring > 0.0f)
    {
        // The orbit guide is a property of where a thing is, not of the thing, so it is
        // never part of what a material shades.
        EndMaterial();
        DrawCircleLines(0.0f, 0.0f, item.ring, DARKGRAY);
        BeginMaterial(item, light);
    }

    // The switch is a separate call so that the material is always ended, whichever of
    // the ten returns below is taken. Leaving a shader bound is not a visible bug where it
    // happens -- it is a visible bug in whatever is drawn next.
    DrawShape(item, c, shaded, light);
    EndMaterial();
}

// A part's colour is its relationship to the object's, never a colour of its own. An
// object's colour belongs to the object (#117), and a shape written once has to work
// whatever colour it is given.
static Color ForRole(Role role, Color c)
{
    switch (role)
    {
        case Role::Hull: return c;
        case Role::Panel:
            return { (unsigned char)(c.r * 0.55f), (unsigned char)(c.g * 0.55f),
                     (unsigned char)(c.b * 0.55f), c.a };
        case Role::Trim:
            return { (unsigned char)(c.r + (255 - c.r) * 0.45f),
                     (unsigned char)(c.g + (255 - c.g) * 0.45f),
                     (unsigned char)(c.b + (255 - c.b) * 0.45f), c.a };
        case Role::Antenna: return Fade(c, 0.55f);
        case Role::Light: return c;  // handled apart: a light is not shaded at all
    }
    return c;
}

void ShapeBackend::DrawPiece(const Piece& p, Color c)
{
    const float a = p.angle * DEG2RAD;
    const float cs = std::cos(a), sn = std::sin(a);
    // Along the part's own axis, and across it. Every elongated form is written in these
    // two so that one rotation is applied in one place.
    const Vector2 along{ cs, sn };
    const Vector2 across{ -sn, cs };
    const Vector2 tip{ p.pos.x + along.x * p.length * 0.5f, p.pos.y + along.y * p.length * 0.5f };
    const Vector2 tail{ p.pos.x - along.x * p.length * 0.5f, p.pos.y - along.y * p.length * 0.5f };

    switch (p.form)
    {
        case Form::Disc: DrawCircleV(p.pos, p.radius, c); return;

        case Form::Ring: DrawRing(p.pos, p.radius - p.width, p.radius, 0.0f, 360.0f, 48, c); return;

        case Form::Polygon:
            if (p.filled)
                DrawPoly(p.pos, p.sides, p.radius, p.angle, c);
            else
                DrawPolyLinesEx(p.pos, p.sides, p.radius, p.angle, fmaxf(1.0f, p.width), c);
            return;

        case Form::Capsule:
            DrawLineEx(tail, tip, fmaxf(1.0f, p.width), c);
            DrawCircleV(tip, p.width * 0.5f, c);
            DrawCircleV(tail, p.width * 0.5f, c);
            return;

        case Form::Chevron:
            // Vertices counter-clockwise in screen space, which with y pointing down means
            // tip, then the far corner, then the near one. Wound the other way raylib
            // culls the triangle and the part simply is not there -- which is what every
            // ship looked like the first time this ran.
            DrawTriangle(
                tip, { tail.x - across.x * p.width * 0.5f, tail.y - across.y * p.width * 0.5f },
                { tail.x + across.x * p.width * 0.5f, tail.y + across.y * p.width * 0.5f }, c);
            return;

        case Form::Bar:
            DrawRectanglePro({ p.pos.x, p.pos.y, p.length, p.width },
                             { p.length * 0.5f, p.width * 0.5f }, p.angle, c);
            return;

        case Form::Lattice:
        {
            // A truss: the two rails, and struts crossing between them. Cheap, and it is
            // the one part that reads as "built" rather than "moulded".
            const Vector2 offs{ across.x * p.width * 0.5f, across.y * p.width * 0.5f };
            // Thickness from the truss's own width, like every other measurement in the
            // grammar. Fixed world units were sub-pixel the moment a card framed a larger
            // object, and the trusses simply stopped being drawn.
            const float rail = fmaxf(1.0f, p.width * 0.10f);
            const float strut = fmaxf(1.0f, p.width * 0.08f);
            DrawLineEx({ tail.x + offs.x, tail.y + offs.y }, { tip.x + offs.x, tip.y + offs.y },
                       rail, c);
            DrawLineEx({ tail.x - offs.x, tail.y - offs.y }, { tip.x - offs.x, tip.y - offs.y },
                       rail, c);
            for (int i = 0; i < p.count; i++)
            {
                const float   t0 = (float)i / (float)p.count;
                const float   t1 = (float)(i + 1) / (float)p.count;
                const Vector2 a0{ tail.x + (tip.x - tail.x) * t0 + offs.x,
                                  tail.y + (tip.y - tail.y) * t0 + offs.y };
                const Vector2 b0{ tail.x + (tip.x - tail.x) * t1 - offs.x,
                                  tail.y + (tip.y - tail.y) * t1 - offs.y };
                DrawLineEx(a0, b0, strut, c);
            }
            return;
        }
    }
}

bool ShapeBackend::DrawComposition(const Item& item, Color c, const Lighting::Sample& light)
{
    if (item.shape == nullptr || item.shape->Empty())
        return false;

    // The id seeds the variation, so two trade hubs differ and neither shimmers between
    // frames. An object with no id yet -- a gallery card, a placement ghost -- gets the
    // unperturbed shape, which is the right thing to be looking at while tuning one.
    Pose pose;
    pose.pos = item.pos;
    pose.size = item.size;
    pose.heading = item.heading;
    pose.seed = item.id;
    pose.pixelsPerUnit = view_.zoom;
    pose.time = (float)GetTime();
    pose.thrusting = item.thrusting;

    const std::vector<Piece> pieces = Compose(*item.shape, pose);

    for (const Piece& p : pieces)
    {
        // A lamp is not lit by the star, it is a light. Shading one is how a beacon ends
        // up dark on its own night side.
        if (p.role == Role::Light)
        {
            // A light dims with damage and with its own blink, and is never shaded: a lamp
            // is not lit by the star, it *is* a light, and shading one is how a beacon ends
            // up dark on its own night side.
            DrawPiece(p, Fade(item.color, (0.25f + 0.75f * item.intensity) * p.brightness));
            continue;
        }

        // Sampled where the piece is rather than where the object is. The difference is
        // nothing for a station beside a star and everything for a structure large enough
        // that its far side is meaningfully further away -- which is what players will
        // build (#44).
        const Lighting::Sample pieceLight = lighting_.At(p.pos);

        // Below a few pixels across there is no surface left to shade, and shading it
        // anyway is worse than not: a rail two pixels wide is *entirely* the part of a
        // cylinder that turns away from the viewer, so it comes out the darkest thing on
        // screen. The same argument as minPixels, one level down.
        const float shadePixels = ShadeRadius(p) * view_.zoom;
        const bool  shaded = shadePixels >= MIN_SHADED_PIXELS &&
                             BeginMaterialAt(item, pieceLight, p.pos, ShadeRadius(p), Axis(p));

        // With a shader the colour stays the object's own and the shading is done in the
        // fragment; without one it is dimmed here. Doing both would darken twice.
        const Color base = shaded ? c : Lit(c, lighting_, pieceLight);
        DrawPiece(p, ForRole(p.role, base));

        if (shaded)
            EndMaterial();
    }
    (void)light;
    return true;
}

void ShapeBackend::DrawShape(const Item& item, Color c, bool shaded, const Lighting::Sample& light)
{
    switch (item.kind)
    {
        case EntityKind::Star:
            DrawCircleV(item.pos, item.size * 1.8f, Fade(c, 0.15f));  // glow
            if (!Tex::DrawSprite(item.sprite.c_str(), item.pos, item.size, 0.0f, c))
                DrawCircleV(item.pos, item.size, c);
            return;

        case EntityKind::Planet:
        case EntityKind::Unknown:
            // Unknown is whatever a player invented; it is drawn exactly like a planet,
            // which is the whole argument for shading over drawing (#44).
            if (shaded)
            {
                // One plain disc. The shader decides which side is lit, where the
                // terminator falls and where the rim sits -- the offset circles below are
                // what this looked like before there was a shader to ask.
                DrawCircleV(item.pos, item.size, c);
            }
            else
            {
                if (!Tex::DrawSprite(item.sprite.c_str(), item.pos, item.size, 0.0f, c))
                    ShadeDisc(item.pos, item.size, item.color, lighting_, light);
                RimArc(item.pos, item.size, lighting_, light, RIM_ARC, RIM_THICKNESS);
            }
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
                                 item.heading * RAD2DEG + 90.0f, c))
            {
                DrawTriangle(toWorld(item.size, 0.0f),
                             toWorld(-item.size * 0.7f, -item.size * 0.6f),
                             toWorld(-item.size * 0.7f, item.size * 0.6f), c);
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
    }
}

}  // namespace Render
