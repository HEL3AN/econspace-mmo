// EconSpace — headless galaxy server (track M, M3).
//
// Runs the SAME simulation as the game, but without rendering/window/input: real
// agents (NpcShip) move and fight, the spawn director holds the population, and
// macrodynamics change security/economy/territory control. This is the authoritative
// server — no client (Game) sits on top yet, but the logic is one (Simulation).
//
// Run: econserver [ticks]   (default 3600 ticks = 60 s at SIM_DT=1/60).

#include "sim/Protocol.h"
#include "sim/Simulation.h"
#include "sim/ClientSession.h"
#include "net/Tcp.h"
#include "net/Transport.h"
#include "core/Faction.h"
#include "entities/Combatant.h"
#include "entities/Nebula.h"
#include "entities/NpcShip.h"
#include "entities/Ship.h"
#include "entities/ShipType.h"
#include "raylib.h"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <memory>
#include <string>
#include <thread>

// How often the galaxy is written to world.json while the server runs (seconds).
static const double WORLD_SAVE_INTERVAL = 60.0;

// Set up the authoritative simulation for the server host: load the world, materialize
// all systems, create the player ship in the start system.
//
// worldPath non-empty: restore the galaxy from it if the file exists. LoadWorld does its
// own Reset+InitGalaxy and restores only the aggregates (population, security, prosperity,
// controllers) plus simulation time. Entities are never persisted, so materialization
// happens afterwards either way. An empty worldPath keeps a run ephemeral (the self-tests).
static ClientSession& SetupHostSim(Simulation& sim, const std::string& dataDir,
                                   const std::string& worldPath = std::string())
{
    SetRandomSeed(0xC0FFEEu);
    Factions::Load(dataDir + "factions.json");
    // Must precede materialization: every entity constructor looks itself up here, and a
    // world whose objects have no components is a world where nothing can be docked with.
    if (!Archetypes::Load(dataDir + "archetypes.json"))
        fprintf(stderr, "FATAL: %s\n", Archetypes::Error().c_str());
    sim.LoadUniverse(dataDir + "universe.json");
    sim.Seed(0xC0FFEEu);
    if (worldPath.empty() || !sim.LoadWorld(worldPath))
        sim.InitGalaxy();
    else
        printf("World restored from %s (t=%.0fs).\n", worldPath.c_str(), sim.Time());
    sim.MaterializeAllSystems(dataDir + "systems/");
    sim.Activate(sim.Universe().startId);
    return sim.CreateSession(Vector2{ 0.0f, 3000.0f }, GetShipCatalog()[0].stats);
}

