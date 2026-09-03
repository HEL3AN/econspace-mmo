#pragma once

#include "raylib.h"
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <vector>

namespace Render
{

struct Item;

// What an object is shaped like, said in data (#122).
//
// `ShapeBackend` used to switch on `EntityKind`, one case per kind, ending in a circle for
// anything it did not recognise. That was the real gap behind "we need an artist for every
// object": not the absence of sprites, but a shape compiled into the renderer.
//
// A shape here is a *composition* -- a trade hub is a hexagonal core, a ring, three arms
// with pads on them, a mast and a row of lamps. Not one figure. The backend assembles;
// nothing about a trade hub appears in C++.
//
// **The vocabulary is deliberately narrow and that is the feature.** Seven primitives with
// strict proportions produce a family of objects that looks intentional; an open-ended set
// of arbitrary shapes reads as programmer art, which is the failure this exists to avoid.
// Like a font: few strokes, many letters.
//
// This file is the half with no drawing in it -- what the parts are, where they end up, and
// which of them are worth drawing at the size the object currently is. That half is
// geometry and is tested. The backend turns the result into raylib calls.

enum class Form
{
    Disc,     // a filled circle -- bodies, pads, lamps
    Ring,     // an annulus -- docking rings, gates, orbit structures
    Polygon,  // a regular n-gon -- cores and hulls
    Capsule,  // a thick rounded bar -- arms, masts, booms
    Chevron,  // a triangle -- noses, fins, thrust
    Bar,      // a rectangle -- panels, plating
    Lattice   // a run of cross-struts between two points -- trusses
};

const char* FormName(Form f);
bool        FormFromName(const std::string& s, Form& out);

// What a part is *for*, which is how it is coloured and whether it is shaded at all. A
// role rather than a colour, because an object's colour belongs to the object and a part
// should keep its relationship to it whatever that colour is (#117).
enum class Role
{
    Hull,     // the object's own colour
    Panel,    // darker -- recessed, in shadow, plating
    Trim,     // lighter -- edges and highlights
    Light,    // emissive: never shaded, never dimmed by damage
    Antenna,  // thin and dim -- masts, aerials, struts
};

const char* RoleName(Role r);
bool        RoleFromName(const std::string& s, Role& out);

// One piece of an object. Every measurement is a fraction of the object's own radius, so a
// shape is written once and works at any size -- a ninety-unit station and a sixteen-unit
// ship use the same grammar.
struct Part
{
    Form    form = Form::Disc;
    Role    role = Role::Hull;
    int     sides = 6;            // Polygon
    Vector2 at = { 0.0f, 0.0f };  // offset from the centre, in radii
    float   angle = 0.0f;         // the part's own rotation, degrees
    float   radius = 1.0f;        // Disc, Ring, Polygon
    float   width = 0.1f;         // Ring thickness, Capsule/Bar/Chevron/Lattice width
    float   length = 1.0f;        // Capsule, Bar, Chevron, Lattice
    int     count = 3;            // Lattice: how many struts
    bool    filled = true;        // Polygon: solid or outline

    // Drawn `repeat` times, each turned a further 360/repeat about the object's centre.
    // One rotational repeat covers most of what a station is: arms, lamps, pads, vents.
    int repeat = 1;

    // Drawn again, reflected across the object's own axis. A different symmetry from
    // `repeat`, and the one every ship has: a pair of wings is not a wing rotated half a
    // turn, which puts the second one in front of the nose. That is what it did the first
    // time a ship was written with `repeat: 2`.
    bool mirror = false;

    // This part appears only once the object is at least this many pixels across. Zero
    // means always. Detail that is invisible is not free, and worse, a hundred parts
    // resolved into eight pixels is a smudge rather than a small object.
    float minPixels = 0.0f;

    // How much the seed is allowed to move this part: degrees of rotation, and a fraction
    // of its size. Two trade hubs should differ without either stopping being a trade hub.
    float jitterAngle = 0.0f;
    float jitterScale = 0.0f;
};

struct Shape
{
    std::vector<Part> parts;
    bool              Empty() const { return parts.empty(); }
};

// A part placed in the world: everything the backend needs, with no fractions left in it.
struct Piece
{
    Form    form = Form::Disc;
    Role    role = Role::Hull;
    int     sides = 6;
    bool    filled = true;
    int     count = 3;
    Vector2 pos = { 0.0f, 0.0f };  // world
    float   angle = 0.0f;          // world, degrees
    float   radius = 0.0f;         // world
    float   width = 0.0f;          // world
    float   length = 0.0f;         // world
};

// Reads a shape from an archetype's `shape` array. Returns false and says why on anything
// it does not recognise: unlike a screen-treatment pass, a part that quietly vanishes
// leaves an object missing a piece, and nobody would know which file to look in.
bool ParseShape(const nlohmann::json& j, Shape& out, std::string& error);

// How far the composition actually reaches, in radii. A shape is written around a radius
// of one but need not stay inside it -- a docking ring at 1.55 is the point of a docking
// ring -- so anything framing an object (the gallery card, a selection highlight) has to
// ask rather than assume. Returns 1 for an empty shape.
float Extent(const Shape& s);

// Places a shape on an object: applies the repeats, the object's own heading, its size and
// position, drops the parts too small to be worth drawing, and perturbs what the shape
// allows to be perturbed.
//
// `pixelsPerUnit` is the camera's zoom -- the same object is eight pixels across on the
// system map and four hundred in the gallery, and the parts that are worth drawing are not
// the same in both.
//
// `seed` should be stable for an object across frames, or its jitter becomes a shimmer.
// The entity id is the right thing to pass.
std::vector<Piece> Compose(const Shape& s, Vector2 pos, float size, float heading, int seed,
                           float pixelsPerUnit);

}  // namespace Render
