#include <doctest/doctest.h>

#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "core/Route.h"
#include "core/ShipDesign.h"
#include "entities/Derelict.h"
#include "entities/JumpGate.h"
#include "entities/Ship.h"
#include "entities/ShipType.h"
#include "entities/Structure.h"
#include "gen/Galaxy.h"
#include "gen/Region.h"
#include "sim/Protocol.h"
#include "sim/SaveSchema.h"
#include "sim/Simulation.h"
#include "sim/WorldRules.h"

#include "temp_dir.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>

// Unstable mouths, Anchors, collapse and drift, and the archive of dead regions (#342 S4).

namespace
{
const std::string DATA = TEST_DATA_DIR;

void LoadRegistries()
{
    Factions::Load(DATA + "factions.json");
    REQUIRE(Archetypes::Load(DATA + "archetypes.json"));
    REQUIRE(Blueprints::Load(DATA + "blueprints.json"));
}

Gen::WorldParams Params(uint64_t seed)
{
    Gen::WorldParams p;
    p.seed = seed;
    p.centres = 3;
    p.owners = Factions::MajorIds();
    p.pirates = Factions::Id(Factions::Pirates());
    p.unaffiliated = Factions::Id(Factions::Unaffiliated());
    return p;
}

void Make(Simulation& sim, uint64_t seed, const std::string& seal = std::string(),
          const std::vector<Simulation::RegionOpening>& log = {})
{
    sim.SetSealDir(seal);
    REQUIRE(sim.OpenGalaxy(Params(seed), DATA, log));
    sim.InitGalaxy();
    sim.MaterializeAllSystems(DATA + "systems/");
}

void RunMinutes(Simulation& sim, double minutes)
{
    for (int i = 0; i < (int)(minutes * 30.0); i++)
        sim.MaintainWorld(2.0f);
}

// The clock moved on to `t`, and a period run there: what is on the clock happens, and what
// the tide's period decides is decided.
void At(Simulation& sim, double t)
{
    sim.SetTime(t);
    RunMinutes(sim, 1.1);
}

// The design's rules for every test -- a day's mouth, Anchors on -- whatever the last test
// set; restored when it ends.
struct Rules
{
    WorldRules::Tide    tide = WorldRules::TideRules();
    WorldRules::Mouths  mouths = WorldRules::MouthRules();
    WorldRules::Archive archive = WorldRules::ArchiveRules();
    Rules()
    {
        WorldRules::SetTideRules(WorldRules::Tide{});
        WorldRules::SetMouthRules(WorldRules::Mouths{});
        WorldRules::SetArchiveRules(WorldRules::Archive{});
    }
    ~Rules()
    {
        WorldRules::SetTideRules(tide);
        WorldRules::SetMouthRules(mouths);
        WorldRules::SetArchiveRules(archive);
    }
};

const Entity* GateTo(const Simulation& sim, const std::string& systemId, const std::string& dest)
{
    const SystemState* st = sim.SystemById(systemId);
    if (st != nullptr)
        for (const auto& e : st->entities)
            if (e->GetKind() == EntityKind::Gate &&
                static_cast<const JumpGate&>(*e).GetDestination() == dest)
                return e.get();
    return nullptr;
}

// A mouth from the first centre, opened by the admin: unstable, with its region beyond.
int OpenFromCentre(Simulation& sim)
{
    std::string why;
    const int   slot = sim.OpenMouthNow(sim.Skeleton().centres.front(), why);
    REQUIRE(slot >= 0);
    return slot;
}

float MassOf(const ShipStats& stats)
{
    const Ships::Catalogue& c = Archetypes::ShipCatalogue();
    const Ships::Design*    d = c.FindDesign(stats.design);
    Ships::Stats            s;
    std::string             why;
    REQUIRE(d != nullptr);
    REQUIRE(Ships::Derive(c, *d, s, why));
    return s.mass;
}

// A spot for an Anchor beside the mouth `gate` in `st`, found by asking the rule -- and, when
// `apart` is given, well clear of it.
Vector2 AnchorSpot(const Simulation& sim, const SystemState& st, const Entity& gate,
                   const Vector2* apart = nullptr)
{
    const Blueprint& bp = *Blueprints::Find(Simulation::ANCHOR_BLUEPRINT);
    for (int ring = 0; ring < 6; ring++)
        for (int k = 0; k < 24; k++)
        {
            const float   r = gate.GetSize() + 2500.0f + ring * 2000.0f;
            const float   a = (float)k * 0.2618f;
            const Vector2 at{ gate.GetPosition().x + r * std::cos(a),
                              gate.GetPosition().y + r * std::sin(a) };
            if (apart != nullptr && std::hypot(at.x - apart->x, at.y - apart->y) < 4000.0f)
                continue;
            if (sim.SpotProblem(st, bp, at).empty())
                return at;
        }
    FAIL("no room for an Anchor at the mouth");
    return Vector2{ 0.0f, 0.0f };
}
}  // namespace

