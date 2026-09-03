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

    // Movement (#136). All of it is a function of the clock and the part's seed, never of
    // anything accumulated per frame: a part whose angle is integrated drifts between
    // clients, and two players would see the same station turned differently. As written,
    // every client computes the same answer from the same time without a byte on the wire.
    // How solid this part is, 0..1. A corona is a glow rather than a ring and a nebula is
    // a place rather than a disc, and neither is expressible with a colour alone -- the
    // colour belongs to the object, so every part of it would go translucent together.
    float alpha = 1.0f;

    // An orbiting part travels around the body rather than sitting on it (#165): a moon,
    // a ring shepherd, a station's tender. Set `orbitRadius` and it stops being a surface
    // feature and becomes something in the space around the object.
    //
    // The whole of the depth effect is draw order: on the far half of the orbit the part is
    // drawn before the body and hidden by it, on the near half after it and in front. A
    // flat scene reads as depth for the price of one comparison.
    float orbitRadius = 0.0f;   // in radii; zero means this part does not orbit
    float orbitPeriod = 60.0f;  // seconds for one lap
    float orbitPhase = 0.0f;    // 0..1, where in the lap it starts
    // How far the orbital plane is tilted out of edge-on. 0 is a line straight across the
    // body, 1 is a circle seen from above -- and at 1 the part never passes behind
    // anything, so the interesting values are in between.
    float orbitTilt = 0.35f;

    float spin = 0.0f;            // degrees per second about the object's centre
    float blink = 0.0f;           // seconds per cycle; 0 is a steady light
    bool  onlyThrusting = false;  // drawn only while the object's engine is burning
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

    // 0..1, from a blinking light's place in its cycle. One for everything steady, so a
    // backend can multiply by it unconditionally.
    // 0..1: the part's own solidity and, if it blinks, where it is in its cycle. Folded
    // into one number because a backend does the same thing with both -- one field to
    // multiply by unconditionally rather than two it has to remember to combine.
    float brightness = 1.0f;

    // Where this piece sits front to back: negative is behind the body, positive in front,
    // zero for everything that is simply part of it. Compose returns pieces already sorted
    // by it, so a backend draws them in order and never has to know why.
    float depth = 0.0f;
};

// How large a piece is *for shading*, which is not the same as how far it reaches.
//
// A material shades by distance from a centre, so what it wants is the piece's cross
// section: an arm two radii long and a tenth wide is lit like a thin cylinder, not like a
// ball two radii across. Round forms give their radius; elongated ones give half their
// width, because that is the direction the surface actually turns in.
float ShadeRadius(const Piece& p);

// Which way an elongated piece runs, as a unit vector. Zero for the round forms, which
// have no axis and are lit as what they are.
//
// A material needs this because a long thin part is not a small sphere. Lit radially, an
// arm two radii long gets one bright band across its middle and both ends in shadow --
// which is what a truss looked like the first time parts were shaded individually.
Vector2 Axis(const Piece& p);

// Reads a shape from an archetype's `shape` array. Returns false and says why on anything
// it does not recognise: unlike a screen-treatment pass, a part that quietly vanishes
// leaves an object missing a piece, and nobody would know which file to look in.
bool ParseShape(const nlohmann::json& j, Shape& out, std::string& error);

// How far the composition actually reaches, in radii. A shape is written around a radius
// of one but need not stay inside it -- a docking ring at 1.55 is the point of a docking
// ring -- so anything framing an object (the gallery card, a selection highlight) has to
// ask rather than assume. Returns 1 for an empty shape.
float Extent(const Shape& s);

// Everything about the object that placing its shape depends on. A struct rather than
// eight positional arguments, because the list grew twice and will grow again.
struct Pose
{
    Vector2 pos = { 0.0f, 0.0f };
    float   size = 1.0f;
    float   heading = 0.0f;  // radians

    // Stable for an object across frames, or its jitter becomes a shimmer. The entity id
    // is the right thing to pass.
    int seed = 0;

    // The camera's zoom. The same object is eight pixels across on the system map and four
    // hundred in the gallery, and the parts worth drawing are not the same in both.
    float pixelsPerUnit = 1.0f;

    float time = 0.0f;        // seconds, for anything that moves (#136)
    bool  thrusting = false;  // whether the object's engine is burning
};

// Places a shape on an object: applies the repeats and the mirror, the object's own
// heading, its size and position, whatever the clock is doing to it, drops the parts too
// small or too still to be worth drawing, and perturbs what the shape allows.
std::vector<Piece> Compose(const Shape& s, const Pose& pose);

}  // namespace Render
