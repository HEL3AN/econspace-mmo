#include <doctest/doctest.h>

#include <cmath>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "core/Fitting.h"
#include "core/Prediction.h"
#include "core/ShipDesign.h"
#include "entities/Ship.h"
#include "entities/ShipType.h"
#include "entities/Station.h"
#include "sim/ClientSession.h"
#include "sim/Orders.h"
#include "sim/PlayerStep.h"
#include "sim/Protocol.h"
#include "sim/Simulation.h"

// The client's prediction against the server's simulation, fed the same commands (#358).
//
// Sim::StepPlayerShip has two callers on purpose, and prediction breaks the moment they
// compute different results from the same input -- silently: the ship is pulled back a
// little at every snapshot and nothing says why (#331 predicted every hull as a Scout).
// These tests are a server and a client in one process, with the wire between them: the
// client numbers and predicts every command, the server steps it some ticks later, the
// snapshot goes through the encoder and back, and the client reconciles and replays.
//
// What they assert is the invariant itself: where the client predicted its ship after
// command N is where the server put it after command N, and a snapshot never moves a
// prediction it agrees with.
namespace
{
// Same process, same compiler, same code: the two should agree to the bit. The tolerance
// is for the JSON a snapshot is written in, not for any slack in the rule.
constexpr float AGREE = 0.01f;

float Dist(Vector2 a, Vector2 b)
{
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y));
}

float Len(Vector2 v)
{
    return std::sqrt(v.x * v.x + v.y * v.y);
}

struct Link
{
    Simulation     sim;
    ClientSession& s;
    // The client's own ship, as Game holds it: the starter at a placeholder until the
    // first snapshot says where it is and what it flies.
    Ship       client{ Vector2{ 0.0f, 3000.0f }, GetShipCatalog()[0].stats };
    Prediction prediction;
    // Where this client thinks the object it holds station on is; null when nothing.
    Prediction::HoldFn clientHold;

    std::deque<Proto::Command> wire;  // sent, not yet stepped by the server
    int                        acked = 0;
    bool                       world = false;  // step the world between commands
    std::set<std::string>      designsSent;    // what the host told this client (#279)

    // seq -> where each side had the ship right after that command.
    std::map<int, Vector2> predicted, served;

    Link() : s(Make(sim)) {}

    static ClientSession& Make(Simulation& sim)
    {
        Factions::Load(std::string(TEST_DATA_DIR) + "factions.json");
        Archetypes::Load(std::string(TEST_DATA_DIR) + "archetypes.json");
        Blueprints::Load(std::string(TEST_DATA_DIR) + "blueprints.json");
        sim.LoadUniverse(std::string(TEST_WORLD_DIR) + "universe.json");
        sim.Seed(1234u);
        sim.InitGalaxy();
        sim.MaterializeAllSystems(std::string(TEST_WORLD_DIR) + "systems/");
        // Open space: nothing near the ship to dock at, hold, or collide with.
        return sim.CreateSession(sim.Universe().startId, Vector2{ 150000.0f, 150000.0f },
                                 GetShipCatalog()[0].stats);
    }

    // The client's fixed tick: number it, predict it, put it on the wire.
    void Send(const Proto::Command& c)
    {
        const Proto::Command& sent = prediction.Predict(client, c, clientHold);
        predicted[sent.seq] = client.GetPosition();
        wire.push_back(sent);
    }

    // The server takes the next n commands off the wire, one player tick each.
    void Serve(size_t n)
    {
        for (size_t i = 0; i < n && !wire.empty(); i++)
        {
            const Proto::Command c = wire.front();
            wire.pop_front();
            // What the host does with a command: a whole tick, or only its intents while a
            // standing order flies the ship (#432).
            sim.StepPlayerShip(s, c, 1.0f, Simulation::CommandDt(s, Sim::SIM_DT));
            if (world)
                WorldTick();
            acked = c.seq;
            served[c.seq] = s.ship->GetPosition();
        }
    }

    // What the host's world tick does to a session, minus NPCs taking notice of it.
    void WorldTick()
    {
        static const std::vector<Simulation::PlayerPresence> nobody;
        for (auto& kv : sim.Systems())
            if (Simulation::IsHot(kv.second))
                sim.StepSystemAgents(kv.second, nobody, nullptr, Sim::SIM_DT);
        sim.StepPlayerAttachment(s, Sim::SIM_DT);
        sim.StepPlayerOrder(s, *sim.SystemOf(s), Sim::SIM_DT);
        sim.MaintainWorld(Sim::SIM_DT);
    }

