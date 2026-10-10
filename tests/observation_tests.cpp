#include <doctest/doctest.h>

#include "core/Archetype.h"
#include "entities/ShipType.h"
#include "missions/Mission.h"
#include "sim/Observation.h"

#include <cstdio>
#include <string>

namespace
{

bool Has(const std::string& text, const std::string& needle)
{
    return text.find(needle) != std::string::npos;
}

Proto::EntitySnapshot Ent(int id, Proto::EntityKind kind, Vector2 pos, const char* name)
{
    Proto::EntitySnapshot e;
    e.id = id;
    e.kind = kind;
    e.pos = pos;
    e.name = name;
    return e;
}

// A player sitting at the origin with a full set of account vectors, so the projection
// exercises the same paths it will at runtime.
Proto::Snapshot BaseSnapshot()
{
    Proto::Snapshot s;
    s.systemId = "core";
    s.player.pos = { 0.0f, 0.0f };
    s.player.hull = 90.0f;
    s.player.maxHull = 100.0f;
    s.player.shields = 20.0f;
    s.player.maxShields = 40.0f;
    s.player.cargoCap = 50;
    s.player.money = 1234.0;
    s.player.reputation.assign(4, 0.0f);
    s.player.bounty.assign(4, 0.0);
    s.player.skillXp.assign(3, 0.0f);
    return s;
}

}  // namespace

TEST_CASE("compass follows the screen, not the raw sign of y")
{
    // World y grows downward. If this ever flips, an agent told to fly north would fly
    // south, so the convention is pinned here rather than left to the reader.
    CHECK(Obs::Compass(0.0f, -100.0f) == "N");
    CHECK(Obs::Compass(0.0f, 100.0f) == "S");
    CHECK(Obs::Compass(100.0f, 0.0f) == "E");
    CHECK(Obs::Compass(-100.0f, 0.0f) == "W");
    CHECK(Obs::Compass(100.0f, -100.0f) == "NE");
    CHECK(Obs::Compass(0.0f, 0.0f) == "--");
}

TEST_CASE("the report states the ship, the system and what is nearby")
{
    Proto::Snapshot s = BaseSnapshot();
    s.entities.push_back(Ent(12, Proto::EntityKind::Station, { 300.0f, 0.0f }, "Ceres Depot"));

    Obs::View v;
    v.snapshot = &s;
    std::string out = Obs::Describe(v, Obs::Detail::Brief);

    CHECK(Has(out, "core"));
    CHECK(Has(out, "hull 90/100"));
    CHECK(Has(out, "1234 cr"));
    CHECK(Has(out, "#12"));
    CHECK(Has(out, "Ceres Depot"));
    CHECK(Has(out, "300u"));
    CHECK(Has(out, "E"));  // due east of the player
    CHECK(Has(out, "dockable"));
}

TEST_CASE("hostiles are called out separately from ordinary traffic")
{
    Proto::Snapshot       s = BaseSnapshot();
    Proto::EntitySnapshot pirate = Ent(57, Proto::EntityKind::Npc, { 0.0f, 400.0f }, "Raider");
    pirate.role = 3;  // NpcRole::Pirate
    pirate.hullFrac = 0.6f;
    Proto::EntitySnapshot trader = Ent(58, Proto::EntityKind::Npc, { 0.0f, -200.0f }, "Hauler");
    trader.role = 0;  // NpcRole::Trader
    s.entities.push_back(pirate);
    s.entities.push_back(trader);

    Obs::View v;
    v.snapshot = &s;
    std::string out = Obs::Describe(v, Obs::Detail::Brief);

    REQUIRE(Has(out, "HOSTILE"));
    // The pirate is listed under HOSTILE, the trader below it under NEARBY.
    CHECK(out.find("#57") < out.find("NEARBY"));
    CHECK(out.find("#58") > out.find("NEARBY"));
    CHECK(Has(out, "hull 60%"));
}

TEST_CASE("a bounty makes that faction's ships read as hostile")
{
    Proto::Snapshot       s = BaseSnapshot();
    Proto::EntitySnapshot police = Ent(70, Proto::EntityKind::Npc, { 500.0f, 0.0f }, "Patrol");
    police.role = 2;  // NpcRole::Police
    police.faction = (FactionId)1;
    s.entities.push_back(police);

    Obs::View v;
    v.snapshot = &s;
    CHECK_FALSE(Has(Obs::Describe(v, Obs::Detail::Brief), "HOSTILE"));

    // Now we are wanted by that faction — the same patrol must stop reading as friendly,
    // because the server has it hunting us.
    s.player.bounty[1] = 250.0;
    CHECK(Has(Obs::Describe(v, Obs::Detail::Brief), "HOSTILE"));
}

