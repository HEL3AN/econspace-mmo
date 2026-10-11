#include <doctest/doctest.h>

#include <fstream>
#include <iterator>
#include <memory>
#include <string>

#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "entities/ShipType.h"
#include "factions_named.h"
#include "sim/ClientSession.h"
#include "sim/SaveSchema.h"
#include "sim/Simulation.h"
#include "temp_dir.h"
#include <nlohmann/json.hpp>

// Factions as data (#355): a registry read strictly from factions.json, growing at runtime,
// and named by id -- never by index -- in everything that outlives the process.

namespace
{
using json = nlohmann::json;

const std::string DATA = TEST_DATA_DIR;

json Shipped()
{
    std::ifstream in(DATA + "factions.json");
    return json::parse(in);
}

// Load()'s verdict on a variant of the shipped file. The shipped one is reloaded by the
// caller once it is done, so later tests see the real registry.
bool LoadJson(const json& j)
{
    const TempDir     tmp;
    const std::string path = tmp.Path("factions.json");
    {
        std::ofstream out(path);
        out << j.dump();
    }
    return Factions::Load(path);
}

bool Mentions(const std::string& needle)
{
    return Factions::Error().find(needle) != std::string::npos;
}

std::string Slurp(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void Spit(const std::string& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary);
    out << text;
}

std::unique_ptr<Simulation> World()
{
    SetRandomSeed(0xC0FFEEu);
    REQUIRE(Factions::Load(DATA + "factions.json"));
    REQUIRE(Archetypes::Load(DATA + "archetypes.json"));
    REQUIRE(Blueprints::Load(DATA + "blueprints.json"));
    auto sim = std::make_unique<Simulation>();
    sim->LoadUniverse(DATA + "universe.json");
    return sim;
}
}  // namespace

TEST_CASE("the four factions of the original game load from data, in their order")
{
    REQUIRE(Factions::Load(DATA + "factions.json"));
    REQUIRE(Factions::Count() == 4);
    const char* ids[] = { "Independent", "TradersGuild", "Syndicate", "Pirates" };
    int         i = 0;
    for (FactionId f : Factions::All())
        CHECK(Factions::Id(f) == ids[i++]);
    CHECK(Factions::Id(Factions::Unaffiliated()) == "Independent");
    CHECK(Factions::Unaffiliated() == FactionId());
    CHECK(Factions::Id(Factions::Pirates()) == "Pirates");
    CHECK(Factions::Id(Factions::DefaultPolice()) == "TradersGuild");
    CHECK(Factions::Id(Factions::Traders()) == "TradersGuild");
    CHECK(FactionName(Fac::TradersGuild()) == "Traders Guild");
    CHECK(Factions::Relation(Fac::TradersGuild(), Fac::Pirates()) == Stance::War);
    CHECK(Factions::Relation(Fac::Pirates(), Fac::TradersGuild()) == Stance::War);
    CHECK(Factions::Relation(Fac::Syndicate(), Fac::Syndicate()) == Stance::Ally);
    CHECK_FALSE(Factions::IsLawful(Fac::Pirates()));
    CHECK(Factions::TemperamentOf(Fac::Pirates()).appetite == doctest::Approx(1.4f));
    CHECK(Factions::TemperamentOf(Fac::Syndicate()).ore == doctest::Approx(0.8f));
    CHECK(Factions::MigrationRules().miner == doctest::Approx(360.0f));
}

