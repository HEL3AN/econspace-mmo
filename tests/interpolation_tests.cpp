#include <doctest/doctest.h>

#include <cmath>
#include <string>
#include <vector>

#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "core/Interpolation.h"
#include "entities/Ship.h"
#include "entities/ShipType.h"
#include "sim/ClientSession.h"
#include "sim/PlayerStep.h"
#include "sim/Protocol.h"
#include "sim/Simulation.h"

// The client's interpolation of everything but its own ship (#358), the other half of the
// netcode beside Prediction: a snapshot is a sample of where things were at a server time,
// and an object is drawn between the two samples around the moment being drawn.
namespace
{
constexpr float PI_F = 3.14159265f;

Proto::EntitySnapshot At(int id, float x, float y, float heading = 0.0f)
{
    Proto::EntitySnapshot e;
    e.id = id;
    e.pos = { x, y };
    e.heading = heading;
    return e;
}

float Dist(Vector2 a, Vector2 b)
{
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y));
}
}  // namespace

TEST_CASE("interpolation: an object is drawn between the two snapshots around the moment")
{
    Interpolation in;
    in.Add(10.0, { At(7, 0.0f, 0.0f, 0.0f) });
    in.Add(10.1, { At(7, 100.0f, 50.0f, 0.4f) });
    in.Settle();

    Interpolation::Pose p;
    REQUIRE(Interpolation::PoseOf(in.Around(10.025), 7, p));
    CHECK(p.pos.x == doctest::Approx(25.0f));
    CHECK(p.pos.y == doctest::Approx(12.5f));
    CHECK(p.heading == doctest::Approx(0.1f));

    // Before the first sample it stands at the first, after the last at the last: never
    // extrapolated.
    REQUIRE(Interpolation::PoseOf(in.Around(9.0), 7, p));
    CHECK(p.pos.x == doctest::Approx(0.0f));
    REQUIRE(Interpolation::PoseOf(in.Around(11.0), 7, p));
    CHECK(p.pos.x == doctest::Approx(100.0f));

    // An object no snapshot has is the caller's to place.
    CHECK_FALSE(Interpolation::PoseOf(in.Around(10.05), 8, p));
}

TEST_CASE("interpolation: a heading turns the short way across the half turn")
{
    Interpolation in;
    in.Add(1.0, { At(3, 0.0f, 0.0f, PI_F - 0.1f) });
    in.Add(2.0, { At(3, 0.0f, 0.0f, -PI_F + 0.1f) });
    in.Settle();
    Interpolation::Pose p;
    REQUIRE(Interpolation::PoseOf(in.Around(1.5), 3, p));
    // Halfway along the 0.2 rad between them, not back round through zero.
    CHECK(std::fabs(std::fabs(p.heading) - PI_F) < 0.01f);
}

TEST_CASE("interpolation: an object in only one of the two is drawn where that one has it")
{
    Interpolation in;
    in.Add(1.0, { At(1, 0.0f, 0.0f) });
    in.Add(2.0, { At(1, 10.0f, 0.0f), At(2, 500.0f, 0.0f) });  // 2 appeared
    in.Settle();
    Interpolation::Pose p;
    REQUIRE(Interpolation::PoseOf(in.Around(1.5), 2, p));
    CHECK(p.pos.x == doctest::Approx(500.0f));
}

TEST_CASE("interpolation: a snapshot that came late takes its place in server time")
{
    Interpolation in;
    in.Add(1.1, { At(1, 20.0f, 0.0f) });
    in.Add(1.0, { At(1, 10.0f, 0.0f) });  // older, arrived after
    in.Add(1.2, { At(1, 30.0f, 0.0f) });
    in.Settle();
    Interpolation::Pose p;
    REQUIRE(Interpolation::PoseOf(in.Around(1.05), 1, p));
    CHECK(p.pos.x == doctest::Approx(15.0f));
    CHECK(in.VelocityOf(1).x == doctest::Approx(100.0f));
}

TEST_CASE("interpolation: half a second is kept, and never fewer than two snapshots")
{
    Interpolation in;
    for (int i = 0; i < 40; i++)
        in.Add(i * 0.05, { At(1, (float)i, 0.0f) });
    in.Settle();
    // 1.95 is the newest: everything from 1.45 on, eleven snapshots.
    CHECK(in.Size() == 11);

    Interpolation two;
    two.Add(0.0, { At(1, 0.0f, 0.0f) });
    two.Add(5.0, { At(1, 1.0f, 0.0f) });
    two.Settle();
    CHECK(two.Size() == 2);  // far apart, but the only two there are

    Interpolation many;
    for (int i = 0; i < 300; i++)
        many.Add(i * 0.001, {});
    many.Settle();
    CHECK(many.Size() == Interpolation::MAX_SNAPSHOTS);

    in.Clear();
    CHECK(in.Size() == 0);
    Interpolation::Pose p;
    CHECK_FALSE(Interpolation::PoseOf(in.Around(1.0), 1, p));
}

