// The galaxy itself: seeding it, materializing a system's NPCs from its aggregate when it
// warms and letting them go when it cools, planning a route across it, and persistence.
//
// One translation unit of Simulation (#17).

#include "sim/Simulation.h"
#include "sim/Names.h"
#include "sim/SaveFile.h"

#include "core/Route.h"
#include "core/Blueprint.h"
#include "core/Orbits.h"
#include "core/World.h"
#include "entities/AsteroidField.h"
#include "entities/Derelict.h"
#include "entities/Structure.h"
#include "entities/JumpGate.h"
#include "entities/NpcShip.h"
#include "entities/Ship.h"
#include "entities/Station.h"

#include "gen/Galaxy.h"
#include "gen/Pack.h"
#include "gen/Pins.h"
#include "gen/Region.h"
#include "gen/Rng.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <set>

namespace
{
// What a system's ships are drawn from when it warms (#295): the system and the second.
constexpr uint64_t HYDRATE_KEY = 0x4D7A7Eu;
// How far before a station or a gate a ship that flew there is seen: it drops out of warp
// this far short and flies the rest (NpcShip::Update).
constexpr float APPROACH = 3000.0f;
// Not to be dropped closer than this to a player (as the spawn director keeps).
constexpr float CLEAR_OF_PLAYERS = 2600.0f;

bool Clear(Vector2 at, const std::vector<Vector2>& avoid)
{
    for (Vector2 a : avoid)
    {
        const float dx = at.x - a.x, dy = at.y - a.y;
        if (dx * dx + dy * dy < CLEAR_OF_PLAYERS * CLEAR_OF_PLAYERS)
            return false;
    }
    return true;
}
}  // namespace

void Simulation::InitGalaxy()
{
    for (const auto& info : universe_.systems)
    {
        SystemState& st = systems_[info.id];  // creates a cold skeleton (no entities)
        st.id = info.id;
        if (!st.agg.seeded)
            SeedAggregate(st, info);
    }
    PriceMarkets();
}

void Simulation::PriceMarkets()
{
    // Out from every centre at once, a jump at a time: each system's count is its shortest
    // supply route to the nearest one. In the order systems are listed, so the same world
    // counts the same way.
    remoteness_.clear();
    std::vector<std::string> frontier;
    for (const auto& info : universe_.systems)
        if (InCentre(info.id))
        {
            remoteness_[info.id] = 0;
            frontier.push_back(info.id);
        }
    for (size_t i = 0; i < frontier.size(); i++)
    {
        const int next = remoteness_[frontier[i]] + 1;
        for (const std::string& n : Neighbors(frontier[i]))
            if (remoteness_.emplace(n, next).second)
                frontier.push_back(n);
    }
    for (auto& kv : systems_)
    {
        const int jumps = Remoteness(kv.first);
        for (ResourceType t : AllResourceTypes())
            kv.second.market.SetBasePrice(t, Market::DefaultBasePrice(t) *
                                                 RemotenessFactor(SupplyOf(t), jumps));
    }
    marketsStale_ = false;
}

int Simulation::Remoteness(const std::string& systemId) const
{
    const auto it = remoteness_.find(systemId);
    return it == remoteness_.end() ? -1 : it->second;
}

FactionId Simulation::NativesOf(int slot) const
{
    FactionId  f;
    const auto it = natives_.find(slot);
    if (it != natives_.end())
        Factions::Find(nlohmann::json::parse(it->second).value("id", std::string()), f);
    return f;
}

// Starting population of a system from its base security (heuristics close to the
// former role-based spawn, but in aggregate numbers).
void Simulation::SeedAggregate(SystemState& st, const WorldLoader::SystemInfo& info)
{
    float            sec = info.security;
    SystemAggregate& a = st.agg;
    a.baseSecurity = sec;
    a.security = sec;
    a.prosperity = sec;
    a.traders = 2.0f + sec * 4.0f;
    a.miners = 2.0f;
    a.police = roundf(sec * 4.0f);
    a.pirates = std::max(0.0f, roundf((0.7f - sec) * 8.0f));
    a.controller = Factions::Unaffiliated();
    if (!info.owner.empty() && !Factions::Find(info.owner, a.controller))
        TraceLog(LOG_WARNING, "Universe: %s is owned by '%s', which is not a faction -- held by %s",
                 info.id.c_str(), info.owner.c_str(), Factions::Id(a.controller).c_str());
    // Beyond a wormhole nobody has been; a civilisation's own space is charted (#342 S2).
    a.visited = regionDocs_.count(info.id) == 0 || InCentre(info.id);
    a.claimed = !info.owner.empty();
    SeedPresence(a);
    a.seeded = true;
}

const std::vector<std::string>& Simulation::Neighbors(const std::string& id) const
{
    static const std::vector<std::string> kNone;
    const auto                            it = neighbors_.find(id);
    return it == neighbors_.end() ? kNone : it->second;
}

void Simulation::IndexLinks()
{
    // In the order the links are listed, as the scan this replaces returned them: the
    // faction step breaks ties by whichever neighbour it met first.
    neighbors_.clear();
    marketsStale_ = true;  // a route to a centre may be shorter or longer now
    for (const auto& l : universe_.links)
    {
        neighbors_[l.a].push_back(l.b);
        if (l.b != l.a)
            neighbors_[l.b].push_back(l.a);
    }
}

std::string Simulation::SystemName(const std::string& id) const
{
    auto named = systems_.find(id);
    if (named != systems_.end() && !named->second.agg.givenName.empty())
        return named->second.agg.givenName;
    for (const auto& s : universe_.systems)
        if (s.id == id)
            return s.name;
    return id;
}

// Materialize (hydrate) the NPCs of a system from its cold aggregate (#295): as many ships
// of each role as the aggregate holds, under the flags the spawn director would give them,
// where ships of that role spend their time -- the last stretch of a lane before a station
// or a gate (a long leg is crossed at warp, so that is where they are seen), over a belt,
// out on the periphery. Drawn from the system and the world second, not from the order
// systems happen to be visited in: two players arriving in the same second find the same
// sky. Pirates keep clear of whoever is already standing here.
void Simulation::HydrateSystem(SystemState& st)
{
    // Stable ids for the static objects (stations/planets/gates/fields) from JSON —
    // for snapshots/selection over the network. NPCs get an id on spawn (SpawnNpcInto).
    for (auto& e : st.entities)
        if (e->GetId() == 0)
            e->SetId(NextAgentId());

    const SpawnNodes       nd = GatherNodes(st);
    const Population       pop = PopulationOf(st, RoomOf(nd));
    const SystemAggregate& agg = st.agg;
    Gen::Rng               rng(Gen::Key(HYDRATE_KEY, Intelligence::KeyOf(st.id),
                                        (uint64_t)std::max(0.0, std::floor(time_))));
    auto                   pick = [&](const std::vector<Vector2>& v) -> Vector2
    { return v[rng.Range(0, (int)v.size() - 1)]; };
    // Somewhere within `r` of `at`, in any direction.
    auto around = [&](Vector2 at, float r) -> Vector2
    {
        const float turn = (float)(rng.Unit() * 2.0 * PI);
        const float d = (float)rng.Between(0.0, (double)r);
        return Vector2{ at.x + d * std::cos(turn), at.y + d * std::sin(turn) };
    };

    // Traders cruise the lanes (stations + gates).
    std::vector<Vector2> lanes = nd.stations;
    lanes.insert(lanes.end(), nd.gates.begin(), nd.gates.end());
    if (lanes.size() >= 2)
    {
        FactionId tradeFactions[] = { pop.tradeFaction, Factions::Unaffiliated(),
                                      Factions::Traders() };
        const int traders = (int)roundf(agg.traders);
        for (int i = 0; i < traders; i++)
            SpawnNpcInto(st, around(pick(lanes), APPROACH), tradeFactions[i % 3], NpcRole::Trader,
                         lanes, i);
    }

    // Miners — at the fields.
    if (!nd.fields.empty())
    {
        const int miners = (int)roundf(agg.miners);
        for (int i = 0; i < miners; i++)
        {
            Vector2              spot = pick(nd.fields);
            std::vector<Vector2> near = { spot };
            // Scattered over the belt, which is six thousand units across (#159), not on its
            // centre.
            SpawnNpcInto(st, around(spot, 3000.0f), Factions::Unaffiliated(), NpcRole::Miner, near,
                         i);
        }
    }

    // The law's police patrol the whole system (lanes + fields).
    std::vector<Vector2> patrolRoute = lanes;
    patrolRoute.insert(patrolRoute.end(), nd.fields.begin(), nd.fields.end());
    if (patrolRoute.size() >= 2)
    {
        const int police = (int)roundf(agg.police);
        for (int i = 0; i < police; i++)
            SpawnNpcInto(st, around(pick(patrolRoute), APPROACH), pop.policeFaction,
                         NpcRole::Police, patrolRoute, i);
    }

    // Pirates — on the dark periphery and at gates into dangerous systems (not at peaceful
    // gates), and not on top of a player.
    std::vector<Vector2> hot = PirateSpots(nd);
    std::vector<Vector2> avoid;
    for (const auto& kv : sessions_)
        if (kv.second.systemId == st.id && kv.second.ship && !kv.second.IsDocked())
            avoid.push_back(kv.second.ship->GetPosition());
    if (!hot.empty())
    {
        const int pirates = (int)roundf(agg.pirates);
        for (int i = 0; i < pirates; i++)
        {
            const Vector2 base = pick(hot);
            Vector2       spot = around(base, 3000.0f);
            for (int attempt = 0; attempt < 4 && !Clear(spot, avoid); attempt++)
                spot = around(base, 3000.0f);
            std::vector<Vector2> patrol = { spot };
            SpawnNpcInto(st, spot, Factions::Pirates(), NpcRole::Pirate, patrol, i);
        }
    }
}

