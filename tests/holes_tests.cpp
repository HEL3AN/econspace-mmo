#include <doctest/doctest.h>

#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "core/ShipDesign.h"
#include "entities/JumpGate.h"
#include "entities/Ship.h"
#include "entities/ShipType.h"
#include "entities/Structure.h"
#include "gen/Galaxy.h"
#include "gen/Region.h"
#include "sim/FactionMind.h"
#include "sim/Protocol.h"
#include "sim/SaveSchema.h"
#include "sim/Simulation.h"
#include "sim/WorldRules.h"

#include "temp_dir.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>

// Roaming holes (#342 S9) and the scanner that finds what is hidden (#346).

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

void Make(Simulation& sim, uint64_t seed, const std::vector<Simulation::RegionOpening>& log = {})
{
    REQUIRE(sim.OpenGalaxy(Params(seed), DATA, log));
    sim.InitGalaxy();
    sim.MaterializeAllSystems(DATA + "systems/");
}

// The design's rules, whatever the last test set; restored when it ends.
struct Rules
{
    WorldRules::Holes  holes = WorldRules::HoleRules();
    WorldRules::Scan   scan = WorldRules::ScanRules();
    WorldRules::Tide   tide = WorldRules::TideRules();
    WorldRules::Mouths mouths = WorldRules::MouthRules();
    Rules()
    {
        WorldRules::SetHoleRules(WorldRules::Holes{});
        WorldRules::SetScanRules(WorldRules::Scan{});
        WorldRules::SetTideRules(WorldRules::Tide{});
        WorldRules::SetMouthRules(WorldRules::Mouths{});
    }
    ~Rules()
    {
        WorldRules::SetHoleRules(holes);
        WorldRules::SetScanRules(scan);
        WorldRules::SetTideRules(tide);
        WorldRules::SetMouthRules(mouths);
    }
};

// Two systems of two different regions.
void TwoRegions(const Simulation& sim, std::string& a, std::string& b)
{
    for (const auto& si : sim.Universe().systems)
        if (a.empty())
            a = si.id;
        else if (Gen::SlotOf(si.id) != Gen::SlotOf(a) && b.empty())
            b = si.id;
    REQUIRE(!a.empty());
    REQUIRE(!b.empty());
}

const Entity* ById(const Simulation& sim, const std::string& systemId, int id)
{
    if (const SystemState* st = sim.SystemById(systemId))
        for (const auto& e : st->entities)
            if (e->GetId() == id)
                return e.get();
    return nullptr;
}

bool InLayout(const Proto::SystemLayout& lay, int id)
{
    for (const Proto::EntityLayout& e : lay.entities)
        if (e.id == id)
            return true;
    return false;
}

float MassOf(int hull)
{
    const Ships::Catalogue& c = Archetypes::ShipCatalogue();
    const Ships::Design*    d = c.FindDesign(GetShipCatalog()[hull].stats.design);
    Ships::Stats            s;
    std::string             why;
    REQUIRE(d != nullptr);
    REQUIRE(Ships::Derive(c, *d, s, why));
    return s.mass;
}

// One whole pass of the scanner, started now and run to its end.
void Pass(Simulation& sim, ClientSession& s)
{
    REQUIRE(sim.StartScan(s));
    sim.SetTime(sim.Time() + WorldRules::ScanRules().seconds + 0.1);
    sim.StepPlayerScan(s);
}
}  // namespace

TEST_CASE("the hole and scan rules read, and a misspelt one is an error (#342 S9, #346)")
{
    Rules r;
    REQUIRE(WorldRules::Load(DATA + "world_rules.json"));
    CHECK(WorldRules::HoleRules().epochHours == doctest::Approx(4.0));
    CHECK(WorldRules::HoleRules().smallMass == doctest::Approx(45.0));
    CHECK(WorldRules::ScanRules().seconds == doctest::Approx(10.0));
    TempDir dir;
    for (const char* bad : { R"({"tide": {}, "holes": {"epochHour": 4}})",
                             R"({"tide": {}, "holes": {"perRegion": 2}})",
                             R"({"tide": {}, "scan": {"seconds": 0}})" })
    {
        const std::string path = dir.Path("rules.json");
        std::ofstream(path) << bad;
        CHECK_FALSE(WorldRules::Load(path));
        CHECK(!WorldRules::Error().empty());
    }
}