TEST_CASE("interpolation: velocity is the two newest snapshots that have the object")
{
    Interpolation in;
    in.Add(1.0, { At(1, 0.0f, 0.0f) });
    in.Add(1.2, { At(1, 10.0f, 0.0f) });
    in.Add(1.4, {});  // the newest does not have it
    in.Settle();
    CHECK(in.VelocityOf(1).x == doctest::Approx(50.0f));
    CHECK(in.VelocityOf(1).y == doctest::Approx(0.0f));
    CHECK(in.VelocityOf(9).x == 0.0f);  // nothing to measure

    Interpolation one;
    one.Add(1.0, { At(1, 5.0f, 5.0f) });
    one.Settle();
    CHECK(one.VelocityOf(1).x == 0.0f);
}

TEST_CASE("interpolation: against the server, an orbiting station is drawn where it was")
{
    // A server and a client in one process: the server steps its world, a snapshot goes out
    // every third tick through the encoder, and the client draws the station 0.1 s in the
    // past. Drawn there, it must be where the server had it then -- a station on an orbit
    // (#210) moves on a curve, and over one snapshot's time a straight line between two
    // samples stays within a unit of it.
    Factions::Load(std::string(TEST_DATA_DIR) + "factions.json");
    Archetypes::Load(std::string(TEST_DATA_DIR) + "archetypes.json");
    Blueprints::Load(std::string(TEST_DATA_DIR) + "blueprints.json");
    Simulation sim;
    sim.LoadUniverse(std::string(TEST_WORLD_DIR) + "universe.json");
    sim.Seed(1234u);
    sim.InitGalaxy();
    sim.MaterializeAllSystems(std::string(TEST_WORLD_DIR) + "systems/");
    const std::string start = sim.Universe().startId;
    ClientSession& s = sim.CreateSession(start, sim.SafeArrival(start), GetShipCatalog()[0].stats);

    int id = 0;
    for (const auto& e : sim.SystemOf(s)->entities)
        if (e->GetKind() == EntityKind::Station && e->GetOrbit().has_value())
        {
            id = e->GetId();
            break;
        }
    REQUIRE(id != 0);
    const auto serverPos = [&]()
    {
        for (const auto& e : sim.SystemOf(s)->entities)
            if (e->GetId() == id)
                return e->GetPosition();
        return Vector2{ 0.0f, 0.0f };
    };

    Interpolation                                        client;
    std::vector<std::pair<double, Vector2>>              truth;  // the server's station, every tick
    float                                                worst = 0.0f, moved = 0.0f;
    int                                                  checked = 0;
    static const std::vector<Simulation::PlayerPresence> nobody;
    for (int tick = 0; tick < 600; tick++)
    {
        // The host's world tick: a hot system's agents and bodies, then the clock.
        sim.StepSystemAgents(*sim.SystemOf(s), nobody, nullptr, Sim::SIM_DT);
        sim.MaintainWorld(Sim::SIM_DT);
        truth.push_back({ sim.Time(), serverPos() });
        if (tick % 3 != 0)
            continue;
        Proto::Snapshot back;
        REQUIRE(
            Proto::DecodeSnapshot(Proto::EncodeSnapshot(sim.BuildSnapshot(s, s.systemId)), back));
        client.Add(back.time, back.entities);
        client.Settle();
        if (tick < 30)
            continue;
        // Draw it as Game does, 0.1 s behind the newest; compare with the tick at that time.
        const double        rt = back.time - 0.1;
        Interpolation::Pose p;
        REQUIRE(Interpolation::PoseOf(client.Around(rt), id, p));
        for (const auto& [t, pos] : truth)
            if (std::fabs(t - rt) < Sim::SIM_DT / 2)
            {
                worst = std::max(worst, Dist(p.pos, pos));
                checked++;
            }
    }
    moved = Dist(truth.front().second, truth.back().second);
    MESSAGE("station moved " << moved << " units in 10 s; drawn at most " << worst
                             << " from where the server had it");
    CHECK(moved > 50.0f);  // it is a test of something that moves
    CHECK(checked > 150);
    CHECK(worst < 1.0f);
}
