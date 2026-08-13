#include <doctest/doctest.h>

#include "sim/Observation.h"

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