void Simulation::Warm(SystemState& st)
{
    st.warmUntil = std::max(st.warmUntil, time_ + COOL_AFTER);
    if (st.populated)
        return;
    // A cold system's bodies are placed once a coarse pass; ships are put among them where
    // they are now.
    Orbits::Place(st.entities, time_);
    HydrateSystem(st);
    st.populated = true;
    // The aggregate keeps fractions of a ship; the system now holds whole ones. Counted
    // here, so the next pass does not read the rounding as ships lost.
    RecountAgg(st);
    // A siege under way here is ships too, at the outpost (#295, slice 5), and so is a convoy
    // passing through (#342 S6).
    FieldSieges(st);
    FieldConvoys(st);
}

void Simulation::Cool(SystemState& st)
{
    // What is here is what the aggregate says from now on. The ships themselves go: nobody
    // is left to see them, and a system that warms again makes them afresh -- once the
    // haulers among them have unloaded, as they would have where nobody is (#343).
    DeliverCargo(st, true);
    RecountAgg(st);
    st.entities.erase(std::remove_if(st.entities.begin(), st.entities.end(),
                                     [](const std::unique_ptr<Entity>& e)
                                     { return e->GetKind() == EntityKind::Npc; }),
                      st.entities.end());
    st.defenceCooldown.clear();
    st.provokedBy.clear();
    st.provokedByNpc.clear();
    st.policeCalledUntil = 0.0;
    st.populated = false;
    for (auto& kv : plans_)  // a siege's warships went with the rest, and a convoy's ships
    {
        if (kv.second.target == st.id && !kv.second.OnTheWay())
            kv.second.fielded = 0;
        if (kv.second.fieldedIn == st.id)
        {
            kv.second.fielded = 0;
            kv.second.fieldedIn.clear();
            kv.second.hauler = 0;
        }
    }
    for (auto& kv : migrants_)  // ...and a ship turning for the gate is through it (#295)
        if (kv.second.from == st.id)
            kv.second.shipId = 0;
}

void Simulation::AttachRegion(uint64_t seed, const std::string& systemsDir, int systems)
{
    AttachRegion(seed, 0, universe_.startId, systemsDir, systems);
}

namespace
{
// data/pins.json applied to a region just made (#147), always after generation. Every pin
// that cannot apply is said, by name.
void ApplyPinsFile(Gen::Region& region, const std::string& path, uint64_t seed)
{
    std::ifstream pinsIn(path);
    if (!pinsIn.is_open())
        return;
    const nlohmann::json     pins = nlohmann::json::parse(pinsIn, nullptr, false);
    std::vector<std::string> problems;
    if (pins.is_discarded())
        problems.push_back("pins.json is not valid JSON");
    else
        Gen::ApplyPins(region, pins, seed, problems);
    for (const std::string& p : problems)
        TraceLog(LOG_WARNING, "Pins: %s -- not applied", p.c_str());
}
}  // namespace

bool Simulation::ParamsFor(int slot, const std::string& homeId, const std::string& systemsDir,
                           Gen::RegionParams& params, nlohmann::json& homeDoc) const
{
    const bool laidOut = !skeleton_.slots.empty();
    // What the slot is -- its distance from a centre decides its law and its riches (S2).
    params = laidOut ? Gen::SlotParams(skeleton_, slot) : Gen::RegionParams{};
    if (!laidOut)
    {
        params.pirates = Factions::Id(Factions::Pirates());
        params.unaffiliated = Factions::Id(Factions::Unaffiliated());
    }
    params.seed = regionSeed_;
    params.slot = slot;
    // A slot a region drifted away from (#342 S4) opens to another region: keyed by how many
    // times the slot has opened before, which the log says, so a world remade from its log
    // makes the same one. The first time, the slot's own.
    int before = 0;
    for (const RegionOpening& o : openings_)
        if (o.slot == slot && (o.state == "archived" || o.state == "returned"))
            before++;
    if (before > 0)
        params.seed = Gen::Key(regionSeed_, 0xD21F7u, (uint64_t)before);
    if (homeId.empty())
        return true;  // a centre opens from nowhere
    const WorldLoader::SystemInfo* home = nullptr;
    for (const auto& info : universe_.systems)
        if (info.id == homeId)
            home = &info;
    if (home == nullptr)
        return false;

    // Home's own document, so the wormhole is not placed in a planet's path: its file, or
    // what the generator made of it.
    const auto generatedHome = regionDocs_.find(home->id);
    homeDoc = nlohmann::json();
    if (generatedHome != regionDocs_.end())
        homeDoc = nlohmann::json::parse(generatedHome->second, nullptr, false);
    else
    {
        std::ifstream in(systemsDir + home->file);
        if (in.is_open())
            homeDoc = nlohmann::json::parse(in, nullptr, false);
    }
    params.homeId = home->id;
    params.homeMap = home->mapPos;
    // Only what never moves counts as known -- the hand-written systems -- so where a region
    // lies does not depend on which others opened before it. From a generated home it grows
    // the way the skeleton says its slot lies.
    for (const auto& info : universe_.systems)
        if (regionDocs_.count(info.id) == 0)
            params.knownMap.push_back(info.mapPos);
    if (generatedHome != regionDocs_.end() && Gen::SlotOf(home->id) >= 0)
    {
        double hx, hy, sx, sy;
        Gen::SlotMapPoint(regionSeed_, Gen::SlotOf(home->id), hx, hy);
        Gen::SlotMapPoint(regionSeed_, slot, sx, sy);
        params.bearing = { (float)(sx - hx), (float)(sy - hy) };
    }
    params.homeSystem = homeDoc.is_object() ? &homeDoc : nullptr;
    return true;
}

namespace
{
// data/pins.json beside a systems directory -- found by name, since the directory itself
// need not exist.
std::string PinsBeside(const std::string& systemsDir)
{
    std::string dir = systemsDir;
    while (!dir.empty() && (dir.back() == '/' || dir.back() == '\\'))
        dir.pop_back();
    const size_t cut = dir.find_last_of("/\\");
    return (cut == std::string::npos ? std::string() : dir.substr(0, cut + 1)) + "pins.json";
}
}  // namespace

bool Simulation::AttachRegion(uint64_t seed, int slot, const std::string& homeId,
                              const std::string& systemsDir, int systems)
{
    // One world, one seed: two seeds would be two galaxies sharing ids (#342).
    if (slot < 0 || openSlots_.count(slot) != 0 || (hasRegion_ && seed != regionSeed_))
        return false;
    // A centre opens from nowhere (OpenGalaxy); everything else from a home.
    const bool laidOut = !skeleton_.slots.empty();
    if (laidOut && (slot >= (int)skeleton_.slots.size() ||
                    skeleton_.slots[(size_t)slot].kind == Gen::SlotKind::Centre))
        return false;
    const uint64_t    keep = regionSeed_;
    Gen::RegionParams params;
    nlohmann::json    homeDoc;
    regionSeed_ = seed;
    if (homeId.empty() || !ParamsFor(slot, homeId, systemsDir, params, homeDoc))
    {
        regionSeed_ = keep;
        return false;
    }
    if (systems > 0)
        params.systems = systems;
    // As it was made when it opened, if it is sealed (S3); made now, and sealed, if not.
    Gen::Region region = MakeSealed(slot, params);
    // Then the hand-written exceptions, always after (#147) -- to the slot's own region, not
    // to one that drifted back into it (S4): the pins were written for what the slot makes.
    if (returning_ == nullptr)
        ApplyPinsFile(region, PinsBeside(systemsDir), seed);
    AddRegion(region);
    LogOpening(slot, homeId, "world", std::string());
    return true;
}

