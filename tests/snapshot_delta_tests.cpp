// Snapshot deltas and the area of interest (#349): a snapshot is a delta against the newest
// state the client acknowledged, entities are integers, and only the neighbourhood is told in
// full. What these hold is that a client rebuilds exactly what the server meant, through a
// lost message and a missing acknowledgement, and that cover still hides.
#include "doctest/doctest.h"

#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "entities/Nebula.h"
#include "entities/Ship.h"
#include "entities/ShipType.h"
#include "sim/ClientSession.h"
#include "sim/Protocol.h"
#include "sim/Simulation.h"
#include "factions_named.h"

#include <map>
#include <memory>
#include <set>
#include <string>

namespace
{
Proto::EntitySnapshot Npc(int id, Vector2 pos)
{
    Proto::EntitySnapshot e;
    e.id = id;
    e.kind = EntityKind::Npc;
    e.pos = pos;
    e.heading = 0.5f;
    e.size = 14.0f;
    e.faction = Fac::Pirates();
    e.role = 3;
    e.name = "Raider";
    e.design = "raider";
    return e;
}

const Proto::EntitySnapshot* Find(const Proto::Snapshot& s, int id)
{
    for (const Proto::EntitySnapshot& e : s.entities)
        if (e.id == id)
            return &e;
    return nullptr;
}

// The server's sender and the client's receiver, joined by a wire that can lose messages.
struct Link
{
    Proto::SnapshotSender   out;
    Proto::SnapshotReceiver in;
    std::string             last;  // the last message sent

    // Sends `s`; the client rebuilds it and acknowledges. False if it could not rebuild it.
    bool Deliver(const Proto::Snapshot& s, Proto::Snapshot& got, const std::set<int>& relevant = {})
    {
        last = out.Encode(s, relevant);
        if (!in.Decode(last, got))
            return false;
        out.Acknowledge(in.Latest());
        return true;
    }
};

Proto::Snapshot Base()
{
    Proto::Snapshot s;
    s.systemId = "core";
    s.time = 100.0;
    s.player.pos = { 0.0f, 0.0f };
    s.player.hull = 80.0f;
    s.player.money = 1000.0;
    s.marketPrices = { 10.0f, 20.0f };
    s.entities.push_back(Npc(10, { 100.0f, 0.0f }));
    s.entities.push_back(Npc(11, { 200.0f, 50.0f }));
    s.entities.push_back(Npc(12, { -300.0f, 25.0f }));
    return s;
}
}  // namespace

