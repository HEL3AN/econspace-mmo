#include <doctest/doctest.h>

#include "core/Archetype.h"
#include "core/WorldLoader.h"
#include "entities/Planet.h"
#include "entities/Station.h"
#include "render/Material.h"
#include "render/Silhouette.h"
#include "temp_dir.h"
#include <nlohmann/json.hpp>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// Mistakes in content that used to load as something plausible and wrong (#191). Each one
// is the same bargain an unknown form (#122) already made: an author can fix a load error
// in a minute, and nobody finds an object that is quietly the wrong object.

namespace
{
std::string DataFile(const char* name)
{
    return std::string(TEST_DATA_DIR) + name;
}

// The registry with one extra entry appended, written to a temp folder. Returns Load()'s
// verdict; the shipped registry is reloaded afterwards so later tests see the real one.
bool LoadWithEntry(const std::string& entry)
{
    const TempDir     tmp;
    const std::string path = tmp.Path("load_mistake.json");
    {
        std::ofstream f(path);
        f << R"({"archetypes":[)" << entry << "]}";
    }
    const bool ok = Archetypes::Load(path);
    return ok;
}

bool Mentions(const std::string& needle)
{
    return Archetypes::Error().find(needle) != std::string::npos;
}
}  // namespace

TEST_CASE("a misspelled archetype field is refused, not read as absent")
{
    REQUIRE(Archetypes::Load(DataFile("archetypes.json")));

    SUBCASE("a well-formed entry still loads, so the refusals below are about the typo")
    {
        CHECK(LoadWithEntry(R"({"id":"x","kind":"Station","size":90,
                                "components":{"dockable":{"range":120}}})"));
        REQUIRE(Archetypes::Load(DataFile("archetypes.json")));
    }

    SUBCASE("at the top level")
    {
        CHECK_FALSE(LoadWithEntry(R"({"id":"x","kind":"Station","szie":90})"));
        CHECK(Mentions("'x'"));
        CHECK(Mentions("szie"));
    }

    SUBCASE("in a component's parameters, where the default would quietly win")
    {
        CHECK_FALSE(LoadWithEntry(
            R"({"id":"x","kind":"Station","components":{"dockable":{"rnage":120}}})"));
        CHECK(Mentions("dockable"));
        CHECK(Mentions("rnage"));
    }

    SUBCASE("a component with no parameters takes none")
    {
        CHECK_FALSE(
            LoadWithEntry(R"({"id":"x","kind":"Station","components":{"market":{"spread":2}}})"));
        CHECK(Mentions("spread"));
    }

    SUBCASE("in the light")
    {
        CHECK_FALSE(LoadWithEntry(R"({"id":"x","kind":"Star","light":{"raduis":5000}})"));
        CHECK(Mentions("raduis"));
        CHECK_FALSE(LoadWithEntry(R"({"id":"x","kind":"Star","light":{"slef":true}})"));
        CHECK(Mentions("slef"));
        // A string where a boolean goes is refused too: "false" read as true is worse.
        CHECK_FALSE(LoadWithEntry(R"({"id":"x","kind":"Star","light":{"self":"false"}})"));
        CHECK(Mentions("self"));
    }

    SUBCASE("in the world block")
    {
        CHECK_FALSE(LoadWithEntry(R"({"id":"x","kind":"Station","size":90,
            "world":{"category":"stations","subType":"Military","sub":"x"}})"));
        CHECK(Mentions("'sub'"));
    }

    SUBCASE("in a part of the shape")
    {
        CHECK_FALSE(
            LoadWithEntry(R"({"id":"x","kind":"Station","shape":[{"form":"disc","raidus":1.0}]})"));
        CHECK(Mentions("raidus"));
    }

    // Nothing above replaced the shipped registry.
    CHECK(Archetypes::Find("station.trade_hub") != nullptr);
    CHECK(Archetypes::Find("x") == nullptr);
}

