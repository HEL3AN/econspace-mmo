// Wire-protocol (Proto) tests: command/snapshot/layout must survive a JSON
// round-trip without losing fields. This used to be econserver selftest.
#include "doctest/doctest.h"

#include "core/Faction.h"
#include "core/World.h"
#include "sim/HostRules.h"
#include "sim/Orders.h"
#include "sim/Protocol.h"
#include "factions_named.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <string>
#include <variant>
#include <vector>

TEST_CASE("command round-trips through JSON")
{
    Proto::Command c;
    c.thrust = true;
    c.turn = -1.0f;
    c.targetId = 42;
    c.navMode = 1;
    c.navTarget = { 123.5f, -67.0f };
    c.navStopDist = 18.0f;

    Proto::Command r;
    REQUIRE(Proto::DecodeCommand(Proto::EncodeCommand(c), r));
    CHECK(r.thrust == c.thrust);
    CHECK(r.turn == c.turn);
    CHECK(r.targetId == 42);
    CHECK(r.navMode == 1);
    CHECK(r.navTarget.x == doctest::Approx(123.5f));
    CHECK(r.navStopDist == doctest::Approx(18.0f));
}

namespace
{
// An action through the wire, or a failure.
bool RoundTrip(const Proto::Action& a, Proto::Action& out)
{
    return Proto::DecodeAction(Proto::EncodeAction(a), out);
}

template <typename T> const T& As(const Proto::Action& a)
{
    REQUIRE(std::holds_alternative<T>(a));
    return std::get<T>(a);
}
}  // namespace

TEST_CASE("every verb round-trips as an action (#354)")
{
    std::vector<Proto::Action> all;
    all.emplace_back(Proto::Act::Dock{});
    all.emplace_back(Proto::Act::Undock{});
    all.emplace_back(Proto::Act::DebugMoney{});
    all.emplace_back(Proto::Act::NameSystem{ "Haven", "r1-1-1" });
    all.emplace_back(Proto::Act::BuyLicence{});
    all.emplace_back(Proto::Act::SellChart{ "r7-1-2" });
    Ships::FitLine line;
    line.module = "laser";
    line.in = "mid";
    line.on = "top";
    line.count = 2;
    all.emplace_back(Proto::Act::Refit{ { line } });
    all.emplace_back(Proto::Act::SaveDesign{ "Scout", { line } });
    all.emplace_back(Proto::Act::ApplyDesign{ "Scout" });
    all.emplace_back(Proto::Act::RenameDesign{ "Scout", "Hauler" });
    all.emplace_back(Proto::Act::DeleteDesign{ "Hauler" });
    all.emplace_back(Proto::Act::SetLivery{ "guild" });
    all.emplace_back(Proto::Act::Move{ "hold", "storage", 1, 3 });
    all.emplace_back(Proto::Act::Sell{ 2, 15, "storage" });
    all.emplace_back(Proto::Act::Buy{ 1, 9, "hold" });
    all.emplace_back(Proto::Act::RentHangar{ 40 });
    all.emplace_back(Proto::Act::RequestAssets{});
    all.emplace_back(Proto::Act::Deliver{ "locked:1", 2, 6 });
    all.emplace_back(Proto::Act::SwitchShip{ 1 });
    all.emplace_back(Proto::Act::BuyShip{ 3 });
    all.emplace_back(Proto::Act::InsureShip{ 2 });
    all.emplace_back(Proto::Act::PayBounty{ "TradersGuild" });
    all.emplace_back(Proto::Act::AcceptOffer{ 2 });
    all.emplace_back(Proto::Act::CompleteMission{ 5 });
    Proto::Act::GiveOrder order;
    order.order.kind = Orders::Kind::Route;
    order.order.destSystem = "reach";
    order.order.waypoints = { "haven" };
    order.order.preference = 1;
    all.emplace_back(order);
    all.emplace_back(Proto::Act::AbortOrder{});
    all.emplace_back(Proto::Act::Jump{ 7 });
    all.emplace_back(Proto::Act::Loot{ 3 });
    all.emplace_back(Proto::Act::Scan{});
    Proto::Act::Deploy deploy;
    deploy.blueprint = "beacon";
    deploy.at = { 100.0f, -200.0f };
    deploy.name = "Waypoint";
    all.emplace_back(deploy);
    all.emplace_back(Proto::Act::Dismantle{ 42 });
    Proto::Act::SetDefence def;
    def.id = 9;
    all.emplace_back(def);
    Proto::Act::Deal deal;
    deal.action = "put";
    deal.with = "bob";
    deal.bundle.goods.push_back({ 1, 12, "hangar" });
    deal.bundle.credits = 500;
    deal.bundle.charts = { "r7-1-2" };
    deal.rev = 4;
    all.emplace_back(deal);
    all.emplace_back(Proto::Act::Lock{ 12 });
    all.emplace_back(Proto::Act::Unlock{ 12 });
    all.emplace_back(Proto::Act::SetActive{ 13 });
    all.emplace_back(Proto::Act::Fire{ true });

    // One of every alternative, so a verb added to the variant and not here fails this.
    REQUIRE(all.size() == std::variant_size_v<Proto::Action>);
    for (size_t i = 0; i < all.size(); ++i)
    {
        CHECK(all[i].index() == i);
        INFO("verb number " << i);
        Proto::Action back;
        REQUIRE(RoundTrip(all[i], back));
        CHECK(back.index() == all[i].index());
    }

    // And the fields that matter come through.
    Proto::Action r;
    REQUIRE(RoundTrip(deal, r));
    CHECK(As<Proto::Act::Deal>(r).action == "put");
    CHECK(As<Proto::Act::Deal>(r).with == "bob");
    REQUIRE(As<Proto::Act::Deal>(r).bundle.goods.size() == 1);
    CHECK(As<Proto::Act::Deal>(r).bundle.goods[0].amount == 12);
    CHECK(As<Proto::Act::Deal>(r).bundle.goods[0].from == "hangar");
    CHECK(As<Proto::Act::Deal>(r).bundle.credits == 500);
    CHECK(As<Proto::Act::Deal>(r).bundle.charts == std::vector<std::string>{ "r7-1-2" });
    CHECK(As<Proto::Act::Deal>(r).rev == 4);
    REQUIRE(RoundTrip(all[3], r));
    CHECK(As<Proto::Act::NameSystem>(r).name == "Haven");
    CHECK(As<Proto::Act::NameSystem>(r).systemId == "r1-1-1");
    REQUIRE(RoundTrip(all[6], r));
    REQUIRE(As<Proto::Act::Refit>(r).lines.size() == 1);
    CHECK(As<Proto::Act::Refit>(r).lines[0].module == "laser");
    CHECK(As<Proto::Act::Refit>(r).lines[0].count == 2);
    REQUIRE(RoundTrip(all[12], r));
    CHECK(As<Proto::Act::Move>(r).from == "hold");
    CHECK(As<Proto::Act::Move>(r).to == "storage");
    CHECK(As<Proto::Act::Move>(r).type == 1);
    CHECK(As<Proto::Act::Move>(r).amount == 3);
    REQUIRE(RoundTrip(all[13], r));
    CHECK(As<Proto::Act::Sell>(r).type == 2);
    CHECK(As<Proto::Act::Sell>(r).amount == 15);
    CHECK(As<Proto::Act::Sell>(r).from == "storage");
    REQUIRE(RoundTrip(all[17], r));
    CHECK(As<Proto::Act::Deliver>(r).from == "locked:1");
    CHECK(As<Proto::Act::Deliver>(r).amount == 6);
    REQUIRE(RoundTrip(all[21], r));
    CHECK(As<Proto::Act::PayBounty>(r).faction == "TradersGuild");
    REQUIRE(RoundTrip(all[22], r));
    CHECK(As<Proto::Act::AcceptOffer>(r).offer == 2);
    REQUIRE(RoundTrip(all[23], r));
    CHECK(As<Proto::Act::CompleteMission>(r).mission == 5);
    REQUIRE(RoundTrip(all[26], r));
    CHECK(As<Proto::Act::Jump>(r).gateId == 7);
    REQUIRE(RoundTrip(all[27], r));
    CHECK(As<Proto::Act::Loot>(r).id == 3);
    REQUIRE(RoundTrip(all[29], r));
    CHECK(As<Proto::Act::Deploy>(r).blueprint == "beacon");
    CHECK(As<Proto::Act::Deploy>(r).at.y == doctest::Approx(-200.0f));
    CHECK(As<Proto::Act::Deploy>(r).name == "Waypoint");
    REQUIRE(RoundTrip(all[30], r));
    CHECK(As<Proto::Act::Dismantle>(r).id == 42);
    REQUIRE(RoundTrip(all[31], r));
    CHECK(As<Proto::Act::SetDefence>(r).id == 9);
    REQUIRE(RoundTrip(all[33], r));
    CHECK(As<Proto::Act::Lock>(r).id == 12);
    REQUIRE(RoundTrip(all[35], r));
    CHECK(As<Proto::Act::SetActive>(r).id == 13);
    REQUIRE(RoundTrip(all[36], r));
    CHECK(As<Proto::Act::Fire>(r).on);

    // It is its own message type, and a command is not an action.
    CHECK(Proto::MessageType(Proto::EncodeAction(all[0])) == "act");
    CHECK_FALSE(Proto::DecodeAction(Proto::EncodeCommand(Proto::Command{}), r));
    Proto::Command notCommand;
    CHECK_FALSE(Proto::DecodeCommand(Proto::EncodeAction(all[0]), notCommand));
}