// One client input = ONE player tick. This is critical for client and server
// prediction to match: the client predicts one StepPlayerShip per input, and the
// server must step the same way (otherwise the ack numbers diverge from the step count
// and the ship "jerks"). Applies movement/jump/loot/combat/mining for command c.
// Returns true if the system changed. Account effects are not yet applied on the server
// (M4f); NPCs do not target the player yet.
static bool HostStepPlayer(Simulation& sim, ClientSession& s, const Proto::Command& c,
                           std::string& activeId, float dt, std::vector<Proto::TradeAck>& acks,
                           std::vector<FireEvent>& fires)
{
    bool changed = false;

    // Dock/undock (server-authoritative). While docked we do not step the player's
    // physics (the ship is frozen at the station), but we still handle trade/undock.
    if (c.dock && !s.IsDocked())
        sim.StepPlayerDock(s, sim.Active());
    if (c.undock)
        sim.StepPlayerUndock(s);

    // Debug (F1): credit money to the server account.
    if (c.debugMoney)
        s.account.AddMoney(1000.0);

    // Station trade/hangar: sell, refit, buy, pay off bounty. Account effects
    // (money/skill/reputation/bounty) are applied by the core in account_ (server-authoritative).
    if (s.IsDocked())
    {
        if (c.sellType >= 0 && c.sellAmount > 0)
        {
            Simulation::PlayerSellResult r =
                sim.StepPlayerSell(s, sim.Active(), c.sellType, c.sellAmount);
            if (r.sold > 0)
                acks.push_back(Proto::TradeAck{ c.sellType, r.sold, r.gross, r.revenue });
        }
        if (c.refitShip >= 0 && c.refitShip < (int)GetShipCatalog().size())
            sim.RefitPlayer(s, GetShipCatalog()[c.refitShip].stats);  // switch to a bought ship
        if (c.buyShip >= 0)
            sim.BuyShip(s, c.buyShip);  // purchase (deducts money)
        if (c.payBountyFaction >= 0)
            sim.PayBounty(s, (FactionId)c.payBountyFaction);  // clear bounty
        if (c.acceptOffer >= 0)
            s.missions.Accept(c.acceptOffer);  // accept a mission from the board
        if (c.completeMission >= 0)
            sim.CompleteMission(s, c.completeMission);  // hand in a mission (reward to account)
        return changed;  // at a station the ship neither moves nor fires
    }

    // Manual control takes the ship back. This is the same rule the autopilot follows,
    // and it is what lets a human grab the stick from an agent mid-order.
    if (s.HasRunningOrder() && (c.thrust || c.brake || c.turn != 0.0f || c.navMode != 0))
        sim.AbortOrder(s, "manual control");

    // A standing order arriving over the wire (#72). It is an intent like any other: the
    // server validates it and owns the execution.
    if (c.abortOrder)
        sim.AbortOrder(s, "aborted by client");
    if (c.orderKind != 0)
    {
        Orders::Order o;
        o.kind = (Orders::Kind)c.orderKind;
        o.targetId = c.orderTarget;
        o.point = c.orderPoint;
        o.stopDist = c.orderStopDist;
        o.useWarp = c.orderWarp;
        o.untilFull = c.orderUntilFull;
        o.destSystem = c.orderDestSystem;
        sim.GiveOrder(s, o);
    }

    if (c.toggleWeapon)
        s.ToggleWeapon();

    sim.StepPlayerShip(s, c, 1.0f, dt);

    if (c.jumpGateId != 0)
    {
        std::string dest = sim.JumpGateDestIfNear(s, sim.Active(), c.jumpGateId);
        if (!dest.empty())
        {
            sim.ServerEnterSystem(s, dest, activeId);
            activeId = sim.ActiveId();
            changed = true;
        }
    }
    if (c.lootId != 0)
        sim.StepPlayerLoot(s, sim.Active(), c.lootId);

    // Player fire: on a shot — a beam event into the snapshot (client draws it blue).
    // Account effects (mission credit/reputation) are not yet applied on the server (3c-ii).
    Simulation::PlayerCombatEvents ev;
    if (sim.StepPlayerFire(s, sim.Active(), c.targetId, dt, &ev))
        fires.push_back(FireEvent{ ev.shotFrom, ev.shotTo, FactionId::Independent, false, true });
    sim.StepPlayerMining(s, sim.Active(), s.account.GetSkills().GetBonus(SkillType::Mining), dt);
    return changed;
}

