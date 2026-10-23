#include <doctest/doctest.h>

#include "goods_named.h"
#include "economy/Inventory.h"
#include "sim/InventoryList.h"
#include "sim/Observation.h"

#include <string>

// The inventory window as lists (#472): which containers are reachable from where the pilot
// is, what each holds, and what a move between them can carry -- the numbers the window's
// buttons and the agent's inventory tool both offer.

using InventoryList::Container;
using InventoryList::Preset;

namespace
{
Proto::PlayerView Pilot()
{
    Proto::PlayerView p;
    p.cargoByType.assign((size_t)Goods::Count(), 0);
    p.cargoByType[(size_t)Good::Iron().Index()] = 30;
    p.cargoByType[(size_t)Good::Crystal().Index()] = 4;
    p.cargoUsed = 34;
    p.cargoCap = 50;
    return p;
}
}  // namespace

TEST_CASE("Inventory: goods by type, as every container holds them")
{
    Inventory inv;
    CHECK(inv.Empty());
    inv.Add(Good::Iron(), 10);
    inv.Add(Good::Iron(), 5);
    inv.Add(Good::Ice(), 0);  // nothing
    inv.Add(Good::Ice(), -3);
    CHECK(inv.Amount(Good::Iron()) == 15);
    CHECK(inv.Amount(Good::Ice()) == 0);
    CHECK(inv.Used() == 15);
    CHECK(inv.All().size() == 1);  // none at zero

    CHECK(inv.Remove(Good::Iron(), 4) == 4);
    CHECK(inv.Remove(Good::Iron(), 100) == 11);  // what there was
    CHECK(inv.Remove(Good::Ice(), 1) == 0);
    CHECK(inv.Empty());
}

TEST_CASE("Inventory: in space only the hold is reachable")
{
    const auto all = InventoryList::Reachable(Pilot());
    REQUIRE(all.size() == 1);
    CHECK(all[0].id == "hold");
    CHECK(all[0].Used() == 34);
    CHECK(all[0].Room() == 16);
    CHECK(InventoryList::Find(all, "storage") == nullptr);
}

TEST_CASE("Inventory: docked at one's own structure, its storage is listed after the hold")
{
    Proto::PlayerView p = Pilot();
    p.docked = true;
    p.storageCap = 1000;
    p.storedByType.assign((size_t)Goods::Count(), 0);
    p.storedByType[(size_t)Good::Ice().Index()] = 200;
    const auto all = InventoryList::Reachable(p);
    REQUIRE(all.size() == 2);
    const Container* store = InventoryList::Find(all, "storage");
    REQUIRE(store != nullptr);
    CHECK(store->Used() == 200);
    CHECK(store->Room() == 800);

    // Docked somewhere whose storage is not the pilot's side's: the server sends none.
    p.storageCap = 0;
    CHECK(InventoryList::Reachable(p).size() == 1);
}

TEST_CASE("Inventory: a container's rows are what it holds, priced where there is a market")
{
    const auto         all = InventoryList::Reachable(Pilot());
    std::vector<float> prices((size_t)Goods::Count(), 0.0f);
    prices[(size_t)Good::Iron().Index()] = 10.0f;
    const auto rows = InventoryList::Rows(all[0], prices);
    REQUIRE(rows.size() == 2);  // nothing that is not there
    CHECK(rows[0].type == Good::Iron());
    CHECK(rows[0].amount == 30);
    CHECK(rows[0].value == doctest::Approx(300.0));
    CHECK(rows[1].type == Good::Crystal());
    CHECK(rows[1].price == 0.0f);  // no market for it here

    // In space there are no prices at all.
    CHECK(InventoryList::Rows(all[0], {})[0].value == 0.0);
}

TEST_CASE("Inventory: the filter keeps names that contain it, in any case")
{
    const auto all = InventoryList::Reachable(Pilot());
    const auto rows = InventoryList::Rows(all[0], {}, "CRYS");
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].type == Good::Crystal());
    CHECK(InventoryList::Rows(all[0], {}, "zz").empty());
}

TEST_CASE("Inventory: a move carries no more than wanted, held, or there is room for")
{
    Container hold;
    hold.id = "hold";
    hold.goods.assign((size_t)Goods::Count(), 0);
    hold.goods[(size_t)Good::Iron().Index()] = 30;
    hold.capacity = 50;
    Container store;
    store.id = "storage";
    store.goods.assign((size_t)Goods::Count(), 0);
    store.capacity = 20;

    CHECK(InventoryList::Movable(hold, store, Good::Iron(), 10) == 10);
    CHECK(InventoryList::Movable(hold, store, Good::Iron(), 100) == 20);  // the room
    CHECK(InventoryList::Movable(hold, store, Good::Ice(), 10) == 0);     // none held
    CHECK(InventoryList::Movable(hold, hold, Good::Iron(), 10) == 0);     // nowhere to go
}

TEST_CASE("Inventory: the amount presets")
{
    CHECK(InventoryList::PresetAmount(Preset::One, 30) == 1);
    CHECK(InventoryList::PresetAmount(Preset::Ten, 30) == 10);
    CHECK(InventoryList::PresetAmount(Preset::Ten, 4) == 4);
    CHECK(InventoryList::PresetAmount(Preset::Half, 31) == 15);
    CHECK(InventoryList::PresetAmount(Preset::Half, 1) == 1);
    CHECK(InventoryList::PresetAmount(Preset::All, 31) == 31);
    CHECK(InventoryList::PresetAmount(Preset::All, 0) == 0);
}

TEST_CASE("Inventory: the agent's inventory tool reads the same containers as the window")
{
    Proto::Snapshot snap;
    snap.player = Pilot();
    Obs::View v;
    v.snapshot = &snap;
    std::string text = Obs::DescribeInventory(v);
    CHECK(text.find("INVENTORY in space") != std::string::npos);
    CHECK(text.find("Hold (hold) 34 / 50: Iron 30, Crystal 4") != std::string::npos);
    CHECK(text.find("Storage") == std::string::npos);

    snap.player.docked = true;
    snap.player.storageCap = 100;
    snap.player.storedByType.assign((size_t)Goods::Count(), 0);
    text = Obs::DescribeInventory(v);
    CHECK(text.find("Storage (storage) 0 / 100: empty") != std::string::npos);
    CHECK(text.find("move_goods") != std::string::npos);
}