    // A snapshot, through the encoder as the host sends it, and reconciled. Design
    // definitions go first, as the host sends them (#279 fitting). Returns how far the
    // reconcile moved the client's ship: a prediction the server agrees with is not moved.
    float Snapshot()
    {
        Proto::Snapshot snap = sim.BuildSnapshot(s, s.systemId);
        snap.player.lastInput = acked;
        for (const Proto::DesignDef& d : sim.DesignDefsFor(snap, designsSent))
        {
            Proto::DesignDef back;
            REQUIRE(Proto::DecodeDesignDef(Proto::EncodeDesignDef(d), back));
            std::string why;
            Archetypes::AddDesign(back.design, why);  // one process: the server filed it too
        }
        Proto::Snapshot back;
        REQUIRE(Proto::DecodeSnapshot(Proto::EncodeSnapshot(snap), back));
        const Vector2 before = client.GetPosition();
        prediction.Reconcile(client, back.player, clientHold);
        return Dist(before, client.GetPosition());
    }

    // Every command both sides have stepped: the prediction was where the server put it.
    float WorstDisagreement() const
    {
        float worst = 0.0f;
        for (const auto& kv : served)
        {
            auto it = predicted.find(kv.first);
            if (it != predicted.end())
                worst = std::max(worst, Dist(it->second, kv.second));
        }
        return worst;
    }
};

// A client `lag` commands ahead of the server, a snapshot every `every` server ticks.
// Runs `ticks` commands from `make(i)` and then lets the server catch up.
template <typename Make>
float Fly(Link& l, int ticks, size_t lag, int every, Make make, float* worstSnap = nullptr)
{
    float jump = 0.0f;
    for (int i = 0; i < ticks; i++)
    {
        l.Send(make(i));
        if (l.wire.size() > lag)
            l.Serve(l.wire.size() - lag);
        if (i % every == 0)
            jump = std::max(jump, l.Snapshot());
    }
    l.Serve(l.wire.size());
    jump = std::max(jump, l.Snapshot());
    if (worstSnap != nullptr)
        *worstSnap = jump;
    return l.WorstDisagreement();
}

Proto::Command Thrust(float turn = 0.0f)
{
    Proto::Command c;
    c.thrust = true;
    c.turn = turn;
    return c;
}
}  // namespace

TEST_CASE("prediction: plain flight is where the server puts it")
{
    Link l;
    l.Snapshot();  // where the ship is, before anything is sent
    CHECK(Dist(l.client.GetPosition(), l.s.ship->GetPosition()) < AGREE);

    float       jump = 0.0f;
    const float worst = Fly(l, 300, 6, 3, [](int) { return Thrust(); }, &jump);
    CHECK(worst < AGREE);
    CHECK(jump < AGREE);
    CHECK(l.prediction.Pending() == 0);
    CHECK(Len(l.s.ship->GetVelocity()) > 10.0f);  // it did fly
    CHECK(Dist(l.client.GetPosition(), l.s.ship->GetPosition()) < AGREE);
}

TEST_CASE("prediction: turning, braking and coasting agree")
{
    Link l;
    l.Snapshot();
    float       jump = 0.0f;
    const float worst = Fly(
        l, 600, 6, 3,
        [](int i)
        {
            Proto::Command c;
            c.thrust = i < 200 || (i > 350 && i < 450);
            c.turn = i < 120 ? 1.0f : (i < 240 ? -0.5f : 0.0f);
            c.brake = i >= 450 && i < 500;
            c.toggleStabilizer = i == 300;  // Newtonian for a while, then back
            c.toggleStabilizer = c.toggleStabilizer || i == 520;
            return c;
        },
        &jump);
    CHECK(worst < AGREE);
    CHECK(jump < AGREE);
    CHECK(std::fabs(l.client.GetHeading() - l.s.ship->GetHeading()) < 1e-4f);
}

TEST_CASE("prediction: a warp ordered by the client is flown the same on both sides")
{
    Link l;
    l.Snapshot();
    const Vector2 from = l.s.ship->GetPosition();
    const Vector2 to = { from.x - 400000.0f, from.y + 120000.0f };
    float         jump = 0.0f;
    // Ordered once, then nothing but empty ticks: the warp aligns, accelerates, slows and
    // drops out -- all of it decided by whoever ordered it (#160) and only flown after.
    const float worst = Fly(
        l, 900, 9, 3,
        [&](int i)
        {
            Proto::Command c;
            if (i == 0)
            {
                c.navMode = 2;
                c.navTarget = to;
                c.navStopDist = 500.0f;
            }
            return c;
        },
        &jump);
    CHECK_FALSE(l.s.ship->IsWarping());
    CHECK(Dist(l.s.ship->GetPosition(), to) < 5000.0f);  // it arrived
    CHECK(worst < AGREE);
    CHECK(jump < AGREE);
}

