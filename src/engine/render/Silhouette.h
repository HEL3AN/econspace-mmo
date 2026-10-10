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
    Lattice,  // a run of cross-struts between two points -- trusses
    Band,     // a latitude band on a sphere -- a gas giant's belts, an ice cap (#166)
    Arc       // a ring segment from one angle to another -- a broken dock, a flare (#214)
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

    // On the sphere rather than on the disc (#166). A surface part has a latitude and a
    // longitude instead of an offset: it is projected onto the body, squashed as it nears
    // the limb, hidden while it is on the far side, and carried round by `spin` -- which
    // for a surface part is how fast the planet turns, in degrees of longitude a second.
    //
    // This replaces turning a crater about the disc's centre like a wheel, which is what
    // a planet's surface did before and is not what a planet does. A planet turns about an
    // axis lying nearly in the picture, so its features travel *across* it and round the
    // back. That is the same trick as a moon passing behind the body (#165), and it is most
    // of what makes a disc read as a ball.
    //
    // A part is on the surface when its data gives `lat` or `lon`. A `band` is always on
    // the surface: it is a latitude, `lat` is its middle and `width` is in degrees.
    bool  surface = false;
    float lat = 0.0f;  // degrees, north positive
    float lon = 0.0f;  // degrees, 0 facing the viewer

    // Movement (#136). All of it is a function of the clock and the part's seed, never of
    // anything accumulated per frame: a part whose angle is integrated drifts between
    // clients, and two players would see the same station turned differently. As written,
    // every client computes the same answer from the same time without a byte on the wire.
    float spin = 0.0f;            // degrees per second about the object's centre
    float blink = 0.0f;           // seconds per cycle; 0 is a steady light
    bool  onlyThrusting = false;  // drawn only while the object's engine is burning
    // Seen only on the side turned away from the light (#240): a city's lights, an aurora,
    // a station's lit windows. Lit cities in daylight are what made them have to be faint.
    bool onlyDark = false;

    // Three shapes the catalogue agents kept faking (#240):
    // - `tip` on a chevron is the width of its narrow end as a fraction of its base, so a
    //   nozzle bell, a wing chord or a fairing is a trapezoid rather than a triangle with
    //   its point hidden under the next part.
    // - `jagged` on a polygon pulls each corner in by up to that fraction of the radius,
    //   by the object's seed: a rock, a torn edge, an ice shard.
    // - `soft` draws a disc as a glow that fades to nothing at its rim and is never shaded:
    //   gas, haze, a corona. Lit like a solid, a nebula wisp reads as a grey ball.
    float tip = 0.0f;
    float jagged = 0.0f;
    bool  soft = false;

    // A part's own colour (#214), in place of the object's: hot cracks on a dark world,
    // crystal glints in a grey belt, a red warning lamp on a grey hull. Still an art
    // decision about the object (#117), never a faction tag; alpha 0 means "the object's".
    Color tint = { 0, 0, 0, 0 };

    // Arc: from and to, in degrees, measured like `angle` (0 along +x, counter-clockwise
    // on screen as every other angle in the grammar).
    float arcFrom = 0.0f;
    float arcTo = 90.0f;

    // A row (#214): copies along a line, beside `repeat`'s copies around the centre. The
    // ribs down a hull, a line of ports, a stack of containers -- each copy `rowStep`
    // further on, in the object's radii, before the object turns.
    int     rowCount = 1;
    Vector2 rowStep = { 0.0f, 0.0f };

    // A module from the library in place of a form (#240): the part is placed, turned,
    // repeated, mirrored and rowed as any part is, and what is drawn there is one of the
    // module's variants at `scale` of the object's radius. `variant` pins one; empty lets
    // the object's seed choose.
    std::string module;
    std::string variant;
    float       scale = 0.1f;

    // Generated variety (#240): a number written as [min, max] is chosen per object by its
    // seed; a tint written as a list of colours is a palette to pick from; `chance` is how
    // often the part is there at all. One written variant becomes a family of looks.
    enum class Field
    {
        Radius,
        Width,
        Length,
        Angle,
        AtX,
        AtY,
        Alpha,
        Scale,
        RowCount,
        Sides,
        Count,
        Lat,
        Lon,
        Spin,
        Blink,
        ArcFrom,
        ArcTo,
        StepX,
        StepY,
        RowTurn,
        RowTaper,
        PivotX,
        PivotY,
        Tip,
        Jagged,
        RowRing,
        RowSpread
    };
    // `var` is the shape's variable this range follows, or -1 for a roll of its own.
    struct Vary
    {
        Field field;
        float lo, hi;
        int   var = -1;
    };
    std::vector<Vary>  vary;
    std::vector<Color> palette;
    int                tintVar = -1;  // the palette's pick follows this variable
    float              chance = 1.0f;
    // Parts in one `group` share the roll their `chance` is compared with, so a lamp and
    // its housing come and go together. A group is a hidden variable of the shape.
    int chanceVar = -1;

    // The row bends by `rowTurn` degrees per copy -- an arc of ribs, a spiral arm, an atoll
    // -- and each copy is `rowTaper` times the size of the one before: rays, tongues of
    // lava, a glacier narrowing to its snout. Each copy also turns with the row.
    float rowTurn = 0.0f;
    float rowTaper = 1.0f;
    // A row laid round a centre instead of along a line: `at` is the centre, `rowRing` the
    // radius, and the copies are spread evenly over `rowSpread` degrees -- all the way
    // round by default, a fan centred on the part's own direction otherwise. Each copy is
    // turned to face out. The count may be a range: the spacing follows it, which a turning
    // row with a step worked out by hand cannot do.
    bool  rowTaperStep = false;  // the step shrinks with the copies
    float rowRing = 0.0f;
    float rowSpread = 360.0f;
    // How far a pivot moved the part, kept apart from `at` for a ring row so each copy
    // swings about its own joint rather than all of them about the first one's.
    Vector2 pivotShift = { 0.0f, 0.0f };

    // The point `angle` turns the part about, in the part's own frame before it is turned
    // (so [-0.5, 0] on a bar of length 1 is its left end): a crane jib or a clamp jaw swings
    // about its joint rather than about its middle.
    bool    hasPivot = false;
    Vector2 pivot = { 0.0f, 0.0f };

    // A row centred on its own `at` rather than starting there, so a row whose count is a
    // range stays balanced instead of growing off one end.
    bool rowCentred = false;

    // A section of the silhouette (#240 phase 3): drawn like any part, and it exposes
    // sockets a kit places modules on. `pitch` is the socket spacing, in object radii.
    bool  section = false;
    float pitch = 0.0f;

    // Only the reflected copy of a mirrored part: how a kit places the twin of a module on a
    // bilateral object, so the module's own shape is reflected and not merely turned.
    bool mirrorOnly = false;
};

