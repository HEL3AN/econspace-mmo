#include <doctest/doctest.h>

#include <fstream>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "core/ShipDesign.h"
#include "entities/Ship.h"
#include "entities/ShipType.h"
#include "sim/ClientSession.h"
#include "sim/DesignStore.h"
#include "sim/Protocol.h"
#include "sim/SaveSchema.h"
#include "sim/Simulation.h"
#include "temp_dir.h"

// A ship flies a design of its own, filed by a hash of its content (#279 fitting, option B):
// the id, the definition on the wire, the store that keeps it and the account that names it.
namespace
{
struct Fixture
{
    Simulation     sim;
    ClientSession& s;

    Fixture() : s(MakeSim()) {}

    ClientSession& MakeSim()
    {
        Factions::Load(std::string(TEST_DATA_DIR) + "factions.json");
        Archetypes::Load(std::string(TEST_DATA_DIR) + "archetypes.json");
        Blueprints::Load(std::string(TEST_DATA_DIR) + "blueprints.json");
        sim.LoadUniverse(std::string(TEST_DATA_DIR) + "universe.json");
        sim.Seed(1234u);
        sim.InitGalaxy();
        return sim.CreateSession(sim.Universe().startId, Vector2{ 0.0f, 0.0f },
                                 GetShipCatalog()[0].stats);
    }
};

// The starter with its tractor swapped for a laser: a fit no stock design has.
Ships::Design ArmedScout()
{
    const Ships::Design* stock = Archetypes::ShipCatalogue().FindDesign("scout");
    REQUIRE(stock != nullptr);
    Ships::Design d = *stock;
    for (Ships::FitLine& l : d.fit)
        if (l.module == "weapons.tractor")
            l.module = "weapons.laser";
    d.name = "Scout";
    return d;
}
}  // namespace

TEST_CASE("a design's id is a hash of what it is, not of what it is called")
{
    Fixture             f;
    const Ships::Design a = ArmedScout();
    const std::string   id = Ships::ContentId(a);
    CHECK(Ships::IsPlayerDesign(id));
    CHECK(id.size() == 10);

    Ships::Design renamed = a;
    renamed.id = "something else";
    renamed.name = "Something Else";
    CHECK(Ships::ContentId(renamed) == id);

    Ships::Design other = a;
    other.fit.back().count = 1;  // one thruster fewer
    CHECK(Ships::ContentId(other) != id);

    // Stock designs keep the names ships.json gives them.
    for (const Ships::HangarEntry& h : Archetypes::ShipCatalogue().hangar)
        CHECK_FALSE(Ships::IsPlayerDesign(h.design));
}

TEST_CASE("a design definition goes over the wire and comes back the same design")
{
    Fixture       f;
    Ships::Design d = ArmedScout();
    d.id = Ships::ContentId(d);

    Proto::DesignDef def;
    def.design = d;
    const std::string msg = Proto::EncodeDesignDef(def);
    CHECK(Proto::MessageType(msg) == "design");

    Proto::DesignDef back;
    REQUIRE(Proto::DecodeDesignDef(msg, back));
    CHECK(back.design.id == d.id);
    CHECK(back.design.frame == d.frame);
    CHECK(back.design.mids == d.mids);
    REQUIRE(back.design.fit.size() == d.fit.size());
    for (size_t i = 0; i < d.fit.size(); i++)
    {
        CHECK(back.design.fit[i].module == d.fit[i].module);
        CHECK(back.design.fit[i].count == d.fit[i].count);
        CHECK(back.design.fit[i].scale == doctest::Approx(d.fit[i].scale));
    }
    CHECK(Ships::ContentId(back.design) == d.id);

    SUBCASE("a definition whose id is not its hash is refused")
    {
        Proto::DesignDef lie = def;
        lie.design.fit.front().count = 2;  // what it says it is no longer
        Proto::DesignDef out;
        CHECK_FALSE(Proto::DecodeDesignDef(Proto::EncodeDesignDef(lie), out));
    }
    SUBCASE("a stock design is never redefined from the wire")
    {
        Proto::DesignDef stock;
        stock.design = *Archetypes::ShipCatalogue().FindDesign("scout");
        Proto::DesignDef out;
        CHECK_FALSE(Proto::DecodeDesignDef(Proto::EncodeDesignDef(stock), out));
    }
    SUBCASE("a client told of it draws and derives it")
    {
        std::string why;
        REQUIRE(Archetypes::AddDesign(back.design, why));
        const Archetype* look = Archetypes::ForDesign(d.id);
        REQUIRE(look != nullptr);
        CHECK(look->design == d.id);
        ShipStats stats;
        REQUIRE(StatsOfDesign(d.id, stats));
        CHECK(stats.damage > GetShipCatalog()[0].stats.damage);          // the laser
        CHECK(stats.miningRate < GetShipCatalog()[0].stats.miningRate);  // no tractor
        CHECK(HullOf(d.id) == 0);
    }
}