void Simulation::AddRegion(const Gen::Region& region)
{
    // Its natives first (#342 S5): its stations fly their flag, and its index names them as
    // the holders of their systems, so the registry must know them before either is read.
    if (!region.natives.empty())
    {
        Gen::RegisterNatives(region.natives);
        natives_[region.slot] = region.natives.dump();
    }
    // ...and an admin pack's own factions (S7), for the same reason.
    if (!region.factions.empty() || !region.relations.empty())
        Gen::RegisterPackFactions(region.factions, region.relations);
    for (const auto& s : region.systems)
    {
        WorldLoader::SystemInfo info;
        info.id = s["id"];
        info.name = s["name"];
        info.mapPos = { s["map"][0].get<float>(), s["map"][1].get<float>() };
        info.security = s["security"];
        info.owner = s["owner"];
        universe_.systems.push_back(info);  // no file: it lives in regionDocs_
    }
    for (const auto& l : region.links)
        universe_.links.push_back({ l[0].get<std::string>(), l[1].get<std::string>() });
    IndexLinks();
    // Kept as text: the header stays free of the JSON library, and a document is parsed
    // once, when its system is built. A region's documents are its own and are never
    // touched once made (Gen::RegionToJson is the whole of it), so a region can be kept as
    // it was when it opened.
    for (const auto& kv : region.documents)
        regionDocs_[kv.first] = kv.second.dump();
    // The wormhole's mouth belongs to the opening, not to home's region: it is kept beside
    // home -- a file or another region's document -- and added when home is built. A centre
    // has none: nothing leads into it.
    if (!region.homeId.empty() && !region.wormhole.empty())
        wormholes_[region.homeId].push_back(region.wormhole.dump());
    // An admin link's gate on the far side is kept the same way (S7), and the region it
    // stands in is as permanent as the pack: it never drifts away from under the link.
    for (const nlohmann::json& c : region.crossings)
        if (c.is_object() && c.contains("home") && c["home"].is_string() && c.contains("gate"))
        {
            wormholes_[c["home"].get<std::string>()].push_back(c["gate"].dump());
            packSlots_.insert(Gen::SlotOf(c["home"].get<std::string>()));
        }
    if (!region.pack.empty() || !region.starts.empty())
        packSlots_.insert(region.slot);
    hasRegion_ = true;
    openSlots_.insert(region.slot);
    AddStarts(region);
}

void Simulation::OpenCentre(int slot)
{
    Gen::RegionParams params;
    nlohmann::json    none;
    ParamsFor(slot, std::string(), std::string(), params, none);
    Gen::Region r = MakeSealed(slot, params);
    ApplyPinsFile(r, dataDir_ + "pins.json", regionSeed_);
    AddRegion(r);
    LogOpening(slot, std::string(), "world", std::string());
    if (r.heartId.empty())
        return;
    StartPoint sp;
    sp.systemId = r.heartId;
    for (const auto& s : r.systems)
        if (s.value("id", std::string()) == r.heartId)
        {
            sp.systemName = s.value("name", r.heartId);
            sp.owner = s.value("owner", std::string());
        }
    const auto doc = r.documents.find(r.heartId);
    if (doc != r.documents.end() && doc->second.contains("stations"))
        for (const auto& st : doc->second["stations"])
            if (sp.station.empty() && st.value("role", std::string()) == "TradeHub")
                sp.station = st.value("name", std::string());
    starts_.push_back(sp);
}

