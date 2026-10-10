#pragma once

#include "economy/Resource.h"
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
};

struct FitLine
{
    std::string module;  // a ModulePart id
    std::string in;      // the position whose sections carry it: "bow", "mid", "stern"
    std::string on;      // the socket kind: "top", "edge", "end", "side", ...
    int         count = 1;
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
// drives sit in the stern, and what goes on a side or an edge comes in pairs.
bool Validate(const Catalogue& c, const Design& d, std::string& error);

// The design's stats. False, with the reason, for a design that does not validate.
bool Derive(const Catalogue& c, const Design& d, Stats& out, std::string& error);
}  // namespace Ships