// A place on a section where a module can go: an `edge` along a long side, an `end`, a
// `top` along a centreline or an inner ring, a `ring` round a rim. Facing outward.
struct Socket
{
    Vector2     pos;
    float       angle;  // degrees, outward
    std::string type;
    int         section;  // which placed copy of which section
    int         line;     // sockets in one row share a line; modules spread along it
    int         index;
    float       size;  // the spacing, which bounds how big a module there may be
    // Which written section it came from, which repeat or mirror copy of it, and which line
    // within that copy: what a radial kit uses to put the same module on every arm.
    int source = 0;
    int copy = 0;
    int localLine = 0;
};

// One line of a kit: a module (by id, or any carrying a tag), how many, on which sockets.
struct KitEntry
{
    std::string of;
    bool        byTag = false;
    float       lo = 1.0f, hi = 1.0f;  // count, every whole number in it equally likely
    std::string on;                    // socket type; empty: the module's first
    float       scale = 0.0f;          // module radius; 0: from the socket spacing
    float       turn = 0.0f;           // degrees added to the socket's outward direction
    std::string variant;               // pinned; empty: the seed picks one for the whole line
    int         in = -1;               // only on this section (its index); -1: any
    // How far in from the socket the module's centre sits, in module radii: 1 puts the
    // whole module on the hull (a hatch, a window), 0 centres it on the edge, -1 hangs it
    // outside (a docking arm, a dish on a boom).
    float inset = 1.0f;
};

// What an archetype says about its modules instead of placing them (#240 phase 3): the
// seed decides where, within these rules -- modules go in even rows along one line, in
// mirrored pairs when the object is bilateral, and each section keeps `plain` of its
// sockets empty. That last rule is the one against mush.
struct Kit
{
    // "bilateral" mirrors every placement across the axis; "radial" repeats it on every
    // copy of a repeated section, and a count is then per copy; "none" places freely.
    std::string           symmetry = "none";
    float                 plain = 0.4f;
    std::vector<KitEntry> entries;
};

