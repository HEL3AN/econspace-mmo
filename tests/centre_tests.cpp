#include <doctest/doctest.h>

#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "entities/Ship.h"
#include "entities/ShipType.h"
#include "gen/Galaxy.h"
#include "gen/Region.h"
#include "sim/HostRules.h"
#include "sim/Protocol.h"
#include "sim/Simulation.h"

#include "temp_dir.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

// Civilisation centres and where a pilot begins (#342 S2): a world that needs nothing
// hand-written, held to being the same everywhere and to the shape the design asks for.

namespace
{
const std::vector<std::string> OWNERS = { "TradersGuild", "Syndicate" };

Gen::GalaxySkeleton World(uint64_t seed, int centres)
{
    Gen::WorldParams p;
    p.seed = seed;
    p.centres = centres;
    p.owners = OWNERS;
    return Gen::GenerateGalaxySkeleton(p);
}

std::string Dump(const Gen::Region& r)
{
    return Gen::RegionToJson(r).dump();
}

uint64_t Fnv1a(const std::string& s)
{
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s)
    {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

std::set<std::string> Roles(const nlohmann::json& doc)
{
    std::set<std::string> out;
    for (const auto& s : doc["stations"])
        out.insert(s["role"].get<std::string>());
    return out;
}
}  // namespace

TEST_CASE("centres are placed by the seed, far apart, and each is a major faction's (#342)")
{
    for (uint64_t seed = 1; seed <= 25; seed++)
        for (int count = 1; count <= Gen::MAX_CENTRES; count++)
        {
            CAPTURE(seed);
            CAPTURE(count);
            const std::vector<int> slots = Gen::CentreSlots(seed, count);
            REQUIRE((int)slots.size() == count);
            CHECK(slots == Gen::CentreSlots(seed, count));
            for (size_t i = 0; i < slots.size(); i++)
            {
                CHECK(slots[i] < Gen::DEFAULT_REGION_CAP);
                int nearest = 1 << 30;
                for (size_t j = 0; j < slots.size(); j++)
                    if (j != i)
                        nearest = std::min(nearest, Gen::LatticeDistance(slots[i], slots[j]));
                if (count == 1)
                    continue;
                // "About four to six regions" to the nearest neighbour: apart, not lost.
                CHECK(nearest >= Gen::CENTRE_SPACING);
                CHECK(nearest <= 6);
            }
        }
    CHECK(Gen::CentreSlots(1, 1) == std::vector<int>{ 0 });
    CHECK(Gen::CentreSlots(1, 0).empty());

    const Gen::GalaxySkeleton g = World(5, 3);
    REQUIRE(g.centres.size() == 3);
    std::set<std::string> owners;
    for (const Gen::Slot& s : g.slots)
    {
        CAPTURE(s.index);
        const bool centre = std::count(g.centres.begin(), g.centres.end(), s.index) == 1;
        CHECK((s.kind == Gen::SlotKind::Centre) == centre);
        CHECK((s.hops == 0) == centre);
        CHECK(s.owner.empty() != centre);
        if (centre)
            owners.insert(s.owner);
        for (int n : s.neighbours)
            CHECK(std::abs(g.slots[(size_t)n].hops - s.hops) <= 1);
    }
    CHECK(owners == std::set<std::string>(OWNERS.begin(), OWNERS.end()));  // both have one
    // The skeleton is still the same skeleton: centres only say what its slots are for.
    const Gen::GalaxySkeleton plain = Gen::GenerateGalaxySkeleton(5, Gen::DEFAULT_REGION_CAP);
    CHECK(plain.links == g.links);
    CHECK(Gen::SkeletonJson(World(5, 3)).dump() == Gen::SkeletonJson(g).dump());
}

TEST_CASE("a centre is a named, policed capital with every kind of station (#342)")
{
    for (uint64_t seed : { 1ull, 2ull, 3ull, 77ull, 4242ull })
    {
        CAPTURE(seed);
        const Gen::GalaxySkeleton g = World(seed, 3);
        for (int c : g.centres)
        {
            CAPTURE(c);
            const Gen::RegionParams p = Gen::SlotParams(g, c);
            const Gen::Region       r = Gen::GenerateRegion(p);
            CHECK(Dump(r) == Dump(Gen::GenerateRegion(p)));
            CHECK(r.slot == c);
            CHECK(r.heartId == Gen::SystemId(c, 1, 1));
            CHECK(r.homeId.empty());
            CHECK(r.wormhole.empty());  // nothing leads into a centre
            REQUIRE(r.systems.size() >= 8);
            CHECK(r.systems.size() <= 14);

            std::set<std::string> names;
            int                   deepest = 1;
            double                edge = 1.0;
            for (const auto& s : r.systems)
            {
                const std::string id = s["id"];
                CAPTURE(id);
                const double sec = s["security"];
                CHECK(s["owner"] == p.owner);
                CHECK(names.insert(s["name"].get<std::string>()).second);  // named, and once
                CHECK(s["name"].get<std::string>().rfind("W-", 0) != 0);
                CHECK(sec >= 0.5);
                CHECK(sec <= 1.0);
                if (id == r.heartId)
                    CHECK(sec == 1.0);
                if (Gen::DepthOf(id) > deepest)
                {
                    deepest = Gen::DepthOf(id);
                    edge = sec;
                }
                else if (Gen::DepthOf(id) == deepest)
                    edge = std::min(edge, sec);
                // Somewhere to dock in every system of settled space.
                CHECK_FALSE(r.documents.at(id)["stations"].empty());
            }
            CHECK(edge <= 0.55);  // about half at the edge

            // The heart has one of every kind.
            CHECK(Roles(r.documents.at(r.heartId)) ==
                  std::set<std::string>{ "TradeHub", "Shipyard", "Military", "MiningOutpost" });

            // One piece from the heart, and every link a gate at both ends.
            std::map<std::string, std::vector<std::string>> adj;
            for (const auto& l : r.links)
            {
                adj[l[0]].push_back(l[1]);
                adj[l[1]].push_back(l[0]);
            }
            std::set<std::string>   reached = { r.heartId };
            std::deque<std::string> todo = { r.heartId };
            while (!todo.empty())
            {
                const std::string at = todo.front();
                todo.pop_front();
                for (const std::string& n : adj[at])
                    if (reached.insert(n).second)
                        todo.push_back(n);
            }
            CHECK(reached.size() == r.systems.size());
            for (const auto& kv : r.documents)
            {
                std::set<std::string> dests;
                for (const auto& gate : kv.second["gates"])
                    dests.insert(gate["destination"].get<std::string>());
                CHECK(dests == std::set<std::string>(adj[kv.first].begin(), adj[kv.first].end()));
            }
        }
    }
}

TEST_CASE("the frontier is poorer in law and richer in ore the further it is from a centre")
{
    Gen::RegionParams near;
    near.seed = 9;
    near.slot = 40;
    near.homeId = "home";
    near.hops = 1;
    Gen::RegionParams far = near;
    far.hops = 3;
    auto averages = [](const Gen::Region& r, double& security, double& ore)
    {
        security = ore = 0.0;
        int belts = 0;
        for (const auto& s : r.systems)
            security += s["security"].get<double>();
        security /= (double)r.systems.size();
        for (const auto& kv : r.documents)
            for (const auto& b : kv.second["asteroidFields"])
            {
                ore += b["ore"].get<double>();
                belts++;
            }
        ore /= std::max(1, belts);
    };
    double nearSec, nearOre, farSec, farOre;
    averages(Gen::GenerateRegion(near), nearSec, nearOre);
    averages(Gen::GenerateRegion(far), farSec, farOre);
    CHECK(farSec < nearSec);
    CHECK(farOre > nearOre);
}

TEST_CASE("a new world opens every centre and one frontier beside each (#342)")
{
    const Gen::GalaxySkeleton       g = World(8, 3);
    const std::vector<Gen::Opening> openings = Gen::InitialOpenings(g);
    const std::vector<Gen::Region>  regions = Gen::GenerateWorld(g);
    REQUIRE(openings.size() == 6);
    REQUIRE(regions.size() == 6);
    std::map<int, const Gen::Region*> bySlot;
    for (const Gen::Region& r : regions)
        bySlot[r.slot] = &r;
    for (const Gen::Opening& o : openings)
    {
        CAPTURE(o.slot);
        const Gen::Region& r = *bySlot.at(o.slot);
        if (o.from < 0)
        {
            CHECK(g.slots[(size_t)o.slot].kind == Gen::SlotKind::Centre);
            CHECK_FALSE(r.heartId.empty());
            continue;
        }
        // Beside its centre, opened from the centre's edge.
        const auto& n = g.slots[(size_t)o.from].neighbours;
        CHECK(std::count(n.begin(), n.end(), o.slot) == 1);
        CHECK(g.slots[(size_t)o.slot].kind == Gen::SlotKind::Frontier);
        CHECK(Gen::SlotOf(r.homeId) == o.from);
        int deepest = 0;
        for (const auto& s : bySlot.at(o.from)->systems)
            deepest = std::max(deepest, Gen::DepthOf(s["id"]));
        CHECK(Gen::DepthOf(r.homeId) == deepest);
        CHECK(r.wormhole["destination"] == r.entryId);
    }
    // Made the same way twice; and the same as each region made on its own.
    const std::vector<Gen::Region> again = Gen::GenerateWorld(g);
    for (size_t i = 0; i < regions.size(); i++)
        CHECK(Dump(regions[i]) == Dump(again[i]));
    for (int c : g.centres)
        CHECK(Dump(*bySlot.at(c)) == Dump(Gen::GenerateRegion(Gen::SlotParams(g, c))));

    // Golden, as gen_tests keeps them (#140): a change to how centres are made or placed
    // shows here, by itself. Bump Gen::GENERATOR_VERSION when it does.
    const uint64_t            skeleton = Fnv1a(Gen::SkeletonJson(World(1, 3)).dump());
    const Gen::GalaxySkeleton first = World(1, 3);
    const uint64_t            centre =
        Fnv1a(Dump(Gen::GenerateRegion(Gen::SlotParams(first, first.centres.front()))));
    MESSAGE("skeleton with 3 centres, seed 1: " << skeleton);
    MESSAGE("first centre's region, seed 1: " << centre);
    CHECK(skeleton == 5604254571228940977ull);
    CHECK(centre == 5038813507080152432ull);
}

TEST_CASE("a server's world: centres to start in, charted, and nothing hand-written (#342)")
{
    const std::string data = TEST_DATA_DIR;
    Factions::Load(data + "factions.json");
    REQUIRE(Archetypes::Load(data + "archetypes.json"));
    REQUIRE(Blueprints::Load(data + "blueprints.json"));
    const std::vector<std::string> majors = Factions::MajorIds();
    CHECK(std::find(majors.begin(), majors.end(), "Pirates") == majors.end());
    REQUIRE_FALSE(majors.empty());

    Simulation       sim;
    Gen::WorldParams p;
    p.seed = 3;
    p.centres = 3;
    p.owners = majors;
    REQUIRE(sim.OpenGalaxy(p, data));
    CHECK_FALSE(sim.OpenGalaxy(p, data));  // once
    sim.InitGalaxy();
    sim.MaterializeAllSystems(data + "systems/");

    REQUIRE(sim.Starts().size() == 3);
    CHECK(sim.CentreCount() == 3);
    for (const Simulation::StartPoint& sp : sim.Starts())
    {
        CAPTURE(sp.systemId);
        CHECK(sim.HasSystem(sp.systemId));
        CHECK(sim.InCentre(sp.systemId));
        CHECK(sim.IsStart(sp.systemId));
        CHECK(Gen::DepthOf(sp.systemId) == 1);
        CHECK_FALSE(sp.station.empty());
        CHECK(std::find(majors.begin(), majors.end(), sp.owner) != majors.end());
        bool hub = false;
        for (const auto& e : sim.Systems().at(sp.systemId).entities)
            hub = hub || e->GetName() == sp.station;
        CHECK(hub);
    }
    CHECK_FALSE(sim.IsStart("core"));

    // A civilisation's own space is on the map from the first minute; the frontier is not.
    const WorldLoader::Universe known = sim.KnownUniverse();
    int                         charted = 0, frontier = 0;
    for (const auto& si : known.systems)
    {
        if (si.charted)
        {
            charted++;
            CHECK(sim.InCentre(si.id));
        }
        else
            frontier++;
    }
    CHECK(charted > 20);
    CHECK(frontier == 3);  // the entry of each frontier, seen through its wormhole

    // Named long ago: a centre system is nobody's to name.
    ClientSession& s = sim.CreateSession(sim.Starts().front().systemId,
                                         sim.SafeArrival(sim.Starts().front().systemId),
                                         GetShipCatalog()[0].stats);
    s.ship->SetPilotName("namer");
    CHECK_FALSE(sim.NameSystem(s, "Mine"));

    // The world file says how many centres made it, so a restart makes the same galaxy.
    TempDir           dir;
    const std::string path = dir.Path("world.json");
    REQUIRE(sim.SaveWorld(path));
    uint64_t seed = 0;
    int      rules = 0, centres = 0;
    REQUIRE(Simulation::ReadWorldSeed(path, seed, rules, &centres));
    CHECK(seed == 3);
    CHECK(rules == Gen::GENERATOR_VERSION);
    CHECK(centres == 3);
}

TEST_CASE("a pilot with no place in this galaxy is given a start (#342)")
{
    const std::string data = TEST_DATA_DIR;
    Factions::Load(data + "factions.json");
    REQUIRE(Archetypes::Load(data + "archetypes.json"));
    REQUIRE(Blueprints::Load(data + "blueprints.json"));
    Simulation       sim;
    Gen::WorldParams p;
    p.seed = 11;
    p.centres = 2;
    p.owners = Factions::MajorIds();
    REQUIRE(sim.OpenGalaxy(p, data));
    sim.InitGalaxy();
    sim.MaterializeAllSystems(data + "systems/");
    const std::string a = sim.Starts()[0].systemId, b = sim.Starts()[1].systemId;

    TempDir dir;
    // A new account has no file: it chooses.
    CHECK(Host::NeedsStart(sim, dir.Path("nobody.json")));
    CHECK(Host::AccountSystem(dir.Path("nobody.json")).empty());

    // The choice is honoured; anything else is the luck of the draw, and so is everything
    // on a server that deals starts.
    CHECK(Host::PickStart(sim, b, false, 0) == b);
    CHECK(sim.IsStart(Host::PickStart(sim, "core", false, 7)));
    CHECK(Host::PickStart(sim, std::string(), false, 0) == a);
    CHECK(Host::PickStart(sim, std::string(), false, 1) == b);
    CHECK(Host::PickStart(sim, b, true, 0) == a);
    const Proto::Starts offer = Host::StartsMessage(sim, false);
    REQUIRE(offer.options.size() == 2);
    CHECK(offer.options[1].system == b);
    CHECK(offer.options[1].station == sim.Starts()[1].station);

    // An account from a galaxy that is gone -- the hand-written one, say -- keeps what it
    // had and is put at the start it chose rather than nowhere.
    const std::string old = dir.Path("veteran.json");
    {
        std::ofstream f(old);
        f << nlohmann::json{
            { "version", Save::ACCOUNT_VERSION },
            { "money", 4321.0 },
            { "place", { { "system", "core" }, { "pos", { 10.0, 20.0 } } } }
        }.dump();
    }
    CHECK(Host::AccountSystem(old) == "core");
    CHECK(Host::NeedsStart(sim, old));
    ClientSession& s = sim.CreateSession(b, sim.SafeArrival(b), GetShipCatalog()[0].stats);
    REQUIRE(sim.LoadAccount(s, old) == Save::Result::Ok);
    CHECK(s.systemId == b);
    CHECK(s.account.GetMoney() == doctest::Approx(4321.0));

    // ...and once saved from there, it has a place and is not asked again.
    const std::string now = dir.Path("veteran2.json");
    REQUIRE(sim.SaveAccount(s, now));
    CHECK(Host::AccountSystem(now) == b);
    CHECK_FALSE(Host::NeedsStart(sim, now));
}

TEST_CASE("the starts and the answer cross the wire (#342)")
{
    Proto::Starts s;
    s.randomOnly = true;
    s.options.push_back({ "r22-1-1", "Lysen", "Lysen Exchange", "TradersGuild" });
    s.options.push_back({ "r28-1-1", "Corox", "Corox Exchange", "Syndicate" });
    Proto::Starts back;
    REQUIRE(Proto::DecodeStarts(Proto::EncodeStarts(s), back));
    CHECK(back.randomOnly);
    REQUIRE(back.options.size() == 2);
    CHECK(back.options[1].system == "r28-1-1");
    CHECK(back.options[1].name == "Corox");
    CHECK(back.options[1].station == "Corox Exchange");
    CHECK(back.options[1].owner == "Syndicate");

    Proto::ChooseStart c;
    c.system = "r28-1-1";
    Proto::ChooseStart cb;
    REQUIRE(Proto::DecodeChooseStart(Proto::EncodeChooseStart(c), cb));
    CHECK(cb.system == "r28-1-1");
    CHECK_FALSE(Proto::DecodeChooseStart(Proto::EncodeStarts(s), cb));
    // A field of the wrong type is a missing one, not an exception (#341).
    CHECK(Proto::DecodeStarts("{\"v\":" + std::to_string(Proto::PROTO_VERSION) +
                                  ",\"t\":\"starts\",\"options\":[{\"sys\":5},7],\"random\":\"x\"}",
                              back));
    CHECK(back.options.empty());
    CHECK_FALSE(back.randomOnly);
}