TEST_CASE("the mouth and archive rules read, and a misspelt one is an error (#342 S4)")
{
    Rules r;
    REQUIRE(WorldRules::Load(DATA + "world_rules.json"));
    CHECK(WorldRules::MouthRules().anchors);
    CHECK(WorldRules::TideRules().mouthHours == doctest::Approx(24.0));
    CHECK(WorldRules::ArchiveRules().deadDays > WorldRules::ArchiveRules().warnDays);

    TempDir           dir;
    const std::string path = dir.Path("rules.json");
    {
        std::ofstream f(path);
        f << R"({ "tide": { "threshold": 50 }, "mouths": { "maxMas": 1 } })";
    }
    CHECK_FALSE(WorldRules::Load(path));
    CHECK(WorldRules::Error().find("maxMas") != std::string::npos);
    {
        std::ofstream f(path);
        f << R"({ "tide": { "threshold": 50 }, "archive": { "returnChance": 2 } })";
    }
    CHECK_FALSE(WorldRules::Load(path));
}

TEST_CASE("a new mouth is unstable: a day to live, small ships only, and told so (#342 S4)")
{
    LoadRegistries();
    Rules      r;
    Simulation sim;
    Make(sim, 7);
    sim.TakeLayoutDeltas();
    const int                        slot = OpenFromCentre(sim);
    const Simulation::RegionOpening* o = sim.OpeningOf(slot);
    REQUIRE(o != nullptr);
    CHECK(o->state == "unstable");
    CHECK(o->closesAt == doctest::Approx(sim.Time() + 24.0 * 3600.0));
    CHECK(sim.YoungMouths() == 1);

    // Both ends are the mouth's look and say when it goes -- the delta to whoever stands at
    // the parent's end, and the layout of the far side.
    const std::string entry = Simulation::EntryOf(slot);
    bool              told = false;
    for (const Proto::LayoutDelta& d : sim.TakeLayoutDeltas())
        for (const Proto::EntityLayout& e : d.added)
            if (d.systemId == o->mouth && e.kind == EntityKind::Gate && e.dest == entry)
            {
                told = true;
                CHECK(e.archetype == "gate.mouth");
                CHECK(e.expiresAt == doctest::Approx(o->closesAt));
            }
    CHECK(told);
    bool back = false;
    for (const Proto::EntityLayout& e : sim.BuildLayout(entry).entities)
        if (e.kind == EntityKind::Gate && e.dest == o->mouth)
        {
            back = true;
            CHECK(e.archetype == "gate.mouth");
            CHECK(e.expiresAt == doctest::Approx(o->closesAt));
        }
    CHECK(back);
    // The mouth's line -- word of it reaching the factions nearby may follow (S6).
    std::string opened;
    for (const ChronicleEntry& e : sim.Chronicle())
        if (e.kind == "tide")
            opened = e.text;
    CHECK(opened.find("collapses in 24 h") != std::string::npos);

    // Through it, only what is light enough.
    int light = 0, heavy = 0;
    for (int i = 0; i < (int)GetShipCatalog().size(); i++)
    {
        if (MassOf(GetShipCatalog()[i].stats) < MassOf(GetShipCatalog()[light].stats))
            light = i;
        if (MassOf(GetShipCatalog()[i].stats) > MassOf(GetShipCatalog()[heavy].stats))
            heavy = i;
    }
    REQUIRE(MassOf(GetShipCatalog()[light].stats) < MassOf(GetShipCatalog()[heavy].stats));
    WorldRules::Mouths m;
    m.maxMass = (MassOf(GetShipCatalog()[light].stats) + MassOf(GetShipCatalog()[heavy].stats)) / 2;
    WorldRules::SetMouthRules(m);
    const Entity* gate = GateTo(sim, o->mouth, entry);
    REQUIRE(gate != nullptr);
    ClientSession& s =
        sim.CreateSession(o->mouth, gate->GetPosition(), GetShipCatalog()[heavy].stats);
    s.ownedShips = { light, heavy };
    s.currentShip = heavy;
    CHECK(sim.MouthProblem(s, *gate).find("unstable") != std::string::npos);
    CHECK(sim.JumpGateDestIfNear(s, *sim.SystemById(o->mouth), gate->GetId()).empty());
    s.currentShip = light;
    CHECK(sim.MouthProblem(s, *gate).empty());
    CHECK(sim.JumpGateDestIfNear(s, *sim.SystemById(o->mouth), gate->GetId()) == entry);
}