TEST_CASE("an epoch's holes are the seed's: the same every time, between two regions (#342 S9)")
{
    LoadRegistries();
    Rules             r;
    WorldRules::Holes h;
    h.perRegion = 1.0;  // every region, so there is something to compare
    WorldRules::SetHoleRules(h);
    Simulation a, b;
    Make(a, 11);
    Make(b, 11);
    a.ScheduleHoles(3);
    b.ScheduleHoles(3);
    a.ScheduleHoles(3);  // an epoch is decided once
    REQUIRE(!a.RoamingHoles().empty());
    REQUIRE(a.RoamingHoles().size() == b.RoamingHoles().size());
    std::set<std::string> ids;
    for (size_t i = 0; i < a.RoamingHoles().size(); i++)
    {
        const Simulation::RoamingHole& x = a.RoamingHoles()[i];
        const Simulation::RoamingHole& y = b.RoamingHoles()[i];
        CAPTURE(x.id);
        CHECK(ids.insert(x.id).second);
        CHECK(x.id == y.id);
        CHECK(x.a == y.a);
        CHECK(x.b == y.b);
        CHECK(x.atA.x == y.atA.x);
        CHECK(x.atB.y == y.atB.y);
        CHECK(x.opensAt == y.opensAt);
        CHECK(x.closesAt == y.closesAt);
        CHECK(Gen::SlotOf(x.a) != Gen::SlotOf(x.b));
        CHECK(Gen::SlotOf(x.a) == x.slot);
        // In its epoch, for four hours to a day, of one of the three classes.
        CHECK(x.opensAt >= 3 * 4 * 3600.0);
        CHECK(x.opensAt < 4 * 4 * 3600.0);
        CHECK(x.closesAt - x.opensAt >= 4 * 3600.0 - 1e-6);
        CHECK(x.closesAt - x.opensAt <= 24 * 3600.0 + 1e-6);
        CHECK((x.maxMass == 45.0 || x.maxMass == 60.0 || x.maxMass == 100.0));
        CHECK(x.budget >= x.maxMass * 4.0 - 1e-6);
        CHECK(x.budget <= x.maxMass * 10.0 + 1e-6);
        CHECK_FALSE(x.open);
    }

    // On the clock: open when its time comes, its two gates hidden from whoever has not
    // found it, and gone when it ends.
    const Simulation::RoamingHole first = a.RoamingHoles().front();
    a.SetTime(first.opensAt + 1.0);
    a.StepHoles();
    const Simulation::RoamingHole* open = a.HoleById(first.id);
    REQUIRE(open != nullptr);
    CHECK(open->open);
    REQUIRE(open->gateA != 0);
    REQUIRE(open->gateB != 0);
    const Entity* g = ById(a, first.a, open->gateA);
    REQUIRE(g != nullptr);
    CHECK(g->GetArchetype()->id == "gate.roaming");
    CHECK(static_cast<const JumpGate*>(g)->GetDestination() == first.b);
    CHECK(Simulation::SignatureOf(*g) > 0.0f);
    const int gid = open->gateA;
    a.SetTime(first.closesAt + 1.0);
    a.StepHoles();
    CHECK(a.HoleById(first.id) == nullptr);
    CHECK(ById(a, first.a, gid) == nullptr);
}

TEST_CASE("a region cut off by a collapsed mouth gets a hole out every epoch (#342 S9)")
{
    LoadRegistries();
    Rules             r;
    WorldRules::Holes h;
    h.perRegion = 0.0;  // nobody else
    WorldRules::SetHoleRules(h);
    Simulation sim;
    Make(sim, 7);
    std::string why;
    const int   slot = sim.OpenMouthNow(sim.Skeleton().centres.front(), why);
    REQUIRE(slot >= 0);
    sim.SetTime(sim.OpeningOf(slot)->closesAt + 1.0);
    for (int i = 0; i < 40; i++)
        sim.MaintainWorld(2.0f);
    REQUIRE(sim.OpeningOf(slot)->state == "drifted");
    for (int epoch = 20; epoch < 24; epoch++)
    {
        sim.ScheduleHoles(epoch);
        int out = 0;
        for (const Simulation::RoamingHole& x : sim.RoamingHoles())
            if (x.epoch == epoch)
            {
                CHECK(x.slot == slot);
                CHECK(Gen::SlotOf(x.b) != slot);
                out++;
            }
        CHECK(out == 1);
    }
}

