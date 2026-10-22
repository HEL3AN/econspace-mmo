#include <doctest/doctest.h>

#include "core/Archetype.h"
#include "core/Faction.h"
#include "entities/Ship.h"
#include "entities/ShipType.h"
#include "entities/Structure.h"
#include "gen/Region.h"
#include "sim/Protocol.h"
#include "sim/SaveSchema.h"
#include "sim/Simulation.h"

#include "temp_dir.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <set>
#include <string>

// Who knows which systems (#431 slice 1): the public charts, a pilot's own, their
// corporation's -- documents/world_knowledge_and_territory.md, section 1.

namespace
{

const std::string SYSTEMS = std::string(TEST_WORLD_DIR) + "systems/";

// Known space and the region of seed 7 beyond its wormhole, as the #144 tests have it.
void Make(Simulation& sim)
{
    REQUIRE(Factions::Load(std::string(TEST_DATA_DIR) + "factions.json"));
    REQUIRE(Archetypes::Load(std::string(TEST_DATA_DIR) + "archetypes.json"));
    sim.LoadUniverse(std::string(TEST_WORLD_DIR) + "universe.json");
    sim.AttachRegion(7, SYSTEMS);
    sim.Seed(1234u);
    sim.InitGalaxy();
    sim.MaterializeAllSystems(SYSTEMS);
}

// The far side of the wormhole, and a system one gate further in.
struct Region
{
    std::string home, entry, deeper;
};

Region Find(const Simulation& sim)
{
    Region r;
    r.home = sim.Universe().startId;
    for (const auto& l : sim.Universe().links)
        if (l.a == r.home && Gen::DepthOf(l.b) == 1)
            r.entry = l.b;
        else if (l.b == r.home && Gen::DepthOf(l.a) == 1)
            r.entry = l.a;
    for (const auto& l : sim.Universe().links)
        if ((l.a == r.entry || l.b == r.entry) && Gen::DepthOf(l.a == r.entry ? l.b : l.a) == 2)
            r.deeper = l.a == r.entry ? l.b : l.a;
    REQUIRE_FALSE(r.entry.empty());
    REQUIRE_FALSE(r.deeper.empty());
    return r;
}

ClientSession& Pilot(Simulation& sim, const std::string& name, const std::string& at)
{
    ClientSession& s = sim.CreateSession(at, Vector2{ 0.0f, 0.0f }, GetShipCatalog()[0].stats);
    s.accountName = name;
    s.ship->SetPilotName(name);
    return s;
}

const WorldLoader::SystemInfo* In(const WorldLoader::Universe& u, const std::string& id)
{
    for (const auto& si : u.systems)
        if (si.id == id)
            return &si;
    return nullptr;
}

bool Charted(const WorldLoader::Universe& u, const std::string& id)
{
    const WorldLoader::SystemInfo* si = In(u, id);
    return si != nullptr && si->charted;
}

bool HasStats(const Proto::GalaxyState& g, const std::string& id)
{
    for (const auto& s : g.systems)
        if (s.id == id)
            return true;
    return false;
}

void Corp(Simulation& sim, const std::string& ceo, const std::string& member)
{
    Corps::Registry& reg = sim.Corporations();
    REQUIRE(reg.Create(ceo, "Far Reach", "FAR", 1000).Ok());
    REQUIRE(reg.InviteTo(ceo, member, { ceo, member }, 1000).Ok());
    REQUIRE(reg.Join(member, "FAR", 1001).Ok());
    // As the host does after a founding or a joining: each brings their charts.
    for (const auto& kv : sim.Sessions())
        sim.ShareCharts(kv.second);
}

}  // namespace