TEST_CASE("a delta carries what changed, and the client rebuilds the whole")
{
    Link            link;
    Proto::Snapshot got;
    Proto::Snapshot s = Base();
    REQUIRE(link.Deliver(s, got));
    CHECK(link.out.LastBase() == 0);  // nothing acknowledged yet: whole
    REQUIRE(got.entities.size() == 3);
    CHECK(Find(got, 11)->name == "Raider");
    CHECK(Find(got, 11)->faction == Fac::Pirates());

    SUBCASE("nothing changed: nothing but the envelope")
    {
        s.time += 0.05;
        REQUIRE(link.Deliver(s, got));
        CHECK(link.out.LastBase() > 0);
        CHECK(link.last.find("\"ents\"") == std::string::npos);
        CHECK(link.last.find("\"pl\"") == std::string::npos);
        CHECK(link.last.find("\"mk\"") == std::string::npos);
        CHECK(got.entities.size() == 3);
        CHECK(got.player.money == doctest::Approx(1000.0));
        CHECK(got.marketPrices.size() == 2);
    }

    SUBCASE("added, changed and removed")
    {
        s.time += 0.05;
        s.entities[0].pos.x += 12.34f;             // 10 moves
        s.entities.erase(s.entities.begin() + 2);  // 12 is gone
        s.entities.push_back(Npc(13, { 400.0f, 400.0f }));
        s.entities.back().name = "Newcomer";
        s.player.money = 1500.0;
        REQUIRE(link.Deliver(s, got));
        // Only the step of the one that moved, and not 11 at all.
        CHECK(link.last.find("\"dp\":[1234,0]") != std::string::npos);
        CHECK(link.last.find("\"i\":11") == std::string::npos);
        CHECK(link.last.find("\"rm\":[12]") != std::string::npos);
        CHECK(link.last.find("Newcomer") != std::string::npos);
        CHECK(link.last.find("\"money\"") != std::string::npos);
        CHECK(link.last.find("\"hull\"") == std::string::npos);

        REQUIRE(got.entities.size() == 3);
        CHECK(Find(got, 10)->pos.x == doctest::Approx(112.34f));
        CHECK(Find(got, 10)->name == "Raider");  // unchanged fields stay
        CHECK(Find(got, 11) != nullptr);
        CHECK(Find(got, 12) == nullptr);
        CHECK(Find(got, 13)->name == "Newcomer");
        CHECK(got.player.money == doctest::Approx(1500.0));
        CHECK(got.player.hull == doctest::Approx(80.0f));
    }

    SUBCASE("a field back at its default is sent as such")
    {
        s.entities[1].hullFrac = 0.5f;
        REQUIRE(link.Deliver(s, got));
        CHECK(Find(got, 11)->hullFrac == doctest::Approx(0.5f));
        s.entities[1].hullFrac = 1.0f;
        s.entities[1].design.clear();
        REQUIRE(link.Deliver(s, got));
        CHECK(Find(got, 11)->hullFrac == doctest::Approx(1.0f));
        CHECK(Find(got, 11)->design.empty());
    }

    SUBCASE("missions go when they change, and an emptied list is empty")
    {
        Proto::MissionView m;
        m.title = "Deliver ice";
        s.missionActive.push_back(m);
        REQUIRE(link.Deliver(s, got));
        CHECK(link.last.find("\"ma\"") != std::string::npos);
        REQUIRE(got.missionActive.size() == 1);
        REQUIRE(link.Deliver(s, got));
        CHECK(link.last.find("\"ma\"") == std::string::npos);  // unchanged: not sent
        REQUIRE(got.missionActive.size() == 1);                // and still there
        CHECK(got.missionActive[0].title == "Deliver ice");
        s.missionActive.clear();
        REQUIRE(link.Deliver(s, got));
        CHECK(got.missionActive.empty());
    }

    SUBCASE("what happens once is in the message it happened in, and only that one")
    {
        s.events.push_back(Ev::Event{ 3, Ev::Kind::Docked, "docked" });
        s.fires.push_back({ { 0.0f, 0.0f }, { 1.0f, 1.0f }, Fac::Pirates(), true, false });
        REQUIRE(link.Deliver(s, got));
        CHECK(got.events.size() == 1);
        CHECK(got.fires.size() == 1);
        s.events.clear();
        s.fires.clear();
        REQUIRE(link.Deliver(s, got));
        CHECK(got.events.empty());
        CHECK(got.fires.empty());
    }

    SUBCASE("another system is never a base")
    {
        s.systemId = "rim";
        REQUIRE(link.Deliver(s, got));
        CHECK(link.out.LastBase() == 0);
        CHECK(got.systemId == "rim");
        CHECK(got.entities.size() == 3);
    }
}

TEST_CASE("the own ship is exact, everything else is integers on both ends")
{
    // Prediction replays inputs on top of the player's own state; quantising it would pull the
    // ship back a few hundredths every snapshot.
    Link            link;
    Proto::Snapshot got;
    Proto::Snapshot s = Base();
    s.player.pos = { 1234.5678f, -0.001f };
    s.player.vel = { 3.14159f, 2.71828f };
    s.entities[0].pos = { 2615.789f, -1234.567f };
    REQUIRE(link.Deliver(s, got));
    CHECK(got.player.pos.x == 1234.5678f);
    CHECK(got.player.vel.y == 2.71828f);
    CHECK(Find(got, 10)->pos.x == doctest::Approx(2615.79f));

    // A long run of small steps does not drift: each is decided against what the client holds.
    for (int i = 0; i < 200; i++)
    {
        s.time += 0.05;
        s.entities[0].pos.x += 0.0137f;
        REQUIRE(link.Deliver(s, got));
    }
    CHECK(Find(got, 10)->pos.x == doctest::Approx(s.entities[0].pos.x).epsilon(1e-5));
}

