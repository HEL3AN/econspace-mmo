#pragma once

#include "economy/Resource.h"
#include "render/Silhouette.h"
#include <nlohmann/json.hpp>
#include <map>
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
// NPCs fly designs (#279 step 4): a faction's doctrine names the designs each NPC role flies,
// and an NPC's speed, hull and guns are its design's. So do players (step 5): the hangar is a
// list of designs, and GetShipCatalog() is read off it.
namespace Ships
{
// What a part contributes. Each is summed over the ship; a part may contribute nothing.
struct Provides
{
    float thrust = 0.0f;  // main drive force; over mass it is the acceleration
    float rcs = 0.0f;     // attitude force; over mass it is the stabiliser's pull and the turn
    float cargo = 0.0f;   // hold, in units of cargo
    float mining = 0.0f;  // ore per second
    float hull = 0.0f;    // structure points: what a hit takes away before the ship is lost
    float damage = 0.0f;  // what one volley of its guns takes off a ship
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
    // The share of its top speed an NPC flies its rounds at. A ship on a job is not racing,
    // and today's traffic moves at about half what the same hull does under a player.
    float cruise = 0.5f;
    // What every ship has in a fight beyond what its parts give (#279 step 5): this much more
    // structure, and a gun of this much a shot that fitted guns add to. They are what every
    // player hull had before ships were designs -- 100 hull, 16 a shot -- less what the frames
    // already give. A ship has them whoever flies it: one combat model for NPCs and players
    // (owner, 2026-10-22, #186), so what differs between the two is the fit.
    float baseHull = 0.0f;
    float baseGun = 0.0f;
};

// A hull a station sells to players (#279 step 5). An account owns hulls by their place in
// the hangar list, so the order is the id: a hull is added at the end and never moved.
struct HangarEntry
{
    std::string design;
    // The list price in credits, before standing (ShipPriceMultiplier). What a station
    // charges for a design is the market's question, not the part list's: the first entry is
    // the starter, given to every pilot, and a fast hull is dear for being fast.
    float price = 0.0f;
};

// The NPC roles a doctrine names designs for, in the game's NpcRole order. The armed ones are
// those a design must carry guns for: a patrol that cannot shoot is a trader in a uniform.
inline constexpr const char* NPC_ROLES[] = { "trader", "miner",   "patrol",
                                             "pirate", "warship", "guard" };
bool                         ArmedRole(const std::string& role);

// What a faction flies (#279 step 4): per NPC role, the designs it builds for it. The doctrine
// called "default" names one for every role; a faction's own names only where it differs.
struct Doctrine
{
    std::string                                                   faction;  // id, or "default"
    std::vector<std::pair<std::string, std::vector<std::string>>> roles;    // role -> designs
};

// A weapon family (#464): the weapon modules that are one kind of gun, and the goods they are
// made of. A region's industry leans its NPCs toward a family by those goods; a design is
// refitted to a family by putting the family's first module where its guns were.
struct Family
{
    std::string               id;    // "laser"
    std::string               name;  // what a pilot reads: "lasers"
    std::vector<std::string>  modules;
    std::vector<ResourceType> goods;
};

// A hull line (#464): a way of building a hull, as the sections it prefers at each position,
// best first, and the goods it is made of. A design is rebuilt to a line by taking, position
// by position, the first of the line's sections the design still validates with -- else its
// own -- so a line changes what it can and never makes a ship that is not one.
struct Line
{
    std::string               id;  // "heavy"
    std::string               name;
    std::vector<std::string>  bow, mid, stern;
    std::vector<ResourceType> goods;
};

// A design derived from a stock one (#464): its guns those of `family`, its hull built to
// `line` (empty: its own). Made when the catalogue loads, on every side of the wire, so a
// client draws a variant an NPC flies without being sent it. The variant of a design's own
// family and own hull is the stock design itself, and is not one of these.
struct Variant
{
    Design      design;  // id "<base>~<family>" or "<base>~<family>~<line>"
    std::string base, family, line;
};

struct Catalogue
{
    Rules                    rules;
    std::vector<ModulePart>  modules;
    std::vector<Section>     sections;
    std::vector<Frame>       frames;
    std::vector<Design>      designs;
    std::vector<Doctrine>    doctrines;
    std::vector<HangarEntry> hangar;  // what players fly; the first is the starter
    std::vector<Family>      families;
    std::vector<Line>        lines;
    // Every variant of every stock design that carries a family's guns, in design, family,
    // line order (its own hull first).
    std::vector<Variant> variants;

