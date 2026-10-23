#pragma once

#include "render/MaterialLibrary.h"
#include "render/Scene.h"
#include <functional>

namespace Render
{

// The ASCII look — the game's primary presentation (#36), not a debug view.
//
// The grammar it draws is deliberately narrow, because a screenshot has to be readable
// at a glance:
//
//   glyph  = what class of thing this is    (a station is `#` whatever it trades in)
//   colour = whose it is                    (faction paint, star type, ore left)
//   size   = how big it actually is         (straight from the world, not a category)
//
// The one thing that is NOT carried by the glyph is what an object can do — that is what
// the overview panel and the archetype's component list are for. Encoding capability in
// the character would mean a player-built structure needs a new letter, and needing new
// art per object type is precisely what glyphs are here to avoid.
//
// Nothing in this class knows what a station or a nebula is. It reads `Item`, and an
// object a player invents draws correctly the moment its archetype names a glyph.
class GlyphBackend : public IBackend
{
public:
    const char* Name() const override { return "glyph"; }

    // A character has no lit side, but it does have a colour, and a readout of a system
    // lit by a red giant should not be the same white it would be under a blue star.
    void SetLighting(const Lighting& l) override { lighting_ = l; }

    void Draw(const Item& item) override;

    // Glyph height as a fraction of the object's diameter. A character's ink fills
    // roughly half its em box, so 1.6 lands a glyph at about the size of the shape it
    // replaced.
    static constexpr float SCALE = 1.6f;
    // Below this many world units a glyph is illegible and is drawn as a point instead,
    // so a distant belt thins out rather than disappearing.
    static constexpr float MIN_PIXELS = 6.0f;
    // How many glyphs trace the edge of a region, and how large each one is relative to
    // the region's radius.
    static constexpr int   REGION_MARKS = 16;
    static constexpr float REGION_GLYPH = 0.12f;

private:
    void DrawPoint(const Item& item, Color c);
    void DrawRegion(const Item& item, Color c);
    void DrawDirectional(const Item& item, Color c);

    Lighting lighting_;  // empty means unlit, which is full colour
};

// The vector-shape and sprite look the game shipped with, moved out of the entity
// classes and behind the same interface. Kept as an alternative rather than deleted:
// it is what the sprite backend (#1) hangs off, and having two presentations of one
// world is the property the seam exists to provide.
class ShapeBackend : public IBackend
{
public:
    const char* Name() const override { return "shapes"; }

    void SetLighting(const Lighting& l) override { lighting_ = l; }
    void SetView(const Camera2D& v) override { view_ = v; }
    void SetMaterials(MaterialLibrary* m) override { materials_ = m; }
    void SetClock(double seconds) override { clock_ = seconds; }

    // A shader's time is a float, and a float counting seconds since the server started
    // has no precision left within days. Wrapped here: 16384 s keeps it to a couple of
    // milliseconds, and the seam once every four and a half hours is a shader's problem
    // only if its period does not divide it.
    static constexpr double SHADER_TIME_WRAP = 16384.0;

    void Draw(const Item& item) override;

    // How far around the light direction the rim highlight reaches, and how thick it is
    // as a fraction of the object's radius. A rim is what tells a disc from a sticker.
    static constexpr float RIM_ARC = 62.0f;
    static constexpr float RIM_THICKNESS = 0.09f;

    // Narrower than this on screen and a part is drawn flat rather than shaded (#135).
    static constexpr float MIN_SHADED_PIXELS = 3.0f;

    // How far outside the target an object may reach and still be culled: lines are drawn at
    // least a pixel wide, so a reach measured in world units can fall short by that much.
    static constexpr float CULL_MARGIN_PIXELS = 4.0f;

    // How far a mark on a planet's outline may fall inside the true ellipse, in pixels: what
    // decides how many corners it is drawn with.
    static constexpr float OUTLINE_SAG_PIXELS = 0.35f;

    // A planet's orbit guide is drawn in full while the planet is at most this many pixels in
    // radius on screen, and is gone by the second: past that the planet is what is being
    // looked at, and its orbit is a line through it (#426).
    static constexpr float ORBIT_GUIDE_FULL_PIXELS = 12.0f;
    static constexpr float ORBIT_GUIDE_GONE_PIXELS = 40.0f;

private:
    // Whether a circle of `reach` world units about `pos` misses the target being drawn into.
    bool OffScreen(Vector2 pos, float reach) const;
    // How many corners a surface mark's outline needs at the size it is on screen.
    int OutlineSegments(const Piece& p) const;
    // How many sides a round body of `radius` world units is drawn with at this zoom.
    int SidesOnScreen(float radius) const;
    // A planet's orbit, while the planet is small enough on screen for it to mean anything.
    void DrawOrbitGuide(const Item& item) const;

