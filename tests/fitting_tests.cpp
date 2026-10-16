#include <doctest/doctest.h>

#include <algorithm>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "core/Fitting.h"
#include "core/ShipDesign.h"
#include "entities/Ship.h"
#include "entities/ShipType.h"
#include "entities/Station.h"
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
        sim.LoadUniverse(std::string(TEST_WORLD_DIR) + "universe.json");
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

// --- Step 2: refit ----------------------------------------------------------------------

namespace
{
// A station in the player's system, with or without a shipyard.
const Station* StationThat(Fixture& f, bool shipyard)
{
    for (const auto& e : f.sim.SystemOf(f.s)->entities)
        if (e->GetKind() == EntityKind::Station && e->GetArchetype() != nullptr &&
            e->GetArchetype()->Has(Component::Shipyard) == shipyard)
            return static_cast<const Station*>(e.get());
    return nullptr;
}

std::vector<Ships::FitLine> Bare(const Ships::Design& d)
{
    std::vector<Ships::FitLine> fit = Ships::Normalized(d.fit);
    for (Ships::FitLine& l : fit)
        l.scale = 0.0f;
    return fit;
}

Ships::FitLine Line(const char* module, const char* in, const char* on, int count)
{
    Ships::FitLine l;
    l.module = module;
    l.in = in;
    l.on = on;
    l.count = count;
    return l;
}
}  // namespace

TEST_CASE("a player's fit changes the fit and nothing else, within the rules")
{
    Fixture                 f;
    const Ships::Catalogue& c = Archetypes::ShipCatalogue();
    const Ships::Design&    scout = *c.FindDesign("scout");
    Ships::Design           out;
    std::string             why;

    SUBCASE("the hull's own fit is the hull's stock design, id and all")
    {
        REQUIRE(Ships::PlayerFit(c, scout, Bare(scout), out, why));
        CHECK(out.id == "scout");
    }
    SUBCASE("a new fit is a design of its own, on the same sections, drawn as the hull draws")
    {
        REQUIRE_MESSAGE(Ships::PlayerFit(c, scout, Bare(ArmedScout()), out, why), why);
        CHECK(Ships::IsPlayerDesign(out.id));
        CHECK(out.frame == scout.frame);
        CHECK(out.bow == scout.bow);
        CHECK(out.stern == scout.stern);
        for (const Ships::FitLine& l : out.fit)
            if (l.module == "hull.engine")
                CHECK(l.scale == doctest::Approx(0.17f));  // the hull's drawing of its drive
        // The same fit in another order is the same design.
        std::vector<Ships::FitLine> shuffled = Bare(ArmedScout());
        std::reverse(shuffled.begin(), shuffled.end());
        Ships::Design again;
        REQUIRE(Ships::PlayerFit(c, scout, shuffled, again, why));
        CHECK(again.id == out.id);
    }
    SUBCASE("a fit sets no scale")
    {
        std::vector<Ships::FitLine> fit = Bare(scout);
        fit.front().scale = 0.5f;
        CHECK_FALSE(Ships::PlayerFit(c, scout, fit, out, why));
        CHECK(why.find("scale") != std::string::npos);
    }
    SUBCASE("a module goes only on a socket its look names")
    {
        std::vector<Ships::FitLine> fit = Bare(scout);
        fit.push_back(Line("weapons.railgun", "mid", "spine", 1));
        CHECK_FALSE(Ships::PlayerFit(c, scout, fit, out, why));
        CHECK(why.find("does not go on") != std::string::npos);
    }
    SUBCASE("sockets run out, pairs stay pairs, and a ship keeps a drive")
    {
        std::vector<Ships::FitLine> fit = Bare(scout);
        fit.push_back(Line("weapons.laser", "bow", "top", 3));
        CHECK_FALSE(Ships::PlayerFit(c, scout, fit, out, why));

        fit = Bare(scout);
        fit.push_back(Line("hull.armour", "bow", "edge", 1));
        CHECK_FALSE(Ships::PlayerFit(c, scout, fit, out, why));

        fit.clear();
        for (const Ships::FitLine& l : Bare(scout))
            if (l.module != "hull.engine" && l.module != "hull.thruster")
                fit.push_back(l);
        CHECK_FALSE(Ships::PlayerFit(c, scout, fit, out, why));
        CHECK(why.find("drive") != std::string::npos);
    }
    SUBCASE("the sockets list says what goes where")
    {
        const std::vector<Ships::SocketGroup> groups = Ships::Sockets(c, scout, scout.fit);
        bool                                  sawBowTop = false;
        for (const Ships::SocketGroup& g : groups)
        {
            if (g.in == "bow" && g.on == "top")
            {
                sawBowTop = true;
                CHECK(g.capacity == 2);
                CHECK(g.used == 2);
                CHECK(std::find(g.modules.begin(), g.modules.end(), "weapons.laser") !=
                      g.modules.end());
                CHECK(std::find(g.modules.begin(), g.modules.end(), "hull.engine") ==
                      g.modules.end());
            }
            for (const std::string& m : g.modules)
                if (c.FindModule(m)->provides.thrust > 0.0f)
                    CHECK(g.in == "stern");
        }
        CHECK(sawBowTop);
    }
}