TEST_CASE("an unanchored mouth collapses, its region drifts, and the next mouth finds it (#342 S4)")
{
    LoadRegistries();
    Rules      r;
    Simulation sim;
    Make(sim, 7);
    const int         slot = OpenFromCentre(sim);
    const std::string mouth = sim.OpeningOf(slot)->mouth;
    const std::string entry = Simulation::EntryOf(slot);
    const int         parent = sim.OpeningOf(slot)->parent;
    const size_t      logged = sim.OpeningLog().size();
    // A player's beacon in the region: never lost, whatever becomes of the way in.
    const Blueprint& beacon = *Blueprints::Find("beacon");
    auto             t =
        std::make_unique<Structure>(Vector2{ 500000.0f, 0.0f }, 0.0f, "Lantern", beacon.archetype);
    t->SetBlueprint(beacon.id);
    REQUIRE(sim.AddStatic(entry, std::move(t), "pilot") != 0);
    sim.TakeLayoutDeltas();
    sim.TakeTopologyChanged();

    // Failing first, told in the chronicle...
    const double closes = sim.OpeningOf(slot)->closesAt;
    At(sim, closes - 3600.0);
    CHECK(sim.OpeningOf(slot)->warned);
    CHECK(sim.OpeningOf(slot)->state == "unstable");
    bool warned = false;
    for (const ChronicleEntry& e : sim.Chronicle())
        warned = warned || (e.kind == "mouth" && e.text.find("failing") != std::string::npos);
    CHECK(warned);

    // ...then gone: no route, no gates, and the region still there, cut off.
    At(sim, closes + 1.0);
    const Simulation::RegionOpening* o = sim.OpeningOf(slot);
    REQUIRE(o != nullptr);
    CHECK(o->state == "drifted");
    CHECK(sim.TakeTopologyChanged());
    CHECK(sim.PlanRoute(mouth, entry).empty());
    CHECK(GateTo(sim, mouth, entry) == nullptr);
    CHECK(GateTo(sim, entry, mouth) == nullptr);
    bool removed = false;
    for (const Proto::LayoutDelta& d : sim.TakeLayoutDeltas())
        removed = removed || (d.systemId == mouth && !d.removed.empty());
    CHECK(removed);
    CHECK(sim.OpenSlots().count(slot) == 1);
    CHECK(sim.HasSystem(entry));
    CHECK(sim.StructuresOwnedBy("pilot") == 1);
    CHECK_FALSE(sim.IsEdgeRegion(slot));  // cut off, it gathers no tide of its own
    CHECK(sim.IsEdgeRegion(parent));

    // The next mouth from the same parent leads there again: no new slot, no new entry.
    std::string why;
    CHECK(sim.OpenMouthNow(parent, why) == slot);
    CHECK(sim.OpeningLog().size() == logged);
    CHECK(sim.OpeningOf(slot)->state == "unstable");
    CHECK(sim.OpeningOf(slot)->closesAt == doctest::Approx(sim.Time() + 24.0 * 3600.0));
    CHECK(sim.PlanRoute(mouth, entry).size() == 2);
    REQUIRE(GateTo(sim, mouth, entry) != nullptr);
    CHECK(static_cast<const JumpGate*>(GateTo(sim, mouth, entry))->IsUnstable());
    CHECK(GateTo(sim, entry, mouth) != nullptr);
}

