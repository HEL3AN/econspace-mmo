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
    in.holdFree = 20;
    const double fee = Goods::Rules().fee;

    const std::vector<StationList::MarketRow> rows = StationList::Market(in);
    REQUIRE(rows.size() == AllResourceTypes().size());
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
    const auto rows = StationList::Hangar(catalog, { 1, 2 }, 1, 100.0, cargoUsed, RepTier::Neutral);
    REQUIRE(rows.size() == catalog.size());

    CHECK(rows[0].berth == StationList::Berth::Owned);  // the starter is always owned
    CHECK(rows[1].berth == StationList::Berth::Current);
    CHECK(rows[2].berth == StationList::Berth::Owned);
    CHECK(rows[2].affordable);  // owned: switching is free
    for (size_t i = 3; i < rows.size(); i++)
    {
        CHECK(rows[i].berth == StationList::Berth::ForSale);
        CHECK(rows[i].price == doctest::Approx(catalog[i].price));
        CHECK_FALSE(rows[i].affordable);
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
    for (const StationList::HangarRow& r : rows)
        CHECK(r.holdFits == (r.cargo >= cargoUsed));

    // Standing prices the hull, by the same table the server charges by (#109).
    const auto hated = StationList::Hangar(catalog, {}, 0, 1e9, 0, RepTier::Hated);
    const auto allied = StationList::Hangar(catalog, {}, 0, 1e9, 0, RepTier::Allied);
    const int  last = (int)catalog.size() - 1;
    CHECK(hated[last].price ==
          doctest::Approx(catalog[last].price * ShipPriceMultiplier(RepTier::Hated)));
    CHECK(allied[last].price < hated[last].price);
    CHECK(allied[last].affordable);
}