TEST_CASE("a placed archetype names a subtype the world file can read back")
{
    REQUIRE(Archetypes::Load(DataFile("archetypes.json")));

    SUBCASE("a station role nobody knows would come back as a trade hub")
    {
        CHECK_FALSE(LoadWithEntry(R"({"id":"station.casino","kind":"Station","size":90,
            "world":{"category":"stations","subType":"Casino"}})"));
        CHECK(Mentions("station.casino"));
        CHECK(Mentions("Casino"));
    }

    SUBCASE("and a planet type nobody knows would come back rocky")
    {
        CHECK_FALSE(LoadWithEntry(R"({"id":"planet.jelly","kind":"Planet","size":90,
            "world":{"category":"planets","subType":"Jelly"}})"));
        CHECK(Mentions("Jelly"));
    }

    SUBCASE("a category no system file has")
    {
        CHECK_FALSE(LoadWithEntry(R"({"id":"x","kind":"Station","size":90,
            "world":{"category":"station"}})"));
        CHECK(Mentions("station"));
    }

    SUBCASE("a category without a type key does not invent one")
    {
        CHECK_FALSE(LoadWithEntry(R"({"id":"x","kind":"Gate","size":90,
            "world":{"category":"gates","subType":"Big"}})"));
    }

    SUBCASE("every role and type a placed archetype can name is read back as itself")
    {
        for (StationRole r : { StationRole::TradeHub, StationRole::MiningOutpost,
                               StationRole::Shipyard, StationRole::Military })
        {
            REQUIRE(LoadWithEntry(R"({"id":"x","kind":"Station","size":90,
                "world":{"category":"stations","subType":")" +
                                  std::string(r == StationRole::TradeHub        ? "TradeHub"
                                              : r == StationRole::MiningOutpost ? "MiningOutpost"
                                              : r == StationRole::Shipyard      ? "Shipyard"
                                                                                : "Military") +
                                  R"("}})"));
            const Archetype* a = Archetypes::Find("x");
            REQUIRE(a != nullptr);
            StationRole back = StationRole::TradeHub;
            CHECK(ParseStationRole(a->worldSubType, back));
            CHECK(back == r);
        }
        PlanetType t;
        CHECK_FALSE(ParsePlanetType("Jelly", t));
        CHECK(ParsePlanetType("Oceanic", t));
        CHECK(t == PlanetType::Oceanic);
    }

    REQUIRE(Archetypes::Load(DataFile("archetypes.json")));
}

TEST_CASE("an archetype naming a material that does not exist is named, not drawn plain in "
          "silence")
{
    REQUIRE(Render::Materials::Load(DataFile("materials.json")));
    REQUIRE(Archetypes::Load(DataFile("archetypes.json")));

    // What ships is clean -- this is what the client logs at load, and it is empty.
    CHECK(Render::Materials::UnknownInArchetypes().empty());

    REQUIRE(LoadWithEntry(R"({"id":"station.shiny","kind":"Station","material":"chrome"})"));
    const std::vector<std::string> unknown = Render::Materials::UnknownInArchetypes();
    REQUIRE(unknown.size() == 1);
    CHECK(unknown[0].find("station.shiny") != std::string::npos);
    CHECK(unknown[0].find("chrome") != std::string::npos);

    REQUIRE(Archetypes::Load(DataFile("archetypes.json")));
}