TEST_CASE("an Anchor makes a mouth permanent gates named for its builder, until it falls (#342 S4)")
{
    LoadRegistries();
    Rules      r;
    Simulation sim;
    Make(sim, 7);
    const int         slot = OpenFromCentre(sim);
    const std::string mouth = sim.OpeningOf(slot)->mouth;
    const std::string entry = Simulation::EntryOf(slot);
    const double      closes = sim.OpeningOf(slot)->closesAt;
    const Entity*     gate = GateTo(sim, mouth, entry);
    REQUIRE(gate != nullptr);
    const Blueprint& bp = *Blueprints::Find(Simulation::ANCHOR_BLUEPRINT);
    CHECK(bp.byPlayers);
    CHECK(bp.byFactions);

    // A hull that holds what it costs.
    int roomy = 0;
    for (int i = 0; i < (int)GetShipCatalog().size(); i++)
        if (GetShipCatalog()[i].stats.cargoCapacity > GetShipCatalog()[roomy].stats.cargoCapacity)
            roomy = i;
    const Vector2  spot = AnchorSpot(sim, *sim.SystemById(mouth), *gate);
    const Vector2  other = AnchorSpot(sim, *sim.SystemById(mouth), *gate, &spot);
    ClientSession& s = sim.CreateSession(mouth, spot, GetShipCatalog()[roomy].stats);
    s.accountName = "kessler";
    s.ownedShips = { roomy };
    s.currentShip = roomy;
    for (const auto& c : bp.cost)
        REQUIRE(s.ship->AddCargo(c.first, c.second));

    // Only at a mouth, and one to a mouth.
    const Vector2 far{ gate->GetPosition().x * 0.5f, gate->GetPosition().y * 0.5f };
    CHECK(sim.SpotProblem(*sim.SystemById(mouth), bp, far).find("mouth") != std::string::npos);
    const int id = sim.Deploy(s, Simulation::ANCHOR_BLUEPRINT, spot, "");
    REQUIRE(id != 0);
    CHECK(sim.SpotProblem(*sim.SystemById(mouth), bp, other).find("Anchor already") !=
          std::string::npos);

    // Finished, it holds the mouth: named for whoever built it, and no longer closing.
    At(sim, sim.Time() + bp.buildSeconds + 1.0);
    const Simulation::RegionOpening* o = sim.OpeningOf(slot);
    CHECK(o->state == "anchored");
    CHECK(o->anchoredBy == "kessler");
    CHECK(o->closesAt == 0.0);
    for (const std::string& end : { mouth, entry })
    {
        const Entity* g = GateTo(sim, end, end == mouth ? entry : mouth);
        REQUIRE(g != nullptr);
        CHECK(g->GetName() == "kessler Gate");
        CHECK_FALSE(static_cast<const JumpGate*>(g)->IsUnstable());
    }
    At(sim, closes + 3600.0);
    CHECK(sim.OpeningOf(slot)->state == "anchored");

    // Brought down, the mouth is unstable again, with another day to be anchored anew.
    REQUIRE(sim.DamageStructure(*sim.SystemById(mouth), id, 1.0e9f, nullptr));
    o = sim.OpeningOf(slot);
    CHECK(o->state == "unstable");
    CHECK(o->closesAt == doctest::Approx(sim.Time() + 24.0 * 3600.0));
    CHECK(static_cast<const JumpGate*>(GateTo(sim, mouth, entry))->IsUnstable());
}