TEST_CASE("an action a client made up is refused or read as none (#354)")
{
    // A hostile action is an honest one with a field changed: the wire's own key names are
    // found in what EncodeAction wrote, not assumed here.
    auto wire = [](const Proto::Action& a)
    { return nlohmann::json::parse(Proto::EncodeAction(a)); };
    auto decode = [](const nlohmann::json& j, Proto::Action& out)
    { return Proto::DecodeAction(j.dump(), out); };
    // The key the verb is written under: the one whose value is its name.
    auto verbKey = [&](const Proto::Action& a, const char* name)
    {
        const nlohmann::json j = wire(a);
        for (auto it = j.begin(); it != j.end(); ++it)
            if (it.value().is_string() && it.value().get<std::string>() == name)
                return it.key();
        return std::string();
    };
    Proto::Action out;

    SUBCASE("an unknown verb, and a missing one, are refused")
    {
        nlohmann::json    j = wire(Proto::Act::Scan{});
        const std::string key = verbKey(Proto::Act::Scan{}, "scan");
        REQUIRE_FALSE(key.empty());
        REQUIRE(decode(j, out));
        j[key] = "teleport";
        CHECK_FALSE(decode(j, out));
        j[key] = 7;  // a verb is a name
        CHECK_FALSE(decode(j, out));
        j.erase(key);
        CHECK_FALSE(decode(j, out));
    }
    SUBCASE("a field of the wrong type refuses the action instead of throwing")
    {
        nlohmann::json j = wire(Proto::Act::Sell{ 1, 5, "" });
        REQUIRE(decode(j, out));
        j["amt"] = "five";
        CHECK_FALSE(decode(j, out));
        j = wire(Proto::Act::SellChart{ "x" });
        j["what"] = 3;
        CHECK_FALSE(decode(j, out));
        j = wire(Proto::Act::PayBounty{ "TradersGuild" });
        j["faction"] = 3;  // a faction is named, never numbered (#355)
        CHECK_FALSE(decode(j, out));
    }
    SUBCASE("a bundle of the wrong shape refuses the action (#473)")
    {
        Proto::Act::Deal d;
        d.action = "put";
        d.bundle.goods.push_back({ 1, 5, "" });
        nlohmann::json j = wire(d);
        REQUIRE(decode(j, out));
        j["bundle"]["goods"][0][0] = 99;  // no such good
        CHECK_FALSE(decode(j, out));
        j = wire(d);
        j["bundle"]["goods"][0][1] = -3;
        CHECK_FALSE(decode(j, out));
        j = wire(d);
        j["bundle"]["cr"] = "lots";
        CHECK_FALSE(decode(j, out));
        j = wire(d);
        j["bundle"]["cr"] = 1000000000000000LL;  // more than a double holds exactly
        CHECK_FALSE(decode(j, out));
        j = wire(d);
        j["bundle"]["goods"] = nlohmann::json::array();
        for (int i = 0; i < 40; ++i)
            j["bundle"]["goods"].push_back({ 1, 1 });
        CHECK_FALSE(decode(j, out));
    }
    SUBCASE("a resource index past the end of the list is no resource")
    {
        nlohmann::json j = wire(Proto::Act::Sell{ 1, 5, "" });
        j["type"] = 99;
        REQUIRE(decode(j, out));
        CHECK(As<Proto::Act::Sell>(out).type == -1);
        j["type"] = -4;
        REQUIRE(decode(j, out));
        CHECK(As<Proto::Act::Sell>(out).type == -1);
    }
    SUBCASE("text is bounded")
    {
        nlohmann::json j = wire(Proto::Act::SetLivery{ "guild" });
        j["livery"] = std::string(256, 'x');
        CHECK(decode(j, out));
        j["livery"] = std::string(257, 'x');
        CHECK_FALSE(decode(j, out));
        j["livery"] = std::string(100000, 'x');
        CHECK_FALSE(decode(j, out));
    }
    SUBCASE("a number that is not finite is refused, and a point is held inside the system")
    {
        Proto::Act::GiveOrder o;
        o.order.kind = Orders::Kind::MoveTo;
        // 1e400 is a fine double on the wire and an infinity once read: written as text, since
        // a json value cannot hold it.
        std::string text = Proto::EncodeAction(o);
        text.insert(text.size() - 1, ",\"stop\":1e400");
        CHECK_FALSE(Proto::DecodeAction(text, out));
        nlohmann::json j;
        j = wire(Proto::Act::Deploy{ "beacon", { 0.0f, 0.0f }, "" });
        j["at"] = { 1e30, -1e30 };
        REQUIRE(decode(j, out));
        CHECK(std::isfinite(As<Proto::Act::Deploy>(out).at.x));
        CHECK(As<Proto::Act::Deploy>(out).at.x <= 4.0f * World::SYSTEM_RADIUS);
        CHECK(As<Proto::Act::Deploy>(out).at.y >= -4.0f * World::SYSTEM_RADIUS);
        j["at"] = { 1, 2, 3 };
        CHECK_FALSE(decode(j, out));
    }
}