TEST_CASE("the store files a design once, under its hash, and keeps it while it is named")
{
    Fixture           f;
    DesignStore&      store = f.sim.Designs();
    std::string       why;
    const std::string id = store.File(ArmedScout(), why);
    REQUIRE_MESSAGE(!id.empty(), why);
    CHECK(id == Ships::ContentId(ArmedScout()));
    CHECK(store.File(ArmedScout(), why) == id);  // the same design, the same id
    CHECK(store.Size() == 1);
    REQUIRE(store.Find(id) != nullptr);
    CHECK(store.Find("scout") != nullptr);  // stock designs are found, never filed
    CHECK_FALSE(store.Holds("scout"));

    SUBCASE("a design that does not validate is not filed")
    {
        Ships::Design bad = ArmedScout();
        bad.fit.front().count = 9;
        CHECK(store.File(bad, why).empty());
        CHECK_FALSE(why.empty());
        CHECK(store.Size() == 1);
    }
    SUBCASE("a design nobody names and nothing flies is pruned")
    {
        store.SetUsers("alice", { id });
        CHECK(store.Prune({}) == 0);
        store.SetUsers("alice", {});
        CHECK(store.Prune({ id }) == 0);  // still flown by somebody online
        CHECK(store.Prune({}) == 1);
        CHECK(store.Find(id) == nullptr);
    }
    SUBCASE("designs.json keeps them, and their users, across a restart")
    {
        store.SetUsers("alice", { id });
        const TempDir     tmp;
        const std::string path = tmp.Path("designs.json");
        store.Save(path);
        DesignStore back;
        REQUIRE(back.Load(path) == Save::Result::Ok);
        CHECK(back.Holds(id));
        CHECK(back.Prune({}) == 0);  // alice still names it

        // From a later build: refused, so it is never written back without what it held.
        nlohmann::json j;
        {
            std::ifstream in(path);
            in >> j;
        }
        j["version"] = Save::DESIGNS_VERSION + 1;
        {
            std::ofstream out(path);
            out << j.dump();
        }
        DesignStore later;
        CHECK(later.Load(path) == Save::Result::TooNew);
        CHECK(DesignStore().Load(tmp.Path("no_such_designs.json")) == Save::Result::Missing);
    }
}

TEST_CASE("a design is defined to a client once, and before the snapshot that names it")
{
    Fixture           f;
    std::string       why;
    const std::string id = f.sim.Designs().File(ArmedScout(), why);
    REQUIRE(!id.empty());

    // A stock ship needs no definition: both sides read ships.json.
    std::set<std::string> sent;
    Proto::Snapshot       stock = f.sim.BuildSnapshot(f.s, f.s.systemId);
    CHECK(f.sim.DesignDefsFor(stock, sent).empty());

    f.sim.FlyDesign(f.s, 0, id);
    CHECK(f.s.ship->GetStats().design == id);
    Proto::Snapshot snap = f.sim.BuildSnapshot(f.s, f.s.systemId);
    REQUIRE(snap.player.shipDesigns.size() == 1);
    CHECK(snap.player.shipDesigns[0] == id);

    const std::vector<Proto::DesignDef> first = f.sim.DesignDefsFor(snap, sent);
    REQUIRE(first.size() == 1);
    CHECK(first[0].design.id == id);
    CHECK(f.sim.DesignDefsFor(snap, sent).empty());  // once per connection

    // Another player in the same system sees the ship, and is told what it is first.
    ClientSession& other =
        f.sim.CreateSession(f.s.systemId, Vector2{ 50.0f, 0.0f }, GetShipCatalog()[0].stats);
    Proto::Snapshot              seen = f.sim.BuildSnapshot(other, other.systemId);
    const Proto::EntitySnapshot* ship = nullptr;
    for (const Proto::EntitySnapshot& e : seen.entities)
        if (e.id == f.s.ship->GetId())
            ship = &e;
    REQUIRE(ship != nullptr);
    CHECK(ship->design == id);
    std::set<std::string> otherSent;
    CHECK(f.sim.DesignDefsFor(seen, otherSent).size() == 1);
}