// World step (NPCs of all systems + maintenance) — on the server timer, independent of inputs.
// The active system (where the player is) is stepped with the player involved: NPCs see him,
// combat produces beams, damage to the player is authoritative; a fallen player respawns at a
// station. Other systems get a background step. fires — active-system NPC shots (client draws
// beams). clientOnline false: nobody is flying the ship. The pilot is treated as absent — NPCs do
// not target them, and no respawn logic runs — while the rest of the galaxy keeps living. The ship
// simply stops being stepped and stays where it was left.
static void HostStepWorld(Simulation& sim, ClientSession& s, const std::string& activeId, float dt,
                          std::vector<FireEvent>& fires, bool clientOnline)
{
    Combatant* player = (!clientOnline || s.IsDocked()) ? nullptr : (Combatant*)s.ship.get();

    // Cover: the player sits inside something that hides ships, so NPCs cannot see him.
    // The nebula is the only such thing today, but the pass asks for the property rather
    // than for the class (#34), so a jammer a player builds (#44) would hide him too.
    bool hidden = false;
    if (player != nullptr)
    {
        Vector2 pp = s.ship.get()->GetPosition();
        for (auto& e : sim.Active().entities)
        {
            if (!e->Has(Component::Hazard) || !e->GetArchetype()->hazardHidesShips)
                continue;
            // radius 0 in the archetype means "as large as the object itself".
            float r = e->GetArchetype()->hazardRadius > 0.0f ? e->GetArchetype()->hazardRadius
                                                             : e->GetSize();
            float dx = pp.x - e->GetPosition().x;
            float dy = pp.y - e->GetPosition().y;
            if (dx * dx + dy * dy <= r * r)
            {
                hidden = true;
                break;
            }
        }
    }
    // Server-side predicate of hostility toward the player by account (M4f): pirates;
    // factions where the player is wanted; factions with Hostile/Hated reputation. Police
    // now react to a wanted player — the account lives on the server.
    auto hostile = [&sim, &s](const NpcShip* n)
    { return sim.AccountHostileToFaction(s, n->GetFaction()); };

    for (auto& kv : sim.Systems())
    {
        if (player != nullptr && kv.first == activeId)
            sim.StepActiveSystemAgents(kv.second, player, hidden, hostile, &fires, dt);
        else
            sim.StepSystemAgents(kv.second, dt);
        kv.second.market.Update(dt);  // market price recovery (in single-player — UpdateAmbient)
    }

    if (player != nullptr && !s.ship.get()->IsAlive())
        sim.ServerRespawnPlayer(s);

    // Standing orders execute here rather than in the input path: an agent issues one
    // order and then sends nothing, so if the order did not drive its own ticks the ship
    // would simply sit there.
    if (clientOnline)
        sim.StepPlayerOrder(s, sim.Active(), dt);

    // Account passive: piloting xp (in flight) + bounty decay.
    sim.StepPlayerAccountTick(s, dt);

    sim.MaintainWorld(dt, activeId, nullptr);
}

// Receives client inputs from the transport and applies EACH as one player tick
// (HostStepPlayer). lastSeq — the highest processed input number (ack to the client).
// Returns true if the layout must be resent (system change).
static bool HostDrainInputs(ITransport& conn, Simulation& sim, ClientSession& s,
                            std::string& activeId, float dt, int& lastSeq,
                            std::vector<Proto::TradeAck>& acks, std::vector<FireEvent>& fires)
{
    bool        layoutDirty = false;
    std::string msg;
    while (conn.Poll(msg))
    {
        if (Proto::MessageType(msg) != "cmd")
            continue;
        Proto::Command c;
        if (!Proto::DecodeCommand(msg, c))
            continue;
        if (c.seq > lastSeq)
            lastSeq = c.seq;
        if (HostStepPlayer(sim, s, c, activeId, dt, acks, fires))
            layoutDirty = true;
    }
    return layoutDirty;
}

// Real server: listens on a port, accepts one client, runs the authoritative
// real-time loop (receive Command -> step the world -> send Snapshot; Layout on
// entering/changing a system). Run: econserver host [port].
// Ctrl+C / termination request. The handler only raises a flag; saving happens on the
// way out of the main loop, where touching files is safe.
static volatile std::sig_atomic_t g_stopRequested = 0;
static void                       OnInterrupt(int)
{
    g_stopRequested = 1;
}