TEST_CASE("a find is its pilot's and their corporation's, and nobody else's (#431)")
{
    Simulation sim;
    Make(sim);
    const Region   r = Find(sim);
    ClientSession& ann = Pilot(sim, "ann", r.home);
    ClientSession& bo = Pilot(sim, "bo", r.home);
    ClientSession& cy = Pilot(sim, "cy", r.home);

    const WorldLoader::Universe  publicBefore = sim.KnownUniverse();
    const std::vector<long long> cyBefore = sim.KnowledgeKey(cy);
    CHECK_FALSE(Charted(sim.KnownUniverseFor(ann), r.entry));  // a stub: a gate leads there
    CHECK(In(sim.KnownUniverseFor(ann), r.entry) != nullptr);

    sim.ServerEnterSystem(ann, r.entry, r.home);
    CHECK(sim.Knows(ann, r.entry));
    CHECK_FALSE(sim.Knows(bo, r.entry));
    CHECK_FALSE(sim.OnPublicCharts(r.entry));
    // Kept: a pointer into an index outlives only the index it points into.
    const WorldLoader::Universe    annMap = sim.KnownUniverseFor(ann);
    const WorldLoader::SystemInfo* mine = In(annMap, r.entry);
    REQUIRE(mine != nullptr);
    CHECK(mine->charted);
    CHECK(mine->knownBy == "own");
    CHECK(mine->discoverer == "ann");
    CHECK(In(sim.KnownUniverseFor(ann), r.deeper) != nullptr);  // and what its gates lead to
    CHECK_FALSE(Charted(sim.KnownUniverseFor(ann), r.deeper));

    // The others' maps are the public charts, as they were.
    CHECK_FALSE(Charted(sim.KnownUniverseFor(bo), r.entry));
    CHECK(In(sim.KnownUniverseFor(bo), r.deeper) == nullptr);
    CHECK(sim.KnowledgeKey(cy) == cyBefore);  // a third pilot's index is not even rebuilt
    CHECK(sim.KnownUniverse().systems.size() == publicBefore.systems.size());

    // Galaxy statistics go only to those who know the system.
    const Proto::GalaxyState all = sim.GalaxyStats();
    CHECK(HasStats(all, r.entry));
    CHECK(HasStats(sim.GalaxyStateFor(ann, all), r.entry));
    CHECK_FALSE(HasStats(sim.GalaxyStateFor(bo, all), r.entry));
    CHECK_FALSE(HasStats(sim.GalaxyStateFor(bo, all), r.deeper));
    CHECK(HasStats(sim.GalaxyStateFor(bo, all), r.home));

    // A corporation shares it, while its members are in it.
    Corp(sim, "ann", "bo");
    CHECK(sim.Knows(bo, r.entry));
    const WorldLoader::Universe    boMap = sim.KnownUniverseFor(bo);
    const WorldLoader::SystemInfo* shared = In(boMap, r.entry);
    REQUIRE(shared != nullptr);
    CHECK(shared->knownBy == "corp");
    CHECK(HasStats(sim.GalaxyStateFor(bo, all), r.entry));
    CHECK_FALSE(sim.Knows(cy, r.entry));
    // ...and what a member finds later reaches the others at once.
    sim.ServerEnterSystem(ann, r.deeper, r.entry);
    CHECK(sim.Knows(bo, r.deeper));
    CHECK_FALSE(sim.Knows(cy, r.deeper));

    REQUIRE(sim.Corporations().Leave("bo").Ok());
    CHECK_FALSE(sim.Knows(bo, r.entry));
    CHECK(sim.Knows(ann, r.entry));  // and its finder keeps it for good (K6)
}

TEST_CASE("a pilot's map is sent again when it changes, not each time the world moves (#431)")
{
    Simulation sim;
    Make(sim);
    const Region                 r = Find(sim);
    ClientSession&               ann = Pilot(sim, "ann", r.home);
    const std::vector<long long> first = sim.KnowledgeKey(ann);
    const long long              stamp = sim.KnowledgeStamp();
    sim.MaintainWorld(1.0f);  // the world moving on is not a change to anybody's charts
    CHECK(sim.KnowledgeKey(ann) == first);
    CHECK(sim.KnowledgeStamp() == stamp);
    sim.ServerEnterSystem(ann, r.entry, r.home);
    CHECK(sim.KnowledgeStamp() != stamp);
    CHECK(sim.KnowledgeKey(ann) != first);
    CHECK(ann.known.count(Simulation::ChartKey(r.entry)) == 1);  // beside what scans found

    // A faction's charts put it on everybody's (the seam slice 2 builds on).
    ClientSession& bo = Pilot(sim, "bo", r.home);
    REQUIRE(sim.ChartBy(r.deeper, Factions::Pirates()));
    CHECK_FALSE(sim.ChartBy(r.deeper, Factions::Unaffiliated()));  // the first charts stand
    CHECK(sim.OnPublicCharts(r.deeper));
    CHECK(sim.Knows(bo, r.deeper));
    const WorldLoader::Universe    charts = sim.KnownUniverse();
    const WorldLoader::SystemInfo* charted = In(charts, r.deeper);
    REQUIRE(charted != nullptr);
    CHECK(charted->chartedBy == Factions::Id(Factions::Pirates()));
}

