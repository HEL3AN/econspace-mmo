#include <doctest/doctest.h>

#include <cmath>
#include <map>
#include <memory>
#include <string>

#include "goods_named.h"
#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "core/ShipDesign.h"
#include "entities/NpcShip.h"
#include "gen/Region.h"
#include "sim/ClientSession.h"
#include "sim/FactionMind.h"
#include "sim/Overview.h"
#include "sim/Protocol.h"
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

// A shelf with everything a ship is made of on it, so that what a test watches is the one thing
// it changes (#566): a ship leaves the shelf now, parts and guns, and a bare shelf buys nothing.
void StockShelf(SystemState& st)
{
    for (ResourceType g : AllResourceTypes())
        st.market.SetStock(g, 1.0e5);
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

    StockShelf(*st);
    // No money, no ships.
    sim->MindOf(f).treasury = 0.0;
    CHECK(sim->BuyShips(*st, f, 2.0f) == doctest::Approx(0.0f));

    // Money: as many as it asked for, paid for at the ask, and the guns' goods are gone.
    const double rich = 1e7;
    StockShelf(*st);
    const ResourceType laser = Good::Named("weapons.laser"), rail = Good::Named("weapons.railgun");
    const ResourceType iron = Good::Named("Iron");
    // Guns are module goods now (#566): the yard has them on its shelf, or there are none.
    st->market.SetStock(laser, 20.0);
    st->market.SetStock(rail, 20.0);
    const double guns = st->market.Stock(laser) + st->market.Stock(rail);
    const double hulls = st->market.Stock(iron);
    sim->MindOf(f).treasury = rich;
    st->agg.fleets.clear();
    const float bought = sim->BuyShips(*st, f, 2.0f);
    CHECK(bought == doctest::Approx(2.0f));
    CHECK(sim->MindOf(f).treasury < rich);
    const double after = st->market.Stock(laser) + st->market.Stock(rail);
    CHECK(after < guns);
    CHECK(st->market.Stock(iron) < hulls);  // and the hull's own goods leave the shelf too
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
    CHECK(spent > 500.0);  // a ship is a ship, not a fee
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
    StockShelf(*st);
    // No lasers to be had: they cannot be fitted here.
    st->market.SetStock(Good::Named("weapons.laser"), 0.0);
    st->market.SetStock(Good::Named("weapons.railgun"), 20.0);
    sim->MindOf(f).treasury = 1e7;
    st->agg.fleets.clear();
    st->agg.presence[f] = 0.0f;
    const float bought = sim->BuyShips(*st, f, 0.5f);
    CHECK(bought > 0.0f);  // as much as the market has the railguns' goods for
    const auto& war = st->agg.fleets.at(Simulation::FleetKey(f, NpcRole::Warship));
    CHECK(GunsOf(war, "laser") == doctest::Approx(0.0f));
    CHECK(GunsOf(war, "railgun") == doctest::Approx(bought));

    // Neither family's goods: nothing, and the growth waits for haulers.
    for (const char* g : { "weapons.laser", "weapons.railgun" })
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

namespace
{
// Ships in `fleet` that carry the guns of `family`, of all of them.
float ShareOf(const std::map<std::string, float>& fleet, const char* family)
{
    float all = 0.0f;
    for (const auto& kv : fleet)
        all += kv.second;
    return all > 0.0f ? GunsOf(fleet, family) / all : 0.0f;
}
}  // namespace

TEST_CASE("a holding refits what it flies to the style of its region, and pays for it")
{
    auto         sim = BuyingWorld();
    SystemState* st = LawfulHoldingWithMarket(*sim);
    REQUIRE(st != nullptr);
    const FactionId         f = st->agg.controller;
    const Ships::Catalogue& cat = Archetypes::ShipCatalogue();
    // Lasers' goods in plenty and railguns' gone: the region leans laser.
    for (const char* g : { "Steel", "Ammunition", "weapons.railgun" })
        st->market.SetStock(Good::Named(g), 0.0);
    for (const char* g : { "Crystal", "Circuits", "weapons.laser" })
        st->market.SetStock(Good::Named(g), 5000.0);
    const Ships::Style style = sim->RegionStyle(Gen::SlotOf(st->id));
    REQUIRE(style.LeadingFamily() == "laser");

    // A fleet of railguns, though.
    const std::string warship = cat.Pick(Factions::Id(f), "warship", 0);
    const std::string rail = cat.VariantOf(warship, "railgun", std::string());
    REQUIRE_FALSE(rail.empty());
    const std::string key = Simulation::FleetKey(f, NpcRole::Warship);
    st->agg.presence[f] = 4.0f;
    st->agg.fleets[key] = { { rail, 4.0f } };
    const float railBefore = ShareOf(st->agg.fleets[key], "railgun");
    REQUIRE(railBefore == doctest::Approx(1.0f));

    // No money, no new guns: a hull line that costs nothing may change, a family may not.
    sim->MindOf(f).treasury = 0.0;
    sim->RefitFleets(*st, f, 0.5);
    CHECK(GunsOf(st->agg.fleets[key], "laser") == doctest::Approx(0.0f));

    // Money: some of the fleet is lasers now, the number of ships the same, the treasury paid
    // and the lasers' goods are gone from the market.
    sim->MindOf(f).treasury = 1e9;
    const double crystal = st->market.Stock(Good::Named("weapons.laser"));
    const float  moved = sim->RefitFleets(*st, f, 0.5);
    CHECK(moved > 0.0f);
    const auto& fleet = st->agg.fleets[key];
    float       total = 0.0f;
    for (const auto& kv : fleet)
        total += kv.second;
    CHECK(total == doctest::Approx(4.0f));
    CHECK(GunsOf(fleet, "laser") > 0.0f);
    CHECK(ShareOf(fleet, "railgun") < 1.0f);
    CHECK(sim->MindOf(f).treasury < 1e9);
    CHECK(st->market.Stock(Good::Named("weapons.laser")) < crystal);
}

TEST_CASE("the mix still wrong for the style halves in the rules' half-life of hours")
{
    // The drift of a region's ships (#464, slice 5), measured in world time through the
    // faction period's own rate: refitHalfLifeHours is what the data says and what fleets do.
    auto         sim = BuyingWorld();
    SystemState* st = LawfulHoldingWithMarket(*sim);
    REQUIRE(st != nullptr);
    const FactionId         f = st->agg.controller;
    const Ships::Catalogue& cat = Archetypes::ShipCatalogue();
    for (const char* g : { "Steel", "Ammunition", "weapons.railgun" })
        st->market.SetStock(Good::Named(g), 0.0);
    for (const char* g : { "Crystal", "Circuits", "weapons.laser" })
        st->market.SetStock(Good::Named(g), 1e9);
    const Ships::Style style = sim->RegionStyle(Gen::SlotOf(st->id));
    float              railTarget = 0.0f;
    for (const auto& [family, share] : style.families)
        if (family == "railgun")
            railTarget = share;
    REQUIRE(railTarget < 0.5f);

    const std::string warship = cat.Pick(Factions::Id(f), "warship", 0);
    const std::string key = Simulation::FleetKey(f, NpcRole::Warship);
    st->agg.presence[f] = 5.0f;
    st->agg.fleets[key] = { { cat.VariantOf(warship, "railgun", std::string()), 5.0f } };
    sim->MindOf(f).treasury = 1e12;

    const double halfLife = WorldRules::StyleRules().refitHalfLifeHours;
    const double periodHours = 30 * 2.0 / 3600.0;
    const double rate = 1.0 - std::pow(0.5, periodHours / halfLife);
    const double start = 1.0 - railTarget;  // how far from the style it begins
    double       at = 0.0;
    double       reached = -1.0;
    for (int period = 0; period < (int)(4 * halfLife / periodHours); period++)
    {
        sim->RefitFleets(*st, f, rate);
        at += periodHours;
        const double wrong = ShareOf(st->agg.fleets[key], "railgun") - railTarget;
        if (reached < 0.0 && wrong <= 0.5 * start)
            reached = at;
        sim->MindOf(f).treasury = 1e12;
        for (const char* g : { "Crystal", "Circuits", "weapons.laser" })
            st->market.SetStock(Good::Named(g), 1e9);
    }
    MESSAGE("half-life measured: " << reached << " hours of " << halfLife);
    CAPTURE(reached);
    // About one half-life, within a quarter of it: the lines go by their own shares too.
    CHECK(reached > 0.75 * halfLife);
    CHECK(reached < 1.25 * halfLife);
}

TEST_CASE("pirates buy at a market that deals with them, and nowhere the law is over")
{
    auto         sim = BuyingWorld();
    SystemState* st = LawfulHoldingWithMarket(*sim);
    REQUIRE(st != nullptr);
    const FactionId pirates = Fac::Pirates();
    sim->MindOf(pirates).treasury = 1e9;
    st->agg.fleets.clear();
    // A lawful system's market sells a gang nothing, however rich.
    CHECK(sim->BuyingMarket(st->id, true) == nullptr);
    CHECK(sim->BuyShips(*st, pirates, 1.0f) == doctest::Approx(0.0f));
    // The same market with no law over it does -- and the ships join its pirates.
    StockShelf(*st);
    st->agg.controller = pirates;
    st->agg.claimed = true;
    st->agg.pirates = 3.0f;
    REQUIRE(sim->BuyingMarket(st->id, true) == st);
    const float bought = sim->BuyShips(*st, pirates, 1.0f);
    CHECK(bought > 0.0f);
    CHECK(sim->MindOf(pirates).treasury < 1e9);
    const auto fleet = st->agg.fleets.find(Simulation::FleetKey(pirates, NpcRole::Pirate));
    REQUIRE(fleet != st->agg.fleets.end());
    float sum = 0.0f;
    for (const auto& kv : fleet->second)
        sum += kv.second;
    CHECK(sum == doctest::Approx(3.0f + bought));
}

TEST_CASE("a gang does not breed where it holds: with no market it grows only by what it takes")
{
    auto         sim = BuyingWorld();
    SystemState* st = LawfulHoldingWithMarket(*sim);
    REQUIRE(st != nullptr);
    const FactionId pirates = Fac::Pirates();
    for (auto& kv : sim->Systems())
        kv.second.profile.markets = 0;  // nobody sells anyone anything
    st->agg.controller = pirates;
    st->agg.claimed = true;
    st->agg.presence.Clear();
    st->agg.presence[pirates] = 1.0f;
    sim->MindOf(pirates).treasury = 1e9;
    for (int pass = 0; pass < 30 * 40; pass++)
        sim->MaintainWorld(2.0f);
    // It held what it had, or lost some of it: it never regrew by itself.
    CHECK(st->agg.presence[pirates] <= 1.0f + 1e-3f);
}

TEST_CASE("what the pirates beat in a cold exchange they take a share of, in the victims' family")
{
    auto         sim = BuyingWorld();
    SystemState* st = LawfulHoldingWithMarket(*sim);
    REQUIRE(st != nullptr);
    const Ships::Catalogue& cat = Archetypes::ShipCatalogue();
    const FactionId         law = st->agg.policeFaction;
    const FactionId         pirates = Fac::Pirates();
    // Police that fly lasers.
    const std::string cutter = cat.Pick(Factions::Id(law), "patrol", 0);
    const std::string laser = cat.VariantOf(cutter, "laser", std::string());
    REQUIRE_FALSE(laser.empty());
    st->agg.police = 4.0f;
    st->agg.fleets.clear();
    st->agg.fleets[Simulation::FleetKey(law, NpcRole::Police)] = { { laser, 4.0f } };
    st->agg.pirates = 2.0f;
    st->agg.presence[pirates] = 2.0f;

    const float share = (float)WorldRules::StyleRules().captureShare;
    const float taken = sim->Capture(*st, 3.0f);
    CHECK(taken == doctest::Approx(3.0f * share));
    CHECK(st->agg.pirates == doctest::Approx(2.0f + taken));
    CHECK(st->agg.presence[pirates] == doctest::Approx(2.0f + taken));
    const auto& mine = st->agg.fleets.at(Simulation::FleetKey(pirates, NpcRole::Pirate));
    float       sum = 0.0f;
    for (const auto& kv : mine)
        sum += kv.second;
    CHECK(sum == doctest::Approx(2.0f + taken));
    CHECK(GunsOf(mine, "laser") == doctest::Approx(taken));  // all of what was taken is lasers
    // Nothing destroyed, nothing taken.
    CHECK(sim->Capture(*st, 0.0f) == doctest::Approx(0.0f));
}

TEST_CASE("an instrument says how an NPC is fitted, and a known region says what its NPCs fly")
{
    const Ships::Catalogue& cat = Archetypes::ShipCatalogue();
    REQUIRE(Archetypes::Load(DATA + "archetypes.json"));
    // A stock design says nothing; a variant says its guns and its hull.
    CHECK(Ships::FitWords(cat, "cutter").empty());
    CHECK(Ships::FitWords(cat, "cutter~railgun~heavy") == "railguns, heavy");
    CHECK(Ships::FitWords(cat, "cruiser~laser") == "lasers");
    // An NPC with a design is labelled by it, and one with a name by that.
    Proto::EntitySnapshot e;
    e.kind = Proto::EntityKind::Npc;
    e.role = 2;
    CHECK(Overview::LabelOf(e) == "police");
    e.design = "cutter~railgun~heavy";
    CHECK(Overview::LabelOf(e) == Archetypes::FindDesign(e.design)->name);
    CHECK(Overview::LabelOf(e).find("railguns") != std::string::npos);
    e.name = "Ann";
    CHECK(Overview::LabelOf(e) == "Ann");

    // A style in words: every family and line, then only the leading ones.
    Ships::Style style;
    style.families = { { "laser", 0.8f }, { "railgun", 0.2f } };
    style.lines = { { "light", 0.2f }, { "heavy", 0.5f }, { "brawler", 0.3f } };
    CHECK(Ships::StyleWords(cat, style, false) ==
          "lasers 80%, railguns 20%; heavy hulls 50%, brawler hulls 30%, light hulls 20%");
    CHECK(Ships::StyleWords(cat, style, true) == "lasers 80%; heavy hulls 50%");

    // A pilot is told it for the systems they know, and it travels.
    auto sim = BuyingWorld();
    for (int pass = 0; pass < 90; pass++)
        sim->MaintainWorld(2.0f);
    ClientSession      pilot;
    Proto::GalaxyState state = sim->GalaxyStateFor(pilot, sim->GalaxyStats());
    int                told = 0;
    for (const auto& g : state.systems)
        if (!g.fits.empty())
            told++;
    CHECK(told > 0);
    const Proto::GalaxyState back = [&]
    {
        Proto::GalaxyState out;
        REQUIRE(Proto::DecodeGalaxy(Proto::EncodeGalaxy(state), out));
        return out;
    }();
    REQUIRE(back.systems.size() == state.systems.size());
    for (size_t i = 0; i < state.systems.size(); i++)
        CHECK(back.systems[i].fits == state.systems[i].fits);
}

TEST_CASE(
    "a purchase takes the whole ship off the shelf, and a short shelf means fewer ships (#566)")
{
    auto         sim = BuyingWorld();
    SystemState* st = LawfulHoldingWithMarket(*sim);
    REQUIRE(st != nullptr);
    const FactionId    f = st->agg.controller;
    const ResourceType laser = Good::Named("weapons.laser"), rail = Good::Named("weapons.railgun");
    const std::string  design = Archetypes::ShipCatalogue().Pick(Factions::Id(f), "warship", 0);
    REQUIRE_FALSE(design.empty());
    // Everything the ship is made of, parts and guns, is on the bill.
    const Ships::Cost parts = sim->PartsCost(design);
    StockShelf(*st);
    const Ships::Cost guns = Simulation::GunCost(design);
    REQUIRE_FALSE(parts.empty());
    REQUIRE_FALSE(guns.empty());
    for (const auto& [good, n] : guns)
        CHECK(Goods::Def(good).IsModule());

    // No guns on the shelf, whatever the fortune: no ships.
    sim->MindOf(f).treasury = 1e9;
    st->agg.fleets.clear();
    for (const auto& [good, n] : guns)
        st->market.SetStock(good, 0.0);
    CHECK(sim->BuyShips(*st, f, 1.0f) == doctest::Approx(0.0f));

    // Guns for two ships at a price that is not dear: no more than two, and they are gone.
    for (const auto& [good, n] : guns)
        st->market.SetStock(good, (double)n * 2.0);
    // The market's own target is what a "not dear" shelf is, so find how many it takes.
    double ships = 1e9;
    for (const auto& [good, n] : guns)
        ships = std::min(ships, st->market.Stock(good) / (double)n);
    std::map<int, double> before;
    for (const auto& [good, n] : parts)
        before[good.Index()] = st->market.Stock(good);
    const float bought = sim->BuyShips(*st, f, 100.0f);
    CHECK(bought <= (float)ships + 1e-3f);
    if (bought > 0.0f)
    {
        for (const auto& [good, n] : parts)
            CHECK(st->market.Stock(good) ==
                  doctest::Approx(before[good.Index()] - n * bought).epsilon(1e-3));
        for (const auto& [good, n] : guns)
            CHECK(st->market.Stock(good) == doctest::Approx(n * 2.0 - n * bought).epsilon(1e-3));
    }

    // A ship's guns survive in its wreck at the share, and are the same every time.
    const Inventory a = Simulation::WreckModules(design, 1.0, 7);
    const Inventory b = Simulation::WreckModules(design, 1.0, 7);
    int             units = 0;
    for (const auto& [good, n] : guns)
    {
        CHECK(a.Amount(good) >= n);
        CHECK(a.Amount(good) == b.Amount(good));
        units += n;
    }
    CHECK(Simulation::WreckModules(design, 0.0, 7).Amount(guns.front().first) <= 1);
    CHECK(units > 0);
}