TEST_CASE("a faction anchors a mouth by its own reckoning, and the same cold as hot (#342 S4)")
{
    LoadRegistries();
    Rules      r;
    Simulation cold, hot;
    hot.SetAllHot(true);
    for (Simulation* sim : { &cold, &hot })
    {
        Make(*sim, 7);
        RunMinutes(*sim, 2);  // the factions know their own space first
        const int slot = OpenFromCentre(*sim);
        // The mouth system's holder has the stock an Anchor takes.
        const SystemState* home = sim->SystemById(sim->OpeningOf(slot)->mouth);
        REQUIRE(home->agg.claimed);
        sim->MindOf(home->agg.controller).stock = 2.0f * Simulation::OutpostCost();
    }
    RunMinutes(cold, 70);
    RunMinutes(hot, 70);
    const Simulation::RegionOpening& a = cold.OpeningLog().back();
    const Simulation::RegionOpening& b = hot.OpeningLog().back();
    CHECK(a.state == "anchored");
    CHECK_FALSE(a.anchoredBy.empty());
    CHECK(a.state == b.state);
    CHECK(a.anchoredBy == b.anchoredBy);
    CHECK(a.anchoredAt == b.anchoredAt);
    bool began = false;
    for (const ChronicleEntry& e : cold.Chronicle())
        began =
            began || (e.kind == "anchor" && e.text.find("began an Anchor") != std::string::npos);
    CHECK(began);
}

TEST_CASE("a rival's Anchor at a mouth is besieged (#342 S4)")
{
    LoadRegistries();
    Rules      r;
    Simulation sim;
    Make(sim, 7);
    RunMinutes(sim, 2);
    const int         slot = OpenFromCentre(sim);
    const std::string mouth = sim.OpeningOf(slot)->mouth;
    const FactionId   holder = sim.SystemById(mouth)->agg.controller;
    REQUIRE(holder != Factions::Pirates());
    // The pirates are going up at the mouth, in the holder's own space.
    const Blueprint& bp = *Blueprints::Find(Simulation::ANCHOR_BLUEPRINT);
    const Entity*    gate = GateTo(sim, mouth, Simulation::EntryOf(slot));
    auto site = std::make_unique<Structure>(AnchorSpot(sim, *sim.SystemById(mouth), *gate), 0.0f,
                                            "Pirates Anchor", bp.archetype);
    site->SetBlueprint(bp.id);
    site->StartBuilding(sim.Time(), sim.Time() + bp.buildSeconds);
    REQUIRE(sim.AddStatic(mouth, std::move(site), Outposts::OwnerOf(Factions::Pirates())) != 0);
    REQUIRE(sim.SystemById(mouth)->profile.anchors.size() == 1);
    sim.MindOf(holder).stock = 2.0f * Simulation::OutpostCost();
    // ...and the holder at full strength in its space, with ships to lend.
    for (auto& kv : sim.Systems())
        if (kv.second.agg.claimed && kv.second.agg.controller == holder)
            kv.second.agg.presence[holder] = Factions::TemperamentOf(holder).capacity;

    bool besieged = false;
    for (int minute = 0; minute < 20 && !besieged; minute++)
    {
        RunMinutes(sim, 1);
        for (const auto& kv : sim.Plans())
            besieged =
                besieged || (kv.second.kind == Plan::Kind::Siege && kv.second.target == mouth &&
                             kv.second.against == Factions::Pirates());
        besieged = besieged || sim.SystemById(mouth)->profile.anchors.empty();
    }
    CHECK(besieged);
}

