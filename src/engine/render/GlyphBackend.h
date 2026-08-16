#pragma once

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
};

// The vector-shape and sprite look the game shipped with, moved out of the entity
// classes and behind the same interface. Kept as an alternative rather than deleted:
// it is what the sprite backend (#1) hangs off, and having two presentations of one
// world is the property the seam exists to provide.
class ShapeBackend : public IBackend
{
public:
    const char* Name() const override { return "shapes"; }

    void Draw(const Item& item) override;
};

}  // namespace Render