// #432: a human client sends a command every tick, standing order or not, and the order
// steps the ship on the world tick. The command used to step it too -- twice a tick on the
// server, once on the client, and a warp the server flew at double rate was pulled back at
// every snapshot. Now the command carries its intents only, and both sides agree.
TEST_CASE("prediction: a ship flying a standing order is stepped once a tick on both sides")
{
    Link l;
    l.world = true;
    l.Snapshot();
    const Vector2 from = l.s.ship->GetPosition();
    Orders::Order o;
    o.kind = Orders::Kind::MoveTo;
    o.point = { from.x - 400000.0f, from.y + 120000.0f };
    o.useWarp = true;
    l.sim.GiveOrder(l.s, o);
    const auto idle = [](int) { return Proto::Command{}; };

    // Until a snapshot says so, the client cannot know the server engaged the warp; after
    // that, every tick of it is predicted where the server flies it.
    Fly(l, 30, 6, 3, idle);
    REQUIRE(l.s.HasRunningOrder());
    l.predicted.clear();
    l.served.clear();
    float       jump = 0.0f;
    const float worst = Fly(l, 600, 6, 3, idle, &jump);
    CHECK(worst < AGREE);
    CHECK(jump < AGREE);
    CHECK(Dist(l.s.ship->GetPosition(), from) > 100000.0f);  // it did warp
    // A command in the middle of an order moves nothing by itself.
    if (l.s.HasRunningOrder())
    {
        const Vector2 before = l.s.ship->GetPosition();
        l.sim.StepPlayerShip(l.s, Proto::Command{}, 1.0f, Simulation::CommandDt(l.s, Sim::SIM_DT));
        CHECK(Dist(before, l.s.ship->GetPosition()) == 0.0f);
    }
}

TEST_CASE("prediction: an ack arriving late is replayed without a snap")
{
    Link l;
    l.Snapshot();
    // Sixty commands go out and the server is silent -- a stall, a slow link -- then it
    // takes them in two bursts, and each snapshot acknowledges only what it has done.
    for (int i = 0; i < 60; i++)
        l.Send(Thrust(i < 30 ? 0.6f : 0.0f));
    const Vector2 ahead = l.client.GetPosition();
    CHECK(l.prediction.Pending() == 60);

    l.Serve(20);
    CHECK(l.Snapshot() < AGREE);  // forty replayed on top: the same place
    CHECK(l.prediction.Pending() == 40);
    CHECK(Dist(l.client.GetPosition(), ahead) < AGREE);

    // The second burst: again only part of what is pending.
    l.Serve(25);
    CHECK(l.Snapshot() < AGREE);
    CHECK(l.prediction.Pending() == 15);

    l.Serve(15);
    CHECK(l.Snapshot() < AGREE);
    CHECK(l.prediction.Pending() == 0);
    CHECK(l.WorstDisagreement() < AGREE);
    CHECK(Dist(l.client.GetPosition(), l.s.ship->GetPosition()) < AGREE);
}

TEST_CASE("prediction: another hull is predicted with its own numbers (#331)")
{
    Link l;
    REQUIRE(GetShipCatalog().size() > 1);
    const int other = 1;
    REQUIRE(GetShipCatalog()[other].stats.maxSpeed != GetShipCatalog()[0].stats.maxSpeed);
    l.s.account.SetMoney(GetShipCatalog()[other].price * 2.0);
    REQUIRE(l.sim.BuyShip(l.s, other));
    l.Snapshot();
    CHECK(l.client.GetStats().design == GetShipCatalog()[other].stats.design);

    float       jump = 0.0f;
    const float worst =
        Fly(l, 400, 6, 3, [](int i) { return Thrust(i < 60 ? 0.4f : 0.0f); }, &jump);
    CHECK(worst < AGREE);
    CHECK(jump < AGREE);
}

TEST_CASE("prediction: a refitted design is predicted with the refit's numbers (#378)")
{
    Link           l;
    const Station* yard = nullptr;
    for (const auto& e : l.sim.SystemOf(l.s)->entities)
        if (e->GetKind() == EntityKind::Station && e->GetArchetype() != nullptr &&
            e->GetArchetype()->Has(Component::Shipyard))
            yard = static_cast<const Station*>(e.get());
    REQUIRE_MESSAGE(yard != nullptr, "the start system has a shipyard");

    // The starter without its cargo pod: lighter, so it flies differently from the stock
    // Scout -- which is what the client would predict if it did not know the refit.
    const Ships::Design*        stock = Archetypes::ShipCatalogue().FindDesign("scout");
    std::vector<Ships::FitLine> fit;
    for (const Ships::FitLine& line : Ships::Normalized(stock->fit))
        if (line.module != "hull.cargo")
        {
            fit.push_back(line);
            fit.back().scale = 0.0f;
        }
    l.s.account.SetMoney(5000.0);
    l.s.dockedStationId = yard->GetId();  // at the yard, as fitting_tests does it
    REQUIRE(l.sim.RefitShip(l.s, fit));
    l.s.dockedStationId = 0;
    const std::string id = l.s.DesignOf(0);
    REQUIRE(Ships::IsPlayerDesign(id));
    const ShipStats& refit = l.s.ship->GetStats();
    const ShipStats& scout = GetShipCatalog()[0].stats;
    REQUIRE((refit.maxSpeed != scout.maxSpeed || refit.thrustPower != scout.thrustPower));

    l.Snapshot();
    CHECK(l.client.GetStats().design == id);
    CHECK(l.client.GetStats().maxSpeed == doctest::Approx(refit.maxSpeed));

    float       jump = 0.0f;
    const float worst =
        Fly(l, 400, 6, 3, [](int i) { return Thrust(i < 60 ? -0.4f : 0.0f); }, &jump);
    CHECK(worst < AGREE);
    CHECK(jump < AGREE);
}