TEST_CASE("a hole is hidden until scanned, then a route of the pilot's own (#342 S9, #346)")
{
    LoadRegistries();
    Rules      r;
    Simulation sim;
    Make(sim, 7);
    std::string a, b;
    TwoRegions(sim, a, b);
    int light = 0, heavy = 0;
    for (int i = 0; i < (int)GetShipCatalog().size(); i++)
    {
        if (MassOf(i) < MassOf(light))
            light = i;
        if (MassOf(i) > MassOf(heavy))
            heavy = i;
    }
    const double      limit = (MassOf(light) + MassOf(heavy)) / 2.0;
    const std::string id = sim.OpenHoleNow(a, b, limit, 10.0, MassOf(light) * 1.5);
    REQUIRE(!id.empty());
    const Simulation::RoamingHole* h = sim.HoleById(id);
    REQUIRE(h != nullptr);
    sim.TakeLayoutDeltas();
    const Entity* gate = ById(sim, a, h->gateA);
    REQUIRE(gate != nullptr);

    ClientSession& s = sim.CreateSession(a, gate->GetPosition(), GetShipCatalog()[light].stats);
    s.accountName = "seer";
    s.ownedShips = { light, heavy };
    s.currentShip = light;
    ClientSession& other = sim.CreateSession(a, { 0.0f, 0.0f }, GetShipCatalog()[light].stats);
    other.accountName = "other";

    // Hidden: not in the layout, not on the scanner, not a way anywhere.
    CHECK_FALSE(InLayout(sim.BuildLayoutFor(s, a), gate->GetId()));
    CHECK(InLayout(sim.BuildLayout(a), gate->GetId()));
    CHECK(sim.ScanViewOf(s).sigs.empty());
    CHECK(sim.JumpGateDestIfNear(s, *sim.SystemById(a), gate->GetId()).empty());
    CHECK(sim.HoleLinksOf(s).empty());

    // A pass resolves half of it: a signature, by code, and nothing more.
    Pass(sim, s);
    Proto::ScanView v = sim.ScanViewOf(s);
    REQUIRE(v.sigs.size() == 1);
    CHECK(v.sigs[0].progress == doctest::Approx(0.5f));
    CHECK(v.sigs[0].kind.empty());
    CHECK(v.sigs[0].id == 0);
    CHECK_FALSE(InLayout(sim.BuildLayoutFor(s, a), gate->GetId()));
    // The second finds it: the pilot's own delta, then their layout, index and routes.
    Pass(sim, s);
    v = sim.ScanViewOf(s);
    REQUIRE(v.sigs.size() == 1);
    CHECK(v.sigs[0].progress == doctest::Approx(1.0f));
    CHECK(v.sigs[0].kind == "wormhole");
    CHECK(v.sigs[0].dest == b);
    CHECK(v.sigs[0].id == gate->GetId());
    CHECK(v.sigs[0].code == Simulation::SignatureCode("hole:" + id));
    CHECK(s.routesChanged);
    const auto reveals = sim.TakeRevealDeltas();
    REQUIRE(reveals.size() == 1);
    CHECK(reveals[0].first == s.id);
    REQUIRE(reveals[0].second.added.size() == 1);
    CHECK(reveals[0].second.added[0].id == gate->GetId());
    CHECK(reveals[0].second.rev == sim.SystemById(a)->layoutRev);
    CHECK(InLayout(sim.BuildLayoutFor(s, a), gate->GetId()));
    CHECK_FALSE(InLayout(sim.BuildLayoutFor(other, a), gate->GetId()));
    bool linked = false;
    for (const WorldLoader::SystemLink& l : sim.KnownUniverseFor(s).links)
        linked = linked || (l.kind == "hole" && l.a == a && l.b == b && l.maxMass > 0.0f);
    CHECK(linked);
    for (const WorldLoader::SystemLink& l : sim.KnownUniverseFor(other).links)
        CHECK(l.kind.empty());
    const std::vector<WorldLoader::SystemLink> mine = sim.HoleLinksOf(s);
    CHECK(sim.PlanRoute(a, b, false, &mine).size() == 2);
    // The first to find it is news; finding it again is not.
    int credited = 0;
    for (const ChronicleEntry& e : sim.Chronicle())
        credited += e.kind == "scan" ? 1 : 0;
    CHECK(credited == 1);

    // Too heavy for its class: refused, and told why.
    s.currentShip = heavy;
    CHECK(sim.MouthProblem(s, *gate).find("wormhole") != std::string::npos);
    CHECK(sim.JumpGateDestIfNear(s, *sim.SystemById(a), gate->GetId()).empty());
    // Light enough: through to its far end, and its mass spent.
    s.currentShip = light;
    REQUIRE(sim.JumpGateDestIfNear(s, *sim.SystemById(a), gate->GetId()) == b);
    sim.EnterThrough(s, gate->GetId(), b);
    CHECK(s.systemId == b);
    const Entity* far = ById(sim, b, h->gateB);
    REQUIRE(far != nullptr);
    const float dx = s.ship->GetPosition().x - far->GetPosition().x;
    const float dy = s.ship->GetPosition().y - far->GetPosition().y;
    CHECK(std::sqrt(dx * dx + dy * dy) < far->GetSize() + 1000.0f);
    CHECK(sim.HoleById(id)->spent == doctest::Approx(MassOf(light)));
    // ...and back, which spends what is left of it: it collapses behind the ship.
    const int farId = far->GetId(), nearId = gate->GetId();
    sim.EnterThrough(s, farId, a);
    CHECK(s.systemId == a);
    sim.StepHoles();
    CHECK(sim.HoleById(id) == nullptr);
    CHECK(ById(sim, a, nearId) == nullptr);
    CHECK(ById(sim, b, farId) == nullptr);
    CHECK(s.scanned.count("hole:" + id) == 0);
    CHECK(sim.HoleLinksOf(s).empty());
}

