#include <doctest/doctest.h>

#include "core/Archetype.h"
#include "core/World.h"
#include "core/WorldLoader.h"
#include "entities/Entity.h"
#include "gen/Region.h"
#include "gen/Rng.h"

#include <cmath>
#include <deque>
#include <fstream>
#include <set>
#include <string>

// The region from a seed (#140). Every rule in M7 is worthless if its answer changes between
// runs, machines or compilers, so that is what most of this file holds the generator to.

namespace
{
Gen::RegionParams Params(uint64_t seed)
{
    Gen::RegionParams p;
    p.seed = seed;
    p.homeId = "core";
    return p;
}

// Everything the generator decides, as one string.
std::string Dump(const Gen::Region& r)
{
    nlohmann::json all;
    all["systems"] = r.systems;
    all["links"] = r.links;
    all["entry"] = r.entryId;
    all["wormhole"] = r.wormhole;
    for (const auto& kv : r.documents)
        all["docs"][kv.first] = kv.second;
    return all.dump();
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
}  // namespace

TEST_CASE("the generator's randomness is the same everywhere")
{
    // SplitMix64 from a published seed: if this changes, every galaxy changes.
    Gen::Rng r(1234567);
    CHECK(r.Next() == 6457827717110365317ull);
    CHECK(r.Next() == 3203168211198807973ull);

    // Ranges are inclusive and unbiased enough to reach both ends.
    Gen::Rng      s(42);
    std::set<int> seen;
    for (int i = 0; i < 2000; i++)
        seen.insert(s.Range(3, 7));
    CHECK(seen == std::set<int>{ 3, 4, 5, 6, 7 });
}

TEST_CASE("the same seed makes the same region, byte for byte")
{
    CHECK(Dump(Gen::GenerateRegion(Params(7))) == Dump(Gen::GenerateRegion(Params(7))));
    CHECK(Dump(Gen::GenerateRegion(Params(7))) != Dump(Gen::GenerateRegion(Params(8))));
}

TEST_CASE("the rules have not changed without saying so")
{
    // A golden value. CI computes it on three compilers (MinGW, GCC, Apple Clang): if it
    // differs between them, the generator is not deterministic across platforms, which is
    // the one thing it must be. If it differs after a change to the rules, that change made
    // a different galaxy from the same seed -- bump Gen::GENERATOR_VERSION (saves from the
    // old rules are then refused) and update the value here.
    const uint64_t h = Fnv1a(Dump(Gen::GenerateRegion(Params(1))));
    MESSAGE("region hash for seed 1: " << h);
    CHECK(Gen::GENERATOR_VERSION == 2);
    CHECK(h == 18147451084883590342ull);
}

TEST_CASE("every system can be reached from home, and every link has a gate on both ends")
{
    for (uint64_t seed : { 1ull, 2ull, 99ull, 123456789ull })
    {
        CAPTURE(seed);
        const Gen::Region r = Gen::GenerateRegion(Params(seed));
        REQUIRE(r.documents.size() == r.systems.size());

        std::map<std::string, std::vector<std::string>> adj;
        for (const auto& l : r.links)
        {
            adj[l[0]].push_back(l[1]);
            adj[l[1]].push_back(l[0]);
        }
        std::set<std::string>   reached = { "core" };
        std::deque<std::string> todo = { "core" };
        while (!todo.empty())
        {
            const std::string at = todo.front();
            todo.pop_front();
            for (const std::string& next : adj[at])
                if (reached.insert(next).second)
                    todo.push_back(next);
        }
        CHECK(reached.size() == r.systems.size() + 1);

        for (const auto& kv : r.documents)
        {
            CAPTURE(kv.first);
            std::set<std::string> dests;
            for (const auto& g : kv.second["gates"])
                dests.insert(g["destination"].get<std::string>());
            std::set<std::string> linked(adj[kv.first].begin(), adj[kv.first].end());
            CHECK(dests == linked);
        }
        CHECK(r.wormhole["destination"] == r.entryId);
    }
}

TEST_CASE("nothing generated sits in a planet's path or past the system's edge")
{
    for (uint64_t seed = 1; seed <= 40; seed++)
    {
        const Gen::Region r = Gen::GenerateRegion(Params(seed));
        for (const auto& kv : r.documents)
        {
            const nlohmann::json& sys = kv.second;
            for (const char* group : { "gates", "asteroidFields", "derelicts" })
                for (const auto& o : sys[group])
                {
                    const double x = o["pos"][0], y = o["pos"][1];
                    const double d = std::sqrt(x * x + y * y), size = o["size"];
                    CAPTURE(seed);
                    CAPTURE(kv.first);
                    CAPTURE(o.dump());
                    CHECK(d + size < World::SYSTEM_RADIUS);
                    for (const auto& p : sys["planets"])
                    {
                        const double R = p["orbitRadius"], ps = p["size"];
                        CHECK(std::fabs(d - R) >= ps + size);
                    }
                    // and not inside a star -- one in the middle, or a binary's two
                    if (sys.contains("star"))
                        CHECK(d > sys["star"]["size"].get<double>() + size);
                    if (sys.contains("stars"))
                        for (const auto& s : sys["stars"])
                        {
                            const double sx = s["pos"][0], sy = s["pos"][1];
                            CHECK(std::hypot(x - sx, y - sy) > s["size"].get<double>() + size);
                        }
                }
        }
    }
}

TEST_CASE("a generated system is a system the game can build")
{
    REQUIRE(Archetypes::Load(std::string(TEST_DATA_DIR) + "archetypes.json"));
    const Gen::Region r = Gen::GenerateRegion(Params(3));
    for (const auto& kv : r.documents)
    {
        CAPTURE(kv.first);
        const auto entities = WorldLoader::BuildSystem(kv.second);
        REQUIRE_FALSE(entities.empty());
        for (const auto& e : entities)
            CHECK(e->GetArchetype() != nullptr);  // every kind it used is a real one
    }
}

TEST_CASE("an object can say which archetype of its kind it is (#142)")
{
    REQUIRE(Archetypes::Load(std::string(TEST_DATA_DIR) + "archetypes.json"));
    const nlohmann::json sys = nlohmann::json::parse(R"({
        "derelicts": [
            { "name": "Plain", "pos": [100, 0], "size": 40, "reward": 100 },
            { "name": "Named", "pos": [200, 0], "size": 40, "reward": 100, "archetype": "derelict.wreck" },
            { "name": "Typo", "pos": [300, 0], "size": 40, "reward": 100, "archetype": "derelict.nonesuch" },
            { "name": "Wrong kind", "pos": [400, 0], "size": 40, "reward": 100, "archetype": "gate.jump" }
        ]
    })");
    const auto           e = WorldLoader::BuildSystem(sys);
    REQUIRE(e.size() == 4);
    for (const auto& d : e)
    {
        // Every one is built -- a typo does not empty a system -- and every one ends up an
        // archetype of its own kind, never a gate's look on a wreck.
        REQUIRE(d->GetArchetype() != nullptr);
        CHECK(d->GetArchetype()->kind == EntityKind::Derelict);
    }
    CHECK(e[1]->GetArchetype()->id == "derelict.wreck");
}