TEST_CASE("mouths, anchors and drift round-trip through the save (#342 S4)")
{
    LoadRegistries();
    Rules             r;
    TempDir           dir;
    const std::string seal = dir.MakeDir("regions");
    const std::string path = dir.Path("world.json");
    Simulation        a;
    Make(a, 9, seal);
    // Two mouths that collapse together; the second is then found again, unstable.
    const int   first = OpenFromCentre(a);
    std::string why;
    const int   second = a.OpenMouthNow(a.Skeleton().centres.back(), why);
    REQUIRE(second >= 0);
    At(a, a.OpeningOf(first)->closesAt + 1.0);
    CHECK(a.OpeningOf(first)->state == "drifted");
    CHECK(a.OpeningOf(second)->state == "drifted");
    CHECK(a.OpenMouthNow(a.OpeningOf(second)->parent, why) == second);
    REQUIRE(a.SaveWorld(path));

    Simulation::WorldHeader h;
    REQUIRE(Simulation::ReadWorldHeader(path, h));
    Simulation b;
    b.SetSealDir(seal);
    REQUIRE(b.OpenGalaxy(Params(9), DATA, h.log));
    REQUIRE(b.LoadWorld(path) == Save::Result::Ok);
    b.MaterializeAllSystems(DATA + "systems/");
    for (int slot : { first, second })
    {
        CAPTURE(slot);
        CHECK(b.OpeningOf(slot)->state == a.OpeningOf(slot)->state);
        CHECK(b.OpeningOf(slot)->closesAt == a.OpeningOf(slot)->closesAt);
        const std::string mouth = a.OpeningOf(slot)->mouth, entry = Simulation::EntryOf(slot);
        CHECK(b.PlanRoute(mouth, entry).size() == a.PlanRoute(mouth, entry).size());
        CHECK((GateTo(b, mouth, entry) == nullptr) == (GateTo(a, mouth, entry) == nullptr));
        CHECK((GateTo(b, entry, mouth) == nullptr) == (GateTo(a, entry, mouth) == nullptr));
    }
    const Entity* g = GateTo(b, b.OpeningOf(second)->mouth, Simulation::EntryOf(second));
    REQUIRE(g != nullptr);
    CHECK(static_cast<const JumpGate*>(g)->GetClosesAt() == a.OpeningOf(second)->closesAt);
}