// Real server: listens on a port and runs the authoritative real-time loop. The galaxy is
// simulated whether or not anyone is connected -- that is the premise of a living world,
// and it is what lets a player reconnect to a galaxy that moved on without them. A client
// session is an episode inside that loop, not the loop itself.
// Run: econserver host [port].
static int RunHost(unsigned short port)
{
    std::string    dataDir = SIM_DATA_DIR;
    std::string    worldPath = std::string(GetApplicationDirectory()) + "world.json";
    Simulation     sim;
    ClientSession& s = SetupHostSim(sim, dataDir, worldPath);

    // Account persistence (M4f-3): load the player's progress next to the server.
    // Only in the real server -- hosttest/selftest stay clean (ephemeral).
    std::string acctPath = std::string(GetApplicationDirectory()) + "account.json";
    if (sim.LoadAccount(s, acctPath))
        printf("Account loaded (money %.0f).\n", s.account.GetMoney());
    else
        printf("New account (money %.0f).\n", s.account.GetMoney());

    if (!Net::Startup())
    {
        printf("host: WSAStartup FAIL\n");
        return 1;
    }
    Net::TcpListener listener;
    if (!listener.Listen(port))
    {
        printf("host: listen on port %d FAIL\n", port);
        Net::Shutdown();
        return 1;
    }
    std::signal(SIGINT, OnInterrupt);
    std::signal(SIGTERM, OnInterrupt);
    printf("EconSpace server on port %d. The galaxy runs with or without a client; "
           "Ctrl+C to stop.\n",
           port);

    std::unique_ptr<Net::TcpConnection> conn;
    std::string                         activeId = sim.ActiveId();

    int         lastSeq = 0;         // last processed input number (ack to the client)
    int         lastEventSeq = 0;    // last journal entry delivered to this session
    bool        wasDocked = false;   // for account checkpoints when entering/leaving a station
    double      worldSaveAcc = 0.0;  // world checkpoint timer
    const float dt = 1.0f / 60.0f;
    using clock = std::chrono::steady_clock;
    auto   prev = clock::now();
    double acc = 0.0;
    double galaxyAcc = 0.0;  // galaxy-snapshot broadcast timer (map statistics)

    while (g_stopRequested == 0)
    {
        auto   now = clock::now();
        double frame = std::chrono::duration<double>(now - prev).count();
        prev = now;
        acc += frame;
        if (acc > 0.25)
            acc = 0.25;

        // Take a waiting client, if any. Session state is reset here rather than at
        // startup, so a reconnect does not inherit the previous session's input
        // numbering or weapon toggle.
        if (!conn)
        {
            conn = listener.Accept();
            if (conn)
            {
                lastSeq = 0;
                lastEventSeq = s.LastEventSeq();  // a new session starts from now
                wasDocked = s.IsDocked();
                activeId = sim.ActiveId();
                conn->Send(Proto::EncodeLayout(sim.BuildLayout(activeId)));
                conn->Send(Proto::EncodeGalaxy(sim.BuildGalaxyState()));
                printf("Client connected.\n");
            }
        }

        // Player: each received input = one tick (1:1 with client prediction).
        // acks -- trade acknowledgments, fires -- shots (player+NPC) for this iteration;
        // both are delivered to the client in the snapshot.
        std::vector<Proto::TradeAck> acks;
        std::vector<FireEvent>       fires;
        bool                         layoutDirty = false;
        if (conn)
        {
            layoutDirty = HostDrainInputs(*conn, sim, s, activeId, dt, lastSeq, acks, fires);

            // Account checkpoint on a dock-state change: on docking -- commit what was
            // earned in flight (loot/bounty), on undocking -- trade and mission hand-ins.
            if (s.IsDocked() != wasDocked)
            {
                wasDocked = s.IsDocked();
                sim.SaveAccount(s, acctPath);
            }
        }

        // World (NPCs): on the server timer, independent of inputs and of whether
        // anyone is watching.
        while (acc >= dt)
        {
            HostStepWorld(sim, s, activeId, dt, fires, conn != nullptr);
            acc -= dt;
        }

        if (conn)
        {
            if (layoutDirty)
                conn->Send(Proto::EncodeLayout(sim.BuildLayout(activeId)));
            Proto::Snapshot snap = sim.BuildSnapshot(s, activeId);
            snap.player.lastInput = lastSeq;  // ack for client reconciliation
            snap.tradeAcks = std::move(acks);
            snap.fires = std::move(fires);
            // Everything the player has not seen yet; the client acknowledges by seq.
            snap.events = s.EventsSince(lastEventSeq);
            if (!snap.events.empty())
                lastEventSeq = snap.events.back().seq;
            conn->Send(Proto::EncodeSnapshot(snap));
        }

        // Galaxy snapshot -- rarely (once a second): the statistics change slowly and the
        // message is large (all systems). The client's galaxy map lives off it.
        galaxyAcc += frame;
        if (galaxyAcc >= 1.0)
        {
            galaxyAcc = 0.0;
            if (conn)
                conn->Send(Proto::EncodeGalaxy(sim.BuildGalaxyState()));
        }

        // World checkpoint. The galaxy drifts continuously (security, prosperity,
        // territory control), so unlike the account there is no natural commit point to
        // hang this on: a timer is what keeps a crash from costing more than a minute.
        worldSaveAcc += frame;
        if (worldSaveAcc >= WORLD_SAVE_INTERVAL)
        {
            worldSaveAcc = 0.0;
            sim.SaveWorld(worldPath);
        }

        // A disconnect ends the session, not the server.
        if (conn && !conn->Alive())
        {
            sim.SaveAccount(s, acctPath);
            conn.reset();
            printf("Client disconnected. Account saved (money %.0f); galaxy still running.\n",
                   s.account.GetMoney());
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }

    if (conn)
        sim.SaveAccount(s, acctPath);
    sim.SaveWorld(worldPath);
    printf("\nShutting down. World and account saved.\n");
    Net::Shutdown();
    return 0;
}

// Host smoke test (no sockets): runs a host tick through LocalTransport. The client
// sends a thrust command, the server steps the world and sends layout+snapshot; we
// check that the layout arrived and the player moved. Run: econserver hosttest.
static int HostSelftest()
{
    std::string    dataDir = SIM_DATA_DIR;
    Simulation     sim;
    ClientSession& s = SetupHostSim(sim, dataDir);

    LocalTransport link;
    std::string    activeId = sim.ActiveId();
    link.Server().Send(Proto::EncodeLayout(sim.BuildLayout(activeId)));

    Proto::Command thrust;
    thrust.thrust = true;
    link.Client().Send(Proto::EncodeCommand(thrust));

    Vector2 startPos = s.ship.get()->GetPosition();

    int         lastSeq = 0;
    const float dt = 1.0f / 60.0f;
    for (int i = 0; i < 120; i++)  // ~2 s of simulation
    {
        thrust.seq = i + 1;  // client: numbered input
        link.Client().Send(Proto::EncodeCommand(thrust));
        std::vector<Proto::TradeAck> acks;
        std::vector<FireEvent>       fires;
        HostDrainInputs(link.Server(), sim, s, activeId, dt, lastSeq, acks,
                        fires);                            // 1 input=1 tick
        HostStepWorld(sim, s, activeId, dt, fires, true);  // world
        Proto::Snapshot snap = sim.BuildSnapshot(s, activeId);
        snap.player.lastInput = lastSeq;
        link.Server().Send(Proto::EncodeSnapshot(snap));
    }

    bool            gotLayout = false, gotSnap = false;
    Proto::Snapshot lastSnap;
    std::string     msg;
    while (link.Client().Poll(msg))
    {
        std::string t = Proto::MessageType(msg);
        if (t == "layout")
            gotLayout = true;
        else if (t == "snap" && Proto::DecodeSnapshot(msg, lastSnap))
            gotSnap = true;
    }
    Vector2 endPos = s.ship.get()->GetPosition();
    bool    moved = (endPos.x != startPos.x || endPos.y != startPos.y);
    bool    snapHasWorld = gotSnap && !lastSnap.entities.empty();

    printf("Host selftest: layout %s, snapshot %s, player-moved %s\n", gotLayout ? "OK" : "FAIL",
           snapHasWorld ? "OK" : "FAIL", moved ? "OK" : "FAIL");
    return (gotLayout && snapHasWorld && moved) ? 0 : 1;
}

// Account persistence smoke test (no network, M4f-3): write the account to a file and
// read it into a clean simulation, compare the fields. Run: econserver accttest.
static int AccountSelftest()
{
    auto approx = [](double x, double y)
    {
        double d = x - y;
        return (d < 0 ? -d : d) < 1e-3;
    };
    std::string path = std::string(GetApplicationDirectory()) + "accttest_tmp.json";

    // The account belongs to a session, so the test needs one on each side (#3). No
    // world is loaded: persistence must not depend on where the ship happens to be.
    Simulation     a;
    ClientSession& as = a.CreateSession(Vector2{ 0.0f, 0.0f }, GetShipCatalog()[0].stats);
    as.account.SetMoney(4242.0);
    as.account.SetReputation(FactionId::Pirates, -7.5f);
    as.account.SetBounty(FactionId::TradersGuild, 300.0);
    as.account.GetSkills().SetXp(SkillType::Mining, 555.0f);
    a.SaveAccount(as, path);

    Simulation     b;  // clean account (money 500) — check that the load overwrote it
    ClientSession& bs = b.CreateSession(Vector2{ 0.0f, 0.0f }, GetShipCatalog()[0].stats);
    bool           loaded = b.LoadAccount(bs, path);
    bool           money = approx(bs.account.GetMoney(), 4242.0);
    bool           rep = approx(bs.account.GetReputation(FactionId::Pirates), -7.5);
    bool           bounty = approx(bs.account.GetBounty(FactionId::TradersGuild), 300.0);
    bool           skill = approx(bs.account.GetSkills().GetXp(SkillType::Mining), 555.0);
    std::remove(path.c_str());

    bool ok = loaded && money && rep && bounty && skill;
    printf("Account selftest: load %s, money %s, rep %s, bounty %s, skill %s => %s\n",
           loaded ? "OK" : "FAIL", money ? "OK" : "FAIL", rep ? "OK" : "FAIL",
           bounty ? "OK" : "FAIL", skill ? "OK" : "FAIL", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

// World persistence smoke test (no network): drift a system's aggregate, write the galaxy
// to a file, read it back into a clean simulation and compare. Guards the property the
// living galaxy depends on -- that a server restart does not reset the world.
// Run: econserver worldtest.
static int WorldSelftest()
{
    auto approx = [](double x, double y)
    {
        double d = x - y;
        return (d < 0 ? -d : d) < 1e-3;
    };
    std::string dataDir = SIM_DATA_DIR;
    std::string path = std::string(GetApplicationDirectory()) + "worldtest_tmp.json";

    Simulation a;
    a.LoadUniverse(dataDir + "universe.json");
    a.InitGalaxy();
    std::string sid = a.Universe().startId;

    // Advance the clock the way the real server does, rather than setting it: this also
    // guards that simulation time moves at all, which it silently did not before #57.
    const float dt = 1.0f / 60.0f;
    for (int i = 0; i < 120; i++)
        a.MaintainWorld(dt, std::string(), nullptr);
    bool ticked = approx(a.Time(), 2.0);

    a.Systems()[sid].agg.security = 0.25f;
    a.Systems()[sid].agg.prosperity = 0.75f;
    a.Systems()[sid].agg.pirates = 7.0f;
    a.Systems()[sid].agg.controller = FactionId::Pirates;
    a.SaveWorld(path);

    Simulation b;  // fresh galaxy: the load must overwrite the defaults
    b.LoadUniverse(dataDir + "universe.json");
    bool loaded = b.LoadWorld(path);
    bool haveSys = b.HasSystem(sid);
    bool time = approx(b.Time(), 2.0);
    bool sec = haveSys && approx(b.Systems()[sid].agg.security, 0.25);
    bool prosp = haveSys && approx(b.Systems()[sid].agg.prosperity, 0.75);
    bool pir = haveSys && approx(b.Systems()[sid].agg.pirates, 7.0);
    bool ctrl = haveSys && b.Systems()[sid].agg.controller == FactionId::Pirates;
    std::remove(path.c_str());

    bool ok = ticked && loaded && time && sec && prosp && pir && ctrl;
    printf("World selftest: clock %s, load %s, time %s, security %s, prosperity %s, "
           "pirates %s, controller %s => %s\n",
           ticked ? "OK" : "FAIL", loaded ? "OK" : "FAIL", time ? "OK" : "FAIL",
           sec ? "OK" : "FAIL", prosp ? "OK" : "FAIL", pir ? "OK" : "FAIL", ctrl ? "OK" : "FAIL",
           ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

// Standing-order smoke test (no network): give the ship an order, tick the server the way
// the host loop does, and check it finishes on its own. This is the property agent play
// rests on -- one order, no further input, the server does the flying.
// Run: econserver ordertest.
static int OrderSelftest()
{
    std::string    dataDir = SIM_DATA_DIR;
    Simulation     sim;
    ClientSession& s = SetupHostSim(sim, dataDir);

    const float dt = 1.0f / 60.0f;
    auto        run = [&sim, &s, dt](int maxTicks)
    {
        for (int i = 0; i < maxTicks && s.HasRunningOrder(); i++)
        {
            sim.StepPlayerOrder(s, sim.Active(), dt);
            sim.MaintainWorld(dt, sim.ActiveId(), nullptr);
        }
        return !s.HasRunningOrder();
    };

    // 1) Fly to a point and stop there.
    Vector2       start = s.ship.get()->GetPosition();
    Orders::Order move;
    move.kind = Orders::Kind::MoveTo;
    move.point = { start.x + 1200.0f, start.y };
    move.stopDist = 150.0f;
    int   id = sim.GiveOrder(s, move);
    bool  moveFinished = run(60 * 120);  // up to two simulated minutes
    float dx = s.ship.get()->GetPosition().x - move.point.x;
    float dy = s.ship.get()->GetPosition().y - move.point.y;
    bool  arrived = moveFinished && s.orderStatus == Orders::Status::Done &&
                    std::sqrt(dx * dx + dy * dy) <= move.stopDist * 1.5f;
    bool  idOk = id > 0;

    // 2) An order naming something that is not here must fail, not hang.
    Orders::Order bogus;
    bogus.kind = Orders::Kind::Dock;
    bogus.targetId = 999999;
    sim.GiveOrder(s, bogus);
    sim.StepPlayerOrder(s, sim.Active(), dt);
    bool rejected = s.orderStatus == Orders::Status::Failed && !s.orderDetail.empty();

    // 3) Manual control takes the ship back mid-order.
    sim.GiveOrder(s, move);
    sim.AbortOrder(s, "manual control");
    bool aborted = s.orderStatus == Orders::Status::Failed && s.orderDetail == "manual control";

    // 4) Route planning across the gate graph.
    const std::string        startSys = sim.Universe().startId;
    std::vector<std::string> self = sim.PlanRoute(startSys, startSys, false);
    bool                     selfRoute = self.size() == 1 && self[0] == startSys;

    // Every other system must be reachable from the start, and the path must begin and
    // end where it was asked to.
    bool allReachable = true;
    for (const WorldLoader::SystemInfo& si : sim.Universe().systems)
    {
        std::vector<std::string> path = sim.PlanRoute(startSys, si.id, false);
        if (path.empty() || path.front() != startSys || path.back() != si.id)
            allReachable = false;
    }
    bool noRoute = sim.PlanRoute(startSys, "nowhere", false).empty();

    // Planning around danger must still produce a valid path, not give up.
    bool safeRouteOk = true;
    for (const WorldLoader::SystemInfo& si : sim.Universe().systems)
    {
        std::vector<std::string> path = sim.PlanRoute(startSys, si.id, true);
        if (path.empty() || path.back() != si.id)
            safeRouteOk = false;
    }

    // 5) A route order to somewhere unreachable fails instead of hanging.
    Orders::Order route;
    route.kind = Orders::Kind::Route;
    route.destSystem = "nowhere";
    sim.GiveOrder(s, route);
    sim.StepPlayerOrder(s, sim.Active(), dt);
    bool routeRejected = s.orderStatus == Orders::Status::Failed;

    // 6) The journal: every order outcome must leave an entry an agent can wait on, and
    // "since seq" must not lose one.
    int before = s.LastEventSeq();
    sim.GiveOrder(s, move);
    sim.AbortOrder(s, "manual control");
    std::vector<Ev::Event> fresh = s.EventsSince(before);
    bool                   journalled =
        fresh.size() == 1 && fresh[0].kind == Ev::Kind::OrderFailed && fresh[0].seq > before;
    // Asking again from the new high-water mark must return nothing.
    bool journalCursor = s.EventsSince(s.LastEventSeq()).empty();
    // And asking from zero must still return the whole history.
    bool journalHistory = s.EventsSince(0).size() >= fresh.size();

    bool ok = idOk && arrived && rejected && aborted && selfRoute && allReachable && noRoute &&
              safeRouteOk && routeRejected && journalled && journalCursor && journalHistory;
    printf("Order selftest: id %s, move %s, bad-target %s, abort %s, route-self %s, "
           "route-all %s, route-none %s, route-safe %s, route-reject %s, journal %s => %s\n",
           idOk ? "OK" : "FAIL", arrived ? "OK" : "FAIL", rejected ? "OK" : "FAIL",
           aborted ? "OK" : "FAIL", selfRoute ? "OK" : "FAIL", allReachable ? "OK" : "FAIL",
           noRoute ? "OK" : "FAIL", safeRouteOk ? "OK" : "FAIL", routeRejected ? "OK" : "FAIL",
           (journalled && journalCursor && journalHistory) ? "OK" : "FAIL", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

int main(int argc, char** argv)
{
    // Unbuffered stdout, set before anything writes to it. A long-running server is
    // usually watched through a redirect or a pipe, where the default block buffering
    // hides progress for minutes. Note _IOLBF is not an option here: the Microsoft C
    // runtime silently treats line buffering as full buffering, so it would look like it
    // worked and change nothing. The server logs a handful of lines, so dropping the
    // buffer entirely costs nothing.
    setvbuf(stdout, nullptr, _IONBF, 0);

    // Protocol/TCP regression tests moved to the `tests` target (ctest); here —
    // only server modes and batch simulation.
    if (argc > 1 && std::string(argv[1]) == "hosttest")
        return HostSelftest();
    if (argc > 1 && std::string(argv[1]) == "accttest")
        return AccountSelftest();
    if (argc > 1 && std::string(argv[1]) == "worldtest")
        return WorldSelftest();
    if (argc > 1 && std::string(argv[1]) == "ordertest")
        return OrderSelftest();
    if (argc > 1 && std::string(argv[1]) == "host")
    {
        unsigned short port = (argc > 2) ? (unsigned short)atoi(argv[2]) : 50800;
        return RunHost(port);
    }

    std::string dataDir = SIM_DATA_DIR;

    // Determinism: fix both the server RNG (Simulation) and the global raylib RNG
    // (NpcShip uses it for patrols/timers) — valid without a window.
    SetRandomSeed(0xC0FFEEu);

    Factions::Load(dataDir + "factions.json");
    if (!Archetypes::Load(dataDir + "archetypes.json"))
        fprintf(stderr, "FATAL: %s\n", Archetypes::Error().c_str());

    Simulation sim;
    sim.LoadUniverse(dataDir + "universe.json");
    sim.Seed(0xC0FFEEu);
    sim.InitGalaxy();
    sim.MaterializeAllSystems(dataDir + "systems/");  // real entities of all systems

    int         ticks = (argc > 1) ? atoi(argv[1]) : 3600;
    const float dt = 1.0f / 60.0f;  // = SIM_DT: server tick as in the game

    printf("EconSpace headless server — %d systems, %d ticks (real agents)\n",
           (int)sim.Universe().systems.size(), ticks);

    int printEvery = ticks / 20;
    if (printEvery < 1)
        printEvery = 1;

    for (int i = 0; i < ticks; i++)
    {
        // The server simulates ALL systems (no player) + coarse world maintenance.
        for (auto& kv : sim.Systems())
            sim.StepSystemAgents(kv.second, dt);
        sim.MaintainWorld(dt, "", nullptr);

        if (i % printEvery == 0 || i == ticks - 1)
        {
            printf("\n[tick %d / t=%.1fs]\n", i, i * dt);
            for (auto& kv : sim.Systems())
            {
                sim.RecountAgg(kv.second);  // fresh real numbers for printing
                const SystemAggregate& a = kv.second.agg;
                printf("  %-10s sec %.2f  pir %2d  pol %2d  trd %2d  econ %3.0f%%  ctrl %s\n",
                       kv.first.c_str(), a.security, (int)a.pirates, (int)a.police, (int)a.traders,
                       a.prosperity * 100.0f, FactionName(a.controller).c_str());
            }
        }
    }

    printf("\nGalactic news:\n");
    if (sim.Events().empty())
        printf("  (no territory changes)\n");
    for (const std::string& e : sim.Events())
        printf("  - %s\n", e.c_str());

    // Pause so the window does not close when launched by double-click. When run
    // from a terminal / with stdin redirected to EOF — exit immediately.
    printf("\nPress Enter to exit...");
    fflush(stdout);
    getchar();

    return 0;
}