TEST_CASE("a hidden base is found by scanning, and the find is the pilot's across a save (#346)")
{
    LoadRegistries();
    Rules      r;
    Simulation sim;
    Make(sim, 7);
    std::string a, b;
    TwoRegions(sim, a, b);
    const int base =
        sim.AddStatic(a,
                      std::make_unique<Structure>(Vector2{ 300000.0f, 0.0f }, 240.0f, "Den",
                                                  std::string("structure.outpost_hidden")),
                      std::string());
    REQUIRE(base != 0);
    sim.TakeLayoutDeltas();
    ClientSession& s = sim.CreateSession(a, { 0.0f, 0.0f }, GetShipCatalog()[0].stats);
    s.accountName = "seer";
    CHECK_FALSE(InLayout(sim.BuildLayoutFor(s, a), base));

    // A delta about it is cut for whoever has not found it.
    Proto::LayoutDelta d;
    d.systemId = a;
    for (const Proto::EntityLayout& e : sim.BuildLayout(a).entities)
        if (e.id == base)
            d.changed.push_back(e);
    REQUIRE(d.changed.size() == 1);
    sim.ForSession(s, d);
    CHECK(d.changed.empty());

    // Strength 0.35: three passes.
    Pass(sim, s);
    Pass(sim, s);
    CHECK_FALSE(InLayout(sim.BuildLayoutFor(s, a), base));
    Pass(sim, s);
    CHECK(InLayout(sim.BuildLayoutFor(s, a), base));
    const Proto::ScanView v = sim.ScanViewOf(s);
    REQUIRE(v.sigs.size() == 1);
    CHECK(v.sigs[0].kind == "hidden base");
    CHECK(v.sigs[0].id == base);

    // Docked, nothing scans.
    s.dockedStationId = 1;
    CHECK_FALSE(sim.StartScan(s));
    s.dockedStationId = 0;

    TempDir           dir;
    const std::string path = dir.Path("account.json");
    REQUIRE(sim.SaveAccount(s, path));
    ClientSession& again = sim.CreateSession(a, { 0.0f, 0.0f }, GetShipCatalog()[0].stats);
    again.accountName = "seer";
    REQUIRE(sim.LoadAccount(again, path) == Save::Result::Ok);
    CHECK(InLayout(sim.BuildLayoutFor(again, a), base));
}