TEST_CASE("a route keeps to the gates on the pilot's map (#431)")
{
    Simulation sim;
    Make(sim);
    const Region   r = Find(sim);
    ClientSession& ann = Pilot(sim, "ann", r.home);
    ClientSession& bo = Pilot(sim, "bo", r.home);
    CHECK(sim.PlanRoute(r.home, r.deeper).size() == 3);  // the server knows the way
    auto route = [&](ClientSession& s, const std::string& to)
    {
        s.order.kind = Orders::Kind::Route;
        s.order.destSystem = to;
        return sim.RouteFor(s).path;
    };
    CHECK(route(ann, r.deeper).empty());     // ann does not
    CHECK(route(ann, r.entry).size() == 2);  // the gate she sees, to its uncharted far side
    sim.ServerEnterSystem(ann, r.entry, r.home);
    sim.ServerEnterSystem(ann, r.home, r.entry);
    CHECK(route(ann, r.deeper).size() == 3);  // now on her map
    CHECK(route(bo, r.deeper).empty());       // and still on nobody else's
    Corp(sim, "ann", "bo");
    CHECK(route(bo, r.deeper).size() == 3);  // until they share a corporation's charts
}

TEST_CASE("charts are kept with the account, and shared while their pilot is away (#431)")
{
    const TempDir     tmp;
    const std::string accounts = tmp.Path("accounts") + "/";
    std::filesystem::create_directories(accounts);
    const std::string world = tmp.Path("world.json");

    Region r;
    {
        Simulation sim;
        Make(sim);
        r = Find(sim);
        sim.SetWorldId("w1");
        ClientSession& ann = Pilot(sim, "ann", r.home);
        ClientSession& bo = Pilot(sim, "bo", r.home);
        Corp(sim, "ann", "bo");
        sim.ServerEnterSystem(ann, r.entry, r.home);
        REQUIRE(sim.SaveAccount(ann, accounts + "account_ann.json"));
        // She logs off: her corporation still has her charts.
        sim.DestroySession(ann.id);
        CHECK(sim.Knows(bo, r.entry));
        REQUIRE(sim.SaveWorld(world));
        REQUIRE(sim.Corporations().Save(tmp.Path("corps.json")));

        // Back on, she has them.
        ClientSession& again = Pilot(sim, "ann", r.home);
        REQUIRE(sim.LoadAccount(again, accounts + "account_ann.json") == Save::Result::Ok);
        CHECK(sim.Knows(again, r.entry));
        CHECK(again.known.count(Simulation::ChartKey(r.entry)) == 1);
    }

    // The corporation's charts are in corps.json: after a restart, an offline member's finds
    // are on the others' maps from the first minute.
    Simulation sim;
    sim.LoadUniverse(std::string(TEST_WORLD_DIR) + "universe.json");
    sim.AttachRegion(7, SYSTEMS);
    REQUIRE(sim.LoadWorld(world) == Save::Result::Ok);
    REQUIRE(sim.Corporations().Load(tmp.Path("corps.json")) == Save::Result::Ok);
    ClientSession& bo = Pilot(sim, "bo", r.home);
    CHECK(sim.Knows(bo, r.entry));
    const WorldLoader::Universe    boMap = sim.KnownUniverseFor(bo);
    const WorldLoader::SystemInfo* shared = In(boMap, r.entry);
    REQUIRE(shared != nullptr);
    CHECK(shared->knownBy == "corp");

    // An account from another world brings no charts: its ids are that world's systems.
    {
        std::ifstream  in(accounts + "account_ann.json");
        nlohmann::json j = nlohmann::json::parse(in);
        j["world"] = "elsewhere";
        std::ofstream(accounts + "account_dee.json") << j.dump();
    }
    Simulation other;
    other.LoadUniverse(std::string(TEST_WORLD_DIR) + "universe.json");
    other.AttachRegion(7, SYSTEMS);
    REQUIRE(other.LoadWorld(world) == Save::Result::Ok);
    other.SetWorldId("w1");
    ClientSession& dee = Pilot(other, "dee", r.home);
    REQUIRE(other.LoadAccount(dee, accounts + "account_dee.json") == Save::Result::Ok);
    CHECK_FALSE(other.Knows(dee, r.entry));
}

