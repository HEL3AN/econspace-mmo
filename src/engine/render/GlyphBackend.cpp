#include "render/GlyphBackend.h"

#include "render/GlState.h"
#include "render/Perf.h"
#include "render/Textures.h"
#include "rlgl.h"
#include "ui/UiTheme.h"
#include <cmath>
#include <utility>

namespace Render
{

namespace
{
// Draws one glyph centred on a world point.
void GlyphAt(const char* glyph, Vector2 centre, float height, Color c)
{
    const Font& font = Ui::GlyphFont();
    Vector2     extent = MeasureTextEx(font, glyph, height, 0.0f);
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
    // It is the surface's share of the light's colour, capped by Lighting (#361), so a
    // star warms a hull and never makes it the star's colour.
    r = r * (1.0f - m) + r * (s.surface.r / 255.0f) * m;
    g = g * (1.0f - m) + g * (s.surface.g / 255.0f) * m;
    b = b * (1.0f - m) + b * (s.surface.b / 255.0f) * m;

    return { (unsigned char)r, (unsigned char)g, (unsigned char)b, base.a };
}

bool HasDirection(const Lighting::Sample& s)
{
    return s.dir.x != 0.0f || s.dir.y != 0.0f;
}

// A thing whose light lights itself is never shaded by one. Shading a star by the light it
// emits is how a sun ends up looking like a moon, and reading it off the item rather than
// off EntityKind means anything a player builds can say the same in data. A light that
// does not light itself -- the default, a beacon -- leaves its hull a surface like any
// other, lit by every light but its own (#119); its `light` parts still glow.
bool Emissive(const Item& it)
{
    return it.lightRadius > 0.0f && it.lightIntensity > 0.0f && it.lightSelf;
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
    Color c = Emissive(item)
                  ? item.color
                  : Lit(item.color, lighting_, lighting_.At(item.pos, lighting_.OwnLight(item)));
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
    const Font& font = Ui::GlyphFont();
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

bool ShapeBackend::Drawable(const std::string& id) const
{
    return materials_ != nullptr && materials_->Has(id);
}

bool ShapeBackend::BeginMaterialAt(const Item& item, const Lighting::Sample& light, Vector2 at,
                                   float size, Vector2 axis, const Material* own,
                                   const Piece* piece)
{
    if (materials_ == nullptr || (own == nullptr && item.material.empty()))
        return false;
    const Material* m =
        own != nullptr ? own
                       : Materials::Pick("", item.material, "hull",
                                         [this](const std::string& id) { return Drawable(id); });
    if (m == nullptr)
        return false;

    MaterialInputs in;
    in.item = &item;
    in.light = light;
    in.ambient = lighting_.ambient;
    in.time = (float)std::fmod(clock_, SHADER_TIME_WRAP);
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
    // Painting at reduced resolution: the fragments are fewer, and where they are scales.
    in.screenPos.x *= paintScale_;
    in.screenPos.y *= paintScale_;
    in.screenSize *= paintScale_;
    in.axis = { axis.x, -axis.y };
    // The whole object, for a part that needs to know where on it it is (#516).
    Vector2 centre = GetWorldToScreen2D(item.pos, view_);
    centre.y = (float)GetScreenHeight() - centre.y;
    in.frame[0] = centre.x * paintScale_;
    in.frame[1] = centre.y * paintScale_;
    in.frame[2] = item.size * view_.zoom * paintScale_;
    in.frame[3] = -item.heading;
    in.livery = Materials::FindLivery(item.livery);
    // A part's own inputs only reach a material that reads them. One that does not -- the
    // hull under a planet's craters -- then has the same inputs for every piece of a body
    // and keeps one bind for all of them (#296).
    if (piece != nullptr && m->readsPart)
    {
        in.partSeed = piece->seed;
        // A piece's angle is measured the way raylib's y runs, downward; turned into
        // OpenGL's sense with the rest, so a pattern turns the same way as its outline.
        in.angle = -piece->angle * DEG2RAD;
        const float   object = item.size > 0.0f ? item.size : 1.0f;
        const Vector2 run = Axis(*piece);
        const bool    elongated = run.x != 0.0f || run.y != 0.0f;
        // A ring has no length: what a pattern on it needs is how thick its band is.
        const bool band = piece->form == Form::Ring || piece->form == Form::Arc;
        in.partShape[0] = (elongated ? piece->length * 0.5f
                           : band    ? piece->width
                                     : piece->radius) /
                          object;
        in.partShape[1] = ShadeRadius(*piece) / object;
        in.partShape[2] = (float)piece->sides;
        in.partShape[3] = (float)(int)piece->form;
    }

    // Already bound with exactly these inputs: keep it (#296). Every mark on a planet is
    // shaded as the planet -- the body's centre, radius and light -- so a crater field was
    // a shader bound, a crater drawn and the shader unbound, forty times over, each one a
    // draw call of its own. Uniforms are the same, so the pieces may share one.
    const bool same =
        bound_ && boundMaterial_ == m && boundItem_ == &item && boundAt_.x == in.screenPos.x &&
        boundAt_.y == in.screenPos.y && boundSize_ == in.screenSize && boundAxis_.x == in.axis.x &&
        boundAxis_.y == in.axis.y && boundSeed_ == in.partSeed && boundAngle_ == in.angle &&
        boundShape_[0] == in.partShape[0] && boundShape_[1] == in.partShape[1] &&
        boundShape_[2] == in.partShape[2] && boundShape_[3] == in.partShape[3] &&
        boundLight_.dir.x == in.light.dir.x && boundLight_.dir.y == in.light.dir.y &&
        boundLight_.strength == in.light.strength && ColorIsEqual(boundLight_.tint, in.light.tint);
    if (same)
        return true;
    EndMaterial();

    if (!materials_->Begin(*m, in))
        return false;
    Perf::Count(Perf::Counter::Binds);
    bound_ = true;
    boundMaterial_ = m;
    boundItem_ = &item;
    boundAt_ = in.screenPos;
    boundSize_ = in.screenSize;
    boundAxis_ = in.axis;
    boundSeed_ = in.partSeed;
    boundAngle_ = in.angle;
    for (int k = 0; k < 4; k++)
        boundShape_[k] = in.partShape[k];
    boundLight_ = in.light;
    return true;
}

void ShapeBackend::EndMaterial()
{
    bound_ = false;
    if (materials_ != nullptr)
        materials_->End();
}

void ShapeBackend::Draw(const Item& described)
{
    // A ship is never drawn smaller than it can be found (#426); everything below, its
    // composition and its culling, works on the size it is shown at.
    const float shown = ShownSize(described.kind, described.size, view_.zoom);
    // A livery's base colour is the colour the object is painted (#516): applied here, to
    // the object's colour, so that it holds with a shader and without one alike (#120).
    const Livery* livery =
        described.livery.empty() ? nullptr : Materials::FindLivery(described.livery);
    const bool painted = livery != nullptr && livery->id == described.livery;
    Item       enlarged;
    if (shown != described.size || painted)
    {
        enlarged = described;
        enlarged.size = shown;
        if (painted)
            enlarged.color = { (unsigned char)(described.color.r * livery->base.r / 255),
                               (unsigned char)(described.color.g * livery->base.g / 255),
                               (unsigned char)(described.color.b * livery->base.b / 255),
                               described.color.a };
    }
    const Item& item = (shown != described.size || painted) ? enlarged : described;

    Perf::Count(Perf::Counter::Items);
    // What the light does to this object, asked once. An object is small next to the
    // distance to its star, so one sample at its centre is the whole of the difference.
    own_ = lighting_.OwnLight(item);
    const Lighting::Sample light = LightAt(item.pos);

    // A composition is shaded a part at a time (#135), so it never begins an object-level
    // material: one sphere at the object's centre is the truth about a planet and a lie
    // about a station, where an arm two radii out was being shaded by whatever slice of
    // that sphere it happened to be standing in.
    if (item.shape != nullptr && !item.shape->Empty())
    {
        if (item.ring > 0.0f)
            DrawOrbitGuide(item);
        // Wholly off screen, it is neither composed nor drawn (#296). Most of a system is
        // outside the window at any zoom worth flying at, and every one of those objects was
        // being assembled piece by piece to be clipped by the GPU.
        if (OffScreen(item.pos, item.size * Reach(*item.shape, item.id)))
        {
            Perf::Count(Perf::Counter::Culled);
            return;
        }
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
        DrawOrbitGuide(item);
        BeginMaterial(item, light);
    }

    // The switch is a separate call so that the material is always ended, whichever of
    // the ten returns below is taken. Leaving a shader bound is not a visible bug where it
    // happens -- it is a visible bug in whatever is drawn next.
    DrawShape(item, c, shaded, light);
    EndMaterial();
}

int ShapeBackend::OutlineSegments(const Piece& p) const
{
    // A chord of a circle of r pixels, n to the turn, falls short of the arc by about
    // r * (pi / n)^2 / 2. Kept under a third of a pixel, which no one can see: a crater ten
    // pixels across needs ten corners, and a storm three hundred across needs a hundred --
    // capped at thirty-six it was a polygon with sides you could count (#296, #426).
    const float r = std::fabs(p.radius) * view_.zoom;
    if (r <= 0.0f)
        return 8;
    const float n = PI / std::sqrt(2.0f * OUTLINE_SAG_PIXELS / r);
    return (int)std::fmin(256.0f, std::fmax(8.0f, std::ceil(n)));
}

int ShapeBackend::SidesOnScreen(float radius) const
{
    return BodySides(std::fabs(radius) * view_.zoom);
}

void ShapeBackend::DrawOrbitGuide(const Item& item) const
{
    // Where a planet is on the map, not part of the planet: it says so while the planet is
    // a mark on a system and is gone by the time the planet is what you are looking at. An
    // orbit runs through the middle of its planet, and zoomed in on one it was a thin line
    // across the screen and across the planet that read as a route nobody had set (#426).
    const float shown = item.size * view_.zoom;
    const float t =
        std::fmin(1.0f, std::fmax(0.0f, (shown - ORBIT_GUIDE_FULL_PIXELS) /
                                            (ORBIT_GUIDE_GONE_PIXELS - ORBIT_GUIDE_FULL_PIXELS)));
    const float keep = 1.0f - t * t * (3.0f - 2.0f * t);
    if (keep <= 0.01f)
        return;
    DrawGuideCircle({ 0.0f, 0.0f }, item.ring, view_, Fade(DARKGRAY, keep));
}

bool ShapeBackend::OffScreen(Vector2 pos, float reach) const
{
    // The target being drawn into, which is the screen, a shot's texture or the treatment's.
    const float   w = (float)rlGetFramebufferWidth(), h = (float)rlGetFramebufferHeight();
    const Vector2 s = GetWorldToScreen2D(pos, view_);
    // Lines are at least a pixel wide whatever the zoom, so a margin of a few pixels.
    const float r = reach * view_.zoom + CULL_MARGIN_PIXELS;
    return s.x + r < 0.0f || s.x - r > w || s.y + r < 0.0f || s.y - r > h;
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

// A triangle in whichever winding raylib will keep. It culls clockwise triangles in screen
// space, and a projected shape -- an ellipse turned to face the limb, a band traced column by
// column -- does not know in advance which way round its corners come out.
static void DrawTriangleAnyWay(Vector2 a, Vector2 b, Vector2 c, Color col)
{
    const float cross = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (cross > 0.0f)
        DrawTriangle(a, c, b, col);
    else
        DrawTriangle(a, b, c, col);
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
        case Form::Disc:
            if (p.surface)
            {
                Perf::Scope timed(Perf::Section::Surface);
                Perf::Count(Perf::Counter::Surface);
                // Cut to the body's disc, so a feature at the limb ends at the limb rather
                // than bulging past it; the piece's centre is always inside what is left.
                const std::vector<Vector2> outline =
                    SurfaceOutline(p, OutlineSegments(p), SidesOnScreen(p.bodyRadius));
                const Vector2 from = SurfaceFanCentre(p, outline);
                for (size_t k = 0; k < outline.size(); k++)
                    DrawTriangleAnyWay(from, outline[k], outline[(k + 1) % outline.size()], c);
                return;
            }
            // As many sides as its size on screen needs, and the same number the marks on
            // it are cut to (#426).
            DrawCircleSector(p.pos, p.radius, 0.0f, 360.0f, SidesOnScreen(p.radius), c);
            return;

        case Form::Band:
            // Already projected: quads, four corners each, traced across the face.
            for (size_t k = 0; k + 3 < p.strip.size(); k += 4)
            {
                DrawTriangleAnyWay(p.strip[k], p.strip[k + 1], p.strip[k + 2], c);
                DrawTriangleAnyWay(p.strip[k], p.strip[k + 2], p.strip[k + 3], c);
            }
            return;

        case Form::Ring:
            DrawRing(p.pos, p.radius - p.width, p.radius, 0.0f, 360.0f, SidesOnScreen(p.radius), c);
            return;

        case Form::Arc:
        {
            // Segments by how much of the circle it covers, so a sliver is not drawn with
            // as many as a whole ring.
            const float span = std::fabs(p.arcTo - p.arcFrom);
            const int segments = std::max(4, (int)((float)SidesOnScreen(p.radius) * span / 360.0f));
            DrawRing(p.pos, p.radius - p.width, p.radius, p.arcFrom, p.arcTo, segments, c);
            return;
        }

        case Form::Polygon:
            if (p.jagged > 0.0f)
            {
                // Each corner pulled in by its own amount, from the piece's seed, so a rock
                // keeps its outline from frame to frame and differs from the next rock.
                const int            n = std::max(3, p.sides);
                std::vector<Vector2> pts((size_t)n);
                for (int k = 0; k < n; k++)
                {
                    const unsigned h = (unsigned)p.jagSeed * 2654435761u + (unsigned)k * 40503u;
                    const float    pull = (float)((h >> 8) % 1000u) / 1000.0f;
                    const float    r = p.radius * (1.0f - p.jagged * pull);
                    const float    t = (p.angle + 360.0f * (float)k / (float)n) * DEG2RAD;
                    pts[(size_t)k] = { p.pos.x + std::cos(t) * r, p.pos.y + std::sin(t) * r };
                }
                for (int k = 0; k < n; k++)
                {
                    const Vector2 a = pts[(size_t)k], b = pts[(size_t)((k + 1) % n)];
                    if (p.filled)
                        DrawTriangleAnyWay(p.pos, a, b, c);
                    else
                        DrawLineEx(a, b, fmaxf(1.0f, p.width), c);
                }
                return;
            }
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
            if (p.tip > 0.0f)
            {
                // A trapezoid: the narrow end at the tip, the base at the tail.
                const float   hb = p.width * 0.5f, ht = p.width * 0.5f * p.tip;
                const Vector2 t0{ tip.x - across.x * ht, tip.y - across.y * ht };
                const Vector2 t1{ tip.x + across.x * ht, tip.y + across.y * ht };
                const Vector2 b0{ tail.x - across.x * hb, tail.y - across.y * hb };
                const Vector2 b1{ tail.x + across.x * hb, tail.y + across.y * hb };
                DrawTriangleAnyWay(t0, b0, b1, c);
                DrawTriangleAnyWay(t0, b1, t1, c);
                return;
            }
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

void ShapeBackend::DrawSoftOnSurface(const Item& item, const Piece& p, bool emissive)
{
    Perf::Scope timed(Perf::Section::Soft);
    Perf::Count(Perf::Counter::Soft);
    Perf::Count(Perf::Counter::Surface);
    const std::vector<Vector2> outline =
        SurfaceOutline(p, OutlineSegments(p), SidesOnScreen(p.bodyRadius));
    if (outline.empty())
        return;

    // Lit as the body, like every other mark on it: by the material with the body's centre
    // and radius where there is one, and otherwise by how squarely the body faces the light
    // under the piece -- so a storm's glow goes dark across the terminator instead of
    // shining on the night side. A light, or a part of something that is one, is not lit.
    const Color own = p.tint.a > 0 ? p.tint : item.color;
    Color       base = own;
    bool        shaded = false;
    if (p.role != Role::Light && !emissive)
    {
        Lighting::Sample l = LightAt(p.bodyPos);
        shaded = p.bodyRadius * view_.zoom >= MIN_SHADED_PIXELS &&
                 BeginMaterialAt(item, l, p.bodyPos, p.bodyRadius, Vector2{ 0.0f, 0.0f });
        if (!shaded)
        {
            EndMaterial();
            const float r = p.bodyRadius > 0.0f ? p.bodyRadius : 1.0f;
            l.strength *=
                SurfaceFacing({ (p.pos.x - p.bodyPos.x) / r, (p.pos.y - p.bodyPos.y) / r }, l.dir);
            base = Lit(own, lighting_, l);
        }
    }
    const Color mid = Fade(ForRole(p.role, base), p.brightness);

    // A fan from the centre, each corner as transparent as it is far out along the ellipse:
    // nothing on the rim, and where the limb cut it, whatever the glow had left there.
    auto corner = [&](Vector2 v)
    {
        const float k = 1.0f - EllipseReach(p, v);
        rlColor4ub(mid.r, mid.g, mid.b, (unsigned char)((float)mid.a * k));
        rlVertex2f(v.x, v.y);
    };
    const Vector2 from = SurfaceFanCentre(p, outline);
    rlBegin(RL_TRIANGLES);
    for (size_t k = 0; k < outline.size(); k++)
    {
        Vector2     a = outline[k], b = outline[(k + 1) % outline.size()];
        const float cross = (a.x - from.x) * (b.y - from.y) - (a.y - from.y) * (b.x - from.x);
        if (cross > 0.0f)
            std::swap(a, b);  // the winding raylib keeps, as DrawTriangleAnyWay
        corner(from);
        corner(a);
        corner(b);
    }
    rlEnd();
    // Left bound if it was: the next mark on this body is very likely shaded the same way,
    // and whoever draws something else ends it (see BeginMaterialAt).
}

const Material* ShapeBackend::OwnMaterial(const Piece& p) const
{
    if (p.material.empty() || materials_ == nullptr)
        return nullptr;
    // Down the material's fallback chain (#516): a radiator whose shader failed is drawn as
    // the plating it falls back to, and that as the hull.
    const Material* m =
        Materials::Pick(p.material, "", "", [this](const std::string& id) { return Drawable(id); });
    if (m == nullptr)
        return nullptr;
    // Below a few pixels there is no surface to paint, and the plain colour of the part is a
    // truer picture of a dot than a shader working on nine fragments.
    const float r = p.surface ? p.bodyRadius : ShadeRadius(p);
    if (r * view_.zoom < MIN_SHADED_PIXELS)
        return nullptr;
    return m;
}

bool ShapeBackend::DrawOwnMaterial(const Item& item, const Piece& p, const Material& m)
{
    // How much of the target the body covers, and so whether it can be painted at full
    // resolution. Only a round body is painted scaled: it is the one that grows past the
    // screen -- a planet flown over, a nebula flown into.
    if (p.form == Form::Disc && !p.surface && paintScale_ == 1.0f)
    {
        const float     w = (float)rlGetFramebufferWidth(), h = (float)rlGetFramebufferHeight();
        const Vector2   c = GetWorldToScreen2D(p.pos, view_);
        const float     reach = p.radius * m.reach * view_.zoom + 2.0f;
        const Rectangle box{ std::fmax(0.0f, c.x - reach), std::fmax(0.0f, c.y - reach), 0.0f,
                             0.0f };
        const float     x1 = std::fmin(w, c.x + reach), y1 = std::fmin(h, c.y + reach);
        const Rectangle clip{ box.x, box.y, std::fmax(0.0f, x1 - box.x),
                              std::fmax(0.0f, y1 - box.y) };
        const float     area = clip.width * clip.height;
        const float     budget = materials_ != nullptr ? materials_->PaintBudget() : area;
        if (area > budget)
        {
            // In eighths, so the target is not resized every frame as a planet grows.
            const float scale =
                std::fmax(PAINT_MIN_SCALE, std::floor(std::sqrt(budget / area) * 8.0f) / 8.0f);
            if (scale < 1.0f && PaintScaled(item, p, m, scale, clip))
                return true;
        }
    }

    const Vector2          at = p.surface ? p.bodyPos : p.pos;
    const float            r = p.surface ? p.bodyRadius : ShadeRadius(p);
    const Lighting::Sample l = LightAt(at);
    if (!BeginMaterialAt(item, l, at, r, p.surface ? Vector2{ 0.0f, 0.0f } : Axis(p), &m, &p))
        return false;
    Perf::Count(Perf::Counter::Painted);
    DrawPainted(item, p, m);
    return true;
}

void ShapeBackend::DrawPainted(const Item& item, const Piece& p, const Material& m)
{
    // What the shader starts from: the part's colour after its role, and its solidity -- a
    // moon dims on the far half of its lap. The shading, all of it, is the shader's.
    const Color own = p.tint.a > 0 ? p.tint : item.color;
    const Color col = Fade(ForRole(p.role, own), p.brightness);
    if (p.form == Form::Disc && !p.surface)
    {
        // Over the material's whole reach, and a little more: raylib's circle is a polygon
        // inscribed in the true one, and the shader -- not the polygon -- is what decides
        // where the limb is, smoothly, to the pixel.
        const float reach = p.radius * m.reach / 0.995f + 2.0f / view_.zoom;
        DrawCircleV(p.pos, reach, col);
        return;
    }
    DrawPiece(p, col);
}

bool ShapeBackend::PaintScaled(const Item& item, const Piece& p, const Material& m, float scale,
                               Rectangle box)
{
    if (materials_ == nullptr)
        return false;
    const int        w = rlGetFramebufferWidth(), h = rlGetFramebufferHeight();
    RenderTexture2D* rt = materials_->PaintTarget(w, h);
    if (rt == nullptr)
        return false;

    // Everything drawn so far goes to the target it was meant for before the target changes.
    EndMaterial();
    rlDrawRenderBatchActive();
    const unsigned int previous = rlGetActiveFramebuffer();
    const Matrix       projection = rlGetMatrixProjection();

    // The same projection into a corner of the offscreen target `scale` of the size: the
    // body lands where it would on screen, only smaller, and the shader is told so.
    const int sw = std::max(1, (int)std::ceil((float)w * scale));
    const int sh = std::max(1, (int)std::ceil((float)h * scale));
    // The caller's scissor is in its own target's pixels and means nothing in this one.
    const GlState::Scissor scissor = GlState::Current();
    rlDisableScissorTest();
    rlEnableFramebuffer(rt->id);
    rlViewport(0, 0, sw, sh);
    rlClearColor(0, 0, 0, 0);
    rlClearScreenBuffers();
    // Kept premultiplied, so the filtering when it is scaled back up has no dark fringe at
    // the limb where the shader left nothing.
    rlSetBlendFactorsSeparate(RL_SRC_ALPHA, RL_ONE_MINUS_SRC_ALPHA, RL_ONE, RL_ONE_MINUS_SRC_ALPHA,
                              RL_FUNC_ADD, RL_FUNC_ADD);
    BeginBlendMode(BLEND_CUSTOM_SEPARATE);
    paintScale_ = (float)sw / (float)w;
    const Vector2          at = p.pos;
    const Lighting::Sample l = LightAt(at);
    const bool             begun = BeginMaterialAt(item, l, at, ShadeRadius(p), Axis(p), &m, &p);
    if (begun)
    {
        Perf::Count(Perf::Counter::Painted);
        DrawPainted(item, p, m);
    }
    EndMaterial();
    rlDrawRenderBatchActive();
    EndBlendMode();
    paintScale_ = 1.0f;

    // Back to the target, as it was.
    if (previous != 0)
        rlEnableFramebuffer(previous);
    else
        rlDisableFramebuffer();
    rlViewport(0, 0, w, h);
    rlSetMatrixProjection(projection);
    GlState::Restore(scissor);
    if (!begun)
        return false;

    // The painted box, scaled back up over where the body is. Rows of a render target count
    // from the bottom, which is what the negative height says.
    const float     s = (float)sw / (float)w;
    const Rectangle source{ box.x * s, ((float)h - box.y - box.height) * s, box.width * s,
                            -box.height * s };
    const Vector2   tl = GetScreenToWorld2D({ box.x, box.y }, view_);
    const Vector2   br = GetScreenToWorld2D({ box.x + box.width, box.y + box.height }, view_);
    BeginBlendMode(BLEND_ALPHA_PREMULTIPLY);
    DrawTexturePro(rt->texture, source, { tl.x, tl.y, br.x - tl.x, br.y - tl.y }, { 0.0f, 0.0f },
                   0.0f, WHITE);
    EndBlendMode();
    return true;
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
    pose.time = clock_;
    pose.thrusting = item.thrusting;

    std::vector<Piece> pieces;
    {
        Perf::Scope timed(Perf::Section::Compose);
        pieces = Compose(*item.shape, pose);
    }
    Perf::Count(Perf::Counter::Pieces, (int)pieces.size());

    // An object that is itself a light is never shaded by one -- the same rule the
    // uncomposed path follows. Without it a star would be shaded by the light it emits,
    // which is how a sun ends up looking like a moon (#119).
    const bool emissive = Emissive(item);

    // The object's surface for each role (#516), resolved once: a station on `plating` draws
    // its panels on the recessed plating and its struts as truss, each down its own fallback
    // chain if this driver could not compile it.
    const Material* byRole[5] = { nullptr, nullptr, nullptr, nullptr, nullptr };
    if (materials_ != nullptr && !item.material.empty())
        for (int r = 0; r < 5; r++)
            byRole[r] = Materials::Pick("", item.material, RoleName((Role)r),
                                        [this](const std::string& id) { return Drawable(id); });

    // Whether the body is painted by a material of its own (#426). If it is, its stand-ins --
    // the continents of discs, the bands, the sunspots -- are not drawn: the shader already
    // drew the real thing, and on a driver that would not compile it they are the picture.
    // A moon is not the body, so a moon's material says nothing about its planet.
    bool painted = false;
    for (const Piece& p : pieces)
        if (!p.fallback && !p.orbits && OwnMaterial(p) != nullptr)
        {
            painted = true;
            break;
        }

    for (const Piece& p0 : pieces)
    {
        if (p0.fallback && painted)
            continue;
        const Material* ownMat = OwnMaterial(p0);
        // A piece that misses the target is skipped like an object that does (#296). A star
        // three hundred thousand units away still reaches the screen with its corona, and
        // its prominences, flares and sunspots on the far side of it were all being drawn.
        if (OffScreen(p0.pos, PieceReach(p0) * (ownMat != nullptr ? ownMat->reach : 1.0f)))
        {
            Perf::Count(Perf::Counter::Skipped);
            continue;
        }

        // A night-side part fades out where its body faces the light: the light's direction
        // against the point's own direction from the body's centre, so it goes out across
        // the terminator rather than at it (#240). With no light at all it is night.
        Piece p = p0;
        if (p.onlyDark)
        {
            const Vector2          body = p.surface ? p.bodyPos : item.pos;
            const float            radius = p.surface ? p.bodyRadius : item.size;
            const Lighting::Sample l = LightAt(body);
            const float            r = radius > 0.0f ? radius : 1.0f;
            const float day = ((p.pos.x - body.x) * l.dir.x + (p.pos.y - body.y) * l.dir.y) / r;
            const float t = std::fmin(1.0f, std::fmax(0.0f, (day * l.strength + 0.15f) / 0.25f));
            const float night = 1.0f - t * t * (3.0f - 2.0f * t);
            if (night <= 0.01f)
                continue;
            p.brightness *= night;
        }
        // Painted by a material of its own, even on something that is itself a light: a
        // star's photosphere is exactly that, and its shader does its own glowing.
        if (ownMat != nullptr && DrawOwnMaterial(item, p, *ownMat))
            continue;
        if (p.soft && p.surface && p.form == Form::Disc)
        {
            DrawSoftOnSurface(item, p, emissive);
            continue;
        }
        if (p.soft)
        {
            // Gas, not a solid: a glow from the colour at its middle to nothing at its rim,
            // never shaded: a lit wisp is a grey ball.
            const Color own = p.tint.a > 0 ? p.tint : item.color;
            const Color mid = Fade(ForRole(p.role, own), p.brightness);
            Perf::Scope timed(Perf::Section::Soft);
            Perf::Count(Perf::Counter::Soft);
            EndMaterial();
            // A fan of its own rather than raylib's gradient, which has thirty-six sides and
            // rounds its centre to a whole unit -- a glow that steps across the screen by a
            // pixel at a time when zoomed in, with corners (#426).
            const int sides = SidesOnScreen(p.radius);
            rlBegin(RL_TRIANGLES);
            for (int k = 0; k < sides; k++)
            {
                const float t0 = 2.0f * PI * (float)k / (float)sides;
                const float t1 = 2.0f * PI * (float)(k + 1) / (float)sides;
                rlColor4ub(mid.r, mid.g, mid.b, mid.a);
                rlVertex2f(p.pos.x, p.pos.y);
                rlColor4ub(mid.r, mid.g, mid.b, 0);
                rlVertex2f(p.pos.x + std::cos(t1) * p.radius, p.pos.y + std::sin(t1) * p.radius);
                rlVertex2f(p.pos.x + std::cos(t0) * p.radius, p.pos.y + std::sin(t0) * p.radius);
            }
            rlEnd();
            continue;
        }
        // A light dims with damage and with its own blink, and is never shaded: a lamp is
        // not lit by the star, it *is* a light, and shading one is how a beacon ends up
        // dark on its own night side.
        // A part with a tint of its own is that colour where the object's would be (#214).
        const Color own = p.tint.a > 0 ? p.tint : item.color;
        const Color ownC = p.tint.a > 0 ? p.tint : c;
        if (p.role == Role::Light || emissive)
        {
            EndMaterial();
            DrawPiece(p,
                      Fade(ForRole(p.role, own), (0.25f + 0.75f * item.intensity) * p.brightness));
            continue;
        }

        // Sampled where the piece is rather than where the object is. The difference is
        // nothing for a station beside a star and everything for a structure large enough
        // that its far side is meaningfully further away -- which is what players will
        // build (#44).
        // A mark on a planet is lit as the planet: with the body's centre and radius, so it
        // falls into the same terminator. Shaded as itself, a crater would carry a small
        // lit side of its own on the planet's night side (#166).
        const Vector2          shadeAt = p.surface ? p.bodyPos : p.pos;
        const float            shadeR = p.surface ? p.bodyRadius : ShadeRadius(p);
        const Vector2          shadeAxis = p.surface ? Vector2{ 0.0f, 0.0f } : Axis(p);
        const Lighting::Sample pieceLight = LightAt(shadeAt);

        // Below a few pixels across there is no surface left to shade, and shading it
        // anyway is worse than not: a rail two pixels wide is *entirely* the part of a
        // cylinder that turns away from the viewer, so it comes out the darkest thing on
        // screen. The same argument as minPixels, one level down.
        const float     shadePixels = shadeR * view_.zoom;
        const Material* surface = byRole[(int)p.role];
        const bool shaded = shadePixels >= MIN_SHADED_PIXELS && surface != nullptr &&
                            BeginMaterialAt(item, pieceLight, shadeAt, shadeR, shadeAxis, surface,
                                            p.surface ? nullptr : &p);
        if (!shaded)
            EndMaterial();

        // With a shader the colour stays the object's own and the shading is done in the
        // fragment; without one it is dimmed here. Doing both would darken twice.
        Color base = shaded ? ownC : Lit(ownC, lighting_, pieceLight);
        // A part's own solidity and whatever the clock is doing to it, in one number, and
        // it applies to any role rather than only to a lamp: a nebula breathes and a belt
        // glitters through the same field.
        if (p.brightness < 1.0f)
            base = Fade(base, p.brightness);
        DrawPiece(p, ForRole(p.role, base));
        // Not ended here: the next piece may be shaded with the same inputs.
    }
    EndMaterial();
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
        case EntityKind::Structure:
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