TEST_CASE("a refit is priced by the modules it adds and takes out")
{
    Fixture                  f;
    const Ships::Catalogue&  c = Archetypes::ShipCatalogue();
    const Ships::Design&     scout = *c.FindDesign("scout");
    const std::vector<float> ore = { 10.0f, 20.0f, 30.0f };  // Iron, Ice, Crystal
    Ships::Design            armed;
    std::string              why;
    REQUIRE(Ships::PlayerFit(c, scout, Bare(ArmedScout()), armed, why));
    // A laser is 4 Iron + 3 Crystal, a tractor 4 Iron + 2 Crystal.
    const double laser = 4 * 10.0 + 3 * 30.0, tractor = 4 * 10.0 + 2 * 30.0;
    CHECK(Ships::RefitPrice(c, scout, armed, ore, 1.5f) ==
          doctest::Approx(laser * 1.5 - tractor * Ships::REFIT_REFUND));
    // And back: the laser comes out at half, the tractor goes in at the markup.
    CHECK(Ships::RefitPrice(c, armed, scout, ore, 1.5f) ==
          doctest::Approx(tractor * 1.5 - laser * Ships::REFIT_REFUND));
    CHECK(Ships::RefitPrice(c, scout, scout, ore, 1.5f) == doctest::Approx(0.0));
}

TEST_CASE("the server refits only at a shipyard, charges for it, and the ship flies the fit")
{
    Fixture f;
    f.sim.MaterializeAllSystems(std::string(TEST_WORLD_DIR) + "systems/");
    const Station* yard = StationThat(f, true);
    REQUIRE_MESSAGE(yard != nullptr, "the start system has a shipyard");
    const int seq = f.s.LastEventSeq();
    f.s.account.SetMoney(5000.0);
    const std::vector<Ships::FitLine> armed = Bare(ArmedScout());

    SUBCASE("in flight it is refused, with a reason")
    {
        CHECK_FALSE(f.sim.RefitShip(f.s, armed));
        const std::vector<Ev::Event> said = f.s.EventsSince(seq);
        REQUIRE_FALSE(said.empty());
        CHECK(said.back().text.find("shipyard") != std::string::npos);
    }
    SUBCASE("at a station without a yard it is refused")
    {
        const Station* plain = StationThat(f, false);
        if (plain != nullptr)
        {
            f.s.dockedStationId = plain->GetId();
            CHECK_FALSE(f.sim.RefitShip(f.s, armed));
            CHECK(f.s.account.GetMoney() == doctest::Approx(5000.0));
        }
    }
    SUBCASE("at a yard it is paid for, filed, flown, and predicted with the same numbers")
    {
        f.s.dockedStationId = yard->GetId();
        const float speedBefore = f.s.ship->GetStats().maxSpeed;
        REQUIRE(f.sim.RefitShip(f.s, armed));
        CHECK(f.sim.TakeDesignsChanged());
        const std::string id = f.s.DesignOf(0);
        CHECK(Ships::IsPlayerDesign(id));
        CHECK(f.sim.Designs().Holds(id));
        CHECK(f.s.ship->GetStats().design == id);
        CHECK(f.s.ship->GetStats().damage > GetShipCatalog()[0].stats.damage);
        CHECK(f.s.ship->GetStats().miningRate < GetShipCatalog()[0].stats.miningRate);
        CHECK(f.s.ship->GetStats().maxSpeed == doctest::Approx(speedBefore));  // same mass
        const double paid = 5000.0 - f.s.account.GetMoney();
        CHECK(paid > 0.0);

        // The client predicts with what a DesignDef makes of it: the same numbers.
        ShipStats predicted;
        REQUIRE(StatsOfDesign(id, predicted));
        CHECK(predicted.maxSpeed == doctest::Approx(f.s.ship->GetStats().maxSpeed));
        CHECK(predicted.thrustPower == doctest::Approx(f.s.ship->GetStats().thrustPower));
        CHECK(predicted.turnSpeed == doctest::Approx(f.s.ship->GetStats().turnSpeed));
        CHECK(predicted.rcsAccel == doctest::Approx(f.s.ship->GetStats().rcsAccel));

        // The same fit again is refused, not charged twice.
        CHECK_FALSE(f.sim.RefitShip(f.s, armed));
        CHECK(5000.0 - f.s.account.GetMoney() == doctest::Approx(paid));

        // Back to stock: the stock design itself.
        const double before = f.s.account.GetMoney();
        REQUIRE(f.sim.RefitShip(f.s, Bare(*Archetypes::ShipCatalogue().FindDesign("scout"))));
        CHECK(f.s.DesignOf(0) == "scout");
        CHECK(f.s.ship->GetStats().design == "scout");
        CHECK(f.s.account.GetMoney() < before);  // the tractor costs more than the laser returns
    }
    SUBCASE("a refit the money does not cover is refused and costs nothing")
    {
        f.s.dockedStationId = yard->GetId();
        f.s.account.SetMoney(1.0);
        CHECK_FALSE(f.sim.RefitShip(f.s, armed));
        CHECK(f.s.account.GetMoney() == doctest::Approx(1.0));
        CHECK(f.s.DesignOf(0) == "scout");
    }
    SUBCASE("a refit travels as a command")
    {
        Proto::Command c;
        c.refit = true;
        c.refitLines = armed;
        Proto::Command back;
        REQUIRE(Proto::DecodeCommand(Proto::EncodeCommand(c), back));
        CHECK(back.refit);
        REQUIRE(back.refitLines.size() == c.refitLines.size());
        CHECK(back.refitLines[0].module == c.refitLines[0].module);
        CHECK(back.refitLines[0].count == c.refitLines[0].count);
        Proto::Command none;
        REQUIRE(Proto::DecodeCommand(Proto::EncodeCommand(Proto::Command{}), none));
        CHECK_FALSE(none.refit);
    }
}