TEST_CASE("an account's ships are designs, and older accounts load as stock ones")
{
    Fixture f;
    f.s.accountName = "fitter";
    const TempDir     tmp;
    const std::string path = tmp.Path("account_fitting.json");

    // Version 3, and version 4 from the faction registry (#355), both list hull indices.
    auto older = [&](const char* text)
    {
        {
            std::ofstream out(path);
            out << text;
        }
        ClientSession& old = f.sim.CreateSession(f.sim.Universe().startId, Vector2{ 0.0f, 0.0f },
                                                 GetShipCatalog()[0].stats);
        REQUIRE(f.sim.LoadAccount(old, path) == Save::Result::Ok);
        CHECK(old.Owns(0));
        CHECK(old.Owns(1));
        CHECK(old.currentShip == 1);
        CHECK(old.DesignOf(0) == GetShipCatalog()[0].stats.design);
        CHECK(old.DesignOf(1) == GetShipCatalog()[1].stats.design);
        CHECK(old.ship->GetStats().design == GetShipCatalog()[1].stats.design);

        // Written back as the current version: each ship with the design it flies.
        f.sim.SaveAccount(old, path);
        nlohmann::json j;
        {
            std::ifstream in(path);
            in >> j;
        }
        CHECK(j["version"] == Save::ACCOUNT_VERSION);
        REQUIRE(j["ships"].size() == 2);
        CHECK(j["ships"][1]["hull"] == 1);
        CHECK(j["ships"][1]["design"] == GetShipCatalog()[1].stats.design);
    };
    SUBCASE("a version 3 account's hull indices are its hulls' stock designs")
    {
        older(R"({"version":3,"money":900.0,"ships":[0,1],"ship":1})");
    }
    SUBCASE("so are a version 4 account's")
    {
        older(R"({"version":4,"money":900.0,"ships":[0,1],"ship":1})");
    }

    SUBCASE("a refitted ship comes back refitted, and its design is kept for it")
    {
        std::string       why;
        const std::string id = f.sim.Designs().File(ArmedScout(), why);
        REQUIRE(!id.empty());
        f.sim.FlyDesign(f.s, 0, id);
        f.sim.SaveAccount(f.s, path);
        CHECK(f.sim.Designs().Prune({}) == 0);  // the save names it

        ClientSession& back = f.sim.CreateSession(f.sim.Universe().startId, Vector2{ 0.0f, 0.0f },
                                                  GetShipCatalog()[0].stats);
        REQUIRE(f.sim.LoadAccount(back, path) == Save::Result::Ok);
        CHECK(back.DesignOf(0) == id);
        CHECK(back.ship->GetStats().design == id);
        CHECK(back.ship->GetStats().damage > GetShipCatalog()[0].stats.damage);
    }

    SUBCASE("a design the server no longer has puts the ship back to stock, and says so")
    {
        {
            std::ofstream out(path);
            out << R"({"version":5,"ships":[{"hull":0,"design":"u:00000000"}],"ship":0})";
        }
        ClientSession& back = f.sim.CreateSession(f.sim.Universe().startId, Vector2{ 0.0f, 0.0f },
                                                  GetShipCatalog()[0].stats);
        REQUIRE(f.sim.LoadAccount(back, path) == Save::Result::Ok);
        CHECK(back.DesignOf(0) == GetShipCatalog()[0].stats.design);
        CHECK(back.ship->GetStats().design == GetShipCatalog()[0].stats.design);
        bool told = false;
        for (const Ev::Event& e : back.EventsSince(0))
            told = told || e.text.find("stock fit") != std::string::npos;
        CHECK(told);
    }
}