TEST_CASE("a mistake in factions.json is a load error, not a silent Independent (#357)")
{
    REQUIRE(Factions::Load(DATA + "factions.json"));
    const json base = Shipped();

    SUBCASE("the shipped file, written back, still loads")
    {
        CHECK(LoadJson(base));
    }
    SUBCASE("an unknown field, at every level")
    {
        json j = base;
        j["factoins"] = json::array();
        CHECK_FALSE(LoadJson(j));
        CHECK(Mentions("factoins"));

        j = base;
        j["factions"][1]["lawfull"] = true;
        CHECK_FALSE(LoadJson(j));
        CHECK(Mentions("TradersGuild"));
        CHECK(Mentions("lawfull"));

        j = base;
        j["factions"][3]["temperament"]["apetite"] = 2.0;
        CHECK_FALSE(LoadJson(j));
        CHECK(Mentions("apetite"));

        j = base;
        j["factions"][2]["temperament"]["values"]["ores"] = 1.0;
        CHECK_FALSE(LoadJson(j));
        CHECK(Mentions("ores"));

        j = base;
        j["migration"]["hauler"] = 10;
        CHECK_FALSE(LoadJson(j));
        CHECK(Mentions("hauler"));

        j = base;
        j["repTiers"]["loved"] = 90;
        CHECK_FALSE(LoadJson(j));
        CHECK(Mentions("loved"));
    }
    SUBCASE("a relation naming a faction that is not there, or a stance that is not one")
    {
        json j = base;
        j["relations"].push_back({ "TradersGuild", "Pirate", "War" });
        CHECK_FALSE(LoadJson(j));
        CHECK(Mentions("Pirate"));

        j = base;
        j["relations"].push_back({ "TradersGuild", "Pirates", "Enemy" });
        CHECK_FALSE(LoadJson(j));
        CHECK(Mentions("Enemy"));
    }
    SUBCASE("a faction listed twice")
    {
        json j = base;
        j["factions"].push_back(base["factions"][2]);
        CHECK_FALSE(LoadJson(j));
        CHECK(Mentions("twice"));
    }
    SUBCASE("roles: each named, each a faction, the unaffiliated first")
    {
        json j = base;
        j["roles"]["pirates"] = "Pirate";
        CHECK_FALSE(LoadJson(j));
        CHECK(Mentions("Pirate"));

        j = base;
        j["roles"].erase("police");
        CHECK_FALSE(LoadJson(j));
        CHECK(Mentions("police"));

        j = base;
        j.erase("roles");
        CHECK_FALSE(LoadJson(j));

        j = base;
        j["roles"]["unaffiliated"] = "Syndicate";
        CHECK_FALSE(LoadJson(j));
        CHECK(Mentions("first"));
    }
    SUBCASE("no file, or not JSON")
    {
        CHECK_FALSE(Factions::Load(DATA + "no_such_factions.json"));
        const TempDir     tmp;
        const std::string path = tmp.Path("broken.json");
        Spit(path, "{ \"factions\": [");
        CHECK_FALSE(Factions::Load(path));
    }
    SUBCASE("a refused file leaves the registry as it was")
    {
        json j = base;
        j["factions"][0]["id"] = "Freeholders";
        j["factions"].push_back(json{ { "id", "Extra" }, { "wat", 1 } });
        CHECK_FALSE(LoadJson(j));
        CHECK(Factions::Count() == 4);
        FactionId f;
        CHECK(Factions::Find("Independent", f));
        CHECK_FALSE(Factions::Find("Freeholders", f));
    }
    REQUIRE(Factions::Load(DATA + "factions.json"));
}

TEST_CASE("a faction added at runtime is a faction like the others (#342)")
{
    REQUIRE(Factions::Load(DATA + "factions.json"));
    PerFaction<float> presence;
    presence[Fac::Syndicate()] = 3.0f;  // sized by the registry as it was

    FactionDef d;
    d.id = "Driftborn";
    d.name = "The Driftborn";
    d.lawful = true;
    d.kind = "Minor";
    d.temperament.capacity = 2.0f;
    const FactionId f = Factions::Add(d);
    CHECK(Factions::Count() == 5);
    CHECK(f.Index() == 4);
    FactionId found;
    REQUIRE(Factions::Find("Driftborn", found));
    CHECK(found == f);
    CHECK(FactionName(f) == "The Driftborn");
    CHECK(Factions::TemperamentOf(f).capacity == doctest::Approx(2.0f));
    // As a pair the file does not list: allied with itself, hostile to the pirates, neutral
    // to everyone else -- and the old relations are untouched.
    CHECK(Factions::Relation(f, f) == Stance::Ally);
    CHECK(Factions::Relation(f, Fac::Pirates()) == Stance::Hostile);
    CHECK(Factions::Relation(Fac::Pirates(), f) == Stance::Hostile);
    CHECK(Factions::Relation(f, Fac::TradersGuild()) == Stance::Neutral);
    CHECK(Factions::Relation(Fac::TradersGuild(), Fac::Pirates()) == Stance::War);
    // Adding it again is the same faction.
    CHECK(Factions::Add(d) == f);
    CHECK(Factions::Count() == 5);

    // What was sized before reads nothing for it, and grows when written.
    const PerFaction<float>& seen = presence;
    CHECK(seen[f] == 0.0f);
    presence[f] = 1.5f;
    CHECK(seen[f] == 1.5f);
    CHECK(seen[Fac::Syndicate()] == 3.0f);
    int n = 0;
    for (FactionId g : Factions::All())
        n += g == f ? 1 : 0;
    CHECK(n == 1);

    REQUIRE(Factions::Load(DATA + "factions.json"));  // and the next test sees the file's four
    CHECK(Factions::Count() == 4);
}