// --- Step 3: the named design library ---------------------------------------------------

namespace
{
bool Said(const ClientSession& s, int since, const std::string& words)
{
    for (const Ev::Event& e : s.EventsSince(since))
        if (e.text.find(words) != std::string::npos)
            return true;
    return false;
}

const Ships::Design& StockOf(int hull)
{
    const Ships::Design* d =
        Archetypes::ShipCatalogue().FindDesign(GetShipCatalog()[(size_t)hull].stats.design);
    REQUIRE(d != nullptr);
    return *d;
}
}  // namespace

TEST_CASE("a design is saved under a name of the player's, and the account keeps the library")
{
    Fixture f;
    f.s.accountName = "librarian";
    const std::vector<Ships::FitLine> armed = Bare(ArmedScout());

    // Saving costs nothing and needs no shipyard: it changes no ship.
    f.s.account.SetMoney(100.0);
    REQUIRE(f.sim.SaveDesign(f.s, "Laser scout", armed));
    REQUIRE(f.sim.SaveDesign(f.s, "Stock", Bare(StockOf(0))));
    CHECK(f.s.account.GetMoney() == doctest::Approx(100.0));
    CHECK(f.s.DesignOf(0) == "scout");
    const std::string id = f.s.designLibrary.at("Laser scout");
    CHECK(Ships::IsPlayerDesign(id));
    CHECK(f.sim.Designs().Holds(id));
    CHECK(f.s.designLibrary.at("Stock") == "scout");  // a stock fit is its stock design
    CHECK(f.sim.TakeDesignsChanged());

    SUBCASE("the account file keeps it, and loads it back")
    {
        const TempDir     tmp;
        const std::string path = tmp.Path("account_library.json");
        REQUIRE(f.sim.SaveAccount(f.s, path));
        ClientSession& back = f.sim.CreateSession(f.sim.Universe().startId, Vector2{ 0.0f, 0.0f },
                                                  GetShipCatalog()[0].stats);
        REQUIRE(f.sim.LoadAccount(back, path) == Save::Result::Ok);
        CHECK(back.designLibrary == f.s.designLibrary);

        // A saved design the server no longer has is dropped, with a word.
        {
            std::ofstream out(path);
            out << R"({"version":5,"ships":[{"hull":0,"design":"scout"}],"ship":0,)"
                   R"("designs":[{"name":"Gone","design":"u:00000000"},)"
                   R"({"name":"Stock","design":"scout"}]})";
        }
        ClientSession& old = f.sim.CreateSession(f.sim.Universe().startId, Vector2{ 0.0f, 0.0f },
                                                 GetShipCatalog()[0].stats);
        const int      seq = old.LastEventSeq();
        REQUIRE(f.sim.LoadAccount(old, path) == Save::Result::Ok);
        CHECK(old.designLibrary.size() == 1);
        CHECK(old.designLibrary.count("Stock") == 1);
        CHECK(Said(old, seq, "Gone"));
    }
    SUBCASE("it reaches the client in the snapshot, after the definitions it names")
    {
        const Proto::Snapshot snap = f.sim.BuildSnapshot(f.s, f.s.systemId);
        REQUIRE(snap.player.library.size() == 2);
        std::set<std::string>               sent;
        const std::vector<Proto::DesignDef> defs = f.sim.DesignDefsFor(snap, sent);
        bool                                defined = false;
        for (const Proto::DesignDef& d : defs)
            defined = defined || d.design.id == id;
        CHECK(defined);

        Proto::Snapshot back;
        REQUIRE(Proto::DecodeSnapshot(Proto::EncodeSnapshot(snap), back));
        REQUIRE(back.player.library.size() == 2);
        CHECK(back.player.library[0].name == snap.player.library[0].name);
        CHECK(back.player.library[0].design == snap.player.library[0].design);
        CHECK(back.player.library[1].design == snap.player.library[1].design);
    }
    SUBCASE("the library's commands travel")
    {
        Proto::Command c;
        c.saveDesign = "Laser scout";
        c.saveLines = armed;
        c.applyDesign = "Stock";
        c.renameDesign = "a";
        c.renameTo = "b";
        c.deleteDesign = "c";
        Proto::Command back;
        REQUIRE(Proto::DecodeCommand(Proto::EncodeCommand(c), back));
        CHECK(back.saveDesign == c.saveDesign);
        CHECK(back.saveLines.size() == armed.size());
        CHECK(back.applyDesign == "Stock");
        CHECK(back.renameDesign == "a");
        CHECK(back.renameTo == "b");
        CHECK(back.deleteDesign == "c");
        CHECK_FALSE(back.refit);  // saving a fit is not refitting to it
    }
    SUBCASE("a fit the rules refuse is not saved")
    {
        std::vector<Ships::FitLine> bad = armed;
        bad.push_back(Line("weapons.railgun", "mid", "spine", 1));
        const int seq = f.s.LastEventSeq();
        CHECK_FALSE(f.sim.SaveDesign(f.s, "Bad", bad));
        CHECK(f.s.designLibrary.count("Bad") == 0);
        CHECK(Said(f.s, seq, "not saved"));
    }
}