TEST_CASE("snapshot round-trips through JSON")
{
    Proto::Snapshot s;
    s.systemId = "core";
    s.time = 604800.125;  // a week of server time, to the millisecond (#192)
    s.player.hull = 80.0f;
    s.player.maxHull = 100.0f;
    s.player.weaponOn = true;
    s.player.cargoByType = { 1, 2, 3 };
    s.player.warpPhase = 1;
    s.player.warpAlign = 0.9f;
    s.player.warpTarget = { 4000.0f, -2500.0f };
    s.player.warpDrop = 120.0f;
    s.player.autopilot = true;
    s.player.apTarget = { -300.0f, 80.0f };
    s.player.apStop = 60.0f;
    s.player.docked = true;
    s.player.dockedStationId = 77;
    s.player.money = 12345.0;
    s.player.reputation = { 1.0f, -2.0f, 3.0f, 4.0f };
    s.player.bounty = { 0.0, 50.0, 0.0, 0.0 };
    s.player.skillXp = { 10.0f, 20.0f, 30.0f };
    s.tradeAcks.push_back({ 1, 12, 345.5, 400.0 });
    {
        Proto::MissionView mv;
        mv.type = 2;
        mv.faction = Fac::TradersGuild();
        mv.title = "Delivery";
        mv.giverStationId = 11;
        mv.destStationId = 22;
        mv.targetCount = 30;
        mv.rewardMoney = 700.0;
        mv.completable = true;
        s.missionActive.push_back(mv);
        Proto::MissionView off;
        off.type = 0;
        off.title = "Bounty";
        off.targetCount = 3;
        s.missionOffers.push_back(off);
    }
    s.entities.push_back({ 5,
                           Proto::EntityKind::Npc,
                           { 1.0f, 2.0f },
                           0.5f,
                           30.0f,
                           Fac::Pirates(),
                           3,
                           0.75f,
                           -1,
                           "Pirate",
                           "raider" });
    s.fires.push_back({ { 0.0f, 0.0f }, { 1.0f, 2.0f }, Fac::Pirates(), true, false });
    s.fires.push_back({ { 5.0f, 5.0f }, { 9.0f, 9.0f }, Fac::Independent(), false, true });
    s.events.push_back(Ev::Event{ 7, Ev::Kind::Docked, "hello" });
    s.marketPrices = { 10.0f, 20.0f, 30.0f };

    Proto::Snapshot r;
    REQUIRE(Proto::DecodeSnapshot(Proto::EncodeSnapshot(s), r));
    CHECK(r.systemId == "core");
    CHECK(r.time == 604800.125);  // a double on the wire: a float would be 0.06 s coarse
    CHECK(r.player.hull == doctest::Approx(80.0f));
    CHECK(r.player.weaponOn);
    REQUIRE(r.entities.size() == 1);
    CHECK(r.entities[0].id == 5);
    CHECK(r.entities[0].faction == Fac::Pirates());
    CHECK(r.entities[0].design == "raider");  // what the client draws it as (#279)
    REQUIRE(r.fires.size() == 2);
    CHECK(r.fires[0].targetIsPlayer);
    CHECK(r.fires[0].fromPlayer == false);
    CHECK(r.fires[1].fromPlayer);
    CHECK(r.fires[1].targetIsPlayer == false);
    REQUIRE(r.events.size() == 1);
    CHECK(r.events[0].text == "hello");
    CHECK(r.events[0].seq == 7);
    CHECK(r.events[0].kind == Ev::Kind::Docked);
    REQUIRE(r.marketPrices.size() == 3);
    CHECK(r.marketPrices[1] == doctest::Approx(20.0f));
    REQUIRE(r.player.cargoByType.size() == 3);
    CHECK(r.player.cargoByType[2] == 3);
    CHECK(r.player.warpPhase == 1);
    CHECK(r.player.warpAlign == doctest::Approx(0.9f));
    CHECK(r.player.warpTarget.x == doctest::Approx(4000.0f));
    CHECK(r.player.warpDrop == doctest::Approx(120.0f));
    CHECK(r.player.autopilot);
    CHECK(r.player.apTarget.y == doctest::Approx(80.0f));
    CHECK(r.player.apStop == doctest::Approx(60.0f));
    CHECK(r.player.docked);
    CHECK(r.player.dockedStationId == 77);
    REQUIRE(r.tradeAcks.size() == 1);
    CHECK(r.tradeAcks[0].type == 1);
    CHECK(r.tradeAcks[0].sold == 12);
    CHECK(r.tradeAcks[0].gross == doctest::Approx(345.5));
    CHECK(r.tradeAcks[0].revenue == doctest::Approx(400.0));
    CHECK(r.player.money == doctest::Approx(12345.0));
    // One per faction this process knows: a world made by an earlier test adds its natives.
    REQUIRE(r.player.reputation.size() == (size_t)Factions::Count());
    CHECK(r.player.reputation[1] == doctest::Approx(-2.0f));
    REQUIRE(r.player.bounty.size() == (size_t)Factions::Count());
    CHECK(r.player.bounty[1] == doctest::Approx(50.0));
    REQUIRE(r.player.skillXp.size() == 3);
    CHECK(r.player.skillXp[2] == doctest::Approx(30.0f));
    REQUIRE(r.missionActive.size() == 1);
    CHECK(r.missionActive[0].type == 2);
    CHECK(r.missionActive[0].destStationId == 22);
    CHECK(r.missionActive[0].rewardMoney == doctest::Approx(700.0));
    CHECK(r.missionActive[0].completable);
    REQUIRE(r.missionOffers.size() == 1);
    CHECK(r.missionOffers[0].title == "Bounty");
    CHECK(r.missionOffers[0].targetCount == 3);
}

TEST_CASE("galaxy state round-trips through JSON")
{
    Proto::GalaxyState g;
    g.systems.push_back({ "core", 0.85f, 2, 0.7f, Fac::TradersGuild() });
    g.systems.push_back({ "rim", 0.2f, 9, 0.3f, Fac::Pirates() });
    g.events.push_back("Pirates seized Rim");

    Proto::GalaxyState r;
    REQUIRE(Proto::DecodeGalaxy(Proto::EncodeGalaxy(g), r));
    REQUIRE(r.systems.size() == 2);
    CHECK(r.systems[0].id == "core");
    CHECK(r.systems[0].security == doctest::Approx(0.85f));
    CHECK(r.systems[0].controller == Fac::TradersGuild());
    CHECK(r.systems[1].pirates == 9);
    CHECK(r.systems[1].controller == Fac::Pirates());
    REQUIRE(r.events.size() == 1);
    CHECK(r.events[0] == "Pirates seized Rim");
}

TEST_CASE("system layout round-trips through JSON")
{
    Proto::SystemLayout l;
    l.systemId = "core";
    Proto::EntityLayout e;
    e.id = 9;
    e.kind = Proto::EntityKind::Planet;
    e.pos = { 100.0f, -50.0f };
    e.size = 40.0f;
    e.color = { 70, 130, 200, 255 };
    e.name = "Planet";
    e.subType = 4;
    e.orbitRadius = 1500.0f;
    e.resource = 2;
    l.entities.push_back(e);

    Proto::SystemLayout r;
    REQUIRE(Proto::DecodeLayout(Proto::EncodeLayout(l), r));
    CHECK(r.systemId == "core");
    REQUIRE(r.entities.size() == 1);
    CHECK(r.entities[0].id == 9);
    CHECK(r.entities[0].kind == Proto::EntityKind::Planet);
    CHECK(r.entities[0].orbitRadius == doctest::Approx(1500.0f));
    CHECK(r.entities[0].color.b == 200);
    CHECK(r.entities[0].subType == 4);
}

