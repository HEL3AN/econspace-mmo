#include <doctest/doctest.h>

#include "goods_named.h"
#include "core/StationList.h"

#include "core/Archetype.h"
#include <algorithm>
#include <string>
#include <vector>

// What the station's market and hangar windows list (#297), decided without a window.

TEST_CASE("StationList: the market lists every good, what selling pays and what buying costs")
{
    StationList::MarketInput in;
    in.prices = { 10.0f, 20.0f, 50.0f };
    in.asks = { 11.0f, 22.0f, 55.0f };
    in.stock = { 400, 0, 30 };
    in.cargo = { 7, 0, 2 };
    in.money = 1000.0;
    in.holdFree = 200;  // of volume: Iron takes 10 a unit (#472)
    const double fee = Goods::Rules().fee;

    const std::vector<StationList::MarketRow> rows = StationList::Market(in);
    // Every good a market may stock, and no module that is not on its shelf (#566).
    size_t ordinary = 0;
    for (ResourceType g : AllResourceTypes())
        ordinary += Goods::Def(g).IsModule() ? 0 : 1;
    REQUIRE(rows.size() == ordinary);
    CHECK(rows[0].type == Good::Iron());
    CHECK(rows[0].name == ResourceName(Good::Iron()));
    CHECK(rows[0].hold == 7);
    CHECK(rows[0].stock == 400);
    // The server's sum: the bid, less the fee everybody pays (#348).
    CHECK(rows[0].perUnit == doctest::Approx(10.0 * (1.0 - fee)));
    CHECK(rows[0].worth == doctest::Approx(70.0 * (1.0 - fee)));
    CHECK(rows[1].hold == 0);
    CHECK(rows[1].worth == doctest::Approx(0.0));
    // Buying: the ask and the fee; as many as the hold takes, the stock has and the money pays.
    CHECK(rows[0].buyUnit == doctest::Approx(11.0 * (1.0 + fee)));
    CHECK(rows[0].canBuy == 20);                                    // the hold
    CHECK(rows[1].canBuy == 0);                                     // none in stock
    CHECK(rows[2].canBuy == (int)(1000.0 / (55.0 * (1.0 + fee))));  // the money

    // A liked pilot with some trading gets more for a sale -- never more than the ask.
    in.tradeBonus = 1.05f;
    in.standing = RepTier::Liked;
    const std::vector<StationList::MarketRow> liked = StationList::Market(in);
    CHECK(liked[0].perUnit ==
          doctest::Approx(std::min(10.0 * 1.05 * SellPriceMultiplier(RepTier::Liked), 11.0) *
                          (1.0 - fee))
              .epsilon(1e-4));
    CHECK(liked[0].perUnit > rows[0].perUnit);
    CHECK(liked[0].perUnit < liked[0].buyUnit);  // a round trip at one market loses
    // ...and a hated one pays more to buy.
    in.standing = RepTier::Hated;
    CHECK(StationList::Market(in)[0].buyUnit > rows[0].buyUnit);

    // Before the first snapshot the lists are empty: every row is there, at nothing.
    const std::vector<StationList::MarketRow> none = StationList::Market({});
    REQUIRE(none.size() == rows.size());
    for (const StationList::MarketRow& r : none)
        CHECK_FALSE(Goods::Def(r.type).IsModule());
    CHECK(none[2].price == 0.0f);
    CHECK(none[2].hold == 0);
    CHECK(none[2].canBuy == 0);
}

TEST_CASE(
    "StationList: the hangar says which hull is flown, which are owned, and what the rest cost")
{
    // The hangar is data/ships.json's (#279 step 5), read with the archetypes.
    REQUIRE(Archetypes::Load(std::string(TEST_DATA_DIR) + "archetypes.json"));
    const std::vector<ShipType>& catalog = GetShipCatalog();
    REQUIRE(catalog.size() >= 3);

    // Flying the second hull, owning the third, with little money and a full small hold.
    const int  cargoUsed = catalog[1].stats.cargoCapacity;
    const auto all = StationList::Hangar(catalog, { 1, 2 }, 1, 100.0, cargoUsed, RepTier::Neutral);
    // One row for each ship owned -- the starter always is -- then one for each hull on sale
    // (#572: an account may own several of a hull, so what is owned is not taken off the list).
    REQUIRE(all.size() == 3 + catalog.size());
    CHECK(all[0].berth == StationList::Berth::Owned);  // the starter is always owned
    CHECK(all[0].key == 0);
    CHECK(all[1].berth == StationList::Berth::Current);
    CHECK(all[1].key == 1);
    CHECK(all[2].berth == StationList::Berth::Owned);
    CHECK(all[2].affordable);  // owned: switching is free
    const std::vector<StationList::HangarRow> rows(all.begin(), all.begin() + 3);
    for (size_t i = 3; i < all.size(); i++)
    {
        const size_t hull = i - 3;
        CHECK(all[i].berth == StationList::Berth::ForSale);
        CHECK(all[i].key == -1);
        CHECK(all[i].index == (int)hull);
        CHECK(all[i].price == doctest::Approx(catalog[hull].price));
        CHECK(all[i].affordable == (100.0 >= all[i].price));  // the starter costs nothing
    }
    CHECK(rows[1].index == 1);
    CHECK(rows[1].name == catalog[1].name);
    CHECK(rows[1].cargo == catalog[1].stats.cargoCapacity);
    // Every number the window shows is the hull's, read off its design: one source.
    CHECK(rows[1].speed == doctest::Approx(catalog[1].stats.maxSpeed));
    CHECK(rows[1].turn == doctest::Approx(catalog[1].stats.turnSpeed));
    CHECK(rows[1].hull == doctest::Approx(catalog[1].stats.hull));
    CHECK(rows[1].damage == doctest::Approx(catalog[1].stats.damage));

    // A hold smaller than what is carried is said before the server refuses it (#219).
    for (const StationList::HangarRow& r : all)
        CHECK(r.holdFits == (r.cargo >= cargoUsed));

    // Standing prices the hull, by the same table the server charges by (#109).
    const auto   hated = StationList::Hangar(catalog, {}, 0, 1e9, 0, RepTier::Hated);
    const auto   allied = StationList::Hangar(catalog, {}, 0, 1e9, 0, RepTier::Allied);
    const size_t last = hated.size() - 1;  // the last hull on sale
    const int    hull = (int)catalog.size() - 1;
    CHECK(hated[last].price ==
          doctest::Approx(catalog[hull].price * ShipPriceMultiplier(RepTier::Hated)));
    CHECK(allied[last].price < hated[last].price);
    CHECK(allied[last].affordable);
}

TEST_CASE("StationList: a module is a row only where the shelf or the hold has one (#566)")
{
    ResourceType laser;
    REQUIRE(ModuleGood("weapons.laser", laser));
    StationList::MarketInput in;
    const size_t             n = (size_t)Goods::Count();
    in.prices.assign(n, 10.0f);
    in.asks.assign(n, 11.0f);
    in.stock.assign(n, 5);
    in.cargo.assign(n, 0);
    in.stock[(size_t)laser.Index()] = 0;
    auto has = [&](const std::vector<StationList::MarketRow>& rows)
    {
        for (const StationList::MarketRow& r : rows)
            if (r.type == laser)
                return true;
        return false;
    };
    CHECK_FALSE(has(StationList::Market(in)));
    in.stock[(size_t)laser.Index()] = 2;
    CHECK(has(StationList::Market(in)));
    in.stock[(size_t)laser.Index()] = 0;
    in.cargo[(size_t)laser.Index()] = 1;  // one aboard: it can be sold here
    CHECK(has(StationList::Market(in)));
}
