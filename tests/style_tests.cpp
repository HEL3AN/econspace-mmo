#include <doctest/doctest.h>

#include <fstream>
#include <memory>
#include <string>

#include "goods_named.h"
#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "core/ShipDesign.h"
#include "gen/Region.h"
#include "sim/Simulation.h"
#include "sim/WorldRules.h"
#include "temp_dir.h"
#include <nlohmann/json.hpp>

// Region styles (#464, slice 3): what a region's markets hold and make leans what its NPCs
// fly, in proportion -- the owner's rule of 2026-10-23: like the real world, not a fixed
// number -- with its people's culture as a modest prior.

namespace
{
const std::string DATA = TEST_DATA_DIR;

Ships::Catalogue Shipped()
{
    Ships::Catalogue c;
    std::string      error;
    REQUIRE_MESSAGE(Ships::Load(DATA + "ships.json", c, error), error);
    return c;
}

float ShareOf(const std::vector<std::pair<std::string, float>>& shares, const std::string& id)
{
    for (const auto& [k, v] : shares)
        if (k == id)
            return v;
    FAIL("no share for " << id);
    return 0.0f;
}

// Units of each good: `laser` of each laser good, `railgun` of each railgun good.
std::vector<double> UnitsOf(double laser, double railgun)
{
    std::vector<double> s((size_t)Goods::Count(), 0.0);
    for (const char* g : { "Crystal", "Circuits" })
        s[(size_t)Good::Named(g).Index()] += laser / 2.0;
    for (const char* g : { "Steel", "Ammunition" })
        s[(size_t)Good::Named(g).Index()] += railgun / 2.0;
    return s;
}

float Sum(const std::vector<std::pair<std::string, float>>& shares)
{
    float n = 0.0f;
    for (const auto& kv : shares)
        n += kv.second;
    return n;
}
}  // namespace

TEST_CASE("a region's style is in proportion to what it holds and makes, not sharpened")
{
    const Ships::Catalogue c = Shipped();
    // 100 units' worth of laser goods against 25 of railgun goods: 80/20.
    const Ships::Style lean = Ships::StyleFrom(c, UnitsOf(100.0, 25.0), nullptr, 0.0);
    CHECK(ShareOf(lean.families, "laser") == doctest::Approx(0.8f));
    CHECK(ShareOf(lean.families, "railgun") == doctest::Approx(0.2f));
    CHECK(lean.LeadingFamily() == "laser");
    // As much of each: half and half.
    const Ships::Style even = Ships::StyleFrom(c, UnitsOf(60.0, 60.0), nullptr, 0.0);
    CHECK(ShareOf(even.families, "laser") == doctest::Approx(0.5f));
    // Twice the goods is twice the share, at any size.
    const Ships::Style big = Ships::StyleFrom(c, UnitsOf(4000.0, 1000.0), nullptr, 0.0);
    CHECK(ShareOf(big.families, "laser") == doctest::Approx(0.8f));
    for (const Ships::Style* s : { &lean, &even, &big })
    {
        CHECK(Sum(s->families) == doctest::Approx(1.0f));
        CHECK(Sum(s->lines) == doctest::Approx(1.0f));
        CHECK(s->families.size() == c.families.size());
        CHECK(s->lines.size() == c.lines.size());
    }
}

TEST_CASE("a region's people lean its style by the prior's weight, and alone where nothing trades")
{
    const Ships::Catalogue              c = Shipped();
    const std::map<std::string, double> clan = { { "railgun", 3.0 },
                                                 { "laser", 1.0 },
                                                 { "brawler", 1.0 } };
    // An even market and a clan: a quarter of the style is the clan's three to one.
    const Ships::Style s = Ships::StyleFrom(c, UnitsOf(60.0, 60.0), &clan, 0.25);
    CHECK(ShareOf(s.families, "railgun") == doctest::Approx(0.75f * 0.5f + 0.25f * 0.75f));
    CHECK(ShareOf(s.families, "laser") == doctest::Approx(0.75f * 0.5f + 0.25f * 0.25f));
    CHECK(Sum(s.families) == doctest::Approx(1.0f));
    CHECK(Sum(s.lines) == doctest::Approx(1.0f));
    // A strong economy outweighs the prior: a laser region of clansmen flies lasers.
    const Ships::Style laserClan = Ships::StyleFrom(c, UnitsOf(100.0, 0.0), &clan, 0.25);
    CHECK(laserClan.LeadingFamily() == "laser");
    // Nothing to trade at all: the culture alone.
    const Ships::Style bare = Ships::StyleFrom(c, UnitsOf(0.0, 0.0), &clan, 0.25);
    CHECK(ShareOf(bare.families, "railgun") == doctest::Approx(0.75f));
    CHECK(bare.LeadingLine() == "brawler");
    // ...and no culture either: evenly.
    const Ships::Style none = Ships::StyleFrom(c, UnitsOf(0.0, 0.0), nullptr, 0.25);
    CHECK(ShareOf(none.families, "laser") == doctest::Approx(0.5f));
    // The economy alone, with the prior weighed at nothing.
    const Ships::Style economy = Ships::StyleFrom(c, UnitsOf(60.0, 60.0), &clan, 0.0);
    CHECK(ShareOf(economy.families, "railgun") == doctest::Approx(0.5f));
}