TEST_CASE("a lost snapshot and a missing acknowledgement heal themselves")
{
    Link            link;
    Proto::Snapshot got;
    Proto::Snapshot s = Base();
    REQUIRE(link.Deliver(s, got));

    SUBCASE("a lost message: the next is still against what the client has")
    {
        s.entities[0].pos.x += 10.0f;
        link.out.Encode(s, {});  // lost on the way, never acknowledged
        s.entities[0].pos.x += 10.0f;
        REQUIRE(link.Deliver(s, got));
        CHECK(Find(got, 10)->pos.x == doctest::Approx(120.0f));
    }

    SUBCASE("a delta against a state the client does not hold is refused, not misread")
    {
        Proto::SnapshotReceiver stranger;
        s.time += 0.05;
        const std::string delta = link.out.Encode(s, {});
        Proto::Snapshot   untouched;
        untouched.systemId = "unchanged";
        CHECK_FALSE(stranger.Decode(delta, untouched));
        CHECK(untouched.systemId == "unchanged");
        CHECK_FALSE(Proto::DecodeSnapshot(delta, untouched));  // nor read as whole
    }

    SUBCASE("acknowledgements that stop: whole again once the base is no longer held")
    {
        int whole = 0;
        for (size_t i = 0; i < Proto::SnapshotSender::HELD + 2; i++)
        {
            s.time += 0.05;
            s.entities[1].pos.y += 1.0f;
            link.last = link.out.Encode(s, {});
            REQUIRE(link.in.Decode(link.last, got));  // received, never acknowledged
            whole += link.out.LastBase() == 0 ? 1 : 0;
        }
        CHECK(whole > 0);
        CHECK(Find(got, 11)->pos.y == doctest::Approx(50.0f + Proto::SnapshotSender::HELD + 2));
        // Acknowledging again brings the deltas back.
        link.out.Acknowledge(link.in.Latest());
        s.time += 0.05;
        REQUIRE(link.Deliver(s, got));
        CHECK(link.out.LastBase() > 0);
    }

    SUBCASE("an acknowledgement of something never sent is ignored")
    {
        link.out.Acknowledge(1000000);
        s.time += 0.05;
        REQUIRE(link.Deliver(s, got));
        CHECK(link.out.LastBase() == 1);
    }
}

TEST_CASE("a client that stops acknowledging is sent one a second until it acks (#411)")
{
    Link            link;
    Proto::Snapshot got;
    Proto::Snapshot s = Base();

    // Twenty a second for as long as the acknowledgements keep up.
    double now = 0.0;
    for (int i = 0; i < 200; i++, now += 0.05)
    {
        REQUIRE(link.out.Due(now));
        s.time = now;
        REQUIRE(link.Deliver(s, got));
    }

    // Then silence: STALL_AFTER more go out at the full rate, after that one a second.
    int sent = 0;
    for (int i = 0; i < 20 * 60; i++, now += 0.05)  // a minute of a model thinking
        if (link.out.Due(now))
        {
            s.time = now;
            link.out.Encode(s, {});
            sent++;
        }
    CHECK(sent >= Proto::SnapshotSender::STALL_AFTER);
    CHECK(sent <= Proto::SnapshotSender::STALL_AFTER + 60);

    // One acknowledgement of the newest and it is back to twenty a second.
    REQUIRE(link.in.Decode(link.out.Encode(s, {}), got));
    link.out.Acknowledge(link.in.Latest());
    CHECK(link.out.Unacknowledged() == 0);
    for (int i = 0; i < 5; i++, now += 0.05)
        CHECK(link.out.Due(now));
}

