#include <doctest/doctest.h>

#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "entities/ShipType.h"
#include "entities/Structure.h"
#include "gen/Galaxy.h"
#include "gen/Region.h"
#include "sim/HostRules.h"
#include "sim/Protocol.h"
#include "sim/SaveSchema.h"
#include "sim/Simulation.h"
#include "sim/WorldRules.h"

#include "temp_dir.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

// How the world grows (#342 S3): the opening log, sealed regions, the tide and the cap.

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

// A world as the server makes it, sealed under `seal` (empty: in memory), from `log` when
// there is one.
void Make(Simulation& sim, uint64_t seed, const std::string& seal = std::string(),
          const std::vector<Simulation::RegionOpening>& log = {})
{
    sim.SetSealDir(seal);
    REQUIRE(sim.OpenGalaxy(Params(seed), DATA, log));
    sim.InitGalaxy();
    sim.MaterializeAllSystems(DATA + "systems/");
}

// The world's coarse passes for `minutes` of world time, one pass a call.
void RunMinutes(Simulation& sim, double minutes)
{
    for (int i = 0; i < (int)(minutes * 30.0); i++)
        sim.MaintainWorld(2.0f);
}

// Rules that open a region every few minutes rather than every few days, so a test sees a
// dozen; the defaults are restored when it ends. Mouths are permanent at once here (option
// A): these are the tide's tests, and an unstable mouth's are in mouth_tests.cpp (S4).
struct FastTide
{
    WorldRules::Tide   saved = WorldRules::TideRules();
    WorldRules::Mouths savedMouths = WorldRules::MouthRules();
    explicit FastTide(bool surveys = true)
    {
        WorldRules::Tide t;
        t.threshold = 10.0;
        t.base = 3.0;
        t.minGapHours = 0.1;
        t.mouthHours = 0.1;
        if (!surveys)
            t.perSurvey = t.surveyedWeight = 0.0;
        WorldRules::SetTideRules(t);
        WorldRules::Mouths m;
        m.anchors = false;
        WorldRules::SetMouthRules(m);
    }
    ~FastTide()
    {
        WorldRules::SetTideRules(saved);
        WorldRules::SetMouthRules(savedMouths);
    }
};

bool SameLog(const std::vector<Simulation::RegionOpening>& a,
             const std::vector<Simulation::RegionOpening>& b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); i++)
        if (a[i].slot != b[i].slot || a[i].parent != b[i].parent || a[i].mouth != b[i].mouth ||
            a[i].openedAt != b[i].openedAt || a[i].by != b[i].by || a[i].why != b[i].why)
            return false;
    return true;
}

bool SameTides(const Simulation& a, const Simulation& b)
{
    if (a.Tides().size() != b.Tides().size())
        return false;
    for (const auto& kv : a.Tides())
    {
        const auto it = b.Tides().find(kv.first);
        if (it == b.Tides().end() || it->second.tide != kv.second.tide ||
            it->second.lastMouthAt != kv.second.lastMouthAt)
            return false;
    }
    return true;
}
}  // namespace

TEST_CASE("the world data's rules read, and a misspelt one is an error (#342 S3)")
{
    const WorldRules::Tide saved = WorldRules::TideRules();
    REQUIRE(WorldRules::Load(DATA + "world_rules.json"));
    CHECK(WorldRules::TideRules().playerCap == 10);
    CHECK(WorldRules::TideRules().minGapHours == doctest::Approx(6.0));

    TempDir           dir;
    const std::string path = dir.Path("rules.json");
    {
        std::ofstream f(path);
        f << R"({ "tide": { "threshold": 50, "perPlayr": 1 } })";
    }
    CHECK_FALSE(WorldRules::Load(path));
    CHECK(WorldRules::Error().find("perPlayr") != std::string::npos);
    CHECK(WorldRules::TideRules().threshold == doctest::Approx(100.0));  // unchanged
    WorldRules::SetTideRules(saved);
}