TEST_CASE("the style rules are data, every culture leans by a family or a line, and a mistake "
          "does not load")
{
    REQUIRE(WorldRules::Load(DATA + "world_rules.json"));
    const WorldRules::Styles r = WorldRules::StyleRules();
    CHECK(r.culture > 0.0);
    CHECK(r.culture < 0.5);  // a modest prior
    CHECK(r.hours > 0.0);
    const Ships::Catalogue c = Shipped();
    for (const char* culture :
         { "clan", "sect", "automata", "colonists", "traders", "guild", "syndicate" })
    {
        CAPTURE(culture);
        REQUIRE(r.cultures.count(culture) == 1);
        for (const auto& kv : r.cultures.at(culture))
        {
            const std::string& id = kv.first;  // not a structured binding: CAPTURE is a lambda
            CAPTURE(id);
            CHECK((c.FindFamily(id) != nullptr || c.FindLine(id) != nullptr));
            CHECK(kv.second >= 0.0);
        }
    }

    const TempDir tmp;
    auto          fails = [&](const char* edit)
    {
        nlohmann::json j;
        {
            std::ifstream in(DATA + "world_rules.json");
            in >> j;
        }
        j.merge_patch(nlohmann::json::parse(edit));
        const std::string path = tmp.Path("world_rules.json");
        {
            std::ofstream out(path);
            out << j.dump();
        }
        return !WorldRules::Load(path);
    };
    CHECK(fails(R"({"styles": {"culture": 1.5}})"));
    CHECK(fails(R"({"styles": {"sharpness": 3}})"));
    CHECK(fails(R"({"styles": {"cultures": {"clan": {"railgun": -1}}}})"));
    CHECK(fails(R"({"styles": {"cultures": {"clan": 3}}})"));
    CHECK_FALSE(fails(R"({"styles": {"culture": 0}})"));
    REQUIRE(WorldRules::Load(DATA + "world_rules.json"));
}

TEST_CASE("a world reckons each region's style from its markets, and a market full of one "
          "family's goods leans it")
{
    SetRandomSeed(0xC0FFEEu);
    REQUIRE(Factions::Load(DATA + "factions.json"));
    REQUIRE(Archetypes::Load(DATA + "archetypes.json"));
    REQUIRE(Blueprints::Load(DATA + "blueprints.json"));
    REQUIRE(WorldRules::Load(DATA + "world_rules.json"));
    auto sim = std::make_unique<Simulation>();
    sim->LoadUniverse(std::string(TEST_WORLD_DIR) + "universe.json");
    sim->AttachRegion(7, std::string(TEST_WORLD_DIR) + "systems/");
    sim->Seed(1234u);
    sim->InitGalaxy();
    sim->MaterializeAllSystems(std::string(TEST_WORLD_DIR) + "systems/");

    const Ships::Style before = sim->RegionStyle(0);
    CHECK(Sum(before.families) == doctest::Approx(1.0f));
    CHECK(Sum(before.lines) == doctest::Approx(1.0f));
    // The same markets, the same style: it is derived, never drawn.
    CHECK(sim->RegionStyle(0).families == before.families);

    // Somebody floods the region's markets with circuits: it leans toward lasers.
    const ResourceType circuits = Good::Named("Circuits");
    int                markets = 0;
    for (auto& kv : sim->Systems())
        if (Gen::SlotOf(kv.first) == 0 && kv.second.profile.markets > 0)
        {
            kv.second.market.SetStock(circuits, kv.second.market.Stock(circuits) + 5000.0);
            markets++;
        }
    REQUIRE(markets > 0);
    const Ships::Style after = sim->RegionStyle(0);
    CHECK(ShareOf(after.families, "laser") > ShareOf(before.families, "laser"));
    CHECK(after.LeadingFamily() == "laser");
}