TEST_CASE("a world from before #431 keeps its public map, and finds go to their finders")
{
    const TempDir     tmp;
    const std::string world = tmp.Path("world.json");
    Region            r;
    std::string       holder;
    {
        Simulation sim;
        Make(sim);
        r = Find(sim);
        REQUIRE(sim.SaveWorld(world));
    }
    // As a version 15 build wrote it: `visited`, no `chartedBy`. The entry was found by ann
    // and nobody holds it; the system past it a faction holds.
    {
        std::ifstream  in(world);
        nlohmann::json j = nlohmann::json::parse(in);
        j["version"] = 15;
        for (auto it = j["galaxy"].begin(); it != j["galaxy"].end(); ++it)
            it.value()["visited"] = false;  // known space is public by its place regardless
        nlohmann::json& e = j["galaxy"][r.entry];
        e["visited"] = true;
        e["discoverer"] = "ann";
        e["claimed"] = false;
        nlohmann::json& d = j["galaxy"][r.deeper];
        d["visited"] = true;
        d["discoverer"] = "bo";
        d["claimed"] = true;
        holder = Factions::MajorIds().front();
        d["controller"] = holder;
        std::ofstream(world) << j.dump();
    }

    Simulation sim;
    sim.LoadUniverse(std::string(TEST_WORLD_DIR) + "universe.json");
    sim.AttachRegion(7, SYSTEMS);
    REQUIRE(sim.LoadWorld(world) == Save::Result::Ok);
    CHECK(sim.OnPublicCharts(r.home));
    CHECK(sim.OnPublicCharts(r.deeper));  // held by a faction: charted by it
    CHECK(sim.SystemById(r.deeper)->agg.chartedBy == holder);
    CHECK_FALSE(sim.OnPublicCharts(r.entry));  // nobody's: its finder's own

    // Kept in the world until ann comes back, across a save.
    const std::string again = tmp.Path("again.json");
    REQUIRE(sim.SaveWorld(again));
    Simulation next;
    next.LoadUniverse(std::string(TEST_WORLD_DIR) + "universe.json");
    next.AttachRegion(7, SYSTEMS);
    REQUIRE(next.LoadWorld(again) == Save::Result::Ok);
    CHECK(next.OnPublicCharts(r.deeper));

    // ann logs in (an account from before #431 has no charts) and has her find; cy does not.
    const std::string acct = tmp.Path("account_ann.json");
    std::ofstream(acct) << nlohmann::json{ { "version", 9 }, { "money", 100.0 } }.dump();
    ClientSession& ann = Pilot(next, "ann", r.home);
    REQUIRE(next.LoadAccount(ann, acct) == Save::Result::Ok);
    CHECK(ann.known.count(Simulation::ChartKey(r.entry)) == 1);
    CHECK(next.Knows(ann, r.entry));
    ClientSession& cy = Pilot(next, "cy", r.home);
    CHECK_FALSE(next.Knows(cy, r.entry));
    CHECK(next.Knows(cy, r.deeper));
}

TEST_CASE("a pilot knows the system they log in to (#431 K6)")
{
    const TempDir tmp;
    Simulation    sim;
    Make(sim);
    const Region   r = Find(sim);
    ClientSession& ann = Pilot(sim, "ann", r.home);
    sim.ServerEnterSystem(ann, r.entry, r.home);
    ann.known.clear();  // as if the system had been on everyone's map before
    const std::string acct = tmp.Path("account_ann.json");
    REQUIRE(sim.SaveAccount(ann, acct));
    sim.DestroySession(ann.id);
    ClientSession& back = Pilot(sim, "zed", r.home);  // another name: nothing kept for it
    REQUIRE(sim.LoadAccount(back, acct) == Save::Result::Ok);
    CHECK(back.systemId == r.entry);
    CHECK(back.known.count(Simulation::ChartKey(r.entry)) == 1);
}

// --- The map over time (#431 slice 14, M1) ----------------------------------------------------