TEST_CASE("a hand-written system with a mistake in it does not load, and says what (#357)")
{
    REQUIRE(Archetypes::Load(DataFile("archetypes.json")));
    const auto problemsOf = [](const nlohmann::json& doc)
    {
        std::vector<std::string> problems;
        WorldLoader::CheckDocument(doc, "sys.json", problems);
        return problems;
    };
    const auto says = [](const std::vector<std::string>& problems, const std::string& what)
    {
        for (const std::string& p : problems)
            if (p.find(what) != std::string::npos)
                return true;
        return false;
    };
    const nlohmann::json good = nlohmann::json::parse(R"({
        "star": { "type": "Yellow", "size": 150000 },
        "planets": [ { "type": "Oceanic", "orbitRadius": 400000, "size": 15000 } ],
        "stations": [ { "name": "Port", "pos": [ 1000, 0 ], "size": 40, "role": "TradeHub",
                        "orbits": { "planet": 0, "radius": 30000, "speed": 2 } } ],
        "gates": [ { "name": "To Reach", "pos": [ 900000, 0 ], "size": 50,
                     "destination": "reach" } ]
    })");
    CHECK(problemsOf(good).empty());

    // A misspelt field, wherever it is, is named with where it is.
    nlohmann::json doc = good;
    doc["planets"][0]["orbitRaduis"] = 1;
    CHECK(says(problemsOf(doc), "sys.json: planets[0]: unknown field \"orbitRaduis\""));
    doc = good;
    doc["stations"][0]["rol"] = "Mining";
    CHECK(says(problemsOf(doc), "stations[0] \"Port\": unknown field \"rol\""));
    doc = good;
    doc["gates"][0]["destinaton"] = "verge";
    CHECK(says(problemsOf(doc), "gates[0] \"To Reach\": unknown field \"destinaton\""));
    doc = good;
    doc["nebula"] = nlohmann::json::array();
    CHECK(says(problemsOf(doc), "sys.json: unknown field \"nebula\""));
    doc = good;
    doc["star"]["colour"] = "gold";
    CHECK(says(problemsOf(doc), "star: unknown field \"colour\""));
    doc = good;
    doc["stations"][0]["orbits"]["phse"] = 1;
    CHECK(says(problemsOf(doc), "orbits: unknown field \"phse\""));

    // What BuildSystem would only warn about and build as something else.
    doc = good;
    doc["planets"][0]["type"] = "Oceanik";
    CHECK(says(problemsOf(doc), "unknown planet type \"Oceanik\""));
    doc = good;
    doc["stations"][0]["role"] = "Casino";
    CHECK(says(problemsOf(doc), "unknown role \"Casino\""));
    doc = good;
    doc["stations"][0]["faction"] = "Nobody";
    CHECK(says(problemsOf(doc), "unknown faction \"Nobody\""));
    doc = good;
    doc["star"]["type"] = "Green";
    CHECK(says(problemsOf(doc), "a star's \"type\" is"));
    doc = good;
    doc["stations"][0]["archetype"] = "planet.rocky";
    CHECK(says(problemsOf(doc), "archetype \"planet.rocky\" is not one of the stations"));

    // From a file: a load error, an empty system rather than a plausible wrong one.
    const TempDir     tmp;
    const std::string goodPath = tmp.Path("good.json");
    const std::string badPath = tmp.Path("bad.json");
    std::ofstream(goodPath) << good.dump();
    doc = good;
    doc["planets"][0]["orbitRaduis"] = 1;
    std::ofstream(badPath) << doc.dump();
    CHECK(WorldLoader::LoadSystem(goodPath).size() == 4);
    CHECK(WorldLoader::LoadSystem(badPath).empty());

    // What the server and the generator write stays permissive: a save from another build,
    // with a field this one does not know, still builds.
    CHECK(WorldLoader::BuildSystem(doc).size() == 4);
}

TEST_CASE("every hand-written system that ships passes the check (#357)")
{
    REQUIRE(Archetypes::Load(DataFile("archetypes.json")));
    int checked = 0;
    for (const std::string& dir :
         { std::string(TEST_WORLD_DIR) + "systems/", std::string(TEST_DATA_DIR) + "systems/" })
    {
        std::error_code ec;
        for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end;
             it.increment(ec))
        {
            if (it->path().extension() != ".json")
                continue;
            std::ifstream            in(it->path());
            const nlohmann::json     doc = nlohmann::json::parse(in, nullptr, false);
            std::vector<std::string> problems;
            const std::string        where = it->path().generic_string();
            WorldLoader::CheckDocument(doc, where, problems);
            for (const std::string& p : problems)
                MESSAGE(p);
            CHECK(problems.empty());
            checked++;
        }
    }
    CHECK(checked >= 3);
}
