#include <doctest/doctest.h>

#include "core/StationList.h"

#include <vector>

// What the station's market and hangar windows list (#297), decided without a window.

TEST_CASE("StationList: the market lists every commodity, and what selling it pays this pilot")
{
    const std::vector<float> prices = { 10.0f, 20.0f, 50.0f };
    const std::vector<int>   cargo = { 7, 0, 2 };

    const std::vector<StationList::MarketRow> rows =
        StationList::Market(prices, cargo, 1.0f, RepTier::Neutral);
    REQUIRE(rows.size() == AllResourceTypes().size());
    CHECK(rows[0].type == ResourceType::Iron);
    CHECK(rows[0].name == ResourceName(ResourceType::Iron));
    CHECK(rows[0].hold == 7);
    CHECK(rows[0].perUnit == doctest::Approx(10.0));
    CHECK(rows[0].worth == doctest::Approx(70.0));
    CHECK(rows[1].hold == 0);
    CHECK(rows[1].worth == doctest::Approx(0.0));
    CHECK(rows[2].worth == doctest::Approx(100.0));

    // The server's sum: price x skill x standing. A liked pilot with some trading gets more.
    const std::vector<StationList::MarketRow> liked =
        StationList::Market(prices, cargo, 1.1f, RepTier::Liked);
    CHECK(liked[0].perUnit ==
          doctest::Approx(10.0 * 1.1 * SellPriceMultiplier(RepTier::Liked)).epsilon(1e-4));
    CHECK(liked[0].perUnit > rows[0].perUnit);

    // Before the first snapshot the lists are empty: every row is there, at nothing.
    const std::vector<StationList::MarketRow> none =
        StationList::Market({}, {}, 1.0f, RepTier::Neutral);
    REQUIRE(none.size() == rows.size());
    CHECK(none[2].price == 0.0f);
    CHECK(none[2].hold == 0);
}

TEST_CASE(
    "StationList: the hangar says which hull is flown, which are owned, and what the rest cost")
{
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
