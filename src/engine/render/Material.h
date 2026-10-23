#pragma once

#include "render/Lighting.h"
#include "raylib.h"
#include <string>
#include <utility>
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
    ItemAxis,        // vec2 -- which way an elongated part runs; zero means it is round
    ItemSeed,        // float, 0..1 -- the object's own, from its id: two planets of a kind differ
    ItemPartSeed,    // float, 0..1 -- this part's own: one rock in a field from the next
    ItemAngle,       // float, radians -- this part's own turn, its spin included (#136)
    // vec4 -- the part's own proportions, in the object's radii: half its length along its
    // axis, half its width across it (its radius if it is round), its sides, its form (#516).
    // What lets a pattern fit the part it is on -- seams that meet its edges -- rather than
    // run on in raw world units.
    ItemPartShape,
    // vec4 -- the whole object on screen: its centre (pixels, origin bottom left), its radius
    // in pixels, and its heading in OpenGL's sense (#516). What a part needs to know which
    // side of the object it is on -- a window on the night side -- and to lay grime that
    // runs on from one part to the next.
    ItemFrame,

    LightDir,       // vec2, unit vector toward the strongest light (#119)
    LightTint,      // vec4, 0..1 -- the colour of the light arriving: a rim, a glow
    LightSurface,   // vec4, 0..1 -- what that light does to an albedo, its hue capped (#361)
    LightStrength,  // float, 0..1
    LightAmbient,   // float -- the floor nothing goes below

    // The object's livery (#516): the art palette of the people who built it, never an
    // allegiance colour (#117). Its base colour is applied to the object's colour before
    // anything is drawn, shader or not; these are what only a material can use.
    LiveryAccent,  // vec4, 0..1 -- stripes, bands, the colour that says whose style this is
    LiveryParams,  // vec4 -- wear, grime, stripe pattern, panel scale
    LiveryStyle,   // vec4 -- panel aspect, patchwork, window warmth, window density

    ClockTime,  // float, seconds

    // sampler2D -- a fixed table of random values, the same on every machine, that noise is
    // read from instead of hashed (#426): one lookup in place of eight hashes, which is the
    // difference between a planet a laptop can draw filling the screen and one it cannot.
    TextureNoise
};

const char* SourceName(Source s);
bool        SourceFromName(const std::string& s, Source& out);

// A uniform's value, as up to four floats. A texture's is none: `count` 0, and the library
// binds the texture itself. Wider than most bindings need, and the same
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

    // How far past the part the shader draws, in the part's own radii (#426). A planet's air
    // is a haze beyond its limb and a star's corona reaches several radii out; neither is
    // there to draw on unless the primitive under the shader is that much larger. The shader
    // is still told the part's own radius, so a limb stays at 1. A round part only.
    float reach = 1.0f;

    // A surface for each role (#516): an object on `plating` draws its panel-role parts on
    // `roles["panel"]` and so on, and a role it does not name stays on this material. One
    // level deep -- a material named here is used as it is, its own `roles` ignored. Both
    // names are checked when the file loads: an unknown role or material is a load error.
    std::vector<std::pair<std::string, std::string>> roles;

    // What to draw with when this material has no shader on this driver (#120, #516): a
    // built surface falls back to `hull`, which is what the object wore before it had one.
    // Followed until a material with a shader is found; empty, the part is drawn plain.
    std::string fallback;

    // Whether any binding reads these. A material that does not read a part's seed, turn
    // or proportions can be bound once for every part that shares the rest (#296) -- a
    // planet's craters -- rather than once per part.
    bool readsPart = false;

    // The material a part of this role is drawn with: the one `roles` names, or this one.
    const std::string& ForRole(const std::string& role) const;
};

// A people's art palette for what they build (#516). Chosen by the culture of the faction
// that owns the object (factions.json), so a Guild hub and a Syndicate one are kin but not
// twins. An art decision, like every colour in the world view (#117): which side a player is
// on is the instruments' business, and two cultures may well be at war in the same livery.
struct Livery
{
    std::string id;
    Color       base = { 255, 255, 255, 255 };           // multiplies the object's colour
    Color       accent = { 200, 160, 60, 255 };          // stripes and bands
    float       params[4] = { 0.3f, 0.3f, 1.0f, 1.0f };  // wear, grime, stripes, panel scale
    float       style[4] = { 2.0f, 0.1f, 0.5f, 0.3f };   // aspect, patchwork, warmth, windows
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
    Vector2          axis = { 0.0f, 0.0f };
    float            time = 0.0f;
    float            partSeed = 0.0f;  // 0..1
    float            angle = 0.0f;     // radians, already in OpenGL's sense (counter-clockwise)
    float            partShape[4] = { 0.0f, 0.0f, 0.0f, 0.0f };  // see Source::ItemPartShape
    float            frame[4] = { 0.0f, 0.0f, 0.0f, 0.0f };      // see Source::ItemFrame
    const Livery*    livery = nullptr;  // null: the registry's default, or a neutral one
};

// An object's seed as a material sees it: its id spread over 0..1, the same on every client.
float SeedOf(int id);

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

// A livery by id, or the one called "default", or null when the file has neither.
const Livery*              FindLivery(const std::string& id);
const std::vector<Livery>& Liveries();

// The material a part is drawn with: its own if it has one, else the object's surface for
// its role (the object material's `roles`), then down `fallback` until `has` says a material
// can be drawn on this driver. Null when nothing in the chain can: the part is drawn plain.
template <class Has>
const Material* Pick(const std::string& own, const std::string& object, const std::string& role,
                     Has has);

// Every archetype in the registry that names a material this registry does not define,
// one line each, naming both. Such an object draws plain -- exactly what it did before
// materials existed -- so nothing on screen would ever say it was a typo (#191). The
// check cannot live in Archetypes::Load: the server and the tests load no materials.
std::vector<std::string> UnknownInArchetypes();
}  // namespace Materials

template <class Has>
const Material* Materials::Pick(const std::string& own, const std::string& object,
                                const std::string& role, Has has)
{
    const Material* m = nullptr;
    if (!own.empty())
        m = Find(own);
    else if (const Material* o = Find(object))
        m = Find(o->ForRole(role));
    // A fallback chain is short and written in the data, but a loop in it must not hang the
    // renderer: never more steps than there are materials.
    for (size_t step = 0; m != nullptr && step <= All().size(); step++)
    {
        if (has(m->id))
            return m;
        m = m->fallback.empty() ? nullptr : Find(m->fallback);
    }
    return nullptr;
}

}  // namespace Render
