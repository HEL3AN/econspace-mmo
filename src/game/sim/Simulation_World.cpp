// The galaxy itself: seeding it, materializing a system's NPCs from its aggregate when it
// warms and letting them go when it cools, planning a route across it, and persistence.
//
// One translation unit of Simulation (#17).

#include "sim/Simulation.h"
#include "sim/Names.h"

#include "core/Orbits.h"
#include "core/World.h"
#include "entities/AsteroidField.h"
#include "entities/Derelict.h"
#include "entities/JumpGate.h"
#include "entities/NpcShip.h"
#include "entities/Ship.h"
#include "entities/Station.h"

#include "gen/Pins.h"
#include "gen/Region.h"
#include "gen/Rng.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
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
    a.controller = info.owner.empty() ? FactionId::Independent : FactionFromString(info.owner);
    a.visited = regionDocs_.count(info.id) == 0;  // beyond the wormhole, nobody has been
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
        FactionId tradeFactions[] = { pop.tradeFaction, FactionId::Independent,
                                      FactionId::TradersGuild };
        const int traders = (int)roundf(agg.traders);
        for (int i = 0; i < traders; i++)
            SpawnNpcInto(st, around(pick(lanes), APPROACH), tradeFactions[i % 3], NpcRole::Trader,
                         lanes);
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
            SpawnNpcInto(st, around(spot, 3000.0f), FactionId::Independent, NpcRole::Miner, near);
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
                         NpcRole::Police, patrolRoute);
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
            SpawnNpcInto(st, spot, FactionId::Pirates, NpcRole::Pirate, patrol);
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
}

void Simulation::Cool(SystemState& st)
{
    // What is here is what the aggregate says from now on. The ships themselves go: nobody
    // is left to see them, and a system that warms again makes them afresh.
    RecountAgg(st);
    st.entities.erase(std::remove_if(st.entities.begin(), st.entities.end(),
                                     [](const std::unique_ptr<Entity>& e)
                                     { return e->GetKind() == EntityKind::Npc; }),
                      st.entities.end());
    st.defenceCooldown.clear();
    st.populated = false;
}

void Simulation::AttachRegion(uint64_t seed, const std::string& systemsDir, int systems)
{
    const WorldLoader::SystemInfo* home = nullptr;
    for (const auto& info : universe_.systems)
        if (info.id == universe_.startId)
            home = &info;
    if (home == nullptr || hasRegion_)
        return;

    nlohmann::json homeDoc;
    {
        std::ifstream in(systemsDir + home->file);
        if (in.is_open())
            homeDoc = nlohmann::json::parse(in, nullptr, false);
    }
    Gen::RegionParams params;
    params.seed = seed;
    params.homeId = home->id;
    params.homeMap = home->mapPos;
    for (const auto& info : universe_.systems)
        params.knownMap.push_back(info.mapPos);
    params.homeSystem = homeDoc.is_discarded() ? nullptr : &homeDoc;
    if (systems > 0)
        params.systems = systems;
    Gen::Region region = Gen::GenerateRegion(params);

    // Then the hand-written exceptions, always after (#147): data/pins.json beside the
    // systems directory. Every pin that cannot apply is said, by name.
    {
        std::ifstream pinsIn(systemsDir + "../pins.json");
        if (pinsIn.is_open())
        {
            const nlohmann::json     pins = nlohmann::json::parse(pinsIn, nullptr, false);
            std::vector<std::string> problems;
            if (pins.is_discarded())
                problems.push_back("pins.json is not valid JSON");
            else
                Gen::ApplyPins(region, pins, seed, problems);
            for (const std::string& p : problems)
                TraceLog(LOG_WARNING, "Pins: %s -- not applied", p.c_str());
        }
    }

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
    // once, when its system is built.
    for (const auto& kv : region.documents)
        regionDocs_[kv.first] = kv.second.dump();
    wormhole_ = region.wormhole.dump();
    regionSeed_ = seed;
    hasRegion_ = true;
}

bool Simulation::ReadWorldSeed(const std::string& path, uint64_t& seed, int& generator)
{
    std::ifstream in(path);
    if (!in.is_open())
        return false;
    const nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.contains("seed"))
        return false;
    seed = j["seed"].get<uint64_t>();
    generator = j.value("generator", 0);
    return true;
}