TEST_CASE("prediction: a standing hold differs by no more than the render delay (#157)")
{
    Link l;
    l.world = true;
    // Something that moves: a station on an orbit, placed by the world clock (#210).
    const Entity* target = nullptr;
    for (const auto& e : l.sim.SystemOf(l.s)->entities)
        if (e->GetKind() == EntityKind::Station)
        {
            target = e.get();
            break;
        }
    REQUIRE(target != nullptr);
    const int id = target->GetId();

    // The ship starts beside it, so the hold is flown rather than approached for minutes.
    l.s.ship->ApplyView({ target->GetPosition().x + 3000.0f, target->GetPosition().y }, 0.0f,
                        { 0.0f, 0.0f }, l.s.ship->GetHull(), l.s.ship->GetShields());

    // The client sees the target the way Game::HoldTarget does: from its interpolated
    // proxy, `delay` ticks behind the server, with a velocity from two snapshots.
    const int            delay = 6;  // 0.1 s, the render delay
    std::vector<Vector2> track;      // the server's target, one entry per world tick
    Sim::HoldTarget      seen;
    l.clientHold = [&]() -> const Sim::HoldTarget*
    {
        if (l.client.GetHoldMode() == HoldMode::None || track.size() < (size_t)delay + 4)
            return nullptr;
        const size_t now = track.size() - 1 - delay;
        seen.pos = track[now];
        seen.vel = { (track[now].x - track[now - 3].x) / (3 * Sim::SIM_DT),
                     (track[now].y - track[now - 3].y) / (3 * Sim::SIM_DT) };
        return &seen;
    };
    auto observe = [&]()
    {
        for (const auto& e : l.sim.SystemOf(l.s)->entities)
            if (e->GetId() == id)
                track.push_back(e->GetPosition());
    };
    for (int i = 0; i < delay + 4; i++)
    {
        l.WorldTick();
        observe();
    }
    l.Snapshot();

    float                speed = 0.0f;  // the fastest the target went
    const int            ticks = 900;
    const int            lag = 6;
    float                jump = 0.0f;
    const Proto::Command order = [&]
    {
        Proto::Command c;
        c.navMode = 4;  // keep at range
        c.navHoldId = id;
        c.navRange = 1500.0f;
        return c;
    }();
    for (int i = 0; i < ticks; i++)
    {
        l.Send(i == 0 ? order : Proto::Command{});
        const size_t before = l.served.size();
        if (l.wire.size() > (size_t)lag)
            l.Serve(l.wire.size() - lag);
        for (size_t k = before; k < l.served.size(); k++)
            observe();
        if (track.size() > 1)
            speed = std::max(speed,
                             Dist(track[track.size() - 1], track[track.size() - 2]) / Sim::SIM_DT);
        if (i % 3 == 0)
            jump = std::max(jump, l.Snapshot());
    }
    CHECK(l.s.ship->GetHoldMode() == HoldMode::Keep);
    CHECK(Dist(l.s.ship->GetPosition(), track.back()) < 3000.0f);  // it is holding

    // The documented difference: the client aims at where the target was a render delay
    // ago, and replays its unacknowledged commands at that one stale point. So the ring it
    // flies to is off by the distance the target covers in the delay plus the replay window,
    // and the ship can be no further off than that. Reconciliation takes the rest away at
    // every snapshot, which is why it does not accumulate.
    const float window = (delay + lag + 3) * Sim::SIM_DT;
    const float bound = speed * window + AGREE;
    const float worst = l.WorstDisagreement();
    INFO("hold: target " << speed << " u/s, worst " << worst << ", snap " << jump << ", bound "
                         << bound);
    CHECK(speed > 0.0f);
    CHECK(worst <= bound);
    CHECK(jump <= bound);
}
