#include <doctest/doctest.h>

#include <memory>
#include <string>

#include "goods_named.h"
#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "core/ShipDesign.h"
#include "entities/NpcShip.h"
#include "sim/FactionMind.h"
#include "sim/Simulation.h"
#include "sim/WorldRules.h"
#include "factions_named.h"

// Buying (#464, slice 4): a holding grows by the ships it buys -- at its market, from its
// faction's treasury, in its region's style -- so money limits how many ships, not only how
// good (owner, 2026-10-23); and the world's own feedback, not a free floor, keeps a poor
// faction from emptying the map.

namespace
{
const std::string DATA = TEST_DATA_DIR;

std::unique_ptr<Simulation> BuyingWorld(uint64_t seed = 7)
{
    SetRandomSeed(0xC0FFEEu);
    REQUIRE(Factions::Load(DATA + "factions.json"));
    REQUIRE(Archetypes::Load(DATA + "archetypes.json"));
    REQUIRE(Blueprints::Load(DATA + "blueprints.json"));
    REQUIRE(WorldRules::Load(DATA + "world_rules.json"));
    auto sim = std::make_unique<Simulation>();
    sim->LoadUniverse(std::string(TEST_WORLD_DIR) + "universe.json");
    sim->AttachRegion(seed, std::string(TEST_WORLD_DIR) + "systems/");
    sim->Seed(1234u);
    sim->InitGalaxy();
    sim->MaterializeAllSystems(std::string(TEST_WORLD_DIR) + "systems/");
    return sim;
}

// A lawful faction's holding with a market of its own.
SystemState* LawfulHoldingWithMarket(Simulation& sim)
{
    for (auto& kv : sim.Systems())
        if (kv.second.agg.claimed && Factions::IsLawful(kv.second.agg.controller) &&
            kv.second.profile.markets > 0)
            return &kv.second;
    return nullptr;
}

float GunsOf(const std::map<std::string, float>& fleet, const char* family)
{
    const Ships::Catalogue& c = Archetypes::ShipCatalogue();
    float                   n = 0.0f;
    for (const auto& [id, count] : fleet)
    {
        const Ships::Design* d = c.FindDesign(id);
        const Ships::Family* f = d != nullptr ? c.FamilyOf(*d) : nullptr;
        if (f != nullptr && f->id == family)
            n += count;
    }
    return n;
}
}  // namespace

TEST_CASE("a holding buys its ships at its market, from its treasury, in its region's style")
{
    auto         sim = BuyingWorld();
    SystemState* st = LawfulHoldingWithMarket(*sim);
    REQUIRE(st != nullptr);
    const FactionId f = st->agg.controller;
    REQUIRE(sim->BuyingMarket(st->id) == st);

    // No money, no ships.
    sim->MindOf(f).treasury = 0.0;
    CHECK(sim->BuyShips(*st, f, 2.0f) == doctest::Approx(0.0f));

    // Money: as many as it asked for, paid for at the ask, and the guns' goods are gone.
    const double       rich = 1e7;
    const ResourceType crystal = Good::Named("Crystal"), circuits = Good::Named("Circuits");
    const ResourceType steel = Good::Named("Steel"), ammo = Good::Named("Ammunition");
    const double       guns = st->market.Stock(crystal) + st->market.Stock(circuits) +
                              st->market.Stock(steel) + st->market.Stock(ammo);
    sim->MindOf(f).treasury = rich;
    st->agg.fleets.clear();
    const float bought = sim->BuyShips(*st, f, 2.0f);
    CHECK(bought == doctest::Approx(2.0f));
    CHECK(sim->MindOf(f).treasury < rich);
    const double after = st->market.Stock(crystal) + st->market.Stock(circuits) +
                         st->market.Stock(steel) + st->market.Stock(ammo);
    CHECK(after < guns);
    // ...and they are in its warships, every one a fit of the catalogue.
    const auto war = st->agg.fleets.find(Simulation::FleetKey(f, NpcRole::Warship));
    REQUIRE(war != st->agg.fleets.end());
    float sum = 0.0f;
    for (const auto& [id, n] : war->second)
    {
        CHECK(Archetypes::ShipCatalogue().FindDesign(id) != nullptr);
        sum += n;
    }
    CHECK(sum >= 2.0f - 1e-3f);

    // The price is the whole ship's at the ask: twice the ships, about twice the money.
    sim->MindOf(f).treasury = rich;
    const float  one = sim->BuyShips(*st, f, 1.0f);
    const double spent = rich - sim->MindOf(f).treasury;
    REQUIRE(one == doctest::Approx(1.0f));
    CHECK(spent > 1000.0);  // a ship is a ship, not a fee
    // Money for half a ship buys half a ship.
    sim->MindOf(f).treasury = spent * 0.5;
    CHECK(sim->BuyShips(*st, f, 1.0f) == doctest::Approx(0.5f).epsilon(0.1));
}