TEST_CASE("a world is its seed plus its log, and both round-trip (#342 S3)")
{
    LoadRegistries();
    FastTide          fast;
    TempDir           dir;
    const std::string seal = dir.MakeDir("regions");
    const std::string path = dir.Path("world.json");

    Simulation a;
    Make(a, 4, seal);
    const size_t first = a.OpeningLog().size();
    CHECK(first == 6);  // three centres and a frontier beside each
    RunMinutes(a, 40);
    REQUIRE(a.OpeningLog().size() > first + 1);
    a.PauseGrowth(true);
    REQUIRE(a.SaveWorld(path));

    Simulation::WorldHeader h;
    REQUIRE(Simulation::ReadWorldHeader(path, h));
    CHECK(h.seed == 4);
    CHECK(h.generator == Gen::GENERATOR_VERSION);
    CHECK(h.skeleton == Gen::SKELETON_VERSION);
    CHECK(SameLog(h.log, a.OpeningLog()));
    for (const Simulation::RegionOpening& o : h.log)
    {
        CAPTURE(o.slot);
        CHECK(std::ifstream(Simulation::SealedPath(seal, o.slot)).is_open());  // every one sealed
        if (o.by == "tide")
            CHECK(o.why.find("time ") == 0);
    }

    // Made again from the seed and the log, it is the same galaxy, tide and all.
    // In the order the server does it: the galaxy, what the save says of it, its systems.
    Simulation b;
    b.SetSealDir(seal);
    REQUIRE(b.OpenGalaxy(Params(4), DATA, h.log));
    CHECK(b.OpenSlots() == a.OpenSlots());
    CHECK(b.RegionDocuments() == a.RegionDocuments());
    CHECK(b.Universe().links.size() == a.Universe().links.size());
    CHECK(SameLog(b.OpeningLog(), a.OpeningLog()));
    REQUIRE(b.LoadWorld(path) == Save::Result::Ok);
    b.MaterializeAllSystems(DATA + "systems/");
    CHECK(SameTides(a, b));
    CHECK(b.GrowthPaused());
    // ...and the wormhole of a region the tide opened is in the system it opened from.
    const Simulation::RegionOpening& last = b.OpeningLog().back();
    bool                             gate = false;
    for (const Proto::EntityLayout& e : b.BuildLayout(last.mouth).entities)
        gate = gate || (e.kind == EntityKind::Gate && Gen::SlotOf(e.dest) == last.slot);
    CHECK(gate);
}

TEST_CASE("a sealed region is read as it was made, whatever the rules are now (#342 S3)")
{
    LoadRegistries();
    TempDir           dir;
    const std::string seal = dir.MakeDir("regions");
    const std::string path = dir.Path("world.json");

    Simulation a;
    Make(a, 9, seal);
    std::string why;
    const int   slot = a.OpenMouthNow(-1, why);
    REQUIRE(slot >= 0);
    REQUIRE(a.SaveWorld(path));

    // A build with other rules made this region: what it made is what the file says. Here
    // the "old rules" named its first system differently.
    const std::string file = Simulation::SealedPath(seal, slot);
    nlohmann::json    doc;
    {
        std::ifstream in(file);
        doc = nlohmann::json::parse(in);
    }
    const std::string first = doc["region"]["systems"][0]["id"];
    doc["region"]["systems"][0]["name"] = "Old Rules";
    {
        std::ofstream out(file);
        out << doc.dump();
    }
    Gen::Region check;
    CHECK(Simulation::ReadSealed(file, 9, check));
    CHECK_FALSE(Simulation::ReadSealed(file, 10, check));  // not another world's

    // A world made under other region rules goes on, because all of it is sealed...
    Simulation::WorldHeader h;
    REQUIRE(Simulation::ReadWorldHeader(path, h));
    h.generator = Gen::GENERATOR_VERSION + 1;
    CHECK(Host::WorldRulesRefusal(h, seal).empty());
    // ...and the region is the one in the file, not what this build would make.
    Simulation b;
    Make(b, 9, seal, h.log);
    const WorldLoader::SystemInfo* info = b.SystemInfoById(first);
    REQUIRE(info != nullptr);
    CHECK(info->name == "Old Rules");
    CHECK(b.SealedText(slot).find("Old Rules") != std::string::npos);
    // The file is never written again by loading it.
    {
        std::ifstream in(file);
        CHECK(nlohmann::json::parse(in) == doc);
    }

    // Not when a region is missing, nor from before the log, nor under other skeleton rules.
    std::remove(file.c_str());
    CHECK(Host::WorldRulesRefusal(h, seal).find(std::to_string(slot)) != std::string::npos);
    Simulation::WorldHeader old = h;
    old.log.clear();
    CHECK_FALSE(Host::WorldRulesRefusal(old, seal).empty());
    Simulation::WorldHeader skeleton = h;
    skeleton.generator = Gen::GENERATOR_VERSION;
    CHECK(Host::WorldRulesRefusal(skeleton, seal).empty());  // same region rules: remade
    skeleton.skeleton = Gen::SKELETON_VERSION + 1;
    CHECK(Host::WorldRulesRefusal(skeleton, seal).find("skeleton") != std::string::npos);
}