// The version check is what makes permissive per-field decoding safe: without it a
// message from another build would decode into defaults instead of failing.
TEST_CASE("protocol version is stamped and enforced")
{
    Proto::Command c;
    c.seq = 42;
    c.thrust = true;
    std::string wire = Proto::EncodeCommand(c);

    CHECK(Proto::MessageVersion(wire) == Proto::PROTO_VERSION);
    CHECK(Proto::MessageType(wire) == "cmd");

    Proto::Command ok;
    REQUIRE(Proto::DecodeCommand(wire, ok));
    CHECK(ok.seq == 42);

    SUBCASE("a different version is rejected rather than silently defaulted")
    {
        std::string other = wire;
        std::string from = "\"v\":" + std::to_string(Proto::PROTO_VERSION);
        std::string to = "\"v\":" + std::to_string(Proto::PROTO_VERSION + 1);
        size_t      at = other.find(from);
        REQUIRE(at != std::string::npos);
        other.replace(at, from.size(), to);

        Proto::Command bad;
        bad.seq = -1;
        CHECK_FALSE(Proto::DecodeCommand(other, bad));
        CHECK(bad.seq == -1);  // untouched: a rejected message must not write through

        // The type still reads, so a mismatch can be diagnosed instead of looking like junk.
        CHECK(Proto::MessageType(other) == "cmd");
        CHECK(Proto::MessageVersion(other) == Proto::PROTO_VERSION + 1);
    }

    SUBCASE("a message with no version at all is rejected")
    {
        Proto::Snapshot snap;
        CHECK_FALSE(Proto::DecodeSnapshot("{\"t\":\"snap\"}", snap));
        CHECK(Proto::MessageVersion("{\"t\":\"snap\"}") == 0);
    }

    SUBCASE("broken input is still just broken")
    {
        Proto::Command bad;
        CHECK_FALSE(Proto::DecodeCommand("not json", bad));
        CHECK(Proto::MessageVersion("not json") == 0);
        CHECK(Proto::MessageType("not json").empty());
    }
}

// A standing order travels as an intent in an action: the server decides whether it is
// possible. If these fields stop round-tripping, an agent silently loses the ability to give
// orders at all.
TEST_CASE("standing orders round-trip as an action")
{
    Proto::Act::GiveOrder c;
    c.order.kind = Orders::Kind::Route;
    c.order.targetId = 42;
    c.order.point = { -300.0f, 900.0f };
    c.order.stopDist = 250.0f;
    c.order.useWarp = true;
    c.order.untilFull = true;
    c.order.destSystem = "reach";

    Proto::Action got;
    REQUIRE(Proto::DecodeAction(Proto::EncodeAction(c), got));
    const Orders::Order& r = As<Proto::Act::GiveOrder>(got).order;
    CHECK(r.kind == Orders::Kind::Route);
    CHECK(r.targetId == 42);
    CHECK(r.point.y == doctest::Approx(900.0f));
    CHECK(r.stopDist == doctest::Approx(250.0f));
    CHECK(r.useWarp);
    CHECK(r.untilFull);
    CHECK(r.destSystem == "reach");

    SUBCASE("dropping the order is a verb of its own")
    {
        Proto::Action back;
        REQUIRE(Proto::DecodeAction(Proto::EncodeAction(Proto::Act::AbortOrder{}), back));
        CHECK(std::holds_alternative<Proto::Act::AbortOrder>(back));
    }
    SUBCASE("a default order stays sparse")
    {
        Proto::Act::GiveOrder plain;
        plain.order.kind = Orders::Kind::MoveTo;
        const std::string wire = Proto::EncodeAction(plain);
        CHECK(wire.find("stop") == std::string::npos);
        CHECK(wire.find("warp") == std::string::npos);
        CHECK(wire.find("via") == std::string::npos);
    }
}

TEST_CASE("the running order is reported back in the snapshot")
{
    // The journal says what finished; this says what is under way. Without it an agent has
    // issued an order it cannot observe.
    Proto::Snapshot s;
    s.player.orderKind = (int)Orders::Kind::Mine;
    s.player.orderStatus = (int)Orders::Status::Running;
    s.player.orderId = 7;
    s.player.orderDetail = "approaching";

    Proto::Snapshot r;
    REQUIRE(Proto::DecodeSnapshot(Proto::EncodeSnapshot(s), r));
    CHECK(r.player.orderKind == (int)Orders::Kind::Mine);
    CHECK(r.player.orderStatus == (int)Orders::Status::Running);
    CHECK(r.player.orderId == 7);
    CHECK(r.player.orderDetail == "approaching");
}

TEST_CASE("a hello names the account, and a mismatched one is refused")
{
    Proto::Hello h;
    h.account = "alice";
    const std::string wire = Proto::EncodeHello(h);
    CHECK(Proto::MessageType(wire) == "hello");
    CHECK(Proto::MessageVersion(wire) == Proto::PROTO_VERSION);

    Proto::Hello back;
    REQUIRE(Proto::DecodeHello(wire, back));
    CHECK(back.account == "alice");

    // Wrong type and wrong version are both refused rather than read as an empty name:
    // an anonymous session would be one the server cannot save progress for.
    Proto::Command c;
    CHECK_FALSE(Proto::DecodeHello(Proto::EncodeCommand(c), back));
    CHECK_FALSE(Proto::DecodeHello("{\"t\":\"hello\",\"v\":1,\"acct\":\"bob\"}", back));
}

TEST_CASE("a snapshot leaves out what the layout already says")
{
    // #16: the static half of an entity was re-sent thirty times a second despite living
    // in the SystemLayout the client is given on entry. The wire drops it; the receiving
    // side puts it back, so nothing downstream has to know.
    Proto::Snapshot s;
    s.systemId = "core";

    Proto::EntitySnapshot station;  // as the server now builds it
    station.id = 8;
    station.kind = EntityKind::Station;
    station.pos = { 100.0f, 200.0f };
    s.entities.push_back(station);

    Proto::EntitySnapshot npc;  // an NPC is in no layout, so it carries its own facts
    npc.id = 20;
    npc.kind = EntityKind::Npc;
    npc.pos = { 0.0f, 0.0f };
    npc.faction = Fac::Pirates();
    npc.role = 3;
    npc.name = "pirate";
    npc.size = 14.0f;
    s.entities.push_back(npc);

    const std::string wire = Proto::EncodeSnapshot(s);
    CHECK(wire.find("Aurora Hub") == std::string::npos);  // never went out

    Proto::Snapshot back;
    REQUIRE(Proto::DecodeSnapshot(wire, back));
    REQUIRE(back.entities.size() == 2);
    CHECK(back.entities[0].name.empty());  // not on the wire...
    CHECK(back.entities[0].size == doctest::Approx(0.0f));

    std::map<int, Proto::EntityLayout> layout;
    Proto::EntityLayout                el;
    el.id = 8;
    el.kind = EntityKind::Station;
    el.name = "Aurora Hub";
    el.size = 90.0f;
    el.faction = Fac::TradersGuild();
    layout[8] = el;

    Proto::CompleteFromLayout(back, layout);
    CHECK(back.entities[0].name == "Aurora Hub");  // ...and back afterwards
    CHECK(back.entities[0].size == doctest::Approx(90.0f));
    CHECK(back.entities[0].faction == Fac::TradersGuild());

    // An entity the layout does not describe is left exactly as it arrived.
    CHECK(back.entities[1].name == "pirate");
    CHECK(back.entities[1].faction == Fac::Pirates());
    CHECK(back.entities[1].role == 3);
    CHECK(back.entities[1].size == doctest::Approx(14.0f));
}

