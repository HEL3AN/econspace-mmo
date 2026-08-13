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
    s.messages.push_back("hello");
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
    REQUIRE(r.messages.size() == 1);
    CHECK(r.messages[0] == "hello");
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