TEST_CASE("the tide is the same every time, and says what it was made of (#342 S3)")
{
    LoadRegistries();
    FastTide   fast;
    Simulation a, b;
    Make(a, 6);
    Make(b, 6);
    RunMinutes(a, 45);
    RunMinutes(b, 45);
    CHECK(a.OpeningLog().size() > 8);
    CHECK(SameLog(a.OpeningLog(), b.OpeningLog()));
    CHECK(SameTides(a, b));
    CHECK(a.RegionDocuments() == b.RegionDocuments());

    // Every region opened from a region that was open, into the slot beside it.
    for (const Simulation::RegionOpening& o : a.OpeningLog())
    {
        if (o.by == "world")
            continue;
        CAPTURE(o.slot);
        CHECK(o.by == "tide");
        const auto& n = a.Skeleton().slots[(size_t)o.parent].neighbours;
        CHECK(std::find(n.begin(), n.end(), o.slot) != n.end());
        CHECK(Gen::SlotOf(o.mouth) == o.parent);
    }
    // News of each, in the chronicle.
    int told = 0;
    for (const ChronicleEntry& e : a.Chronicle())
        told += e.kind == "tide" ? 1 : 0;
    CHECK(told == (int)a.OpeningLog().size() - 6);
}

TEST_CASE("the tide runs cold: a world nobody is in grows as a watched one does (#342 S3)")
{
    LoadRegistries();
    // Faction surveys follow ships, and ships are real only where a system is hot; the rest
    // of the tide must not care.
    FastTide   fast(false);
    Simulation cold, hot;
    hot.SetAllHot(true);
    Make(cold, 12);
    Make(hot, 12);
    for (Simulation* sim : { &cold, &hot })
    {
        const std::string start = sim->Starts().front().systemId;
        sim->CreateSession(start, sim->SafeArrival(start), GetShipCatalog()[0].stats);
    }
    RunMinutes(cold, 20);
    RunMinutes(hot, 20);
    CHECK(cold.OpeningLog().size() > 6);
    CHECK(SameLog(cold.OpeningLog(), hot.OpeningLog()));
    CHECK(SameTides(cold, hot));
}

TEST_CASE("at the cap nothing opens, and the tide that would have is kept (#342 S3)")
{
    LoadRegistries();
    FastTide   fast;
    Simulation sim;
    Make(sim, 3);
    const int open = (int)sim.OpenSlots().size();
    sim.SetGrowthCap(open + 1, 0);
    RunMinutes(sim, 30);
    CHECK((int)sim.OpenSlots().size() == open + 1);
    CHECK(sim.AtCap());
    CHECK(sim.SurplusTide() > 0.0);
    std::string why;
    CHECK(sim.OpenMouthNow(-1, why) < 0);
    CHECK(why.find("cap") != std::string::npos);

    // ...counted in systems as well.
    Simulation small;
    Make(small, 3);
    small.SetGrowthCap(0, (int)small.Universe().systems.size());
    CHECK(small.AtCap());
    CHECK(small.OpenMouthNow(-1, why) < 0);
}