TEST_CASE("a saved design is applied at a shipyard for what the same refit costs")
{
    Fixture f;
    f.sim.MaterializeAllSystems(std::string(TEST_WORLD_DIR) + "systems/");
    const Station* yard = StationThat(f, true);
    REQUIRE(yard != nullptr);
    const std::vector<Ships::FitLine> armed = Bare(ArmedScout());
    REQUIRE(f.sim.SaveDesign(f.s, "Laser scout", armed));
    f.s.account.SetMoney(5000.0);

    SUBCASE("in flight it is refused, with the refit's reason")
    {
        const int seq = f.s.LastEventSeq();
        CHECK_FALSE(f.sim.ApplyDesign(f.s, "Laser scout"));
        CHECK(Said(f.s, seq, "shipyard"));
        CHECK(f.s.DesignOf(0) == "scout");
    }
    SUBCASE("at a yard it costs exactly what refitting to the same fit costs")
    {
        // Another pilot, the same hull and standing, refits by hand.
        ClientSession& other =
            f.sim.CreateSession(f.s.systemId, Vector2{ 0.0f, 0.0f }, GetShipCatalog()[0].stats);
        other.account.SetMoney(5000.0);
        other.dockedStationId = yard->GetId();
        REQUIRE(f.sim.RefitShip(other, armed));
        const double byHand = 5000.0 - other.account.GetMoney();
        CHECK(byHand > 0.0);

        f.s.dockedStationId = yard->GetId();
        REQUIRE(f.sim.ApplyDesign(f.s, "Laser scout"));
        CHECK(5000.0 - f.s.account.GetMoney() == doctest::Approx(byHand));
        CHECK(f.s.DesignOf(0) == f.s.designLibrary.at("Laser scout"));
        CHECK(f.s.DesignOf(0) == other.DesignOf(0));  // one design, filed once
        CHECK(f.s.ship->GetStats().design == f.s.DesignOf(0));

        // Applied again, it is the fit the ship has, and costs nothing.
        CHECK_FALSE(f.sim.ApplyDesign(f.s, "Laser scout"));
        CHECK(5000.0 - f.s.account.GetMoney() == doctest::Approx(byHand));
    }
    SUBCASE("a design for another hull, or none at all, is refused")
    {
        f.s.dockedStationId = yard->GetId();
        // A design for the second hull, saved while flying it.
        f.s.ownedShips.push_back(1);
        REQUIRE(f.sim.SwitchShip(f.s, 1));
        REQUIRE(f.sim.SaveDesign(f.s, "Other hull", Bare(StockOf(1))));
        REQUIRE(f.sim.SwitchShip(f.s, 0));
        int seq = f.s.LastEventSeq();
        CHECK_FALSE(f.sim.ApplyDesign(f.s, "Other hull"));
        CHECK(Said(f.s, seq, GetShipCatalog()[1].name));
        seq = f.s.LastEventSeq();
        CHECK_FALSE(f.sim.ApplyDesign(f.s, "Nothing"));
        CHECK(Said(f.s, seq, "no design"));
        CHECK(f.s.account.GetMoney() == doctest::Approx(5000.0));
    }
}