TEST_CASE("a hidden base that fires on a pilot is revealed to them, and to nobody else (#415)")
{
    LoadRegistries();
    Rules      r;
    Simulation sim;
    Make(sim, 7);
    std::string a, b;
    TwoRegions(sim, a, b);
    auto& ents = sim.SystemById(a)->entities;
    ents.erase(std::remove_if(ents.begin(), ents.end(), [](const std::unique_ptr<Entity>& e)
                              { return e->GetKind() == EntityKind::Npc; }),
               ents.end());
    const Vector2 at{ 300000.0f, 0.0f };
    const int     base = sim.AddStatic(
        a, std::make_unique<Structure>(at, 240.0f, "Den", std::string("structure.outpost_hidden")),
        Outposts::OwnerOf(Factions::Pirates()));
    REQUIRE(base != 0);
    sim.TakeLayoutDeltas();

    // One pilot in its reach, one beside it who is not a target -- docked, say.
    ClientSession& victim =
        sim.CreateSession(a, { at.x + 400.0f, at.y }, GetShipCatalog()[0].stats);
    victim.accountName = "victim";
    ClientSession& other = sim.CreateSession(a, { at.x - 400.0f, at.y }, GetShipCatalog()[0].stats);
    other.accountName = "other";
    CHECK_FALSE(InLayout(sim.BuildLayoutFor(victim, a), base));
    std::vector<Simulation::PlayerPresence> players(1);
    players[0].session = &victim;
    players[0].ship = victim.ship.get();

    // The tick it fires is the tick it is found: the delta is the victim's before the
    // snapshot that shows the shot can be built.
    std::vector<FireEvent> fires;
    for (int i = 0; i < 120 && fires.empty(); i++)
        sim.StepStationDefence(*sim.SystemById(a), players, &fires, 1.0f / 60.0f);
    REQUIRE(fires.size() == 1);
    CHECK(fires[0].targetSessionId == victim.id);
    CHECK(fires[0].shooterEntityId == base);
    const auto reveals = sim.TakeRevealDeltas();
    REQUIRE(reveals.size() == 1);
    CHECK(reveals[0].first == victim.id);
    REQUIRE(reveals[0].second.added.size() == 1);
    CHECK(reveals[0].second.added[0].id == base);

    // Found as a scan finds it: the account has it, the journal says how.
    const std::string key = sim.SignatureKey(*sim.SystemById(a), *ById(sim, a, base));
    CHECK(victim.scanned.count(key) == 1);
    bool told = false;
    for (const Ev::Event& e : victim.journal)
        told = told || e.text.find("revealed by its fire") != std::string::npos;
    CHECK(told);
    CHECK(InLayout(sim.BuildLayoutFor(victim, a), base));
    CHECK_FALSE(sim.BeamHiddenFrom(victim, a, fires[0]));

    // The other pilot: not in the layout, not the beam, and not a target for their guns.
    CHECK_FALSE(InLayout(sim.BuildLayoutFor(other, a), base));
    CHECK(other.scanned.empty());
    CHECK(sim.BeamHiddenFrom(other, a, fires[0]));
    other.weaponOn = true;
    CHECK_FALSE(sim.StepPlayerFire(other, *sim.SystemById(a), base, 1.0f / 60.0f, nullptr));
    victim.weaponOn = true;
    CHECK(sim.StepPlayerFire(victim, *sim.SystemById(a), base, 1.0f / 60.0f, nullptr));

    // Found once: the next volley tells nobody anything new.
    for (int i = 0; i < 120; i++)
        sim.StepStationDefence(*sim.SystemById(a), players, &fires, 1.0f / 60.0f);
    CHECK(fires.size() > 1);
    CHECK(sim.TakeRevealDeltas().empty());
}

TEST_CASE("holes and finds round-trip through the world save (#342 S9)")
{
    LoadRegistries();
    Rules             r;
    TempDir           dir;
    const std::string path = dir.Path("world.json");
    Simulation        a;
    Make(a, 9);
    std::string x, y;
    TwoRegions(a, x, y);
    const std::string id = a.OpenHoleNow(x, y, 60.0, 8.0, 500.0);
    REQUIRE(!id.empty());
    REQUIRE(a.SaveWorld(path));

    Simulation::WorldHeader h;
    REQUIRE(Simulation::ReadWorldHeader(path, h));
    Simulation b;
    REQUIRE(b.OpenGalaxy(Params(9), DATA, h.log));
    REQUIRE(b.LoadWorld(path) == Save::Result::Ok);
    b.MaterializeAllSystems(DATA + "systems/");
    const Simulation::RoamingHole* got = b.HoleById(id);
    REQUIRE(got != nullptr);
    CHECK(got->a == x);
    CHECK(got->b == y);
    CHECK(got->maxMass == doctest::Approx(60.0));
    CHECK(got->closesAt == doctest::Approx(a.HoleById(id)->closesAt));
    b.StepHoles();  // its gates go up on the first pass
    CHECK(ById(b, x, b.HoleById(id)->gateA) != nullptr);
    CHECK(ById(b, y, b.HoleById(id)->gateB) != nullptr);
}

TEST_CASE("a scan and the scanner survive the wire (#346)")
{
    Proto::Command c;
    c.scan = true;
    Proto::Command back;
    REQUIRE(Proto::DecodeCommand(Proto::EncodeCommand(c), back));
    CHECK(back.scan);

    WorldLoader::Universe u;
    u.links.push_back({ "r1-1-1", "r2-1-1" });
    WorldLoader::SystemLink hole{ "r1-1-2", "r3-2-1" };
    hole.kind = "hole";
    hole.closesAt = 1234.5;
    hole.maxMass = 60.0f;
    u.links.push_back(hole);
    WorldLoader::Universe got;
    REQUIRE(Proto::DecodeUniverse(Proto::EncodeUniverse(u), got));
    REQUIRE(got.links.size() == 2);
    CHECK(got.links[0].kind.empty());
    CHECK(got.links[1].kind == "hole");
    CHECK(got.links[1].closesAt == doctest::Approx(1234.5));
    CHECK(got.links[1].maxMass == doctest::Approx(60.0f));
}