    double clock_ = 0.0;  // seconds; set before every scene (Present)
    // True when a material took the object over. It then draws one plain primitive and the
    // shader does the shading, instead of the offset discs and rim arcs this backend fakes
    // it with (#119) -- those exist precisely because there was no shader yet.
    // Turns the item's material on for what follows, positioned on `at` with radius
    // `size` in world units. A composition passes each piece's own centre and cross
    // section (#135); an uncomposed object passes its own.
    // `own` is the material to use in place of the object's: a part's own (#426), or the
    // object's surface for the part's role (#516). `piece` is the part, for the sources that
    // ask about it -- its seed, its turn, its proportions; null for none.
    bool BeginMaterialAt(const Item& item, const Lighting::Sample& light, Vector2 at, float size,
                         Vector2 axis = { 0.0f, 0.0f }, const Material* own = nullptr,
                         const Piece* piece = nullptr);
    bool BeginMaterial(const Item& item, const Lighting::Sample& light);
    void EndMaterial();
    // The shapes themselves, split off so that whichever of its ten returns is taken, the
    // material is still ended by the caller. A shader left bound is not a visible bug
    // where it happens; it is a visible bug in whatever is drawn next.
    void DrawShape(const Item& item, Color c, bool shaded, const Lighting::Sample& light);

    // The composition an archetype describes (#122), in place of the switch above. Returns
    // false when the object has no shape in data, which is every object that has not been
    // given one yet -- and those keep the shape that used to be compiled in.
    bool DrawComposition(const Item& item, Color c, const Lighting::Sample& light);
    void DrawPiece(const Piece& p, Color c);
    // A glow on a planet's surface: projected and cut like any surface piece, lit as the
    // body, and still fading to nothing at its rim.
    void DrawSoftOnSurface(const Item& item, const Piece& p, bool emissive);
    // A part's own material, if it has one that will draw at this size: compiled here, and
    // the part big enough on screen to have a surface (#426). Null otherwise.
    const Material* OwnMaterial(const Piece& p) const;
    // A piece painted by its own material, drawn over its reach. False if it could not be,
    // and the piece is then drawn as any other.
    bool DrawOwnMaterial(const Item& item, const Piece& p, const Material& m);
    // The same, painted into an offscreen target at `scale` of the target's resolution and
    // drawn back over `box` (screen pixels), for a body too large on screen to paint at full
    // resolution every frame (#426). False if it could not be.
    bool PaintScaled(const Item& item, const Piece& p, const Material& m, float scale,
                     Rectangle box);
    // Whatever `draw` draws, painted into the offscreen target at `scale` of the target's
    // resolution and drawn back over `box` (screen pixels): a piece (PaintScaled) or a whole
    // built object (#516). False if it could not be, and nothing was drawn.
    bool PaintInto(float scale, Rectangle box, const std::function<bool()>& draw);
    // The resolution to paint something `reach` world units round `centre` at, under the
    // paint budget: 1 when it fits, else down to PAINT_MIN_SCALE. `clip` is its box on screen.
    float PaintScaleFor(Vector2 centre, float reach, Rectangle& clip) const;
    // Whether an object's surface is a built one, finished part by part (#516).
    bool BuiltSurface(const Item& item) const;
    // The primitive under a material of a piece's own, the material already begun.
    void DrawPainted(const Item& item, const Piece& p, const Material& m);

    // A body painted by a material of its own is painted at reduced resolution once what it
    // covers on screen passes the material library's paint budget (a player setting,
    // TreatmentConfig::paintBudget), so that what it costs stops growing with it: a planet
    // filling a 1080p screen is two million fragments of noise, which an integrated GPU
    // cannot shade every frame. Never below this of the target's resolution, where the
    // softening starts to show.
    static constexpr float PAINT_MIN_SCALE = 0.5f;

    // What reaches `p` on the object being drawn: every light but its own, unless it is
    // one that lights itself (#119). Every sample this backend takes goes through here, so
    // the plain path and the material's uniforms cannot disagree about it.
    Lighting::Sample LightAt(Vector2 p) const { return lighting_.At(p, own_); }

    Lighting lighting_;  // a copy: a backend outlives the scene that handed it over
    int      own_ = -1;  // index in lighting_ of the drawn item's own light, or -1

    // What the material is bound with now, so a piece shaded with the same inputs as the
    // one before it shares the bind instead of being a draw call of its own (#296).
    bool            bound_ = false;
    const Material* boundMaterial_ = nullptr;
    const Item*     boundItem_ = nullptr;
    Vector2         boundAt_{};
    float           boundSize_ = 0.0f;
    Vector2         boundAxis_{};
    float           boundSeed_ = 0.0f;
    // What screen pixels are worth in the target being painted into: 1, or less while a body
    // is painted at reduced resolution (PaintScaled).
    float            paintScale_ = 1.0f;
    float            boundAngle_ = 0.0f;
    float            boundShape_[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    Lighting::Sample boundLight_{};
    // Whether the material library has a shader for a material, for Materials::Pick.
    bool             Drawable(const std::string& id) const;
    Camera2D         view_{};
    MaterialLibrary* materials_ = nullptr;  // borrowed; null draws everything plain
};

}  // namespace Render