// The sockets of a set of (resolved) sections, in a fixed order.
std::vector<Socket> Sockets(const std::vector<Part>& sections);

// The module parts a kit places on these sections for this seed.
std::vector<Part> PlaceKit(const Kit& kit, const std::vector<Part>& sections, int seed);

// The part as this object has it: ranges chosen, a colour picked, and false when the part's
// chance says it is not there. Same seed and salt, same answer, on every client (#240).
//
// `rolls` holds one value in [0, 1) per variable of the shape the part belongs to (see
// RollVars); without it a part that names a variable rolls on its own.
bool Resolve(const Part& p, int seed, int salt, Part& out, const float* rolls = nullptr);

struct Shape
{
    std::vector<Part> parts;

    // Named values rolled once per object and shared by every part that names them (#240):
    // `"vars": { "tubes": [2, 6], "paint": [[200, 80, 40], [60, 90, 160]] }`, then
    // `"count": "$tubes"` on the tubes and on their caps, `"tint": "$paint"` on both
    // stripes. Without them every range is rolled on its own, and a wing's hull and its
    // trim come out at two different sweeps. In a module the roll is per placed copy.
    struct Var
    {
        std::string        name;
        float              lo = 0.0f, hi = 0.0f;
        std::vector<Color> palette;  // set for a colour variable
    };
    std::vector<Var> vars;

    Kit kit;

    // How far the body's north pole is tipped toward the viewer, in degrees. It is a
    // property of the body rather than of a part -- every feature on one planet shares
    // its axis -- and it is what makes a latitude band *curve*. Seen exactly edge-on a
    // band is a straight chord and the planet reads as a disc with stripes; tipped a
    // little, the bands bow, and it reads as a ball.
    float axisTilt = 18.0f;

    bool Empty() const { return parts.empty(); }
};

// One roll per variable of the shape, for this object and this copy.
std::vector<float> RollVars(const Shape& s, int seed, int salt);

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

    // 0..1: the part's own solidity and, if it blinks, where it is in its cycle. Folded
    // into one number because a backend does the same thing with both -- one field to
    // multiply by unconditionally rather than two it has to remember to combine.
    float brightness = 1.0f;

    // Where this piece sits front to back: negative is behind the body, positive in front,
    // zero for everything that is simply part of it. Compose returns pieces already sorted
    // by it, so a backend draws them in order and never has to know why.
    float depth = 0.0f;

    // How much a round piece is flattened toward the body's centre, 1 for not at all. A
    // crater facing the viewer is a circle; one near the limb is seen at a slant and is an
    // ellipse whose short axis points at the centre -- that one multiply is most of what
    // makes a textured sphere look spherical (#166). `angle` gives the short axis's
    // direction.
    float squash = 1.0f;

    // Lies on the body's surface, so it is lit as the body: with the body's centre and
    // radius rather than its own. A crater is a mark on a planet, not a small planet of
    // its own sitting in front of it.
    bool    surface = false;
    Vector2 bodyPos = { 0.0f, 0.0f };
    float   bodyRadius = 0.0f;

    // Faded out on the lit side of the body it belongs to; the backend knows the light.
    bool onlyDark = false;

    // A band is not a primitive: it is the visible part of a latitude strip, projected.
    // Stored as a strip -- upper edge and lower edge alternating -- in world coordinates.
    std::vector<Vector2> strip;

    Color tint = { 0, 0, 0, 0 };  // the part's own colour, alpha 0 for the object's
    float arcFrom = 0.0f;         // Arc, world degrees
    float arcTo = 90.0f;

    float tip = 0.0f;     // Chevron: the narrow end's width over the base's; 0 is a point
    float jagged = 0.0f;  // Polygon: how far corners are pulled in, 0..1
    int   jagSeed = 0;    // ...and which corners, the same every frame
    bool  soft = false;   // a glow fading to its rim, never shaded
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

    // Seconds, for anything that moves (#136). A double: it is the world's clock (#192),
    // and a float counting a week of seconds is a quarter of a second coarse.
    double time = 0.0;
    bool   thrusting = false;  // whether the object's engine is burning
};

// Places a shape on an object: applies the repeats and the mirror, the object's own
// heading, its size and position, whatever the clock is doing to it, drops the parts too
// small or too still to be worth drawing, and perturbs what the shape allows.
std::vector<Piece> Compose(const Shape& s, const Pose& pose);

}  // namespace Render