namespace
{

// One coarse pass of the world: what anyone of a pilot's is looking at goes onto their maps.
void Pass(Simulation& sim)
{
    sim.MaintainWorld(2.0f);
}

// A structure of `owner`'s in `system` (an account, or a faction's "faction:<id>").
int Build(Simulation& sim, const std::string& system, const std::string& name,
          const std::string& owner)
{
    auto t =
        std::make_unique<Structure>(Vector2{ 5000.0f, 5000.0f }, 0.0f, name, "structure.beacon");
    const int id = sim.AddStatic(system, std::move(t), owner);
    REQUIRE(id != 0);
    return id;
}

bool Saw(const WorldLoader::Sighting& o, const std::string& name)
{
    for (const WorldLoader::SeenObject& s : o.seen)
        if (s.name == name)
            return true;
    return false;
}

// What `s`'s map says of `system`, as the host would send it.
WorldLoader::SystemInfo MapOf(const Simulation& sim, const ClientSession& s,
                              const std::string& system)
{
    const WorldLoader::Universe    u = sim.KnownUniverseFor(s);
    const WorldLoader::SystemInfo* si = In(u, system);
    REQUIRE(si != nullptr);
    return *si;
}

}  // namespace

TEST_CASE("what stands in a system is as last seen; geography stays, and it is live where "
          "you are (#431 slice 14)")
{
    Simulation sim;
    Make(sim);
    const Region   r = Find(sim);
    ClientSession& ann = Pilot(sim, "ann", r.home);

    sim.ServerEnterSystem(ann, r.entry, r.home);
    Pass(sim);
    WorldLoader::SystemInfo there = MapOf(sim, ann, r.entry);
    CHECK(there.observed.Observed());
    CHECK(there.observedLive);
    const double left = sim.Time();

    // She goes home. The system stays on her map, as she last saw it, no longer live.
    sim.ServerEnterSystem(ann, r.home, r.entry);
    Pass(sim);
    there = MapOf(sim, ann, r.entry);
    CHECK(there.charted);
    CHECK_FALSE(there.observedLive);
    CHECK(there.observed.at <= left + 0.001);
    const std::vector<long long> key = sim.KnowledgeKey(ann);

    // Somebody builds there while she is away: not on her map, and her map is not even sent
    // again for it.
    Build(sim, r.entry, "Zed's Beacon", "zed");
    Pass(sim);
    Pass(sim);
    there = MapOf(sim, ann, r.entry);
    CHECK_FALSE(Saw(there.observed, "Zed's Beacon"));
    CHECK(sim.KnowledgeKey(ann) == key);  // nothing she can see changed
    CHECK(there.charted);                 // the geography is hers for good

    // ...until she returns.
    sim.ServerEnterSystem(ann, r.entry, r.home);
    Pass(sim);
    there = MapOf(sim, ann, r.entry);
    CHECK(there.observedLive);
    CHECK(Saw(there.observed, "Zed's Beacon"));
    CHECK(sim.KnowledgeKey(ann) != key);
    // The wire carries it, date and all.
    WorldLoader::Universe got;
    REQUIRE(Proto::DecodeUniverse(Proto::EncodeUniverse(sim.KnownUniverseFor(ann)), got));
    const WorldLoader::SystemInfo* wire = In(got, r.entry);
    REQUIRE(wire != nullptr);
    CHECK(wire->observedLive);
    CHECK(wire->observed.at == doctest::Approx(there.observed.at));
    CHECK(Saw(wire->observed, "Zed's Beacon"));
}

TEST_CASE("owner and security on the public charts are live, never as seen (#431 slice 14)")
{
    Simulation sim;
    Make(sim);
    const Region   r = Find(sim);
    ClientSession& ann = Pilot(sim, "ann", r.home);
    std::string    elsewhere;  // a public system she has never been to
    for (const auto& si : sim.KnownUniverse().systems)
        if (si.charted && si.id != r.home && sim.OnPublicCharts(si.id))
            elsewhere = si.id;
    REQUIRE_FALSE(elsewhere.empty());
    Pass(sim);
    CHECK_FALSE(MapOf(sim, ann, elsewhere).observed.Observed());  // nothing seen there yet

    SystemState* st = sim.SystemById(elsewhere);
    REQUIRE(st != nullptr);
    st->agg.security = 0.123f;
    st->agg.controller = Factions::Pirates();
    bool found = false;
    for (const Proto::GalaxySystemStat& g : sim.GalaxyStateFor(ann, sim.GalaxyStats()).systems)
        if (g.id == elsewhere)
        {
            found = true;
            CHECK(g.security == doctest::Approx(0.123f));
            CHECK(g.controller == Factions::Pirates());
        }
    CHECK(found);
}

