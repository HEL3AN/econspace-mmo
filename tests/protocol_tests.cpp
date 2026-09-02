// Wire-protocol (Proto) tests: command/snapshot/layout must survive a JSON
// round-trip without losing fields. This used to be econserver selftest.
#include "doctest/doctest.h"

#include "core/Faction.h"
#include "sim/Protocol.h"

TEST_CASE("command round-trips through JSON")
{
    Proto::Command c;
    c.thrust = true;
    c.turn = -1.0f;
    c.toggleWeapon = true;
    c.targetId = 42;
    c.navMode = 1;
    c.navTarget = { 123.5f, -67.0f };
    c.navStopDist = 18.0f;
    c.jumpGateId = 7;
    c.lootId = 3;
    c.sellType = 2;
    c.sellAmount = 15;
    c.refitShip = 4;
    c.buyShip = 3;
    c.payBountyFaction = 1;
    c.debugMoney = true;
    c.acceptOffer = 2;
    c.completeMission = 5;

    Proto::Command r;
    REQUIRE(Proto::DecodeCommand(Proto::EncodeCommand(c), r));
    CHECK(r.thrust == c.thrust);
    CHECK(r.turn == c.turn);
    CHECK(r.toggleWeapon);
    CHECK(r.targetId == 42);
    CHECK(r.navMode == 1);
    CHECK(r.navTarget.x == doctest::Approx(123.5f));
    CHECK(r.navStopDist == doctest::Approx(18.0f));
    CHECK(r.jumpGateId == 7);
    CHECK(r.lootId == 3);
    CHECK(r.sellType == 2);
    CHECK(r.sellAmount == 15);
    CHECK(r.refitShip == 4);
    CHECK(r.buyShip == 3);
    CHECK(r.payBountyFaction == 1);
    CHECK(r.debugMoney);
    CHECK(r.acceptOffer == 2);
    CHECK(r.completeMission == 5);
}

TEST_CASE("snapshot round-trips through JSON")
{
    Proto::Snapshot s;
    s.systemId = "core";
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
        mv.faction = 1;
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
                           FactionId::Pirates,
                           3,
                           0.75f,
                           -1,
                           "Pirate" });
    s.fires.push_back({ { 0.0f, 0.0f }, { 1.0f, 2.0f }, FactionId::Pirates, true, false });
    s.fires.push_back({ { 5.0f, 5.0f }, { 9.0f, 9.0f }, FactionId::Independent, false, true });
    s.events.push_back(Ev::Event{ 7, Ev::Kind::Docked, "hello" });
    s.marketPrices = { 10.0f, 20.0f, 30.0f };

    Proto::Snapshot r;
    REQUIRE(Proto::DecodeSnapshot(Proto::EncodeSnapshot(s), r));
    CHECK(r.systemId == "core");
    CHECK(r.player.hull == doctest::Approx(80.0f));
    CHECK(r.player.weaponOn);
    REQUIRE(r.entities.size() == 1);
    CHECK(r.entities[0].id == 5);
    CHECK(r.entities[0].faction == FactionId::Pirates);
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
    REQUIRE(r.player.reputation.size() == 4);
    CHECK(r.player.reputation[1] == doctest::Approx(-2.0f));
    REQUIRE(r.player.bounty.size() == 4);
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
    g.systems.push_back({ "core", 0.85f, 2, 0.7f, FactionId::TradersGuild });
    g.systems.push_back({ "rim", 0.2f, 9, 0.3f, FactionId::Pirates });
    g.events.push_back("Pirates seized Rim");

    Proto::GalaxyState r;
    REQUIRE(Proto::DecodeGalaxy(Proto::EncodeGalaxy(g), r));
    REQUIRE(r.systems.size() == 2);
    CHECK(r.systems[0].id == "core");
    CHECK(r.systems[0].security == doctest::Approx(0.85f));
    CHECK(r.systems[0].controller == FactionId::TradersGuild);
    CHECK(r.systems[1].pirates == 9);
    CHECK(r.systems[1].controller == FactionId::Pirates);
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

// A standing order travels as an intent on the ordinary command: the server decides
// whether it is possible. If these fields stop round-tripping, an agent silently loses
// the ability to give orders at all.
TEST_CASE("standing orders round-trip on the command")
{
    Proto::Command c;
    c.orderKind = (int)Orders::Kind::Route;
    c.orderTarget = 42;
    c.orderPoint = { -300.0f, 900.0f };
    c.orderStopDist = 250.0f;
    c.orderWarp = true;
    c.orderUntilFull = true;
    c.orderDestSystem = "reach";
    c.abortOrder = true;

    Proto::Command r;
    REQUIRE(Proto::DecodeCommand(Proto::EncodeCommand(c), r));
    CHECK(r.orderKind == (int)Orders::Kind::Route);
    CHECK(r.orderTarget == 42);
    CHECK(r.orderPoint.y == doctest::Approx(900.0f));
    CHECK(r.orderStopDist == doctest::Approx(250.0f));
    CHECK(r.orderWarp);
    CHECK(r.orderUntilFull);
    CHECK(r.orderDestSystem == "reach");
    CHECK(r.abortOrder);

    SUBCASE("a command with no order carries none")
    {
        Proto::Command plain, back;
        REQUIRE(Proto::DecodeCommand(Proto::EncodeCommand(plain), back));
        CHECK(back.orderKind == 0);
        CHECK_FALSE(back.abortOrder);
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
    npc.faction = FactionId::Pirates;
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
    el.faction = FactionId::TradersGuild;
    layout[8] = el;

    Proto::CompleteFromLayout(back, layout);
    CHECK(back.entities[0].name == "Aurora Hub");  // ...and back afterwards
    CHECK(back.entities[0].size == doctest::Approx(90.0f));
    CHECK(back.entities[0].faction == FactionId::TradersGuild);

    // An entity the layout does not describe is left exactly as it arrived.
    CHECK(back.entities[1].name == "pirate");
    CHECK(back.entities[1].faction == FactionId::Pirates);
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

    const std::string wire = Proto::EncodeSnapshot(s);
    CHECK(wire.find("2615.7900390625") == std::string::npos);
    CHECK(wire.find("2615.79") != std::string::npos);

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
    station.faction = FactionId::TradersGuild;
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
    CHECK(st->faction == FactionId::TradersGuild);
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