TEST_CASE("at the cap a dead region drifts into the archive and comes back decayed (#342 S4)")
{
    LoadRegistries();
    Rules             r;
    TempDir           dir;
    const std::string seal = dir.MakeDir("regions");
    const std::string path = dir.Path("world.json");
    Simulation        sim;
    Make(sim, 9, seal);
    // A frontier the world began with, and what it has seen: a faction's outpost, a wreck, a
    // name a pilot gave.
    // Every frontier is as dead as the next: the first in the log goes first.
    int frontier = -1;
    for (const Simulation::RegionOpening& o : sim.OpeningLog())
        if (o.parent >= 0 && frontier < 0)
            frontier = o.slot;
    REQUIRE(frontier >= 0);
    const int         parent = sim.OpeningOf(frontier)->parent;
    const std::string system = sim.RegionSystems(frontier).back();
    const Blueprint&  outpost = *Blueprints::Find("outpost");
    auto t = std::make_unique<Structure>(Vector2{ 600000.0f, 0.0f }, 0.0f, "Syndicate Outpost",
                                         outpost.archetype);
    t->SetBlueprint(outpost.id);
    REQUIRE(sim.AddStatic(system, std::move(t), Outposts::OwnerOf(Factions::Pirates())) != 0);
    auto wreck =
        std::make_unique<Derelict>(Vector2{ 610000.0f, 0.0f }, 50.0f, "Wreck of Thing", 300.0);
    wreck->SetExpiresAt(sim.Time() + 10.0 * 86400.0);  // would rot, in a region that is kept
    REQUIRE(sim.AddStatic(system, std::move(wreck), std::string()) != 0);
    sim.Systems().at(system).agg.givenName = "Haven";

    // At the cap, and nobody comes: warned days before, then gone.
    sim.SetGrowthCap((int)sim.OpenSlots().size(), 0);
    REQUIRE(sim.AtCap());
    WorldRules::Archive rules;
    rules.deadDays = 2.0;
    rules.warnDays = 1.0;
    rules.returnChance = 1.0;
    WorldRules::SetArchiveRules(rules);
    At(sim, 1.5 * 86400.0);
    REQUIRE(sim.OpeningOf(frontier) != nullptr);
    CHECK(sim.OpeningOf(frontier)->fading);
    At(sim, 2.1 * 86400.0);
    // (any frontier could have gone; this is the one with nothing opened from it)
    REQUIRE(sim.Archives().size() >= 1);
    int id = -1;
    for (const auto& kv : sim.Archives())
        if (kv.second.slot == frontier)
            id = kv.first;
    REQUIRE(id >= 0);
    CHECK(sim.OpeningOf(frontier) == nullptr);
    CHECK(sim.OpenSlots().count(frontier) == 0);
    CHECK_FALSE(sim.HasSystem(system));
    CHECK_FALSE(sim.AtCap());
    CHECK(std::ifstream(Simulation::ArchivePath(seal, id)).is_open());
    CHECK_FALSE(std::ifstream(Simulation::SealedPath(seal, frontier)).is_open());

    // The archive outlives a restart.
    sim.PauseGrowth(true);
    REQUIRE(sim.SaveWorld(path));
    Simulation::WorldHeader h;
    REQUIRE(Simulation::ReadWorldHeader(path, h));
    Simulation b;
    b.SetSealDir(seal);
    REQUIRE(b.OpenGalaxy(Params(9), DATA, h.log));
    REQUIRE(b.LoadWorld(path) == Save::Result::Ok);
    b.MaterializeAllSystems(DATA + "systems/");
    REQUIRE(b.Archives().count(id) == 1);
    CHECK(b.Archives().at(id).text == sim.Archives().at(id).text);
    CHECK(b.OpenSlots() == sim.OpenSlots());

    // The next slot the parent opens takes it back: decayed, renamed for where it is now.
    std::string why;
    const int   slot = b.OpenMouthNow(parent, why);
    REQUIRE(slot >= 0);
    CHECK(b.Archives().empty());
    const Simulation::RegionOpening* o = b.OpeningOf(slot);
    REQUIRE(o != nullptr);
    CHECK(o->archive == id);
    CHECK(o->state == "unstable");
    const std::string back = "r" + std::to_string(slot) + system.substr(system.find('-'));
    REQUIRE(b.HasSystem(back));
    const SystemState& st = b.Systems().at(back);
    CHECK(st.agg.givenName == "Haven");
    bool ruin = false, wreckStays = false, outpostGone = true;
    for (const auto& e : st.entities)
    {
        if (e->GetKind() == EntityKind::Derelict)
        {
            const Derelict& d = static_cast<const Derelict&>(*e);
            ruin = ruin || (d.GetBaseName() == "Ruin of Syndicate Outpost" && d.GetReward() > 0.0);
            wreckStays =
                wreckStays || (d.GetBaseName() == "Wreck of Thing" && d.GetExpiresAt() == 0.0);
        }
        if (e->GetKind() == EntityKind::Structure && e->GetName() == "Syndicate Outpost")
            outpostGone = false;
    }
    CHECK(ruin);
    CHECK(wreckStays);
    CHECK(outpostGone);
    bool told = false;
    for (const ChronicleEntry& e : b.Chronicle())
        told = told || (e.kind == "drift" && e.text.find("drifted back") != std::string::npos);
    CHECK(told);
    // ...and sealed as it came back: a restart reads it, not the slot's own.
    Gen::Region sealed;
    REQUIRE(Simulation::ReadSealed(Simulation::SealedPath(seal, slot), 9, sealed));
    CHECK(sealed.documents.count(back) == 1);
    CHECK_FALSE(std::ifstream(Simulation::ArchivePath(seal, id)).is_open());
}

