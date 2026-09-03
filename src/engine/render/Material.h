#pragma once

#include "render/Lighting.h"
#include "raylib.h"
#include <string>
#include <vector>

namespace Render
{

struct Item;

// A material is a shader plus what feeds it (#121).
//
// The direction is that any shader can sit on any object, driven by what the game already
// knows about that object -- a damaged hull, the star it is standing next to, whether the
// engine is burning. The channel for that has existed since #35: `Render::Item` is what an
// entity says about itself, and it already carries `intensity`, `heading` and `thrusting`.
// They were shader uniforms with no shader on the other end.
//
// So a material names a shader and a list of *bindings*: uniform name on one side, where
// its value comes from on the other. An archetype names a material and nothing else
// changes. Entities stay as ignorant of shaders as they already are of backends.
//
// This file is the half with no GPU in it: what the bindings are and what they resolve to.
// MaterialLibrary is the half that compiles shaders and sets uniforms, and it is untestable
// for the same reason the screen treatment is (#120).

// Where a uniform's value comes from.
enum class Source
{
    Constant,  // written in the data

    ItemColor,       // vec4, 0..1 -- the object's own colour
    ItemIntensity,   // float -- hull left, ore left, how looted a wreck is
    ItemHeading,     // float, radians
    ItemThrusting,   // float, 0 or 1
    ItemSize,        // float, world units
    ItemScreenPos,   // vec2, pixels -- where the object's centre landed
    ItemScreenSize,  // float, pixels -- its radius after the camera

    LightDir,       // vec2, unit vector toward the strongest light (#119)
    LightTint,      // vec4, 0..1 -- the colour of the light arriving
    LightStrength,  // float, 0..1
    LightAmbient,   // float -- the floor nothing goes below

    ClockTime  // float, seconds
};

const char* SourceName(Source s);
bool        SourceFromName(const std::string& s, Source& out);

// A uniform's value, as up to four floats. Wider than most bindings need, and the same
// shape for all of them, because the alternative is a variant per width and a switch at
// every use.
struct UniformValue
{
    int   count = 1;
    float v[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
};

struct Binding
{
    std::string  uniform;
    Source       source = Source::Constant;
    UniformValue constant;
};

struct Material
{
    std::string          id;      // "hull" -- how an archetype refers to this
    std::string          shader;  // file stem under data/shaders/materials/
    std::vector<Binding> bindings;
};

// Everything a binding can be resolved from. Gathered by the backend once per item, so
// resolving is arithmetic on values rather than a reach back into the world.
struct MaterialInputs
{
    const Item*      item = nullptr;
    Lighting::Sample light;
    float            ambient = 0.0f;
    Vector2          screenPos = { 0.0f, 0.0f };
    float            screenSize = 0.0f;
    float            time = 0.0f;
};

// What this binding is worth right now. A binding whose source needs an item it was not
// given resolves to zero rather than reading through a null pointer: a material on an
// object that cannot supply it should look wrong, not crash.
UniformValue Resolve(const Binding& b, const MaterialInputs& in);

// The registry, loaded once like the archetypes and the factions.
namespace Materials
{
// Reads data/materials.json. Returns false and leaves the previous contents alone when the
// file is missing or malformed; Error() says which. A material naming an unknown source is
// rejected outright rather than silently losing that binding -- unlike a look setting, a
// material with a missing uniform draws something wrong rather than something plainer.
bool Load(const std::string& path);

const Material* Find(const std::string& id);

const std::vector<Material>& All();

const std::string& Error();
}  // namespace Materials

}  // namespace Render