TEST_CASE("paused, the tide neither gathers nor breaks (#342 S3)")
{
    LoadRegistries();
    FastTide   fast;
    Simulation sim;
    Make(sim, 5);
    RunMinutes(sim, 2);
    sim.PauseGrowth(true);
    const auto   before = sim.Tides();
    const size_t opened = sim.OpeningLog().size();
    RunMinutes(sim, 20);
    CHECK(sim.OpeningLog().size() == opened);
    for (const auto& kv : before)
        CHECK(sim.Tides().at(kv.first).tide == kv.second.tide);
}

TEST_CASE("a region opened while the world runs is told before anything shows it (#342 S3)")
{
    LoadRegistries();
    Simulation sim;
    Make(sim, 7);
    sim.TakeLayoutDeltas();
    const int   centre = sim.Skeleton().centres.front();
    std::string why;
    const int   slot = sim.OpenMouthNow(centre, why);
    REQUIRE(slot >= 0);
    CHECK(sim.TakeOpenedRegions() == std::vector<int>{ slot });
    CHECK(sim.TakeOpenedRegions().empty());
    const Simulation::RegionOpening& o = sim.OpeningLog().back();
    CHECK(o.by == "admin");
    CHECK(o.parent == centre);

    // Its systems exist, live, with aggregates and static layers.
    const std::vector<std::string> systems = sim.RegionSystems(slot);
    REQUIRE_FALSE(systems.empty());
    for (const std::string& id : systems)
    {
        CAPTURE(id);
        REQUIRE(sim.HasSystem(id));
        CHECK_FALSE(sim.Systems().at(id).entities.empty());
    }
    // The gate goes to whoever stands at the mouth as a delta, the index has the far side --
    // a civilisation's edge is charted, so its new neighbour is on the map at once -- and the
    // chronicle says so.
    bool gate = false;
    for (const Proto::LayoutDelta& d : sim.TakeLayoutDeltas())
        if (d.systemId == o.mouth)
            for (const Proto::EntityLayout& e : d.added)
                gate = gate || (e.kind == EntityKind::Gate && Gen::SlotOf(e.dest) == slot);
    CHECK(gate);
    bool onMap = false;
    for (const auto& s : sim.KnownUniverse().systems)
        onMap = onMap || Gen::SlotOf(s.id) == slot;
    CHECK(onMap);
    REQUIRE_FALSE(sim.Chronicle().empty());
    CHECK(sim.Chronicle().back().kind == "tide");
    CHECK(sim.PlanRoute(o.mouth, systems.front(), false).size() == 2);
}

TEST_CASE("a region is remade by the admin only while no player has built in it (#342 S3)")
{
    LoadRegistries();
    TempDir           dir;
    const std::string seal = dir.MakeDir("regions");
    Simulation        sim;
    Make(sim, 2, seal);
    int frontier = -1;
    for (const Simulation::RegionOpening& o : sim.OpeningLog())
        if (o.parent >= 0)
            frontier = o.slot;
    REQUIRE(frontier >= 0);
    const std::string system = sim.RegionSystems(frontier).front();

    const Blueprint& beacon = *Blueprints::Find("beacon");
    auto             t =
        std::make_unique<Structure>(Vector2{ 500000.0f, 0.0f }, 0.0f, "Lantern", beacon.archetype);
    t->SetBlueprint(beacon.id);
    const int id = sim.AddStatic(system, std::move(t), "pilot");
    REQUIRE(id != 0);
    std::string why;
    CHECK_FALSE(sim.RegenerateRegion(frontier, why));
    CHECK(why.find("player structure") != std::string::npos);
    CHECK_FALSE(sim.RegenerateRegion(999, why));

    REQUIRE(sim.RemoveStatic(system, id));
    CHECK(sim.RegenerateRegion(frontier, why));
    CHECK(why.empty());
    Gen::Region again;
    CHECK(Simulation::ReadSealed(Simulation::SealedPath(seal, frontier), 2, again));
}