TEST_CASE("positions are sent to a precision a player could see")
{
    // Rounding a float and letting JSON widen it writes the float's exact value back out:
    // 2615.79 became "2615.7900390625", longer than what it replaced (#16).
    Proto::Snapshot       s;
    Proto::EntitySnapshot e;
    e.id = 1;
    e.kind = EntityKind::Npc;
    e.pos = { 2615.79f, -1234.5678f };
    e.heading = 1.23456f;
    s.entities.push_back(e);

    // Integers in hundredths since #349: both ends hold exactly what was on the wire.
    const std::string wire = Proto::EncodeSnapshot(s);
    CHECK(wire.find("2615.7900390625") == std::string::npos);
    CHECK(wire.find("261579") != std::string::npos);

    Proto::Snapshot back;
    REQUIRE(Proto::DecodeSnapshot(wire, back));
    REQUIRE(back.entities.size() == 1);
    // Close enough that nothing can see the difference, which is the whole claim.
    CHECK(back.entities[0].pos.x == doctest::Approx(2615.79f).epsilon(0.001));
    CHECK(back.entities[0].pos.y == doctest::Approx(-1234.5678f).epsilon(0.001));
    CHECK(back.entities[0].heading == doctest::Approx(1.23456f).epsilon(0.001));
}

TEST_CASE("an object that cannot move is not described again every tick")
{
    // #97: a station is where the layout said it was. Sending that twenty times a second
    // was describing a world that had not changed. The wire carries only what moves; the
    // receiving side rebuilds the rest, so the entity list is the whole system again.
    Proto::Snapshot s;
    s.systemId = "core";

    Proto::EntitySnapshot npc;  // the only thing actually moving here
    npc.id = 20;
    npc.kind = EntityKind::Npc;
    npc.pos = { 500.0f, 0.0f };
    npc.name = "pirate";
    s.entities.push_back(npc);

    const std::string wire = Proto::EncodeSnapshot(s);

    Proto::Snapshot back;
    REQUIRE(Proto::DecodeSnapshot(wire, back));
    REQUIRE(back.entities.size() == 1);  // nothing static went over the wire

    std::map<int, Proto::EntityLayout> layout;
    Proto::EntityLayout                station;
    station.id = 8;
    station.kind = EntityKind::Station;
    station.pos = { 100.0f, 200.0f };
    station.size = 90.0f;
    station.name = "Aurora Hub";
    station.faction = Fac::TradersGuild();
    layout[8] = station;

    Proto::EntityLayout belt;
    belt.id = 13;
    belt.kind = EntityKind::Field;
    belt.pos = { -400.0f, 0.0f };
    belt.name = "Inner Iron Belt";
    belt.resource = 0;
    layout[13] = belt;

    Proto::CompleteFromLayout(back, layout);

    REQUIRE(back.entities.size() == 3);
    auto Find = [&](int id) -> const Proto::EntitySnapshot*
    {
        for (const Proto::EntitySnapshot& e : back.entities)
            if (e.id == id)
                return &e;
        return nullptr;
    };
    const Proto::EntitySnapshot* st = Find(8);
    REQUIRE(st != nullptr);
    CHECK(st->name == "Aurora Hub");
    CHECK(st->pos.x == doctest::Approx(100.0f));
    CHECK(st->size == doctest::Approx(90.0f));
    CHECK(st->faction == Fac::TradersGuild());
    CHECK(Find(13) != nullptr);
    CHECK(Find(13)->ore == 0);   // a belt still says what it holds
    CHECK(Find(20) != nullptr);  // and the NPC is untouched

    SUBCASE("completing twice does not duplicate anything")
    {
        // Snapshots arrive continuously; a rebuild that grew the list each time would be
        // a leak that looked like a busy system.
        Proto::CompleteFromLayout(back, layout);
        CHECK(back.entities.size() == 3);
    }
}

TEST_CASE("the server can end a session and say why")
{
    // Before this the only way to refuse anyone was to drop the socket, which a client
    // cannot tell apart from a crash or a cable (#105).
    Proto::Bye b;
    b.reason = "Signed in from somewhere else";
    const std::string wire = Proto::EncodeBye(b);
    CHECK(Proto::MessageType(wire) == "bye");

    Proto::Bye back;
    REQUIRE(Proto::DecodeBye(wire, back));
    CHECK(back.reason == "Signed in from somewhere else");

    // Not confused with anything else on the wire.
    Proto::Command c;
    CHECK_FALSE(Proto::DecodeBye(Proto::EncodeCommand(c), back));
    // ...but read across a version gap, unlike every other message (#341): a Bye is how a
    // server tells a client of another version that it is one, and the reason has to arrive.
    REQUIRE(Proto::DecodeBye("{\"t\":\"bye\",\"v\":1,\"why\":\"x\"}", back));
    CHECK(back.reason == "x");
}

TEST_CASE("a client of another version is told both versions (#341)")
{
    const std::string why = Host::VersionRefusal(Proto::PROTO_VERSION - 1);
    CHECK(why.find("v" + std::to_string(Proto::PROTO_VERSION)) != std::string::npos);
    CHECK(why.find("v" + std::to_string(Proto::PROTO_VERSION - 1)) != std::string::npos);
}

