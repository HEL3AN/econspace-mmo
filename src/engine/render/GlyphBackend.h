#pragma once

#include "render/Scene.h"

namespace Render
{

// The ASCII look, drawn on screen (#36).
//
// Every object is one character, sized from its world radius and coloured from the
// item. Nothing here knows what a station or a nebula is — an object a player invents
// draws correctly the moment its archetype names a glyph, which is the whole reason the
// constructible-world track can work without new art.
class GlyphBackend : public IBackend
{
public:
    const char* Name() const override { return "glyph"; }

    void Draw(const Item& item) override;

    // Glyph height as a fraction of the object's diameter. Below this many pixels an
    // object is drawn as a point instead, so a distant belt does not vanish entirely.
    static constexpr float SCALE = 1.6f;
    static constexpr float MIN_PIXELS = 6.0f;
};

// The vector-shape and sprite look the game shipped with, moved out of the entity
// classes and behind the same interface. Kept while the migration runs so that the
// glyph presentation can be compared against something rather than replacing it blind.
class ShapeBackend : public IBackend
{
public:
    const char* Name() const override { return "shapes"; }

    void Draw(const Item& item) override;
};

}  // namespace Render