    const ModulePart* FindModule(const std::string& id) const;
    const Section*    FindSection(const std::string& id) const;
    const Frame*      FindFrame(const std::string& id) const;
    // A stock design or a variant of one.
    const Design*  FindDesign(const std::string& id) const;
    const Family*  FindFamily(const std::string& id) const;
    const Line*    FindLine(const std::string& id) const;
    const Variant* FindVariant(const std::string& id) const;
    // The family whose module a design's first gun is; null for an unarmed design.
    const Family* FamilyOf(const Design& d) const;
    // The family a module is a gun of; null for a module that is in none.
    const Family* FamilyOfModule(const std::string& module) const;
    // The design `base` flies as with `family`'s guns on a hull of `line` (empty: its own):
    // the variant's id, `base` itself where that is the stock design, and empty where there
    // is no such variant -- an unarmed design, an unknown family or line.
    std::string VariantOf(const std::string& base, const std::string& family,
                          const std::string& line) const;

    // The designs `faction` flies in `role`: its own doctrine's, else the default's. Null for
    // a role nobody names, which a catalogue that loaded does not have.
    const std::vector<std::string>* DesignsFor(const std::string& faction,
                                               const std::string& role) const;
    // One of them, by `key` (an NPC's id), so a role with several designs flies all of them
    // and one ship keeps its design for life. Empty when there is none.
    std::string Pick(const std::string& faction, const std::string& role, unsigned key) const;
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
    float hull = 0.0f;    // what its parts give: frame, sections and armour
    float damage = 0.0f;  // what its fitted guns add to a volley; zero for an unarmed design
    float cruise = 0.0f;  // what an NPC flies its rounds at: maxSpeed * rules.cruise
    // What it is in a fight, under a player or a crew alike: hull + rules.baseHull, and what
    // one volley takes off a target, damage + rules.baseGun (Ships::Dps for it a second).
    float combatHull = 0.0f;
    float volley = 0.0f;
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

// The variant of `base` with `family`'s guns and a hull of `line` (null: its own), by the
// rules above. False, with the reason, where the result does not validate -- which, for a
// catalogue, is a load error: a family whose module has no look for a socket its guns are on.
bool MakeVariant(const Catalogue& c, const Design& base, const Family& family, const Line* line,
                 Design& out, std::string& error);

// A region's style (#464): the share of each weapon family and each hull line its NPCs fly,
// each list in catalogue order and summing to 1 (empty where the catalogue has none).
struct Style
{
    std::vector<std::pair<std::string, float>> families, lines;
    // The family or line with the largest share; empty for none.
    std::string LeadingFamily() const;
    std::string LeadingLine() const;
};

// The style of a region whose markets hold and make `supply` (units by good index) and
// whose people lean by `culture` (weights by family or line id; null for no lean): for each
// family, and each line, the share of its goods in the supply, times 1 - `cultureWeight`,
// plus its share of the culture's weights, times `cultureWeight`. Proportional -- twice the
// goods is twice the share -- and with no supply at all, the culture alone. A culture that
// names no family (or no line) leans evenly across them.
Style StyleFrom(const Catalogue& c, const std::vector<double>& supply,
                const std::map<std::string, double>* culture, double cultureWeight);

// The design's stats. False, with the reason, for a design that does not validate.
bool Derive(const Catalogue& c, const Design& d, Stats& out, std::string& error);

// What the design looks like, as the shape JSON an archetype carries (#279 step 3): its
// sections laid end to end along x, stern to bow, centred; their trim; and every fit line as
// a kit line that carries function (`"fit": true`) on the sections at its position, so the
// count on the hull is the count in the design. Read with Render::ParseShape, which is where
// a module the render library does not have is an error.
bool ShapeOf(const Catalogue& c, const Design& d, nlohmann::json& out, std::string& error);

// A design as data, the shape ships.json writes it in, and back (#279 fitting). What a server
// files and sends a client is exactly this, so the reader is the catalogue's own: a design
// from the wire with a field nobody knows is refused like one in the file (#191).
nlohmann::json ToJson(const Design& d);
bool           FromJson(const nlohmann::json& j, Design& out, std::string& error);

// A design a player made is filed under a hash of what it is (#279, option B): its frame,
// sections and fit -- not its id or its name -- so two pilots who arrive at the same fit
// fly one design, and an id names one design for good. "u:" and eight hex digits, from a
// 32-bit FNV-1a of the canonical JSON: the same on every compiler, which is the point.
// A stock design (one in ships.json) keeps the id the file gives it.
std::string ContentId(const Design& d);
// Whether an id is one ContentId makes, rather than a stock design's.
bool IsPlayerDesign(const std::string& id);

// How a design's hull measures against its frame's class rule.
struct Silhouette
{
    float length = 0.0f, width = 0.0f;
    float massAt = 0.0f;  // where the hull's area is centred: -1 the stern, 0 the middle, 1 the bow
};
Silhouette Measure(const Catalogue& c, const Design& d);
}  // namespace Ships