TEST_CASE("a route order plans over what its pilot knows, and round a heavy mouth (#432)")
{
    LoadRegistries();
    Rules      r;
    Simulation sim;
    Make(sim, 7);
    const int                        slot = OpenFromCentre(sim);
    const Simulation::RegionOpening& o = *sim.OpeningOf(slot);
    const std::string                mouth = o.mouth;
    const std::string                entry = Simulation::EntryOf(slot);
    int                              light = 0, heavy = 0;
    for (int i = 0; i < (int)GetShipCatalog().size(); i++)
    {
        if (MassOf(GetShipCatalog()[i].stats) < MassOf(GetShipCatalog()[light].stats))
            light = i;
        if (MassOf(GetShipCatalog()[i].stats) > MassOf(GetShipCatalog()[heavy].stats))
            heavy = i;
    }
    WorldRules::Mouths m;
    m.maxMass = (MassOf(GetShipCatalog()[light].stats) + MassOf(GetShipCatalog()[heavy].stats)) / 2;
    WorldRules::SetMouthRules(m);
    ClientSession& s = sim.CreateSession(mouth, { 0.0f, 0.0f }, GetShipCatalog()[light].stats);
    s.ownedShips = { light, heavy };
    s.currentShip = light;

    // The index marks the mouth as a crossing that closes and lets only so much by.
    bool marked = false;
    for (const WorldLoader::SystemLink& l : sim.KnownUniverseFor(s).links)
        if (l.kind == "mouth")
        {
            marked = true;
            CHECK(((l.a == mouth && l.b == entry) || (l.a == entry && l.b == mouth)));
            CHECK(l.closesAt == doctest::Approx(o.closesAt));
            CHECK(l.maxMass == doctest::Approx(m.maxMass));
        }
    CHECK(marked);

    Orders::Order order;
    order.kind = Orders::Kind::Route;
    order.destSystem = entry;
    sim.GiveOrder(s, order);
    CHECK(sim.RouteFor(s).path == std::vector<std::string>{ mouth, entry });

    // Too heavy for it: no way, and told why.
    s.currentShip = heavy;
    const Route::Result barred = sim.RouteFor(s);
    CHECK(barred.path.empty());
    CHECK(barred.reason.find("too heavy") != std::string::npos);
    s.currentShip = light;

    // Deep in the region the world has a way, but nobody has charted it: no order goes there
    // blind -- the frontier one gate past the charted is as far as a route can be planned.
    std::set<std::string> known;
    for (const WorldLoader::SystemInfo& si : sim.KnownUniverseFor(s).systems)
        known.insert(si.id);
    std::string deep;
    for (const std::string& id : sim.RegionSystems(slot))
        if (known.count(id) == 0)
            deep = id;
    REQUIRE_FALSE(deep.empty());
    CHECK_FALSE(sim.PlanRoute(mouth, deep).empty());
    order.destSystem = deep;
    sim.GiveOrder(s, order);
    const Route::Result blind = sim.RouteFor(s);
    CHECK(blind.path.empty());
    CHECK(blind.reason.find("known map") != std::string::npos);
    sim.StepPlayerOrder(s, *sim.SystemOf(s), 1.0f / 60.0f);
    CHECK(s.orderStatus == Orders::Status::Failed);
}