void Simulation::MaterializeAllSystems(const std::string& systemsDir)
{
    for (const auto& info : universe_.systems)
    {
        SystemState& st = systems_[info.id];  // created by InitGalaxy (aggregate already exists)
        st.id = info.id;
        if (st.entities.empty())
        {
            auto doc = regionDocs_.find(info.id);
            if (doc != regionDocs_.end())
                st.entities =
                    WorldLoader::BuildSystem(nlohmann::json::parse(doc->second));  // (#140)
            else if (hasRegion_ && info.id == universe_.startId)
            {
                // The start system as written, plus the wormhole's mouth.
                std::ifstream  in(systemsDir + info.file);
                nlohmann::json home = nlohmann::json::parse(in, nullptr, false);
                if (home.is_discarded())
                    home = nlohmann::json::object();
                home["gates"].push_back(nlohmann::json::parse(wormhole_));
                st.entities = WorldLoader::BuildSystem(home);
            }
            else
                st.entities = WorldLoader::LoadSystem(systemsDir + info.file);
            WorldLoader::NamePlanets(st.entities, SystemName(info.id));  // (#259)
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
}

namespace
{
// What a save keeps about an object beyond its description (#38): who owns it, and whether a
// wreck has been searched. Empty for an object in the state it was made in.
nlohmann::json StateOf(const Entity& e)
{
    nlohmann::json s = nlohmann::json::object();
    if (!e.GetOwner().empty())
        s["owner"] = e.GetOwner();
    if (e.GetKind() == EntityKind::Derelict && static_cast<const Derelict&>(e).IsLooted())
        s["looted"] = true;
    return s;
}

void ApplyState(Entity& e, const nlohmann::json& s)
{
    if (!s.is_object())
        return;
    e.SetOwner(s.value("owner", std::string()));
    if (e.GetKind() == EntityKind::Derelict && s.value("looted", false))
        static_cast<Derelict&>(e).SetLooted();
}

// A faction by the id factions.json gives it, or false for one this build does not have:
// the reader maps anything it does not know to the Independents, which for a save would
// hand one faction's knowledge to another.
bool FactionByName(const nlohmann::json& v, FactionId& f)
{
    if (!v.is_string())
        return false;
    f = FactionFromString(v.get<std::string>());
    return Factions::Id(f) == v.get<std::string>();
}

nlohmann::json IntelJson(const Intel& i)
{
    nlohmann::json defenders = nlohmann::json::array();
    for (FactionId d : i.defenders)
        defenders.push_back(Factions::Id(d));
    return { { "seenAt", i.seenAt },
             { "traffic", i.traffic },
             { "security", i.security },
             { "belts", i.belts },
             { "wrecks", i.wrecks },
             { "stations", i.stations },
             { "planets", i.planets },
             { "gates", i.gates },
             { "defenders", defenders },
             { "presence", i.presence },
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
    if (j.contains("presence") && j["presence"].is_array() && j["presence"].size() == FACTION_COUNT)
        for (int f = 0; f < FACTION_COUNT; f++)
            i.presence[f] = j["presence"][f].get<float>();
    FactionId c;
    if (j.contains("controller") && FactionByName(j["controller"], c))
        i.controller = c;
    i.claimed = j.value("claimed", false);
    return i;
}
}  // namespace

// Server world persistence: the galaxy (system aggregates) + time. The sup* "pressure"
// field is transient — not written (on load the world has "rested").
void Simulation::SaveWorld(const std::string& path) const
{
    using nlohmann::json;
    json j;
    j["version"] = Save::WORLD_VERSION;
    j["simTime"] = time_;
    // The region is not saved: it is remade from these two (#140).
    if (hasRegion_)
    {
        j["seed"] = regionSeed_;
        j["generator"] = Gen::GENERATOR_VERSION;
    }
    json galaxy = json::object();
    for (const auto& kv : systems_)
    {
        const SystemAggregate& a = kv.second.agg;
        galaxy[kv.first] = { { "traders", a.traders },       { "miners", a.miners },
                             { "police", a.police },         { "pirates", a.pirates },
                             { "security", a.security },     { "baseSecurity", a.baseSecurity },
                             { "prosperity", a.prosperity }, { "controller", (int)a.controller },
                             { "visited", a.visited },       { "presence", a.presence },
                             { "claimed", a.claimed },       { "discoverer", a.discoverer },
                             { "name", a.givenName } };
    }
    j["galaxy"] = galaxy;

    // What each faction knows, what it has under way, and the history so far (#295).
    // Knowledge is written only once there is some: a world saved before its first macro
    // pass seeds it on load, as an older save does.
    if (mindsSeeded_)
    {
        json minds = json::object();
        for (int f = 0; f < FACTION_COUNT; f++)
        {
            json intel = json::object();
            // Not what it sees for itself -- its holdings and where its ships are: the
            // faction looks at those again before it next decides anything, and in a
            // region the pirates have overrun they are most of the file.
            for (const auto& kv : minds_[f].intel)
            {
                const auto st = systems_.find(kv.first);
                if (st != systems_.end() &&
                    ((st->second.agg.claimed && st->second.agg.controller == (FactionId)f) ||
                     st->second.agg.presence[f] >= 1.0f))
                    continue;
                intel[kv.first] = IntelJson(kv.second);
            }
            minds[Factions::Id((FactionId)f)] = { { "intel", std::move(intel) },
                                                  { "stock", minds_[f].stock } };
        }
        j["factions"] = std::move(minds);
    }
    json plans = json::array();
    for (const auto& kv : plans_)
    {
        const Plan& p = kv.second;
        json        pj = { { "id", p.id },
                           { "kind", p.kind == Plan::Kind::Settle ? "settle" : "survey" },
                           { "faction", Factions::Id(p.faction) },
                           { "from", p.from },
                           { "target", p.target },
                           { "startedAt", p.startedAt },
                           { "dueAt", p.dueAt } };
        if (!p.site.empty())
            pj["site"] = p.site;
        plans.push_back(std::move(pj));
    }
    j["plans"] = std::move(plans);
    j["nextPlan"] = nextPlanId_;
    json history = json::array();
    for (const ChronicleEntry& e : chronicle_)
        history.push_back(
            { { "seq", e.seq },
              { "time", e.time },
              { "kind", e.kind },
              { "faction", e.faction < 0 ? std::string() : Factions::Id((FactionId)e.faction) },
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
        const SystemState& st = kv.second;
        json               added = json::array();
        json               state = json::object();
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
            continue;
        json c = json::object();
        if (!st.removedKeys.empty())
            c["removed"] = st.removedKeys;
        if (!state.empty())
            c["state"] = std::move(state);
        if (!added.empty())
            c["added"] = std::move(added);
        c["nextAdded"] = st.nextAddedKey;
        changes[kv.first] = std::move(c);
    }
    j["changes"] = std::move(changes);

    std::ofstream out(path);
    if (out.is_open())
        out << j.dump(2) << "\n";
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
        a.controller = (FactionId)gj.value("controller", (int)a.controller);
        a.visited = gj.value("visited", a.visited);
        a.claimed = gj.value("claimed", a.claimed);
        a.discoverer = gj.value("discoverer", a.discoverer);
        a.givenName = gj.value("name", a.givenName);
        // An older save has no presence: the seeded one stands, from what it did save.
        if (gj.contains("presence") && gj["presence"].is_array() &&
            gj["presence"].size() == FACTION_COUNT)
            for (int f = 0; f < FACTION_COUNT; f++)
                a.presence[f] = gj["presence"][f].get<float>();
        else
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
                    minds_[(int)f].intel[k.key()] = IntelFrom(k.value());
            // A version 5 world has no stock: its factions begin saving from nothing.
            const json& stock = it.value().value("stock", json(0.0));
            minds_[(int)f].stock = stock.is_number() ? std::max(0.0f, stock.get<float>()) : 0.0f;
        }
    }
    if (j.contains("plans") && j["plans"].is_array())
        for (const json& pj : j["plans"])
        {
            Plan              p;
            const std::string kind = pj.is_object() ? pj.value("kind", std::string()) : "";
            if ((kind != "survey" && kind != "settle") || !pj.contains("faction") ||
                !FactionByName(pj["faction"], p.faction))
                continue;
            p.id = pj.value("id", 0);
            p.kind = kind == "settle" ? Plan::Kind::Settle : Plan::Kind::Survey;
            p.from = pj.value("from", std::string());
            p.target = pj.value("target", std::string());
            p.startedAt = pj.value("startedAt", 0.0);
            p.dueAt = pj.value("dueAt", 0.0);
            p.site = pj.value("site", std::string());
            if (p.id <= 0 || plans_.count(p.id) != 0 || !HasSystem(p.target) ||
                (p.kind == Plan::Kind::Settle && p.site.empty()))
                continue;
            plans_[p.id] = p;
            due_.insert({ p.dueAt, p.id });
        }
    nextPlanId_ = std::max(1, j.value("nextPlan", 1));
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
            e.faction = ej.contains("faction") && FactionByName(ej["faction"], f) ? (int)f : -1;
            e.system = ej.value("system", std::string());
            e.text = ej.value("text", std::string());
            chronicle_.push_back(std::move(e));
        }
    chronicleSeq_ = j.value("chronicleSeq", 0LL);
    for (const ChronicleEntry& e : chronicle_)
        chronicleSeq_ = std::max(chronicleSeq_, e.seq);

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
            const json&  c = it.value();
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
    for (FactionMind& m : minds_)
        m = FactionMind();
    mindsSeeded_ = false;
    plans_.clear();
    due_.clear();
    nextPlanId_ = 1;
    chronicle_.clear();
    chronicleSeq_ = 0;
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
// journey into one order.
std::vector<std::string> Simulation::PlanRoute(const std::string& from, const std::string& to,
                                               bool avoidDanger) const
{
    if (from.empty() || to.empty() || !HasSystem(from) || !HasSystem(to))
        return {};
    if (from == to)
        return { from };

    // What one hop into a system costs. Counting hops gives the short way; weighing danger
    // gives the way a loaded hauler survives. Both are Dijkstra over the same small graph,
    // so the difference is only this function.
    auto cost = [this, avoidDanger](const std::string& id) -> double
    {
        if (!avoidDanger)
            return 1.0;
        auto it = systems_.find(id);
        if (it == systems_.end())
            return 1.0;
        const SystemAggregate& a = it->second.agg;
        // Low security and a pirate presence both make a system expensive to cross. The
        // numbers only need to order routes sensibly, not to model risk exactly.
        double danger = (1.0 - (double)a.security) * 4.0 + (double)a.pirates * 0.5;
        return 1.0 + danger;
    };

    std::map<std::string, double>      best;
    std::map<std::string, std::string> cameFrom;
    // The galaxy is a handful of systems, so a linear scan for the next node is cheaper to
    // read than a priority queue and indistinguishable in cost.
    std::map<std::string, bool> settled;
    for (const auto& kv : systems_)
        best[kv.first] = 1e18;
    best[from] = 0.0;

    for (;;)
    {
        std::string cur;
        double      curCost = 1e18;
        for (const auto& kv : best)
            if (!settled[kv.first] && kv.second < curCost)
            {
                cur = kv.first;
                curCost = kv.second;
            }
        if (cur.empty())
            break;  // nothing reachable left
        if (cur == to)
            break;
        settled[cur] = true;

        for (const std::string& n : Neighbors(cur))
        {
            if (settled[n])
                continue;
            double via = curCost + cost(n);
            auto   it = best.find(n);
            if (it != best.end() && via < it->second)
            {
                it->second = via;
                cameFrom[n] = cur;
            }
        }
    }

    if (best[to] >= 1e18)
        return {};  // unreachable: say so rather than returning a partial path

    std::vector<std::string> path;
    for (std::string at = to;; at = cameFrom[at])
    {
        path.push_back(at);
        if (at == from)
            break;
        if (cameFrom.find(at) == cameFrom.end())
            return {};
    }
    std::reverse(path.begin(), path.end());
    return path;
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