TEST_CASE("a command a client made up is clamped or refused, never trusted (#341)")
{
    auto decode = [](const std::string& fields, Proto::Command& out)
    {
        return Proto::DecodeCommand("{\"t\":\"cmd\",\"v\":" + std::to_string(Proto::PROTO_VERSION) +
                                        (fields.empty() ? "" : ",") + fields + "}",
                                    out);
    };
    Proto::Command c;

    SUBCASE("a turn is an axis: fifty is a full turn, not fifty of them in one tick")
    {
        REQUIRE(decode("\"turn\":50", c));
        CHECK(c.turn == 1.0f);
        REQUIRE(decode("\"turn\":-50", c));
        CHECK(c.turn == -1.0f);
        REQUIRE(decode("\"turn\":0.25", c));
        CHECK(c.turn == 0.25f);
    }
    SUBCASE("a number too large for a float is clamped, and one that is not finite refused")
    {
        // 1e39 is a finite double and an infinite float; the turn rate became NaN positions
        // broadcast to everyone in the system.
        REQUIRE(decode("\"turn\":1e39,\"nav\":[1e39,0],\"navRange\":-1e39", c));
        CHECK(c.turn == 1.0f);
        CHECK(std::isfinite(c.navTarget.x));
        CHECK(c.navRange == 0.0f);
        CHECK_FALSE(decode("\"turn\":-1e400", c));
    }
    SUBCASE("a point is held inside the reach of a system")
    {
        REQUIRE(decode("\"nav\":[1e30,-1e30],\"navMode\":1", c));
        CHECK(std::isfinite(c.navTarget.x));
        CHECK(c.navTarget.x <= 4.0f * World::SYSTEM_RADIUS);
        CHECK(c.navTarget.y >= -4.0f * World::SYSTEM_RADIUS);
        CHECK_FALSE(decode("\"nav\":[1,2,3]", c));
    }
    SUBCASE("an index past the end of a list is no index")
    {
        REQUIRE(decode("\"navMode\":9", c));
        CHECK(c.navMode == 0);
        REQUIRE(decode("\"target\":-3,\"seq\":99999999999", c));
        CHECK(c.targetId == 0);
        CHECK(c.seq == 0);
    }
    SUBCASE("a field of the wrong type refuses the command instead of throwing")
    {
        // nlohmann's value() throws on a type mismatch, and nothing caught it: one line of
        // JSON ended the server for everyone.
        CHECK_FALSE(decode("\"seq\":\"x\"", c));
        CHECK_FALSE(decode("\"thrust\":1", c));
        CHECK_FALSE(decode("\"turn\":\"fast\"", c));
        CHECK(Proto::MessageVersion("{\"v\":\"21\",\"t\":\"cmd\"}") == 0);
        CHECK(Proto::MessageType("{\"v\":21,\"t\":7}").empty());
        Proto::Hello h;
        CHECK_FALSE(Proto::DecodeHello("{\"v\":[],\"t\":\"hello\",\"acct\":\"x\"}", h));
        REQUIRE(Proto::DecodeHello(
            "{\"v\":" + std::to_string(Proto::PROTO_VERSION) + ",\"t\":\"hello\",\"acct\":5}", h));
        CHECK(h.account.empty());  // not a name, so not a login
    }
    SUBCASE("an honest command still reads exactly")
    {
        Proto::Command sent;
        sent.seq = 41;
        sent.turn = -0.5f;
        sent.navMode = 2;
        sent.navTarget = { 123456.0f, -98765.0f };
        REQUIRE(Proto::DecodeCommand(Proto::EncodeCommand(sent), c));
        CHECK(c.seq == 41);
        CHECK(c.turn == -0.5f);
        CHECK(c.navMode == 2);
        CHECK(c.navTarget.x == 123456.0f);
    }
}

TEST_CASE("the login exchange survives the wire")
{
    // The three messages a login is made of (#106). Each is refused across a version gap
    // for the same reason every other message is: reading one leniently would turn a
    // failed login into a successful-looking one.
    Proto::Challenge c;
    c.nonce = "0123456789abcdef0123456789abcdef";
    c.salt = "fedcba9876543210fedcba9876543210";
    c.isNew = true;

    Proto::Challenge cb;
    REQUIRE(Proto::DecodeChallenge(Proto::EncodeChallenge(c), cb));
    CHECK(cb.nonce == c.nonce);
    CHECK(cb.salt == c.salt);
    CHECK(cb.isNew);

    Proto::Auth a;
    a.proof = "beef";
    a.stored = "cafe";
    Proto::Auth ab;
    REQUIRE(Proto::DecodeAuth(Proto::EncodeAuth(a), ab));
    CHECK(ab.proof == "beef");
    CHECK(ab.stored == "cafe");

    CHECK_FALSE(Proto::DecodeAuth(Proto::EncodeChallenge(c), ab));
    CHECK_FALSE(Proto::DecodeChallenge(Proto::EncodeAuth(a), cb));
    CHECK_FALSE(Proto::DecodeChallenge("{\"t\":\"chal\",\"v\":1}", cb));
}

// #157: a standing hold -- orbit or keep at range -- is an order that does not finish, and
// unlike the autopilot it follows something that moves. Both halves of it cross the wire:
// the order out, and the state back, because the client has to have the state in order to
// replay unacknowledged inputs on top of a snapshot.
TEST_CASE("a standing hold survives the wire, and costs nothing when nobody asked for one")
{
    Proto::Command c;
    c.navMode = 3;  // orbit
    c.navHoldId = 4242;
    c.navRange = 850.0f;

    Proto::Command back;
    REQUIRE(Proto::DecodeCommand(Proto::EncodeCommand(c), back));
    CHECK(back.navMode == 3);
    CHECK(back.navHoldId == 4242);
    CHECK(back.navRange == doctest::Approx(850.0f));

    SUBCASE("an ordinary command does not pay for the fields it is not using")
    {
        // A command goes out sixty times a second, and #16 and #97 were both about exactly
        // this: what a message carries when it has nothing to say.
        const std::string idle = Proto::EncodeCommand(Proto::Command{});
        CHECK(idle.find("navHold") == std::string::npos);
        CHECK(idle.find("navRange") == std::string::npos);
    }

    SUBCASE("the state comes back, or the client cannot replay its own inputs")
    {
        Proto::Snapshot s;
        s.player.holdMode = 2;  // keep at range
        s.player.holdTargetId = 77;
        s.player.holdRange = 1200.0f;

        Proto::Snapshot got;
        REQUIRE(Proto::DecodeSnapshot(Proto::EncodeSnapshot(s), got));
        CHECK(got.player.holdMode == 2);
        CHECK(got.player.holdTargetId == 77);
        CHECK(got.player.holdRange == doctest::Approx(1200.0f));
    }
}

// #171: a route order can ask for the safe way. The server could always plan one, and
// econagent's travel_to_system advertised the option to models -- but nothing on the wire
// carried it, so it was accepted and silently dropped. Since #432 it is a preference, and
// the order names the systems to pass through on its way.
TEST_CASE("a route order's preference and waypoints survive the wire")
{
    Proto::Act::GiveOrder c;
    c.order.kind = Orders::Kind::Route;
    c.order.destSystem = "verge";
    c.order.waypoints = { "reach", "haven" };
    c.order.preference = 2;

    Proto::Action got;
    REQUIRE(Proto::DecodeAction(Proto::EncodeAction(c), got));
    const Orders::Order& back = As<Proto::Act::GiveOrder>(got).order;
    CHECK(back.destSystem == "verge");
    CHECK(back.waypoints == std::vector<std::string>{ "reach", "haven" });
    CHECK(back.preference == 2);

    SUBCASE("and an order that does not ask does not pay for the field")
    {
        Proto::Act::GiveOrder plain;
        plain.order.kind = Orders::Kind::Route;
        plain.order.destSystem = "verge";
        const std::string wire = Proto::EncodeAction(plain);
        CHECK(wire.find("pref") == std::string::npos);
        CHECK(wire.find("via") == std::string::npos);
    }

    SUBCASE("a preference past the last, or too many waypoints, is not an order")
    {
        Proto::Act::GiveOrder odd = c;
        odd.order.preference = 9;
        Proto::Action g;
        REQUIRE(Proto::DecodeAction(Proto::EncodeAction(odd), g));
        // read as the default, like any index out of range
        CHECK(As<Proto::Act::GiveOrder>(g).order.preference == 0);
        Proto::Act::GiveOrder many = c;
        many.order.waypoints.assign(Proto::ROUTE_WAYPOINTS_MAX + 1, "reach");
        CHECK_FALSE(Proto::DecodeAction(Proto::EncodeAction(many), g));
    }
}

