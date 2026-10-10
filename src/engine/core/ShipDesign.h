#pragma once

#include "economy/Resource.h"
#include "render/Silhouette.h"
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

// A ship is a design, and what it can do is read off its parts (#279, M8 groundwork).
//
// A design is a frame, one section per position the frame offers -- a bow, one or more mids,
// a stern -- and a fit: modules on the sockets those sections expose, each with a pinned
// count. Its stats are a pure function of that and of the part catalogue in data/ships.json:
// mass is the sum of the parts, acceleration is the drives' thrust over the mass, the hold is
// what the sections and pods hold. See documents/ship_design.md.
//
// What a part does lives here, apart from what it looks like (render/Modules), for one reason:
// a module's *variant* is the seed's choice and a kit line's count may be too, and neither may
// ever change what a ship can do. So function is written once per module, never per variant,
// and a count that carries function is pinned in the design rather than drawn from the seed.
// Two Haulers of one design carry the same hold whichever hatches they wear.
//
// Nothing in the game reads these stats yet: GetShipCatalog() still holds the hand-typed
// numbers, and a test holds the derived ones within 10% of them until the switch.
namespace Ships
{
// What a part contributes. Each is summed over the ship; a part may contribute nothing.
struct Provides
{
    float thrust = 0.0f;  // main drive force; over mass it is the acceleration
    float rcs = 0.0f;     // attitude force; over mass it is the stabiliser's pull and the turn
    float cargo = 0.0f;   // hold, in units of cargo
    float mining = 0.0f;  // ore per second
};

using Cost = std::vector<std::pair<ResourceType, int>>;

// What a module does, whichever of its variants is drawn. `id` is the module's id in the
// render library ("hull.engine").
struct ModulePart
{
    std::string id;
    float       mass = 0.0f;
    Provides    provides;
    Cost        cost;
    // How it meets each kind of socket when a design is drawn: socket kind -> the kit line's
    // look ({ "mount", "turn", "scale", "z" }). An engine stands out of the stern, turned aft
    // and under the hull; that is true of every design that fits one, so it is said once.
    std::vector<std::pair<std::string, nlohmann::json>> look;
};

// A section of a ship: a piece of hull that stands at one position of a frame and exposes
// sockets, each kind with how many modules it takes.
struct Section
{
    std::string                              id;
    std::string                              position;  // "bow", "mid" or "stern"
    float                                    mass = 0.0f;
    Provides                                 provides;
    Cost                                     cost;
    std::vector<std::pair<std::string, int>> sockets;  // kind -> capacity
    // What it is drawn as, in its own frame, +x forward (#279 step 3): `sections` are its
    // hull -- fixed, mirrored, no module and nothing the seed rolls, so the class rule is
    // checked on the hull every ship of the design has -- `kit` the trim that goes on them
    // (`in` counts this section's own hull parts) and `parts` anything else. A frame lays
    // its sections end to end, stern to bow, and that is the ship.
    nlohmann::json            shape;
    std::vector<Render::Part> hull;                     // `sections`, read
    float                     aft = 0.0f, fore = 0.0f;  // how far the hull reaches along x
};

// A frame is a hull class (Proposal A's interceptor, frigate, hauler, barge...): the spine
// the sections hang on, and how many mid sections it takes.
struct Frame
{
    std::string id;
    std::string hullClass;
    float       mass = 0.0f;
    Provides    provides;
    Cost        cost;
    int         minMids = 1;
    int         maxMids = 1;
    // The class's silhouette rule (Proposal A), checked on every design's hull: at least this
    // long for its width, so no capsule or oval is a main body, and its mass aft of the middle
    // -- the drives are the heaviest thing on a ship -- unless the class is the one that
    // carries its work in front of it.
    float minAspect = 1.0f;
    bool  bowHeavy = false;
};

struct FitLine
{
    std::string module;  // a ModulePart id
    std::string in;      // the position whose sections carry it: "bow", "mid", "stern"
    std::string on;      // the socket kind: "top", "edge", "stern", "side", ...
    int         count = 1;
    float       scale = 0.0f;  // how big it is drawn; 0: the module's look for the socket
};

struct Design
{
    std::string              id;
    std::string              name;
    std::string              frame;
    std::string              bow;
    std::vector<std::string> mids;
    std::string              stern;
    std::vector<FitLine>     fit;
};

// The constants of the derivation, in data so they are tuned with the parts they apply to.
struct Rules
{
    float speedBase = 0.0f;  // top speed = speedBase + speedPerAccel * acceleration
    float speedPerAccel = 0.0f;
    float rcsPerTurn = 1.0f;           // turn rate (rad/s) = rcs acceleration / rcsPerTurn
    float buildSecondsPerMass = 0.0f;  // build time = perMass * mass + perPart * parts
    float buildSecondsPerPart = 0.0f;
    float plain = 0.5f;  // the share of a drawn ship's sockets its trim leaves empty
};

struct Catalogue
{
    Rules                   rules;
    std::vector<ModulePart> modules;
    std::vector<Section>    sections;
    std::vector<Frame>      frames;
    std::vector<Design>     designs;

    const ModulePart* FindModule(const std::string& id) const;
    const Section*    FindSection(const std::string& id) const;
    const Frame*      FindFrame(const std::string& id) const;
    const Design*     FindDesign(const std::string& id) const;
};

// What a design can do. Field names follow the game's ShipStats where they mean the same.
struct Stats
{
    float mass = 0.0f;
    float acceleration = 0.0f;  // ShipStats::thrustPower
    float maxSpeed = 0.0f;
    float rcsAccel = 0.0f;
    float turnSpeed = 0.0f;  // rad/s
    int   cargoCapacity = 0;
    float miningRate = 0.0f;
    Cost  cost;       // every part's, summed, in ResourceType order
    int   parts = 0;  // frame, sections and every fitted module counted once each
    float buildSeconds = 0.0f;
};

// Reads a catalogue. Every design in it is validated, so a catalogue that loads holds only
// designs that derive. Any unknown field, anywhere, is an error (#191).
bool Parse(const nlohmann::json& j, Catalogue& out, std::string& error);
bool Load(const std::string& path, Catalogue& out, std::string& error);

// Whether a design can be built from the catalogue's parts: every name exists, each section
// stands where the frame puts it, every module is placed -- a fit that runs out of sockets is
// an invalid design, never a quietly weaker ship -- and the rules a readable ship keeps hold:
// drives sit in the stern, what goes on a side or an edge comes in pairs, and the hull keeps
// its frame's class rule (Measure).
bool Validate(const Catalogue& c, const Design& d, std::string& error);

// The design's stats. False, with the reason, for a design that does not validate.
bool Derive(const Catalogue& c, const Design& d, Stats& out, std::string& error);

// What the design looks like, as the shape JSON an archetype carries (#279 step 3): its
// sections laid end to end along x, stern to bow, centred; their trim; and every fit line as
// a kit line that carries function (`"fit": true`) on the sections at its position, so the
// count on the hull is the count in the design. Read with Render::ParseShape, which is where
// a module the render library does not have is an error.
bool ShapeOf(const Catalogue& c, const Design& d, nlohmann::json& out, std::string& error);

// How a design's hull measures against its frame's class rule.
struct Silhouette
{
    float length = 0.0f, width = 0.0f;
    float massAt = 0.0f;  // where the hull's area is centred: -1 the stern, 0 the middle, 1 the bow
};
Silhouette Measure(const Catalogue& c, const Design& d);
}  // namespace Ships