TEST_CASE("where one family's goods are dear or gone, the holding buys the other's")
{
    auto         sim = BuyingWorld();
    SystemState* st = LawfulHoldingWithMarket(*sim);
    REQUIRE(st != nullptr);
    const FactionId f = st->agg.controller;
    // No circuits to be had: lasers cannot be fitted here.
    st->market.SetStock(Good::Named("Circuits"), 0.0);
    sim->MindOf(f).treasury = 1e7;
    st->agg.fleets.clear();
    st->agg.presence[f] = 0.0f;
    const float bought = sim->BuyShips(*st, f, 0.5f);
    CHECK(bought > 0.0f);  // as much as the market has the railguns' goods for
    const auto& war = st->agg.fleets.at(Simulation::FleetKey(f, NpcRole::Warship));
    CHECK(GunsOf(war, "laser") == doctest::Approx(0.0f));
    CHECK(GunsOf(war, "railgun") == doctest::Approx(bought));

    // Neither family's goods: nothing, and the growth waits for haulers.
    for (const char* g : { "Crystal", "Steel", "Ammunition" })
        st->market.SetStock(Good::Named(g), 0.0);
    CHECK(sim->BuyShips(*st, f, 1.0f) == doctest::Approx(0.0f));
}

TEST_CASE("a faction worn down below what its holdings keep turns defensive, and recovers")
{
    auto         sim = BuyingWorld();
    SystemState* st = LawfulHoldingWithMarket(*sim);
    REQUIRE(st != nullptr);
    const FactionId f = st->agg.controller;
    const float     keep = Factions::TemperamentOf(f).capacity;
    for (auto& kv : sim->Systems())
        if (kv.second.agg.claimed && kv.second.agg.controller == f)
            kv.second.agg.presence[f] = keep;
    CHECK_FALSE(sim->Defensive(f));
    for (auto& kv : sim->Systems())
        if (kv.second.agg.claimed && kv.second.agg.controller == f)
            kv.second.agg.presence[f] = keep * 0.3f;
    CHECK(sim->Defensive(f));
}

TEST_CASE("over the long run no faction's ships fall to nothing without losing its holdings")
{
    // The owner's failure mode (2026-10-23): a poor faction quietly emptying half the map.
    // Every holding loses most of its ships at once, and the world -- holdings' income, the
    // stock design as the fallback, the money going to the thinnest holdings first -- keeps
    // them from emptying. Short here, on a small world with pirates at every door; the
    // recovery itself is `econserver fleetbench`'s, over days on bigger worlds.
    for (uint64_t seed : { 7ull, 11ull })
    {
        CAPTURE(seed);
        auto sim = BuyingWorld(seed);
        for (int pass = 0; pass < 300; pass++)
            sim->MaintainWorld(2.0f);
        std::map<int, float> before;
        for (auto& kv : sim->Systems())
            if (kv.second.agg.claimed && kv.second.agg.controller != Fac::Pirates())
            {
                kv.second.agg.presence[kv.second.agg.controller] *= 0.3f;
                before[kv.second.agg.controller.Index()] +=
                    kv.second.agg.presence[kv.second.agg.controller];
            }
        REQUIRE_FALSE(before.empty());
        for (int pass = 0; pass < 2700; pass++)  // an hour and a half of world time
            sim->MaintainWorld(2.0f);
        for (FactionId f : Factions::All())
        {
            if (f == Fac::Pirates() || before.count(f.Index()) == 0)
                continue;
            float ships = 0.0f;
            int   held = 0;
            for (const auto& kv : sim->Systems())
                if (kv.second.agg.claimed && kv.second.agg.controller == f)
                {
                    held++;
                    ships += kv.second.agg.presence[f];
                }
            CAPTURE(Factions::Id(f));
            if (held == 0)
                continue;  // beaten, which is the world's right
            // Holding systems with next to no ships in them is the collapse the bench fails on.
            CHECK(ships >= 0.05f * Factions::TemperamentOf(f).capacity * (float)held);
        }
    }
}