TEST_CASE("a corporation's eyes and structures keep its members' maps live (#431 slice 14)")
{
    Simulation sim;
    Make(sim);
    const Region   r = Find(sim);
    ClientSession& ann = Pilot(sim, "ann", r.home);
    ClientSession& bo = Pilot(sim, "bo", r.home);
    Corp(sim, "ann", "bo");

    // ann is at the entry: bo, at home, sees it live through her.
    sim.ServerEnterSystem(ann, r.entry, r.home);
    Build(sim, r.entry, "Zed's Beacon", "zed");
    Pass(sim);
    WorldLoader::SystemInfo seen = MapOf(sim, bo, r.entry);
    CHECK(seen.observedLive);
    CHECK(Saw(seen.observed, "Zed's Beacon"));

    // She leaves: it is as of then for both of them.
    sim.ServerEnterSystem(ann, r.home, r.entry);
    Pass(sim);
    CHECK_FALSE(MapOf(sim, bo, r.entry).observedLive);
    CHECK(MapOf(sim, bo, r.entry).observed.Observed());
    Build(sim, r.entry, "Later Beacon", "zed");
    Pass(sim);
    CHECK_FALSE(Saw(MapOf(sim, bo, r.entry).observed, "Later Beacon"));

    // A structure of ann's watches the deeper system for both, with nobody there and ann off.
    sim.ServerEnterSystem(ann, r.entry, r.home);
    sim.ServerEnterSystem(ann, r.deeper, r.entry);  // charted for the corporation
    Build(sim, r.deeper, "Ann's Beacon", "ann");
    sim.DestroySession(ann.id);
    Pass(sim);
    Build(sim, r.deeper, "Zed's Second", "zed");
    Pass(sim);
    seen = MapOf(sim, bo, r.deeper);
    CHECK(seen.observedLive);
    CHECK(Saw(seen.observed, "Zed's Second"));
    // ...and a pilot in no corporation has none of it.
    ClientSession& cy = Pilot(sim, "cy", r.home);
    Pass(sim);
    CHECK_FALSE(sim.Knows(cy, r.deeper));
    CHECK(cy.liveSystems.count(r.deeper) == 0);
}

TEST_CASE("what a pilot saw is kept with the account and corporation, and bounded "
          "(#431 slice 14)")
{
    const TempDir     tmp;
    const std::string acct = tmp.Path("account_ann.json");
    Simulation        sim;
    Make(sim);
    const Region   r = Find(sim);
    ClientSession& ann = Pilot(sim, "ann", r.home);
    Pilot(sim, "bo", r.home);
    Corp(sim, "ann", "bo");
    const size_t extra = 8;
    for (size_t i = 0; i < WorldLoader::Sighting::MAX_SEEN + extra; ++i)
        Build(sim, r.entry, "Beacon " + std::to_string(i), "zed");
    sim.ServerEnterSystem(ann, r.entry, r.home);
    Pass(sim);
    sim.ServerEnterSystem(ann, r.home, r.entry);
    Pass(sim);

    // However much stands there, a summary is MAX_SEEN lines and a count.
    const WorldLoader::Sighting mine = ann.sightings.at(r.entry);
    CHECK(mine.seen.size() == WorldLoader::Sighting::MAX_SEEN);
    CHECK(mine.more >= (int)extra);
    CHECK(WorldLoader::ToJson(mine).dump().size() < 4096);

    REQUIRE(sim.SaveAccount(ann, acct));
    REQUIRE(sim.Corporations().Save(tmp.Path("corps.json")));
    sim.DestroySession(ann.id);
    ClientSession& back = Pilot(sim, "ann", r.home);
    REQUIRE(sim.LoadAccount(back, acct) == Save::Result::Ok);
    REQUIRE(back.sightings.count(r.entry) == 1);
    CHECK(back.sightings.at(r.entry).SameAs(mine));
    CHECK(back.sightings.at(r.entry).at == doctest::Approx(mine.at));

    Corps::Registry reg;
    REQUIRE(reg.Load(tmp.Path("corps.json")) == Save::Result::Ok);
    const Corps::Corp* corp = reg.Of("bo");
    REQUIRE(corp != nullptr);
    REQUIRE(corp->sightings.count(r.entry) == 1);
    CHECK(corp->sightings.at(r.entry).seen.size() == WorldLoader::Sighting::MAX_SEEN);
    CHECK(corp->sightings.at(r.entry).at == doctest::Approx(mine.at));
}