bool Simulation::OpenGalaxy(const Gen::WorldParams& params, const std::string& dataDir,
                            const std::vector<RegionOpening>& log)
{
    if (hasRegion_)
        return false;
    skeleton_ = Gen::GenerateGalaxySkeleton(params);
    regionSeed_ = params.seed;
    dataDir_ = dataDir;
    if (maxRegions_ <= 0 || maxRegions_ > (int)skeleton_.slots.size())
        maxRegions_ = (int)skeleton_.slots.size();
    // A new world opens what Gen::GenerateWorld opens, in its order -- the editor, galaxydump
    // and the tests make the same regions, because the rules are one (#140). A saved one
    // opens what its log says, in the order it opened (S3), each as it was sealed.
    std::vector<RegionOpening> plan = log;
    if (plan.empty())
    {
        std::vector<int> frontier;
        for (const Gen::Opening& o : Gen::InitialOpenings(skeleton_))
        {
            RegionOpening e;
            e.slot = o.slot;
            e.parent = o.from;
            plan.push_back(e);
            if (o.from >= 0)
                frontier.push_back(o.slot);
        }
        // Where a new world's system packs go (S7): decided before any region is made, so
        // each is generated with its system already in it.
        woven_.clear();
        packProblems_.clear();
        for (const auto& p : packs_)
            if (p->kind == "system")
            {
                const int slot = Gen::PlaceSystemPack(*p, skeleton_, frontier, packProblems_);
                if (slot >= 0)
                    woven_.push_back({ p->id, p->kind, p->hash, slot, std::string() });
            }
    }
    else if (!sealDir_.empty())
        for (const RegionOpening& e : plan)
        {
            std::ifstream in(SealedPath(sealDir_, e.slot));
            if (!in.is_open())
                continue;
            std::string text((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
            Gen::Region check;
            if (ReadSealedText(text, regionSeed_, check) && check.slot == e.slot)
                sealedIn_[e.slot] = std::move(text);
            else
                TraceLog(LOG_WARNING, "World: %s is not this world's region %d -- made again",
                         SealedPath(sealDir_, e.slot).c_str(), e.slot);
        }
    for (const RegionOpening& e : plan)
    {
        // A region that drifted away is in the archive, not the galaxy (S4): only its entry
        // stays, as history, and as the count of how often its slot has opened.
        if (e.state == "archived" || e.state == "returned")
        {
            openings_.push_back(e);
            continue;
        }
        const bool centre = e.slot >= 0 && e.slot < (int)skeleton_.slots.size() &&
                            skeleton_.slots[(size_t)e.slot].kind == Gen::SlotKind::Centre;
        if (centre)
            OpenCentre(e.slot);
        else if (e.by == "pack")
        {
            // An admin's region (S7): as sealed, or woven again from its pack.
            if (!OpenPackRegion(e.slot, e.why))
                continue;
        }
        else
        {
            std::string mouth = e.mouth;
            if (mouth.empty())
            {
                double tx, ty;
                Gen::SlotMapPoint(regionSeed_, e.slot, tx, ty);
                mouth = Gen::MouthSystem(RegionView(e.parent), tx, ty);
            }
            if (!AttachRegion(regionSeed_, e.slot, mouth, dataDir + "systems/"))
            {
                TraceLog(LOG_WARNING, "World: region %d (from %s) could not be opened", e.slot,
                         mouth.c_str());
                continue;
            }
        }
        // As the log says, when there is one: when it opened, from where, and why.
        RegionOpening&    now = openings_.back();
        const std::string mouth = now.mouth;
        const int         parent = now.parent;
        now = e;
        now.mouth = mouth;
        now.parent = parent;
    }
    // A new world's region packs, beside what it begins with (S7).
    if (log.empty())
        WeaveRegionPacks();
    // ...and one whose mouth collapsed is cut off from its parent (S4): no route, no gates.
    for (const RegionOpening& o : openings_)
        if (o.state == "drifted")
            CutMouth(o);
    sealedIn_.clear();
    hasRegion_ = true;
    return !starts_.empty();
}

bool Simulation::OpenSlotFrom(int slot, int from, const std::string& dataDir)
{
    if (openSlots_.count(from) == 0)
        return false;
    double tx, ty;
    Gen::SlotMapPoint(regionSeed_, slot, tx, ty);
    const std::string home = Gen::MouthSystem(RegionView(from), tx, ty);
    return !home.empty() && AttachRegion(regionSeed_, slot, home, dataDir + "systems/");
}

Gen::Region Simulation::RegionView(int slot) const
{
    // An open region as far as MouthSystem asks: which systems, and where they lie.
    Gen::Region view;
    view.slot = slot;
    for (const auto& info : universe_.systems)
        if (Gen::SlotOf(info.id) == slot && regionDocs_.count(info.id) != 0)
            view.systems.push_back(
                { { "id", info.id },
                  { "map", nlohmann::json::array({ info.mapPos.x, info.mapPos.y }) } });
    return view;
}

bool Simulation::InCentre(const std::string& systemId) const
{
    const int slot = Gen::SlotOf(systemId);
    return slot >= 0 && slot < (int)skeleton_.slots.size() &&
           skeleton_.slots[(size_t)slot].kind == Gen::SlotKind::Centre;
}

bool Simulation::IsStart(const std::string& systemId) const
{
    for (const StartPoint& s : starts_)
        if (s.systemId == systemId)
            return true;
    return false;
}

bool Simulation::ReadWorldSeed(const std::string& path, uint64_t& seed, int& generator,
                               int* centres)
{
    WorldHeader h;
    if (!ReadWorldHeader(path, h))
        return false;
    seed = h.seed;
    generator = h.generator;
    if (centres != nullptr)
        *centres = h.centres;
    return true;
}

void Simulation::MaterializeAllSystems(const std::string& systemsDir)
{
    materialized_ = true;
    for (const auto& info : universe_.systems)
        MaterializeSystem(info, systemsDir);
}

void Simulation::MaterializeSystem(const WorldLoader::SystemInfo& info,
                                   const std::string&             systemsDir)
{
    SystemState& st = systems_[info.id];  // created by InitGalaxy (aggregate already exists)
    st.id = info.id;
    if (st.entities.empty())
    {
        auto       doc = regionDocs_.find(info.id);
        const auto mouths = wormholes_.find(info.id);
        if (doc == regionDocs_.end() && mouths == wormholes_.end())
            st.entities = WorldLoader::LoadSystem(systemsDir + info.file);
        else
        {
            // A generated system (#140) or a hand-written one as written, plus the mouths
            // of the wormholes opened from it (#342).
            nlohmann::json sys;
            if (doc != regionDocs_.end())
                sys = nlohmann::json::parse(doc->second);
            else
            {
                std::ifstream in(systemsDir + info.file);
                sys = nlohmann::json::parse(in, nullptr, false);
            }
            if (!sys.is_object())
                sys = nlohmann::json::object();
            if (mouths != wormholes_.end())
                for (const std::string& gate : mouths->second)
                    sys["gates"].push_back(nlohmann::json::parse(gate));
            st.entities = WorldLoader::BuildSystem(sys);
        }
        WorldLoader::NamePlanets(st.entities, SystemName(info.id));  // (#259)
        // A wormhole's gates as its opening says they are now: unstable, anchored, or gone
        // with a mouth that collapsed (#342 S4). Before the ids, as if the data said so.
        DressCrossings(st);
        // Ids here rather than in HydrateSystem, in the same order as ever, because what a
        // save changed is replayed by id-bearing objects before any NPC exists.
        for (auto& e : st.entities)
            if (e->GetId() == 0)
                e->SetId(NextAgentId());
        KeyWorldObjects(st);
        ReplayChanges(st);
        st.profile = ProfileOf(st);
        // Whatever structures the data wrote or the save brought back, on the clock
        // StepStructures reads (#295).
        for (const auto& e : st.entities)
            ScheduleStructure(info.id, *e);
    }
    // Ships only where somebody is (#295): a system warms when a player arrives.
    if (allHot_)
        Warm(st);
}

namespace
{
// What a save keeps about an object beyond its description (#38): who owns it, whether a
// wreck has been searched, and how much of a structure has been shot off (#39). Empty for an
// object in the state it was made in.
nlohmann::json StateOf(const Entity& e)
{
    nlohmann::json s = nlohmann::json::object();
    if (!e.GetOwner().empty())
        s["owner"] = e.GetOwner();
    if (e.GetKind() == EntityKind::Derelict && static_cast<const Derelict&>(e).IsLooted())
        s["looted"] = true;
    // What a hauler's wreck still holds (#343), by good id.
    if (e.GetKind() == EntityKind::Derelict && static_cast<const Derelict&>(e).GetCargoAmount() > 0)
    {
        const Derelict& d = static_cast<const Derelict&>(e);
        s["cargo"] = { { Goods::Id(d.GetCargo()), d.GetCargoAmount() } };
    }
    // A belt mined below full (#348): what is left. An older build reads it full.
    if (e.GetKind() == EntityKind::Field)
    {
        const AsteroidField& f = static_cast<const AsteroidField&>(e);
        if (f.GetOre() < f.GetOreMax() - 0.005)
            s["ore"] = std::round(f.GetOre() * 100.0) / 100.0;
    }
    if (e.GetKind() == EntityKind::Structure && static_cast<const Structure&>(e).GetDamage() > 0.0f)
        s["damage"] = static_cast<const Structure&>(e).GetDamage();
    if (e.GetKind() == EntityKind::Structure &&
        !static_cast<const Structure&>(e).GetDefence().IsDefault())  // (#340)
        s["defence"] = DefenceJson(static_cast<const Structure&>(e).GetDefence());
    return s;
}

void ApplyState(Entity& e, const nlohmann::json& s)
{
    if (!s.is_object())
        return;
    e.SetOwner(s.value("owner", std::string()));
    if (e.GetKind() == EntityKind::Derelict && s.value("looted", false))
        static_cast<Derelict&>(e).SetLooted();
    if (e.GetKind() == EntityKind::Derelict && s.contains("cargo") && s["cargo"].is_object())
        for (auto c = s["cargo"].begin(); c != s["cargo"].end(); ++c)
        {
            ResourceType good;  // a good this build does not have is let go
            if (c.value().is_number_integer() && Goods::Find(c.key(), good))
                static_cast<Derelict&>(e).SetCargo(good, c.value().get<int>());
        }
    if (e.GetKind() == EntityKind::Field && s.contains("ore") && s["ore"].is_number())
        static_cast<AsteroidField&>(e).SetOre(s["ore"].get<double>());
    if (e.GetKind() == EntityKind::Structure)
        static_cast<Structure&>(e).SetDamage(s.value("damage", 0.0f));
    Defence d;  // rules it cannot read are the default: it fires back, as before #340
    if (e.GetKind() == EntityKind::Structure && s.contains("defence") &&
        DefenceFrom(s["defence"], d))
        static_cast<Structure&>(e).SetDefence(d);
}

// A faction by the id factions.json gives it, or false for one this build does not have:
// mapping anything it does not know to the unaffiliated faction would, for a save, hand one
// faction's knowledge to another.
bool FactionByName(const nlohmann::json& v, FactionId& f)
{
    return v.is_string() && Factions::Find(v.get<std::string>(), f);
}

// A faction by what a save wrote: its id, or -- in a world from before #355 -- its index in
// the order the factions were compiled in.
bool FactionInSave(const nlohmann::json& v, FactionId& f)
{
    if (v.is_number_integer())
        return Factions::FromLegacyIndex(v.get<int>(), f);
    return FactionByName(v, f);
}

// Each faction's presence, by id, and only where there is some: a region's natives (#342)
// make a world of many factions, each present in a few systems.
nlohmann::json PresenceJson(const PerFaction<float>& presence)
{
    nlohmann::json o = nlohmann::json::object();
    for (FactionId f : Factions::All())
        if (presence[f] != 0.0f)
            o[Factions::Id(f)] = presence[f];
    return o;
}

// False when the save has none. A version 10 world and older wrote a list of four, by the
// compiled index.
bool PresenceFrom(const nlohmann::json& j, PerFaction<float>& presence)
{
    presence.Clear();
    FactionId f;
    if (j.is_array() && j.size() == 4)
    {
        for (int i = 0; i < 4; i++)
            if (j[(size_t)i].is_number() && Factions::FromLegacyIndex(i, f))
                presence[f] = j[(size_t)i].get<float>();
        return true;
    }
    if (!j.is_object())
        return false;
    for (auto it = j.begin(); it != j.end(); ++it)
        if (it->is_number() && Factions::Find(it.key(), f))
            presence[f] = it->get<float>();
    return true;
}

const char* PlanKindName(Plan::Kind k)
{
    switch (k)
    {
        case Plan::Kind::Settle: return "settle";
        case Plan::Kind::Siege: return "siege";
        case Plan::Kind::Return: return "return";
        case Plan::Kind::Anchor: return "anchor";
        case Plan::Kind::Upgrade: return "upgrade";
        case Plan::Kind::Survey: break;
    }
    return "survey";
}
bool PlanKindFrom(const std::string& name, Plan::Kind& out)
{
    for (Plan::Kind k : { Plan::Kind::Survey, Plan::Kind::Settle, Plan::Kind::Siege,
                          Plan::Kind::Return, Plan::Kind::Anchor, Plan::Kind::Upgrade })
        if (name == PlanKindName(k))
        {
            out = k;
            return true;
        }
    return false;
}

nlohmann::json IntelJson(const Intel& i)
{
    nlohmann::json defenders = nlohmann::json::array(), outposts = nlohmann::json::array();
    for (FactionId d : i.defenders)
        defenders.push_back(Factions::Id(d));
    for (FactionId o : i.outposts)
        outposts.push_back(Factions::Id(o));
    return { { "seenAt", i.seenAt },
             { "traffic", i.traffic },
             { "security", i.security },
             { "belts", i.belts },
             { "wrecks", i.wrecks },
             { "stations", i.stations },
             { "planets", i.planets },
             { "gates", i.gates },
             { "defenders", defenders },
             { "outposts", outposts },
             { "presence", PresenceJson(i.presence) },
             { "controller", Factions::Id(i.controller) },
             { "claimed", i.claimed } };
}

Intel IntelFrom(const nlohmann::json& j)
{
    Intel i;
    i.seenAt = j.value("seenAt", 0.0);
    i.traffic = j.value("traffic", i.traffic);
    i.security = j.value("security", i.security);
    i.belts = j.value("belts", 0);
    i.wrecks = j.value("wrecks", 0);
    i.stations = j.value("stations", 0);
    i.planets = j.value("planets", 0);
    i.gates = j.value("gates", 0);
    if (j.contains("defenders") && j["defenders"].is_array())
        for (const auto& d : j["defenders"])
        {
            FactionId f;
            if (FactionByName(d, f))
                i.defenders.push_back(f);
        }
    if (j.contains("outposts") && j["outposts"].is_array())
        for (const auto& o : j["outposts"])
        {
            FactionId f;
            if (FactionByName(o, f))
                i.outposts.push_back(f);
        }
    PresenceFrom(j.value("presence", nlohmann::json()), i.presence);
    FactionId c;
    if (j.contains("controller") && FactionByName(j["controller"], c))
        i.controller = c;
    i.claimed = j.value("claimed", false);
    return i;
}
}  // namespace

nlohmann::json Simulation::ChangesOf(const SystemState& st) const
{
    using nlohmann::json;
    json added = json::array();
    json state = json::object();
    for (const auto& e : st.entities)
    {
        const auto key = st.keys.find(e->GetId());
        if (key == st.keys.end())
            continue;
        json now = StateOf(*e);
        if (key->second[0] == '+')
        {
            std::string array;
            json        object = WorldLoader::DescribeObject(*e, array);
            if (array.empty())
                continue;
            now["key"] = key->second;
            now["array"] = array;
            now["object"] = std::move(object);
            added.push_back(std::move(now));
        }
        else if (!now.empty())
            state[key->second] = std::move(now);
    }
    if (st.removedKeys.empty() && added.empty() && state.empty())
        return json();
    json c = json::object();
    if (!st.removedKeys.empty())
        c["removed"] = st.removedKeys;
    if (!state.empty())
        c["state"] = std::move(state);
    if (!added.empty())
        c["added"] = std::move(added);
    c["nextAdded"] = st.nextAddedKey;
    return c;
}

// Server world persistence: the galaxy (system aggregates) + time. The sup* "pressure"

// field is transient — not written (on load the world has "rested").
bool Simulation::SaveWorld(const std::string& path) const
{
    using nlohmann::json;
    json j;
    j["version"] = Save::WORLD_VERSION;
    j["simTime"] = time_;
    // Which world this is, for the accounts made in it (#392). A field an older build
    // ignores, so no version bump (#20).
    if (!worldId_.empty())
        j["worldId"] = worldId_;
    // The regions are not in it: each is sealed in its own file when it opens, or remade
    // from these (#140, #342 S3).
    if (hasRegion_)
    {
        j["seed"] = regionSeed_;
        j["generator"] = Gen::GENERATOR_VERSION;
        if (!skeleton_.slots.empty())  // and how many civilisations it was made with (S2)
        {
            j["centres"] = (int)skeleton_.centres.size();
            j["skeleton"] = Gen::SKELETON_VERSION;
        }
    }
    // ...and which regions opened, when and from where, and how the tide stands (S3).
    SaveGrowth(j);
    json galaxy = json::object();
    for (const auto& kv : systems_)
    {
        const SystemAggregate& a = kv.second.agg;
        galaxy[kv.first] = {
            { "traders", a.traders },       { "miners", a.miners },
            { "police", a.police },         { "pirates", a.pirates },
            { "security", a.security },     { "baseSecurity", a.baseSecurity },
            { "prosperity", a.prosperity }, { "controller", Factions::Id(a.controller) },
            { "visited", a.visited },       { "presence", PresenceJson(a.presence) },
            { "claimed", a.claimed },       { "discoverer", a.discoverer },
            { "name", a.givenName }
        };
        // What its market holds, by good id (#343): only where there is one. A field an older
        // build ignores, and an older file's markets start at their targets.
        if (kv.second.profile.markets > 0)
        {
            json stock = json::object();
            for (ResourceType g : AllResourceTypes())
                stock[Goods::Id(g)] = std::round(kv.second.market.Stock(g) * 100.0) / 100.0;
            galaxy[kv.first]["stock"] = std::move(stock);
        }
    }
    j["galaxy"] = galaxy;

    // What each faction knows, what it has under way, and the history so far (#295).
    // Knowledge is written only once there is some: a world saved before its first macro
    // pass seeds it on load, as an older save does.
    if (mindsSeeded_)
    {
        json minds = json::object();
        for (FactionId f : Factions::All())
        {
            json intel = json::object();
            // Not what it sees for itself -- its holdings and where its ships are: the
            // faction looks at those again before it next decides anything, and in a
            // region the pirates have overrun they are most of the file.
            for (const auto& kv : minds_[f].intel)
            {
                const auto st = systems_.find(kv.first);
                if (st != systems_.end() &&
                    ((st->second.agg.claimed && st->second.agg.controller == f) ||
                     st->second.agg.presence[f] >= 1.0f))
                    continue;
                intel[kv.first] = IntelJson(kv.second);
            }
            minds[Factions::Id(f)] = { { "intel", std::move(intel) },
                                       { "stock", minds_[f].stock },
                                       { "unsupplied", minds_[f].unsupplied },
                                       { "lostAt", minds_[f].lostAt } };
            // Word of a mouth it is still going by (#342 S6), by the region's slot.
            if (!minds_[f].expeditions.empty())
            {
                json heard = json::object();
                for (const auto& e : minds_[f].expeditions)
                    heard[std::to_string(e.first)] = e.second;
                minds[Factions::Id(f)]["expeditions"] = std::move(heard);
            }
        }
        j["factions"] = std::move(minds);
    }
    json plans = json::array();
    for (const auto& kv : plans_)
    {
        const Plan& p = kv.second;
        json        pj = { { "id", p.id },
                           { "kind", PlanKindName(p.kind) },
                           { "faction", Factions::Id(p.faction) },
                           { "from", p.from },
                           { "target", p.target },
                           { "startedAt", p.startedAt },
                           { "dueAt", p.dueAt } };
        if (!p.site.empty())
            pj["site"] = p.site;
        if (p.supplied > 0)
            pj["supplied"] = p.supplied;  // pilots' deliveries to its site (#345)
        if (p.kind == Plan::Kind::Siege)
        {
            pj["against"] = Factions::Id(p.against);
            pj["force"] = p.force;
            json lent = json::array();
            for (const Plan::Lent& l : p.lent)
                lent.push_back({ { "from", l.from }, { "force", l.force }, { "hops", l.hops } });
            pj["lent"] = std::move(lent);
            pj["via"] = p.via;
            pj["arrivesAt"] = p.arrivesAt;
            pj["arrived"] = p.arrived;
        }
        if (p.kind == Plan::Kind::Return)
            pj["force"] = p.force;
        // An Anchor's and an Upgrade's (#342 S6): what it builds, and its convoy while that is
        // on its way.
        if (p.kind == Plan::Kind::Anchor || p.kind == Plan::Kind::Upgrade)
        {
            pj["arrived"] = p.arrived;
            if (p.kind == Plan::Kind::Anchor)
                pj["slot"] = p.slot;
            else
                pj["blueprint"] = p.blueprint;
            if (p.OnTheWay())
            {
                json lent = json::array();
                for (const Plan::Lent& l : p.lent)
                    lent.push_back(
                        { { "from", l.from }, { "force", l.force }, { "hops", l.hops } });
                pj["lent"] = std::move(lent);
                pj["force"] = p.force;
                pj["route"] = p.route;
                pj["leg"] = p.leg;
                pj["legAt"] = p.legAt;
                pj["freight"] = p.freight;
                pj["setbacks"] = p.setbacks;
                if (p.waits > 0)
                    pj["waits"] = p.waits;  // held back or turned back (#407)
            }
        }
        plans.push_back(std::move(pj));
    }
    j["plans"] = std::move(plans);
    j["nextPlan"] = nextPlanId_;
    // Ships on their way between systems (#295): counted nowhere else, so lost if not here.
    json migrants = json::array();
    for (const auto& kv : migrants_)
    {
        const Migrant& m = kv.second;
        migrants.push_back({ { "from", m.from },
                             { "to", m.to },
                             { "role", NpcRoleId(m.role) },
                             { "faction", Factions::Id(m.faction) },
                             { "leftAt", m.leftAt },
                             { "arrivesAt", m.arrivesAt } });
        // What a hauler carries (#343): on the trip and in no market, so lost if not here.
        if (m.cargo > 0)
            migrants.back()["cargo"] = { { Goods::Id(m.good), m.cargo } };
    }
    j["migrants"] = std::move(migrants);
    // What each faction's markets have taken in fees (#343), by faction id: only what is not
    // nothing. An older file has none, and its factions start with empty treasuries.
    json treasuries = json::object();
    for (FactionId f : Factions::All())
        if (minds_[f].treasury > 0.0)
            treasuries[Factions::Id(f)] = std::round(minds_[f].treasury * 100.0) / 100.0;
    if (!treasuries.empty())
        j["treasuries"] = std::move(treasuries);
    // The markets' open contracts (#343): what each owner set aside is in them, not in its
    // treasury, so it would be lost if they were not here.
    SaveContracts(j);
    json history = json::array();
    for (const ChronicleEntry& e : chronicle_)
        history.push_back({ { "seq", e.seq },
                            { "time", e.time },
                            { "kind", e.kind },
                            { "faction", e.faction ? Factions::Id(*e.faction) : std::string() },
                            { "system", e.system },
                            { "text", e.text } });
    j["chronicle"] = std::move(history);
    j["chronicleSeq"] = chronicleSeq_;

    // What players changed (#38). Stored as changes rather than as the systems themselves:
    // the region is remade from its seed (#140) and the hand-written systems from data, so
    // the save holds only what neither of them says.
    json changes = json::object();
    for (const auto& kv : systems_)
    {
        json c = ChangesOf(kv.second);
        if (!c.is_null())
            changes[kv.first] = std::move(c);
    }
    j["changes"] = std::move(changes);
    // What the world remembers of structures' owners while they are away (#340).
    if (!owners_.empty())
        j["owners"] = OwnersJson();

    // Beside the real file, then renamed over it, with older copies kept (#341): a crash
    // mid-write used to leave a truncated world that the next boot replaced with a new galaxy.
    return Save::WriteFileAtomic(path, j.dump(2) + "\n", WORLD_BACKUPS);
}

Save::Result Simulation::LoadWorld(const std::string& path)
{
    using nlohmann::json;
    std::ifstream in(path);
    if (!in.is_open())
        return Save::Result::Missing;
    json j = json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.contains("galaxy") || !j["galaxy"].is_object())
        return Save::Result::Corrupt;
    // A file from a later build is refused rather than read leniently. Reading it would
    // mean guessing at fields this build has never heard of, and the next checkpoint
    // would write the guess back over the original.
    if (j.value("version", Save::UNVERSIONED) > Save::WORLD_VERSION)
        return Save::Result::TooNew;

    Reset();
    InitGalaxy();  // skeletons with default aggregates for all systems
    time_ = j.value("simTime", 0.0);
    // A world from before #392 has none; the host gives it one.
    worldId_ = j.contains("worldId") && j["worldId"].is_string() ? j["worldId"].get<std::string>()
                                                                 : std::string();
    OwnersFrom(j.value("owners", json()));  // (#340)
    for (auto it = j["galaxy"].begin(); it != j["galaxy"].end(); ++it)
    {
        const std::string& sid = it.key();
        if (!HasSystem(sid))
            continue;
        SystemAggregate& a = systems_[sid].agg;
        const json&      gj = it.value();
        a.traders = gj.value("traders", a.traders);
        a.miners = gj.value("miners", a.miners);
        a.police = gj.value("police", a.police);
        a.pirates = gj.value("pirates", a.pirates);
        a.security = gj.value("security", a.security);
        a.baseSecurity = gj.value("baseSecurity", a.baseSecurity);
        a.prosperity = gj.value("prosperity", a.prosperity);
        FactionId controller;
        if (gj.contains("controller") && FactionInSave(gj["controller"], controller))
            a.controller = controller;
        a.visited = gj.value("visited", a.visited);
        a.claimed = gj.value("claimed", a.claimed);
        a.discoverer = gj.value("discoverer", a.discoverer);
        a.givenName = gj.value("name", a.givenName);
        if (gj.contains("stock") && gj["stock"].is_object())
            for (auto g = gj["stock"].begin(); g != gj["stock"].end(); ++g)
            {
                ResourceType good;  // a good this build does not have is let go
                if (g.value().is_number() && Goods::Find(g.key(), good))
                    systems_[sid].market.SetStock(good, g.value().get<double>());
            }
        // An older save has no presence: the seeded one stands, from what it did save.
        if (!PresenceFrom(gj.value("presence", json()), a.presence))
            SeedPresence(a);
        a.seeded = true;
    }

    // What factions knew (#295). An older save has none: the factions start again from
    // what they hold and what is next door, the first time they are stepped.
    if (j.contains("factions") && j["factions"].is_object())
    {
        mindsSeeded_ = true;
        for (auto it = j["factions"].begin(); it != j["factions"].end(); ++it)
        {
            FactionId f;
            if (!FactionByName(json(it.key()), f) || !it.value().is_object())
                continue;
            const json& intel = it.value().value("intel", json::object());
            for (auto k = intel.begin(); intel.is_object() && k != intel.end(); ++k)
                if (HasSystem(k.key()) && k.value().is_object())
                    minds_[f].intel[k.key()] = IntelFrom(k.value());
            // A version 5 world has no stock: its factions begin saving from nothing.
            const json& stock = it.value().value("stock", json(0.0));
            minds_[f].stock = stock.is_number() ? std::max(0.0f, stock.get<float>()) : 0.0f;
            const json& cut = it.value().value("unsupplied", json::array());
            for (const json& k : cut)
                if (k.is_string())
                    minds_[f].unsupplied.insert(k.get<std::string>());
            const json& lost = it.value().value("lostAt", json::object());
            for (auto l = lost.begin(); lost.is_object() && l != lost.end(); ++l)
                if (HasSystem(l.key()) && l.value().is_number())
                    minds_[f].lostAt[l.key()] = l.value().get<double>();
            const json& heard = it.value().value("expeditions", json::object());
            for (auto e = heard.begin(); heard.is_object() && e != heard.end(); ++e)
                if (e.value().is_number())
                    minds_[f].expeditions[std::atoi(e.key().c_str())] = e.value().get<double>();
        }
    }
    if (j.contains("plans") && j["plans"].is_array())
        for (const json& pj : j["plans"])
        {
            Plan              p;
            const std::string kind = pj.is_object() ? pj.value("kind", std::string()) : "";
            if (!PlanKindFrom(kind, p.kind) || !pj.contains("faction") ||
                !FactionByName(pj["faction"], p.faction))
                continue;
            p.id = pj.value("id", 0);
            p.from = pj.value("from", std::string());
            p.target = pj.value("target", std::string());
            p.startedAt = pj.value("startedAt", 0.0);
            p.dueAt = pj.value("dueAt", 0.0);
            p.site = pj.value("site", std::string());
            p.supplied = std::max(0, pj.value("supplied", 0));
            if (p.kind == Plan::Kind::Siege &&
                (!pj.contains("against") || !FactionByName(pj["against"], p.against)))
                continue;
            p.force = std::max(0.0f, pj.value("force", 0.0f));
            // A siege from before pooling (version 7) is already at its outpost, and lent by
            // nobody: what is left of it stays where it is, as it did then.
            p.arrived = pj.value("arrived", true);
            p.arrivesAt = pj.value("arrivesAt", p.startedAt);
            p.via = pj.value("via", p.from);
            const json& lent = pj.value("lent", json::array());
            for (const json& lj : lent)
                if (lj.is_object() && HasSystem(lj.value("from", std::string())))
                {
                    Plan::Lent l;
                    l.from = lj.value("from", std::string());
                    l.force = std::max(0.0f, lj.value("force", 0.0f));
                    l.hops = std::max(1, lj.value("hops", 1));
                    p.lent.push_back(l);
                }
            // An Anchor's or an Upgrade's convoy (#342 S6); one from before convoys is built.
            p.slot = pj.value("slot", -1);
            p.blueprint = pj.value("blueprint", std::string());
            p.leg = pj.value("leg", -1);
            p.legAt = pj.value("legAt", p.startedAt);
            p.freight = pj.value("freight", 1.0f);
            p.setbacks = pj.value("setbacks", 0);
            p.waits = std::max(0, pj.value("waits", 0));
            const json& route = pj.value("route", json::array());
            for (const json& r : route)
                if (r.is_string())
                    p.route.push_back(r.get<std::string>());
            bool lost = p.leg >= (int)p.route.size();
            for (const std::string& r : p.route)
                lost = lost || !HasSystem(r);
            if (p.OnTheWay() && lost)
            {
                // Its way is not this galaxy's any more: it sets out again.
                p.route.clear();
                p.leg = -1;
            }
            const bool sited = p.kind == Plan::Kind::Settle || p.kind == Plan::Kind::Siege ||
                               (p.kind == Plan::Kind::Anchor && p.arrived) ||
                               p.kind == Plan::Kind::Upgrade;
            if (p.id <= 0 || plans_.count(p.id) != 0 || !HasSystem(p.target) ||
                (sited && p.site.empty()) ||
                (p.kind == Plan::Kind::Upgrade && Blueprints::Find(p.blueprint) == nullptr))
                continue;
            plans_[p.id] = p;
            if (!p.OnTheWay())  // a convoy is stepped, not due
                due_.insert({ p.dueAt, p.id });
        }
    nextPlanId_ = std::max(1, j.value("nextPlan", 1));
    if (j.contains("treasuries") && j["treasuries"].is_object())
        for (auto it = j["treasuries"].begin(); it != j["treasuries"].end(); ++it)
        {
            FactionId f;
            if (FactionByName(json(it.key()), f) && it.value().is_number())
                minds_[f].treasury = std::max(0.0, it.value().get<double>());
        }
    ReadContracts(j);
    if (j.contains("migrants") && j["migrants"].is_array())
        for (const json& mj : j["migrants"])
        {
            Migrant m;
            if (!mj.is_object() || !mj.contains("faction") ||
                !FactionByName(mj["faction"], m.faction))
                continue;
            m.from = mj.value("from", std::string());
            m.to = mj.value("to", std::string());
            const std::string role = mj.value("role", std::string());
            bool              known = false;
            for (NpcRole r : { NpcRole::Trader, NpcRole::Miner, NpcRole::Police, NpcRole::Pirate })
                if (role == NpcRoleId(r))
                {
                    m.role = r;
                    known = true;
                }
            if (!known || !HasSystem(m.to))
                continue;
            m.leftAt = mj.value("leftAt", 0.0);
            m.arrivesAt = mj.value("arrivesAt", 0.0);
            if (mj.contains("cargo") && mj["cargo"].is_object())
                for (auto c = mj["cargo"].begin(); c != mj["cargo"].end(); ++c)
                    if (c.value().is_number_integer() && Goods::Find(c.key(), m.good))
                        m.cargo = std::max(0, c.value().get<int>());
            m.id = nextMigrant_++;
            migrants_[m.id] = m;
            arrivals_.insert({ m.arrivesAt, m.id });
        }
    for (const auto& kv : plans_)
        nextPlanId_ = std::max(nextPlanId_, kv.first + 1);
    if (j.contains("chronicle") && j["chronicle"].is_array())
        for (const json& ej : j["chronicle"])
        {
            if (!ej.is_object())
                continue;
            ChronicleEntry e;
            e.seq = ej.value("seq", 0LL);
            e.time = ej.value("time", 0.0);
            e.kind = ej.value("kind", std::string());
            FactionId f;
            if (ej.contains("faction") && FactionByName(ej["faction"], f))
                e.faction = f;
            e.system = ej.value("system", std::string());
            e.text = ej.value("text", std::string());
            chronicle_.push_back(std::move(e));
        }
    chronicleSeq_ = j.value("chronicleSeq", 0LL);
    for (const ChronicleEntry& e : chronicle_)
        chronicleSeq_ = std::max(chronicleSeq_, e.seq);
    LoadGrowth(j);

    // Before version 3 nothing a player changed was kept, so an older save has nothing to
    // replay and its world is the generated or written one, as it always was.
    if (j.value("version", Save::UNVERSIONED) >= 3 && j.contains("changes") &&
        j["changes"].is_object())
        for (auto it = j["changes"].begin(); it != j["changes"].end(); ++it)
        {
            if (!HasSystem(it.key()) || !it.value().is_object())
            {
                TraceLog(LOG_WARNING,
                         "World: changes to '%s', which this galaxy has not -- dropped",
                         it.key().c_str());
                continue;
            }
            SystemState& st = systems_[it.key()];
            json         c = it.value();
            // Before version 9 a structure's wreck lay for ever. Every wreck a save added is one
            // (nothing else adds them), and each is given its time from now: the clock starts
            // when the rule does, rather than taking away at once what a player may be flying
            // to.
            if (j.value("version", Save::UNVERSIONED) < 9 && c.contains("added") &&
                c["added"].is_array() && Blueprints::Rules().wreckSeconds > 0.0f)
                for (json& a : c["added"])
                    if (a.is_object() && a.value("array", std::string()) == "derelicts" &&
                        a.contains("object") && a["object"].is_object() &&
                        !a["object"].contains("expiresAt"))
                        a["object"]["expiresAt"] = time_ + Blueprints::Rules().wreckSeconds;
            if (c.contains("removed") && c["removed"].is_array())
                for (const json& k : c["removed"])
                    if (k.is_string())
                        st.removedKeys.insert(k.get<std::string>());
            st.nextAddedKey = std::max(1, c.value("nextAdded", 1));
            st.restore = c.dump();
        }
    return Save::Result::Ok;
}

void Simulation::Reset()
{
    systems_.clear();
    structureDue_.clear();
    agentIdCounter_ = 0;
    minds_.Clear();
    mindsSeeded_ = false;
    plans_.clear();
    migrants_.clear();
    arrivals_.clear();
    due_.clear();
    nextPlanId_ = 1;
    chronicle_.clear();
    chronicleSeq_ = 0;
    // The tide is the save's; the log and the sealed regions are the galaxy's (OpenGalaxy).
    tides_.clear();
    explored_.clear();
    surveyed_.clear();
    playersSeen_.clear();
    surplusTide_ = 0.0;
    growthPaused_ = false;
    tidePasses_ = 0;
    archives_.clear();  // the save's too (S4)
    nextArchive_ = 1;
}

SystemState* Simulation::SystemById(const std::string& id)
{
    auto it = systems_.find(id);
    return it == systems_.end() ? nullptr : &it->second;  // pointer in std::map is stable
}

const SystemState* Simulation::SystemById(const std::string& id) const
{
    auto it = systems_.find(id);
    return it == systems_.end() ? nullptr : &it->second;
}

const WorldLoader::SystemInfo* Simulation::SystemInfoById(const std::string& id) const
{
    for (const auto& si : universe_.systems)
        if (si.id == id)
            return &si;
    return nullptr;
}

// --- Route planning (#30) ----------------------------------------------------
//
// A jump today is a single hop: the client names one gate and the server checks it is
// close enough. "Fly to Verge" therefore meant planning the hops by hand, and for an
// agent every extra hop is another round trip to a language model. This turns the whole
// journey into one order. The search is Route::Find (#432), the one a client's map uses.
std::vector<std::string>
Simulation::PlanRoute(const std::string& from, const std::string& to, Route::Preference preference,
                      const std::vector<WorldLoader::SystemLink>* extra) const
{
    if (from.empty() || to.empty() || !HasSystem(from) || !HasSystem(to))
        return {};
    if (extra == nullptr || extra->empty())
        return Route::Find(universe_, from, to, 0.0, time_, preference).path;
    WorldLoader::Universe with = universe_;
    with.links.insert(with.links.end(), extra->begin(), extra->end());
    return Route::Find(with, from, to, 0.0, time_, preference).path;
}

WorldLoader::Universe Simulation::KnownUniverse() const
{
    auto charted = [&](const std::string& id)
    {
        auto it = systems_.find(id);
        return it == systems_.end() || it->second.agg.visited;
    };
    std::set<std::string> frontier;  // uncharted, but a charted system has a gate to it
    for (const WorldLoader::SystemLink& l : universe_.links)
    {
        if (charted(l.a) && !charted(l.b))
            frontier.insert(l.b);
        if (charted(l.b) && !charted(l.a))
            frontier.insert(l.a);
    }

    WorldLoader::Universe out;
    out.startId = universe_.startId;
    for (const WorldLoader::SystemInfo& info : universe_.systems)
    {
        if (charted(info.id))
        {
            out.systems.push_back(info);
            const SystemAggregate& a = systems_.at(info.id).agg;
            out.systems.back().discoverer = a.discoverer;
            if (!a.givenName.empty())
            {
                out.systems.back().designation = info.name;
                out.systems.back().name = a.givenName;
            }
        }
        else if (frontier.count(info.id))
        {
            WorldLoader::SystemInfo seen;
            seen.id = info.id;
            seen.name = info.name;  // a designation: nobody has named it yet (#145)
            seen.mapPos = info.mapPos;
            seen.security = 0.0f;
            seen.charted = false;
            out.systems.push_back(seen);
        }
    }
    for (const WorldLoader::SystemLink& l : universe_.links)
        if (charted(l.a) || charted(l.b))
            out.links.push_back(l);
    return out;
}

bool Simulation::NameSystem(ClientSession& s, const std::string& name)
{
    auto refuse = [&](const std::string& why)
    {
        s.RecordEvent(Ev::Kind::Notice, "Not named: " + why);
        return false;
    };
    auto it = systems_.find(s.systemId);
    if (it == systems_.end() || regionDocs_.count(s.systemId) == 0)
        return refuse("only a system beyond the wormhole can be named");
    if (InCentre(s.systemId))
        return refuse(SystemName(s.systemId) + " was named long before you were born");
    SystemAggregate& a = it->second.agg;
    if (!a.givenName.empty())
        return refuse(SystemName(s.systemId) + " already has its name");
    const std::string pilot = s.ship ? s.ship->GetPilotName() : std::string();
    if (a.discoverer.empty() || a.discoverer != pilot)
        return refuse("only " +
                      (a.discoverer.empty() ? std::string("its discoverer") : a.discoverer) +
                      ", who got here first, may name it");
    std::string why;
    if (!Names::ValidSystemName(name, why))
        return refuse(why);
    for (const auto& info : universe_.systems)
        if (Names::SameName(info.name, name) || Names::SameName(info.id, name) ||
            Names::SameName(systems_[info.id].agg.givenName, name))
            return refuse("there is already a system called " + name);

    const std::string designation = SystemName(s.systemId);
    a.givenName = name;
    chartsChanged_ = true;  // everyone's index changes
    // Its planets are called after it (#259). A client already here keeps the names its
    // layout gave it until it next enters; everyone after sees the new ones.
    WorldLoader::NamePlanets(it->second.entities, name);
    PushEvent(designation + " is now " + name + ", named by " + pilot);
    s.RecordEvent(Ev::Kind::Notice, "Named " + designation + " " + name);
    return true;
}

// --- Authoritative world mutation (#38) ---

bool Simulation::IsMutableKind(EntityKind k)
{
    switch (k)
    {
        case EntityKind::Station:
        case EntityKind::Field:
        case EntityKind::Nebula:
        case EntityKind::Derelict:
        case EntityKind::Structure: return true;
        // Bodies are the generator's, and satellites find their planet by its place in the
        // file (#210): one more or one fewer planet moves every moon in the system. A gate
        // is an edge of the route graph, which is the galaxy index's, not the system's.
        case EntityKind::Star:
        case EntityKind::Planet:
        case EntityKind::Gate:
        // Not static at all: ships come and go through the snapshot.
        case EntityKind::Npc:
        case EntityKind::PlayerShip:
        case EntityKind::Unknown: return false;
    }
    return false;
}

int Simulation::AddStatic(const std::string& systemId, std::unique_ptr<Entity> e,
                          const std::string& owner)
{
    SystemState* st = SystemById(systemId);
    if (st == nullptr || !e || !IsMutableKind(e->GetKind()))
        return 0;
    // Always a fresh id, whatever the object came with: ids are never reused, which is
    // what lets a delta name an object without saying which one of two it meant.
    const int id = NextAgentId();
    e->SetId(id);
    e->SetOwner(owner);
    st->keys[id] = "+" + std::to_string(st->nextAddedKey++);
    ScheduleStructure(systemId, *e);
    st->entities.push_back(std::move(e));
    st->profile = ProfileOf(*st);
    st->pendingAdded.insert(id);
    st->layoutRev++;
    return id;
}

bool Simulation::RemoveStatic(const std::string& systemId, int id)
{
    SystemState* st = SystemById(systemId);
    if (st == nullptr || id == 0)
        return false;
    auto it = std::find_if(st->entities.begin(), st->entities.end(),
                           [id](const std::unique_ptr<Entity>& e) { return e->GetId() == id; });
    if (it == st->entities.end() || !IsMutableKind((*it)->GetKind()))
        return false;

    // Nobody is left inside a station that no longer exists. Undocked where it stood,
    // which is where their ship already is.
    for (auto& kv : sessions_)
    {
        ClientSession& s = kv.second;
        if (s.systemId == systemId && s.dockedStationId == id)
        {
            s.dockedStationId = 0;
            s.RecordEvent(Ev::Kind::Undocked, (*it)->GetName() + " is gone; you are in space");
        }
    }
    st->defenceCooldown.erase(id);
    st->provokedBy.erase(id);
    st->provokedByNpc.erase(id);
    // One of the world's own stays gone across a restart; one added since simply stops
    // being saved.
    const auto key = st->keys.find(id);
    if (key != st->keys.end())
    {
        if (key->second[0] != '+')
            st->removedKeys.insert(key->second);
        st->keys.erase(key);
    }
    st->entities.erase(it);
    st->profile = ProfileOf(*st);
    st->pendingAdded.erase(id);  // added and gone again before anyone was told: only gone
    st->pendingChanged.erase(id);
    st->pendingRemoved.insert(id);
    st->layoutRev++;
    return true;
}

void Simulation::MarkStaticChanged(SystemState& st, int id)
{
    if (id == 0)
        return;
    // An object nobody has been told about yet goes out whole, as it is now; it is still
    // a change to the layer, so the revision moves either way.
    if (st.pendingAdded.count(id) == 0)
        st.pendingChanged.insert(id);
    st.layoutRev++;
}

std::string Simulation::StaticKey(const std::string& systemId, int id) const
{
    const SystemState* st = SystemById(systemId);
    if (st == nullptr)
        return std::string();
    const auto key = st->keys.find(id);
    return key == st->keys.end() ? std::string() : key->second;
}

// --- Keeping what changed across a restart (#38) ---

void Simulation::KeyWorldObjects(SystemState& st)
{
    // Named by what the object is, never by where it came in the build: one more belt in a
    // hand-written file does not rename every station, and the generator's rule (#140) --
    // keyed by what, not by order -- holds for the save too. The nth only tells apart two
    // of one name, as a saved mission does (#227).
    std::map<std::string, int> seen;
    for (const auto& e : st.entities)
    {
        if (!IsMutableKind(e->GetKind()))
            continue;
        std::string array;
        WorldLoader::DescribeObject(*e, array);
        const std::string name = e->GetKind() == EntityKind::Derelict
                                     ? static_cast<const Derelict&>(*e).GetBaseName()
                                     : e->GetName();
        const std::string stem = array + "/" + name + "#";
        st.keys[e->GetId()] = stem + std::to_string(seen[stem]++);
    }
}

void Simulation::ReplayChanges(SystemState& st)
{
    using nlohmann::json;
    if (st.restore.empty() && st.removedKeys.empty())
        return;
    const json c = st.restore.empty() ? json::object() : json::parse(st.restore, nullptr, false);
    st.restore.clear();
    if (!c.is_object())
        return;

    std::map<std::string, Entity*> byKey;
    for (const auto& e : st.entities)
    {
        const auto key = st.keys.find(e->GetId());
        if (key != st.keys.end())
            byKey[key->second] = e.get();
    }

    // Changed in place.
    if (c.contains("state") && c["state"].is_object())
        for (auto it = c["state"].begin(); it != c["state"].end(); ++it)
        {
            const auto e = byKey.find(it.key());
            if (e != byKey.end())
                ApplyState(*e->second, it.value());
        }

    // Taken away. A key the data no longer has is said, and kept: the object stays gone if
    // the data comes back to it.
    for (const std::string& k : st.removedKeys)
        if (byKey.count(k) == 0)
            TraceLog(LOG_WARNING, "World: %s in %s was removed, and the data no longer has it",
                     k.c_str(), st.id.c_str());
    auto gone = [&](const std::unique_ptr<Entity>& e)
    {
        const auto key = st.keys.find(e->GetId());
        if (key == st.keys.end() || st.removedKeys.count(key->second) == 0)
            return false;
        st.keys.erase(key);
        return true;
    };
    st.entities.erase(std::remove_if(st.entities.begin(), st.entities.end(), gone),
                      st.entities.end());

    // Added, after the world's own and in the order they were added, which is the order they
    // were saved in: the same objects in the same places in the list as before the restart.
    if (c.contains("added") && c["added"].is_array())
        for (const json& a : c["added"])
        {
            const std::string key = a.value("key", std::string());
            const std::string array = a.value("array", std::string());
            if (key.empty() || key[0] != '+' || array.empty() || !a.contains("object"))
                continue;
            std::vector<std::unique_ptr<Entity>> built =
                WorldLoader::BuildSystem(json{ { array, json::array({ a["object"] }) } });
            if (built.size() != 1 || !IsMutableKind(built[0]->GetKind()))
            {
                TraceLog(LOG_WARNING, "World: %s in %s could not be rebuilt -- dropped",
                         key.c_str(), st.id.c_str());
                continue;
            }
            std::unique_ptr<Entity>& e = built[0];
            e->SetId(NextAgentId());
            ApplyState(*e, a);
            st.keys[e->GetId()] = key;
            st.entities.push_back(std::move(e));
        }
}
