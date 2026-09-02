#pragma once

#include "entities/EntityKind.h"
#include "core/Archetype.h"
#include "raylib.h"
#include <string>
#include <vector>

// The seam between WHAT is in the world and HOW it is shown (#35).
//
// Until now every entity drew itself: `Entity::Draw()` called raylib directly, with a
// sprite path and a vector-shape fallback baked in. With no seam there is no way to
// present the same world twice, and three consumers now need exactly that — the player
// in glyphs, an agent in text, and tests with no window at all.
//
// An entity now describes itself as a Render::Item and stops there. A backend decides
// what that becomes. The payoff is the one the constructible-world track depends on: an
// object a player invents needs no new drawing code, because it already has an archetype
// and the archetype already has a glyph.

namespace Render
{

// One drawable thing, in world coordinates. Everything a backend could reasonably need
// and nothing about how it will be drawn.
struct Item
{
    int        id = 0;
    EntityKind kind = EntityKind::Unknown;

    Vector2 pos = { 0.0f, 0.0f };
    float   size = 0.0f;
    Color   color = { 255, 255, 255, 255 };

    std::string glyph = "?";                // the ASCII presentation
    std::string sprite;                     // texture name; empty means this thing has no sprite
    int         layer = 0;                  // draw order, lowest first
    GlyphStyle  style = GlyphStyle::Point;  // how the glyph occupies the object's extent

    std::string label;  // display name, for backends that show one

    float heading = 0.0f;    // radians; 0 for things that do not point anywhere
    float intensity = 1.0f;  // 0..1 — ore left in a belt, hull left on a ship, a looted wreck
    float ring = 0.0f;       // guide circle radius (a planet's orbit); 0 draws none
    bool  thrusting = false;
};

// Turns items into pixels, characters or lines of text. A backend may ignore any field
// it has no use for — a text projection has no use for a heading, a glyph grid has no
// use for a sprite.
class IBackend
{
public:
    virtual ~IBackend() = default;

    virtual const char* Name() const = 0;

    virtual void Begin() {}
    virtual void Draw(const Item& item) = 0;
    virtual void End() {}
};

// The one place an archetype's Visual becomes an Item. An entity calls this from
// Describe() and then overrides what only the instance knows; the gallery (#118) has no
// entity to ask and calls it directly. Having one mapping is what stops the tool used to
// judge a look from showing something the game would not.
Item FromArchetype(const Archetype& a, Vector2 pos, float size);

// Draws a scene: sorts by layer, then hands every item to the backend between Begin and
// End. Sorting here rather than in each backend is what makes `layer` mean the same
// thing in all of them.
void Present(std::vector<Item> items, IBackend& backend);

}  // namespace Render