TEST_CASE("the nearby list is capped but never drops navigation anchors")
{
    Proto::Snapshot s = BaseSnapshot();
    for (int i = 0; i < 40; i++)  // crowd it with close-in traffic
    {
        Proto::EntitySnapshot n =
            Ent(100 + i, Proto::EntityKind::Npc, { (float)(10 + i), 0.0f }, "Hauler");
        n.role = 0;
        s.entities.push_back(n);
    }
    // A station and a gate far outside the cap: exactly what a nearest-first cut would lose.
    s.entities.push_back(Ent(9001, Proto::EntityKind::Station, { 9000.0f, 0.0f }, "Far Depot"));
    s.entities.push_back(Ent(9002, Proto::EntityKind::Gate, { 9500.0f, 0.0f }, "Reach Gate"));

    Obs::View v;
    v.snapshot = &s;
    std::string brief = Obs::Describe(v, Obs::Detail::Brief);

    CHECK(Has(brief, "not listed"));  // says what it left out rather than lying by omission
    CHECK(Has(brief, "#9001"));
    CHECK(Has(brief, "#9002"));

    // Full shows strictly more of the traffic than brief.
    std::string full = Obs::Describe(v, Obs::Detail::Full);
    CHECK(full.size() > brief.size());
}

TEST_CASE("layout fills in what the snapshot leaves out")
{
    Proto::Snapshot s = BaseSnapshot();
    s.entities.push_back(Ent(5, Proto::EntityKind::Gate, { 0.0f, -800.0f }, ""));

    std::map<int, Proto::EntityLayout> layout;
    Proto::EntityLayout                gate;
    gate.id = 5;
    gate.kind = Proto::EntityKind::Gate;
    gate.name = "Reach Gate";
    gate.dest = "reach";
    layout[5] = gate;

    Obs::View v;
    v.snapshot = &s;
    v.layout = &layout;
    std::string out = Obs::Describe(v, Obs::Detail::Brief);

    CHECK(Has(out, "Reach Gate"));  // name came from the layout, not the snapshot
    CHECK(Has(out, "to reach"));    // and so did the destination
}

TEST_CASE("missing state degrades to fewer lines, not to a failure")
{
    // An agent asking "where am I" mid system change should get a partial answer.
    Obs::View empty;
    CHECK(Has(Obs::Describe(empty, Obs::Detail::Brief), "No world state"));

    Proto::Snapshot s = BaseSnapshot();
    Obs::View       v;
    v.snapshot = &s;  // no layout, no universe, no galaxy
    std::string out = Obs::Describe(v, Obs::Detail::Full);
    CHECK(Has(out, "SYSTEM"));
    CHECK(Has(out, "SHIP"));
}

TEST_CASE("dockable is the archetype's component, not the kind's (#195)")
{
    REQUIRE(Archetypes::Load(std::string(TEST_DATA_DIR) + "archetypes.json"));

    // A wreck someone fitted out as a dock, and a station shell with no dock at all. Both
    // stand in for what a player will build (#44): the kind says nothing, the archetype
    // says everything.
    Proto::Snapshot s = BaseSnapshot();
    s.entities.push_back(Ent(7, Proto::EntityKind::Derelict, { 0.0f, -500.0f }, "Fitted Hulk"));
    s.entities.push_back(Ent(8, Proto::EntityKind::Station, { 0.0f, 500.0f }, "Empty Shell"));

    std::map<int, Proto::EntityLayout> layout;
    layout[7].id = 7;
    layout[7].kind = Proto::EntityKind::Derelict;
    layout[7].archetype = "station.trade_hub";  // has the dockable component
    layout[8].id = 8;
    layout[8].kind = Proto::EntityKind::Station;
    layout[8].archetype = "derelict.wreck";  // does not

    Obs::View v;
    v.snapshot = &s;
    v.layout = &layout;
    const std::string out = Obs::Describe(v, Obs::Detail::Brief);

    auto lineOf = [&](const char* name)
    {
        const size_t at = out.find(name);
        REQUIRE(at != std::string::npos);
        return out.substr(at, out.find('\n', at) - at);
    };
    CHECK(Has(lineOf("Fitted Hulk"), "dockable"));
    CHECK_FALSE(Has(lineOf("Empty Shell"), "dockable"));
}

