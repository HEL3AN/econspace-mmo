#include <doctest/doctest.h>

#include "core/Archetype.h"
#include "entities/Planet.h"
#include "entities/Station.h"
#include "render/Material.h"
#include "render/Silhouette.h"
#include <nlohmann/json.hpp>
#include <cstdio>
#include <fstream>
#include <string>

// Mistakes in content that used to load as something plausible and wrong (#191). Each one
// is the same bargain an unknown form (#122) already made: an author can fix a load error
// in a minute, and nobody finds an object that is quietly the wrong object.

namespace
{
std::string DataFile(const char* name)
{
    return std::string(TEST_DATA_DIR) + name;
}

// The registry with one extra entry appended, written to the build tree. Returns Load()'s
// verdict; the shipped registry is reloaded afterwards so later tests see the real one.
bool LoadWithEntry(const std::string& entry)
{
    const std::string path = "load_mistake_tmp.json";
    {
        std::ofstream f(path);
        f << R"({"archetypes":[)" << entry << "]}";
    }
    const bool ok = Archetypes::Load(path);
    std::remove(path.c_str());
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