// #206: the galaxy index comes from the server, because a generated region (#140) exists
// nowhere else. Everything the client and econagent read from it has to survive the wire.
TEST_CASE("the galaxy index survives the wire, without the server's file names")
{
    WorldLoader::Universe u;
    u.systems.push_back(
        { "sol", "Sol", "sol.json", { 10.5f, -20.0f }, 0.9f, "federation", true, "", "", "", "" });
    // A system on one pilot's charts alone, which a faction had charted first (#431).
    u.systems.push_back(
        { "vega", "Vega", "vega.json", { 300.0f, 40.25f }, 0.1f, "", true, "", "", "", "" });
    u.systems.back().knownBy = "own";
    u.systems.back().chartedBy = "guild";
    u.links.push_back({ "sol", "vega" });
    u.startId = "sol";

    const std::string wire = Proto::EncodeUniverse(u);
    CHECK(Proto::MessageType(wire) == "universe");
    CHECK(Proto::MessageVersion(wire) == Proto::PROTO_VERSION);

    WorldLoader::Universe r;
    REQUIRE(Proto::DecodeUniverse(wire, r));
    REQUIRE(r.systems.size() == 2);
    CHECK(r.systems[0].id == "sol");
    CHECK(r.systems[0].name == "Sol");
    CHECK(r.systems[0].mapPos.x == doctest::Approx(10.5f));
    CHECK(r.systems[0].mapPos.y == doctest::Approx(-20.0f));
    CHECK(r.systems[0].security == doctest::Approx(0.9f));
    CHECK(r.systems[0].owner == "federation");
    CHECK(r.systems[1].id == "vega");
    CHECK(r.systems[1].mapPos.y == doctest::Approx(40.25f));
    CHECK(r.systems[1].owner.empty());
    CHECK(r.systems[0].knownBy.empty());  // the public charts
    CHECK(r.systems[0].chartedBy.empty());
    CHECK(r.systems[1].knownBy == "own");
    CHECK(r.systems[1].chartedBy == "guild");
    REQUIRE(r.links.size() == 1);
    CHECK(r.links[0].a == "sol");
    CHECK(r.links[0].b == "vega");
    CHECK(r.startId == "sol");

    SUBCASE("where the server keeps a system is its own business")
    {
        CHECK(wire.find("sol.json") == std::string::npos);
        CHECK(wire.find("vega.json") == std::string::npos);
        CHECK(r.systems[0].file.empty());
    }

    SUBCASE("a decode replaces the index rather than adding to it")
    {
        REQUIRE(Proto::DecodeUniverse(wire, r));
        CHECK(r.systems.size() == 2);
        CHECK(r.links.size() == 1);
    }

    SUBCASE("refused across a version gap, and not confused with another message")
    {
        std::string       other = wire;
        const std::string from = "\"v\":" + std::to_string(Proto::PROTO_VERSION);
        const size_t      at = other.find(from);
        REQUIRE(at != std::string::npos);
        other.replace(at, from.size(), "\"v\":" + std::to_string(Proto::PROTO_VERSION + 1));
        CHECK_FALSE(Proto::DecodeUniverse(other, r));
        CHECK_FALSE(Proto::DecodeUniverse(Proto::EncodeGalaxy(Proto::GalaxyState{}), r));
        CHECK_FALSE(Proto::DecodeUniverse("not json", r));
    }
}

// #38: the static layer changes while people are in the system, and the change travels as a
// delta rather than as the whole layout again.
TEST_CASE("a layout delta survives the wire")
{
    Proto::LayoutDelta d;
    d.systemId = "core";
    d.rev = 7;
    Proto::EntityLayout depot;
    depot.id = 501;
    depot.kind = EntityKind::Station;
    depot.pos = { 900.0f, -300.0f };
    depot.size = 60.0f;
    depot.name = "Hunter's Depot";
    depot.archetype = "station.trade_hub";
    depot.owner = "hunter";
    d.added.push_back(depot);
    Proto::EntityLayout wreck;
    wreck.id = 44;
    wreck.kind = EntityKind::Derelict;
    wreck.name = "Hauler Wreck";
    wreck.reward = 300.0;
    wreck.looted = true;
    d.changed.push_back(wreck);
    d.removed = { 12, 13 };

    Proto::LayoutDelta r;
    REQUIRE(Proto::DecodeLayoutDelta(Proto::EncodeLayoutDelta(d), r));
    CHECK(Proto::MessageType(Proto::EncodeLayoutDelta(d)) == "ldelta");
    CHECK(r.systemId == "core");
    CHECK(r.rev == 7);
    REQUIRE(r.added.size() == 1);
    CHECK(r.added[0].id == 501);
    CHECK(r.added[0].kind == EntityKind::Station);
    CHECK(r.added[0].owner == "hunter");
    CHECK(r.added[0].archetype == "station.trade_hub");
    CHECK(r.added[0].pos.x == doctest::Approx(900.0f));
    REQUIRE(r.changed.size() == 1);
    CHECK(r.changed[0].looted);
    CHECK(r.changed[0].owner.empty());  // the world's, said by saying nothing
    CHECK(r.removed == std::vector<int>{ 12, 13 });

    SUBCASE("a whole layout carries its revision, and an owner")
    {
        Proto::SystemLayout l;
        l.systemId = "core";
        l.rev = 3;
        l.entities.push_back(depot);
        Proto::SystemLayout back;
        REQUIRE(Proto::DecodeLayout(Proto::EncodeLayout(l), back));
        CHECK(back.rev == 3);
        REQUIRE(back.entities.size() == 1);
        CHECK(back.entities[0].owner == "hunter");
    }

    SUBCASE("it is not mistaken for another message")
    {
        Proto::LayoutDelta x;
        CHECK_FALSE(Proto::DecodeLayoutDelta(Proto::EncodeLayout(Proto::SystemLayout{}), x));
        CHECK_FALSE(Proto::DecodeLayoutDelta("not json", x));
    }
}