TEST_CASE("the area of interest: near in full, far coarse and once a second")
{
    Proto::Interest i;
    i.nearRadius = 5000.0f;
    i.farRefresh = 1.0;
    i.farGrid = 10.0f;
    Link link{ Proto::SnapshotSender(i), {}, {} };

    Proto::Snapshot s = Base();
    s.entities.push_back(Npc(20, { 100000.0f, 3.33f }));  // far
    s.entities.push_back(Npc(21, { 0.0f, -200000.0f }));  // far, and selected
    Proto::Snapshot got;
    REQUIRE(link.Deliver(s, got, { 21 }));
    // Still listed -- the overview and the sensor screen show the whole system...
    REQUIRE(Find(got, 20) != nullptr);
    // ...but to ten units, and a selected one in full however far it is.
    CHECK(Find(got, 20)->pos.y == doctest::Approx(0.0f));
    CHECK(Find(got, 20)->name == "Raider");
    CHECK(Find(got, 21)->pos.y == doctest::Approx(-200000.0f));

    // Far things move without a word until their refresh comes round; near ones every time.
    int farSeen = 0;
    for (int k = 1; k <= 40; k++)  // two seconds at 20 Hz
    {
        s.time += 0.05;
        s.entities[0].pos.x += 5.0f;
        for (Proto::EntitySnapshot& e : s.entities)
            if (e.id == 20 || e.id == 21)
                e.pos.x += 50.0f;
        REQUIRE(link.Deliver(s, got, { 21 }));
        farSeen += link.last.find("\"i\":20") != std::string::npos ? 1 : 0;
        CHECK(link.last.find("\"i\":10") != std::string::npos);
        CHECK(link.last.find("\"i\":21") != std::string::npos);
        CHECK(Find(got, 21)->pos.x == doctest::Approx(50.0f * k));
    }
    CHECK(farSeen >= 1);
    CHECK(farSeen <= 3);
    CHECK(Find(got, 20)->pos.x > 100000.0f);  // and it did move

    SUBCASE("coming near is in full at once")
    {
        for (Proto::EntitySnapshot& e : s.entities)
            if (e.id == 20)
                e.pos = { 1000.0f, 3.33f };
        s.time += 0.05;
        REQUIRE(link.Deliver(s, got, { 21 }));
        CHECK(Find(got, 20)->pos.y == doctest::Approx(3.33f));
    }
}

TEST_CASE("the area of interest has a cap, nearest first")
{
    Proto::Interest i;
    i.nearRadius = 100000.0f;
    i.nearCap = 2;
    Link            link{ Proto::SnapshotSender(i), {}, {} };
    Proto::Snapshot s = Base();  // 10 at 100, 11 at ~206, 12 at ~301
    s.entities[2].pos.y = 25.07f;
    Proto::Snapshot got;
    REQUIRE(link.Deliver(s, got));
    CHECK(Find(got, 12)->pos.y == doctest::Approx(30.0f));  // third: on the far grid
    s.entities[0].pos.y = 0.07f;
    s.time += 0.05;
    REQUIRE(link.Deliver(s, got));
    CHECK(Find(got, 10)->pos.y == doctest::Approx(0.07f));
}

namespace
{
std::unique_ptr<Simulation> World()
{
    SetRandomSeed(0xC0FFEEu);
    REQUIRE(Factions::Load(std::string(TEST_DATA_DIR) + "factions.json"));
    REQUIRE(Archetypes::Load(std::string(TEST_DATA_DIR) + "archetypes.json"));
    REQUIRE(Blueprints::Load(std::string(TEST_DATA_DIR) + "blueprints.json"));
    auto sim = std::make_unique<Simulation>();
    sim->LoadUniverse(std::string(TEST_WORLD_DIR) + "universe.json");
    sim->AttachRegion(7, std::string(TEST_WORLD_DIR) + "systems/");
    sim->Seed(1234u);
    sim->InitGalaxy();
    sim->MaterializeAllSystems(std::string(TEST_WORLD_DIR) + "systems/");
    return sim;
}

ClientSession& Arrive(Simulation& sim)
{
    const std::string& start = sim.Universe().startId;
    return sim.CreateSession(start, sim.SafeArrival(start), GetShipCatalog()[0].stats);
}
}  // namespace

TEST_CASE("cover hides a player through the delta, even when selected")
{
    auto           sim = World();
    ClientSession& hider = Arrive(*sim);
    ClientSession& seeker = Arrive(*sim);
    REQUIRE(hider.systemId == seeker.systemId);
    const int hiderShip = hider.ship->GetId();
    seeker.selectedId = hiderShip;
    CHECK(sim->InterestOf(seeker).count(hiderShip) == 1);

    Link            link;
    Proto::Snapshot got;
    REQUIRE(
        link.Deliver(sim->BuildSnapshot(seeker, seeker.systemId), got, sim->InterestOf(seeker)));
    CHECK(Find(got, hiderShip) != nullptr);

    const Vector2 at = { hider.ship->GetPosition().x + 50000.0f, hider.ship->GetPosition().y };
    REQUIRE(sim->AddStatic(hider.systemId, std::make_unique<Nebula>(at, 4000.0f, "Shroud"), "") !=
            0);
    hider.ship->Teleport(at);
    REQUIRE(sim->InCover(hider));
    REQUIRE(
        link.Deliver(sim->BuildSnapshot(seeker, seeker.systemId), got, sim->InterestOf(seeker)));
    CHECK(Find(got, hiderShip) == nullptr);  // gone from the client, by an explicit removal
    CHECK(link.last.find("\"rm\"") != std::string::npos);

    hider.ship->Teleport({ at.x + 20000.0f, at.y });
    REQUIRE(
        link.Deliver(sim->BuildSnapshot(seeker, seeker.systemId), got, sim->InterestOf(seeker)));
    CHECK(Find(got, hiderShip) != nullptr);
}

