#pragma once

#include "raylib.h"
#include <vector>

namespace Render
{

struct Item;

// Where a system's light comes from (#119).
//
// Flat colour is what makes a simple shape read as a toy: a disc filled with one value is
// a sticker, and the same disc with a lit side, a terminator and a rim is an object. The
// cheapest way to get that without an artist is to know where the light is, and the light
// is already in the world -- a star has a position, a colour and a size, and it is in the
// SystemLayout the client is given on entry. Nothing about this needs the wire to change.
//
// It is a *list* from the first line rather than "the sun". A system with two stars is
// something the world format can already describe, a dying giant beside a white dwarf is
// a system worth building on purpose, and once players build (#44) a glowing structure is
// a light too. Retrofitting a list onto a single sun is much more expensive than starting
// with one.
struct Light
{
    Vector2 pos = { 0.0f, 0.0f };
    Color   color = { 255, 255, 255, 255 };
    float   intensity = 1.0f;  // brightness at the source
    float   radius = 1.0f;     // distance at which it contributes nothing at all
};

// The lights of one system and the floor beneath them.
struct Lighting
{
    std::vector<Light> lights;

    // Space is dark, but a game that is actually black is a game nobody can read, and
    // this one is played zoomed out, where an object is a few pixels and its dark side is
    // most of them. This is the fraction of an object's own colour that survives with no
    // light on it at all; it was tuned by looking at a whole system, not at one planet.
    float ambient = 0.38f;

    // No lights means lighting is off, not that the system is pitch dark. Everything that
    // draws without a light list -- the tests, an agent, an editor view of a system whose
    // star has no light in data -- keeps drawing at full colour.
    bool Empty() const { return lights.empty(); }

    // What reaches a point.
    struct Sample
    {
        Vector2 dir = { 0.0f, 0.0f };  // unit vector toward the light; zero means "no direction"
        float   strength = 0.0f;       // 0..1 above the ambient floor
        Color   tint = { 255, 255, 255, 255 };  // the colour of the light arriving
    };

    // Only the strongest few sources are combined. A system can hold many lights once
    // players build, and the fifth-brightest has never changed a picture.
    static constexpr int MAX_CONTRIBUTORS = 4;

    Sample At(Vector2 p) const;
};

// The lights an already-described scene contains. An item lights the system when its
// archetype says so, which is what lets a second star, or a beacon a player builds, be a
// light without anything here learning what either of them is.
Lighting LightsFrom(const std::vector<Item>& items, float ambient = 0.38f);

}  // namespace Render