TEST_CASE("a client applies a delta once, to the system it is about")
{
    Proto::SystemLayout lay;
    lay.systemId = "core";
    lay.rev = 4;
    Proto::EntityLayout station;
    station.id = 8;
    station.kind = EntityKind::Station;
    station.name = "Aurora Hub";
    lay.entities.push_back(station);
    Proto::EntityLayout wreck;
    wreck.id = 9;
    wreck.kind = EntityKind::Derelict;
    wreck.name = "Hauler Wreck";
    lay.entities.push_back(wreck);

    Proto::LayoutMirror m;
    m.Reset(lay);
    REQUIRE(m.byId.size() == 2);

    Proto::LayoutDelta d;
    d.systemId = "core";
    d.rev = 6;
    Proto::EntityLayout depot;
    depot.id = 20;
    depot.kind = EntityKind::Station;
    depot.name = "Depot";
    d.added.push_back(depot);
    d.removed.push_back(8);
    wreck.looted = true;
    d.changed.push_back(wreck);

    REQUIRE(m.Apply(d));
    CHECK(m.rev == 6);
    CHECK(m.byId.count(8) == 0);
    CHECK(m.byId.count(20) == 1);
    CHECK(m.byId.at(9).looted);

    SUBCASE("a delta the layout already included is old news")
    {
        // A layout built after a change and sent before the delta about it: the delta
        // must not take anything back.
        Proto::LayoutDelta stale = d;
        stale.removed = { 20 };
        CHECK_FALSE(m.Apply(stale));
        CHECK(m.byId.count(20) == 1);
    }

    SUBCASE("a delta about the system just left changes nothing")
    {
        Proto::LayoutDelta other;
        other.systemId = "rim";
        other.rev = 99;
        other.removed = { 9 };
        CHECK_FALSE(m.Apply(other));
        CHECK(m.byId.count(9) == 1);
        CHECK(m.rev == 6);
    }

    SUBCASE("an object added and gone again in between is gone")
    {
        // The server folds both into a removal; a client that had it from a layout loses
        // it, and one that never had it loses nothing.
        Proto::LayoutDelta gone;
        gone.systemId = "core";
        gone.rev = 8;
        gone.removed = { 20, 777 };
        REQUIRE(m.Apply(gone));
        CHECK(m.byId.count(20) == 0);
        CHECK(m.byId.size() == 1);
    }

    SUBCASE("the completed snapshot follows the layout, searched wreck and all")
    {
        Proto::Snapshot s;
        s.systemId = "core";
        Proto::CompleteFromLayout(s, m.byId);
        REQUIRE(s.entities.size() == 2);
        bool sawWreck = false;
        for (const Proto::EntitySnapshot& e : s.entities)
        {
            CHECK(e.id != 8);  // the removed station does not come back
            if (e.id == 9)
            {
                sawWreck = true;
                CHECK(e.name == "Hauler Wreck (searched)");
            }
        }
        CHECK(sawWreck);
    }
}

TEST_CASE("a command that says nothing is a few bytes, and still says it (#341)")
{
    // Sixty a second from every client, and nearly every field at its default.
    Proto::Command idle;
    idle.seq = 1234;
    const std::string wire = Proto::EncodeCommand(idle);
    CHECK(wire.size() < 40);
    Proto::Command back;
    REQUIRE(Proto::DecodeCommand(wire, back));
    CHECK(back.seq == 1234);
    CHECK_FALSE(back.thrust);

    // Seq and a held thrust, all a pilot flying in a straight line sends.
    Proto::Command flying;
    flying.seq = 1234;
    flying.thrust = true;
    CHECK(Proto::EncodeCommand(flying).size() < 80);

    // And everything that is not a default comes through as it was.
    Proto::Command busy;
    busy.thrust = true;
    busy.turn = 0.5f;
    busy.navMode = 1;
    busy.navStopDist = 0.0f;
    busy.navTarget = { 10.0f, 0.0f };
    REQUIRE(Proto::DecodeCommand(Proto::EncodeCommand(busy), back));
    CHECK(back.thrust);
    CHECK(back.turn == 0.5f);
    CHECK(back.navMode == 1);
    CHECK(back.navStopDist == 0.0f);
    CHECK(back.navTarget.x == 10.0f);
}

// Found by the fuzzers (#518): the client-side decoders read their fields with nlohmann's
// value(), which throws on a field of the wrong type. A hostile server, or anyone between a
// client and its server -- there is no transport encryption -- could end the client with one
// line of JSON. A field that is not what it must be reads as its default, like every other
// field.
TEST_CASE("a decoder never throws on a field of the wrong type")
{
    const std::string v = std::to_string(Proto::PROTO_VERSION);

    WorldLoader::Universe u;
    // The input the fuzzer found: `start` is an array, not text.
    CHECK_NOTHROW(
        Proto::DecodeUniverse(R"({"links":[],"start":[],"t":"universe","v":)" + v + "}", u));
    CHECK(u.startId.empty());

    // Wrong types inside a system: each field falls back, the rest is kept.
    REQUIRE(Proto::DecodeUniverse(
        R"({"t":"universe","v":)" + v +
            R"(,"start":"sol","sys":[{"id":"sol","name":7,"pos":["x",2],"sec":"high",)"
            R"("charted":1,"seen":5}]})",
        u));
    REQUIRE(u.systems.size() == 1);
    CHECK(u.systems[0].id == "sol");
    CHECK(u.systems[0].name.empty());
    CHECK(u.systems[0].security == doctest::Approx(0.5f));
    CHECK(u.systems[0].charted);
    CHECK(u.startId == "sol");

    // A number that does not fit the field is clamped, not cast out of range.
    REQUIRE(Proto::DecodeUniverse(R"({"t":"universe","v":)" + v +
                                      R"(,"sys":[{"id":"a","sec":1e300,"pos":[1e300,-1e300]}]})",
                                  u));
    REQUIRE(u.systems.size() == 1);
    CHECK(std::isfinite(u.systems[0].security));
    CHECK(std::isfinite(u.systems[0].mapPos.x));
    CHECK(std::isfinite(u.systems[0].mapPos.y));

    // And the other client decoders, on the same kind of hostile field.
    Proto::SystemLayout lay;
    Proto::Snapshot     snap;
    Proto::GalaxyState  galaxy;
    CHECK_NOTHROW(Proto::DecodeLayout(R"({"t":"layout","v":)" + v +
                                          R"(,"ents":[{"id":"a","pos":"b","size":{}}],"sys":[]})",
                                      lay));
    CHECK_NOTHROW(Proto::DecodeSnapshot(
        R"({"t":"snap","v":)" + v + R"(,"seq":"x","player":[],"ents":{"a":1}})", snap));
    CHECK_NOTHROW(Proto::DecodeGalaxy(R"({"t":"galaxy","v":)" + v + R"(,"time":"now"})", galaxy));
}

TEST_CASE("locks, who locks the pilot and an entity's shield cross the wire (#619)")
{
    Proto::Snapshot snap;
    snap.player.lockMax = 3;
    snap.player.lockRange = 20000.0f;
    snap.player.locks = { { 5, 1.0f, true, true }, { 6, 0.5f, false, false } };
    snap.player.lockedBy = { { 9, true }, { 11, false } };
    Proto::EntitySnapshot e;
    e.id = 5;
    e.kind = EntityKind::PlayerShip;
    e.shieldFrac = 0.25f;
    snap.entities.push_back(e);

    Proto::Snapshot back;
    REQUIRE(Proto::DecodeSnapshot(Proto::EncodeSnapshot(snap), back));
    CHECK(back.player.lockMax == 3);
    CHECK(back.player.lockRange == doctest::Approx(20000.0f));
    REQUIRE(back.player.locks.size() == 2);
    CHECK(back.player.locks[0].id == 5);
    CHECK(back.player.locks[0].held);
    CHECK(back.player.locks[0].active);
    CHECK(back.player.locks[1].progress == doctest::Approx(0.5f));
    CHECK_FALSE(back.player.locks[1].held);
    REQUIRE(back.player.lockedBy.size() == 2);
    CHECK(back.player.lockedBy[0].id == 9);
    CHECK(back.player.lockedBy[0].held);
    CHECK_FALSE(back.player.lockedBy[1].held);
    REQUIRE(back.entities.size() == 1);
    CHECK(back.entities[0].shieldFrac == doctest::Approx(0.25f).epsilon(0.01));
}