TEST_CASE("the library is bounded, and renames and deletes keep it consistent")
{
    Fixture                           f;
    const std::vector<Ships::FitLine> armed = Bare(ArmedScout());
    std::string                       why;
    CHECK_FALSE(Ships::ValidDesignName("", why));
    CHECK_FALSE(Ships::ValidDesignName(" padded", why));
    CHECK_FALSE(Ships::ValidDesignName(std::string(Ships::DESIGN_NAME_MAX + 1, 'x'), why));
    CHECK_FALSE(Ships::ValidDesignName("tab\there", why));
    CHECK(Ships::ValidDesignName("Miner Mk.2 (twin lasers)", why));
    CHECK_FALSE(f.sim.SaveDesign(f.s, " padded", armed));

    for (int i = 0; i < Ships::LIBRARY_MAX; i++)
        REQUIRE(f.sim.SaveDesign(f.s, "Design " + std::to_string(i), armed));
    const int seq = f.s.LastEventSeq();
    CHECK_FALSE(f.sim.SaveDesign(f.s, "One too many", armed));
    CHECK(Said(f.s, seq, "delete one first"));
    CHECK((int)f.s.designLibrary.size() == Ships::LIBRARY_MAX);
    // Saving over a name it has is not a new entry.
    CHECK(f.sim.SaveDesign(f.s, "Design 0", Bare(StockOf(0))));
    CHECK(f.s.designLibrary.at("Design 0") == "scout");

    CHECK_FALSE(f.sim.RenameDesign(f.s, "Design 1", "Design 2"));  // taken
    CHECK_FALSE(f.sim.RenameDesign(f.s, "Nothing", "Something"));
    REQUIRE(f.sim.RenameDesign(f.s, "Design 1", "Renamed"));
    CHECK(f.s.designLibrary.count("Design 1") == 0);
    CHECK(Ships::IsPlayerDesign(f.s.designLibrary.at("Renamed")));

    REQUIRE(f.sim.DeleteDesign(f.s, "Renamed"));
    CHECK_FALSE(f.sim.DeleteDesign(f.s, "Renamed"));
    CHECK(f.sim.SaveDesign(f.s, "One too many", armed));  // there is room again
}

TEST_CASE("a design a library names outlives every ship that flew it")
{
    Fixture f;
    f.s.accountName = "keeper";
    const TempDir     tmp;
    const std::string account = tmp.Path("account_keeper.json");
    const std::string designs = tmp.Path("designs.json");
    REQUIRE(f.sim.SaveDesign(f.s, "Keep", Bare(ArmedScout())));
    const std::string id = f.s.designLibrary.at("Keep");
    CHECK(f.s.DesignOf(0) == "scout");  // no ship flies it

    // Online: the session's library keeps it through a checkpoint's pruning.
    f.sim.SaveDesigns(designs);
    CHECK(f.sim.Designs().Holds(id));

    // Offline: the account's save names it.
    REQUIRE(f.sim.SaveAccount(f.s, account));
    CHECK(f.sim.Designs().Prune({}) == 0);
    CHECK(f.sim.Designs().Holds(id));

    // Deleted and saved again, nothing names it, and it goes.
    REQUIRE(f.sim.DeleteDesign(f.s, "Keep"));
    REQUIRE(f.sim.SaveAccount(f.s, account));
    CHECK(f.sim.Designs().Prune({}) == 1);
    CHECK_FALSE(f.sim.Designs().Holds(id));
}
