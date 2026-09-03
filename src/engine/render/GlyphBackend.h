#pragma once

#include "render/MaterialLibrary.h"
#include "render/Scene.h"

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

    void Draw(const Item& item) override;

    // How far around the light direction the rim highlight reaches, and how thick it is
    // as a fraction of the object's radius. A rim is what tells a disc from a sticker.
    static constexpr float RIM_ARC = 62.0f;
    static constexpr float RIM_THICKNESS = 0.09f;

private:
    // True when a material took the object over. It then draws one plain primitive and the
    // shader does the shading, instead of the offset discs and rim arcs this backend fakes
    // it with (#119) -- those exist precisely because there was no shader yet.
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

    Lighting         lighting_;  // a copy: a backend outlives the scene that handed it over
    Camera2D         view_{};
    MaterialLibrary* materials_ = nullptr;  // borrowed; null draws everything plain
};

}  // namespace Render