TEST_CASE("a save from before #355 names factions by the compiled index, and still loads")
{
    FactionId f;
    REQUIRE(Factions::Load(DATA + "factions.json"));
    REQUIRE(Factions::FromLegacyIndex(3, f));
    CHECK(f == Fac::Pirates());
    REQUIRE(Factions::FromLegacyIndex(1, f));
    CHECK(f == Fac::TradersGuild());
    CHECK_FALSE(Factions::FromLegacyIndex(4, f));
    CHECK_FALSE(Factions::FromLegacyIndex(-1, f));

    SUBCASE("a version 10 world: a controller is a number, presence a list of four")
    {
        const TempDir     tmp;
        const std::string path = tmp.Path("world_v10.json");
        Spit(path, R"({ "version": 10, "simTime": 600.0,
            "galaxy": {
              "core":  { "controller": 2, "presence": [0.0, 1.0, 4.5, 0.25], "claimed": true },
              "verge": { "controller": 3, "presence": [0.5, 0.0, 0.0, 6.0], "claimed": true } },
            "factions": { "Syndicate": { "intel": { "verge": {
                "seenAt": 300.0, "presence": [0.0, 0.0, 0.0, 5.0], "controller": "Pirates",
                "claimed": true } }, "stock": 2.0 } } })");
        auto sim = World();
        REQUIRE(sim->LoadWorld(path) == Save::Result::Ok);
        const SystemAggregate& core = sim->Systems().at("core").agg;
        CHECK(core.controller == Fac::Syndicate());
        CHECK(core.presence[Fac::TradersGuild()] == doctest::Approx(1.0f));
        CHECK(core.presence[Fac::Syndicate()] == doctest::Approx(4.5f));
        CHECK(core.presence[Fac::Pirates()] == doctest::Approx(0.25f));
        const SystemAggregate& verge = sim->Systems().at("verge").agg;
        CHECK(verge.controller == Fac::Pirates());
        CHECK(verge.presence[Fac::Independent()] == doctest::Approx(0.5f));
        const auto& intel = sim->MindOf(Fac::Syndicate()).intel;
        REQUIRE(intel.count("verge") == 1);
        CHECK(intel.at("verge").presence[Fac::Pirates()] == doctest::Approx(5.0f));

        // Written back, by id: nothing in it is an index any more.
        sim->SaveWorld(path);
        const json back = json::parse(Slurp(path));
        CHECK(back["version"] == Save::WORLD_VERSION);
        CHECK(back["galaxy"]["core"]["controller"] == "Syndicate");
        CHECK(back["galaxy"]["core"]["presence"]["Syndicate"] == doctest::Approx(4.5));
        CHECK_FALSE(back["galaxy"]["core"]["presence"].contains("Independent"));  // none there
        auto again = World();
        REQUIRE(again->LoadWorld(path) == Save::Result::Ok);
        CHECK(again->Systems().at("core").agg.controller == Fac::Syndicate());
        CHECK(again->Systems().at("verge").agg.presence[Fac::Pirates()] == doctest::Approx(6.0f));
    }

    SUBCASE("an older account: reputation and bounty are lists, a mission's faction a number")
    {
        const TempDir     tmp;
        const std::string path = tmp.Path("account_v2.json");
        Spit(path, R"({ "version": 2, "money": 1234.0,
            "reputation": [0.0, 12.0, -3.0, -40.0], "bounty": [0.0, 0.0, 250.0, 0.0],
            "missions": [ { "type": 0, "faction": 2, "title": "Old job", "giver": 5,
                            "dest": 6, "target": 1 } ] })");
        auto sim = World();
        sim->InitGalaxy();
        sim->MaterializeAllSystems(DATA + "systems/");
        const std::string& start = sim->Universe().startId;
        ClientSession&     s =
            sim->CreateSession(start, sim->SafeArrival(start), GetShipCatalog()[0].stats);
        REQUIRE(sim->LoadAccount(s, path) == Save::Result::Ok);
        CHECK(s.account.GetReputation(Fac::TradersGuild()) == doctest::Approx(12.0f));
        CHECK(s.account.GetReputation(Fac::Pirates()) == doctest::Approx(-40.0f));
        CHECK(s.account.GetBounty(Fac::Syndicate()) == doctest::Approx(250.0));
        REQUIRE(s.missions.Active().size() == 1);
        CHECK(s.missions.Active()[0].faction == Fac::Syndicate());

        sim->SaveAccount(s, path);
        const json back = json::parse(Slurp(path));
        CHECK(back["version"] == Save::ACCOUNT_VERSION);
        CHECK(back["reputation"]["Pirates"] == doctest::Approx(-40.0));
        CHECK(back["bounty"]["Syndicate"] == doctest::Approx(250.0));
        CHECK(back["missions"][0]["faction"] == "Syndicate");
        ClientSession& t =
            sim->CreateSession(start, sim->SafeArrival(start), GetShipCatalog()[0].stats);
        REQUIRE(sim->LoadAccount(t, path) == Save::Result::Ok);
        CHECK(t.account.GetReputation(Fac::Pirates()) == doctest::Approx(-40.0f));
        CHECK(t.account.GetBounty(Fac::Syndicate()) == doctest::Approx(250.0));
    }
}