TEST_CASE("an acknowledgement round-trips and refuses junk")
{
    Proto::Ack a;
    a.snapshot = 4242;
    Proto::Ack back;
    REQUIRE(Proto::DecodeAck(Proto::EncodeAck(a), back));
    CHECK(back.snapshot == 4242);
    CHECK(Proto::MessageType(Proto::EncodeAck(a)) == "ack");
    CHECK_FALSE(Proto::DecodeAck(Proto::EncodeCommand(Proto::Command{}), back));
    CHECK_FALSE(Proto::DecodeAck("{\"t\":\"ack\",\"v\":1,\"n\":3}", back));
}

TEST_CASE("gates, mouths and Anchors are never on the far list")
{
    // They are the static layer: in the layout, never in a snapshot, so the area of interest
    // has nothing to decide about them and a client lists them wherever they are.
    Proto::Interest i;
    i.nearRadius = 1000.0f;
    Link            link{ Proto::SnapshotSender(i), {}, {} };
    Proto::Snapshot got;
    REQUIRE(link.Deliver(Base(), got));

    std::map<int, Proto::EntityLayout> layout;
    Proto::EntityLayout                gate;
    gate.id = 90;
    gate.kind = EntityKind::Gate;
    gate.pos = { 900000.0f, 0.0f };
    gate.name = "Mouth";
    layout[90] = gate;
    Proto::EntityLayout anchor;
    anchor.id = 91;
    anchor.kind = EntityKind::Structure;
    anchor.pos = { 900100.0f, 0.0f };
    anchor.name = "Anchor";
    layout[91] = anchor;

    Proto::CompleteFromLayout(got, layout);
    REQUIRE(Find(got, 90) != nullptr);
    CHECK(Find(got, 90)->pos.x == doctest::Approx(900000.0f));
    CHECK(Find(got, 91)->name == "Anchor");
}

TEST_CASE("a system's shared wire cache changes nothing a recipient is sent (#350)")
{
    // Three pilots in one system, each with a sender of its own; one pair of each encodes
    // through the shared cache, the other alone. Round by round -- objects moving, one taken
    // hurt, one gone, one far off -- every message must be the same byte for byte.
    Proto::Interest i;
    i.nearRadius = 250.0f;
    i.nearCap = 2;
    Proto::WireCache                   wires;
    std::vector<Proto::SnapshotSender> shared(3, Proto::SnapshotSender(i));
    std::vector<Proto::SnapshotSender> alone(3, Proto::SnapshotSender(i));
    const Vector2 at[3] = { { 0.0f, 0.0f }, { 180.0f, 40.0f }, { -250.0f, 0.0f } };

    Proto::Snapshot s = Base();
    s.entities.push_back(Npc(13, { 5000.0f, 5000.0f }));
    for (int round = 0; round < 40; round++)
    {
        s.time = 100.0 + round * 0.05;
        s.entities[0].pos.x += 3.0f;
        if (round == 10)
            s.entities[1].hullFrac = 0.5f;
        if (round == 20)
            s.entities.erase(s.entities.begin() + 2);
        for (int k = 0; k < 3; k++)
        {
            s.player.pos = at[k];
            const std::string a = shared[k].Encode(s, { 11 }, &wires);
            const std::string b = alone[k].Encode(s, { 11 });
            CHECK(a == b);
            if (round % 3 != 2)  // now and then a lost acknowledgement
            {
                shared[k].Acknowledge(round + 1);
                alone[k].Acknowledge(round + 1);
            }
        }
        CHECK(wires.Sweep());
    }
    CHECK_FALSE(Proto::WireCache().Sweep());
}