namespace
{

// Docked at station 3, owned by the Syndicate, with a layout that names it.
struct Docked
{
    Proto::Snapshot                    s = BaseSnapshot();
    std::map<int, Proto::EntityLayout> layout;
    Obs::View                          v;

    Docked()
    {
        s.player.docked = true;
        s.player.dockedStationId = 3;
        s.player.ownedShips = { 0 };
        s.player.shipIndex = 0;
        s.player.cargoByType.assign(AllResourceTypes().size(), 0);
        layout[3].id = 3;
        layout[3].kind = Proto::EntityKind::Station;
        layout[3].name = "Vale Station";
        layout[3].faction = FactionId::Syndicate;
        v.snapshot = &s;
        v.layout = &layout;
    }
};

Proto::MissionView Job(MissionType type, int giver, int dest, int target, int progress)
{
    Proto::MissionView m;
    m.type = (int)type;
    m.faction = (int)FactionId::Syndicate;
    m.title = "A job";
    m.description = "Do the thing";
    m.giverStationId = giver;
    m.destStationId = dest;
    m.targetCount = target;
    m.progress = progress;
    m.rewardMoney = 900.0;
    m.rewardRep = 2.0f;
    return m;
}

}  // namespace

TEST_CASE("the mission list numbers what accept and complete take, and says what is missing (#109)")
{
    Docked d;
    d.s.missionOffers.push_back(Job(MissionType::Delivery, 3, 44, 0, 0));
    d.s.missionActive.push_back(Job(MissionType::Bounty, 3, 0, 3, 1));
    Proto::MissionView mining = Job(MissionType::Mining, 3, 0, 20, 0);
    mining.resource = 0;
    d.s.player.cargoByType[0] = 5;
    d.s.missionActive.push_back(mining);
    Proto::MissionView ready = Job(MissionType::Delivery, 9, 3, 0, 0);
    ready.completable = true;
    d.s.missionActive.push_back(ready);

    const std::string out = Obs::DescribeMissions(d.v);
    CHECK(Has(out, "OFFERS at Vale Station (#3)"));
    CHECK(Has(out, "[0] delivery: A job"));
    CHECK(Has(out, "900 cr"));
    // A delivery is handed in at its destination, which is in another system here.
    CHECK(Has(out, "station #44, not in this system"));
    CHECK(Has(out, "progress 1/3"));
    CHECK(Has(out, "destroy 2 more pirates"));
    CHECK(Has(out, "carry 20 " + ResourceName((ResourceType)0) + " (the hold has 5)"));
    CHECK(Has(out, "READY TO HAND IN"));

    CHECK(Obs::MissionNeeds(d.v, ready).empty());
}

TEST_CASE("outside a station there is no board, but the missions taken are still listed (#109)")
{
    Docked d;
    d.s.player.docked = false;
    d.s.player.dockedStationId = 0;
    d.s.missionActive.push_back(Job(MissionType::Bounty, 3, 0, 3, 3));

    const std::string out = Obs::DescribeMissions(d.v);
    CHECK(Has(out, "dock to see its work"));
    CHECK(Has(out, "[0] bounty"));
    // Enough pirates, but the hand-in is at the giver.
    CHECK(Has(out, "needs: dock at Vale Station (#3)"));
    CHECK(Obs::DockedFaction(d.v) == FactionId::Independent);
}

TEST_CASE("the hangar quotes the price this player would be charged here (#109)")
{
    Docked d;
    REQUIRE(GetShipCatalog().size() > 2);
    d.s.player.ownedShips = { 0, 2 };
    d.s.player.money = 100000.0;
    d.s.player.reputation[(int)FactionId::Syndicate] = 60.0f;  // allied with the owner

    CHECK(Obs::DockedFaction(d.v) == FactionId::Syndicate);
    const std::string out = Obs::DescribeHangar(d.v);
    CHECK(Has(out, "owned by Syndicate"));
    CHECK(Has(out, "FLYING"));
    CHECK(Has(out, "owned, switch_ship"));

    // The same multiplier the server charges with, not a copy of it.
    const double price = GetShipCatalog()[1].price * ShipPriceMultiplier(RepTier::Allied);
    char         buf[64];
    std::snprintf(buf, sizeof(buf), "for sale %.0f cr", price);
    CHECK(Has(out, buf));
    CHECK(price < GetShipCatalog()[1].price);
}
