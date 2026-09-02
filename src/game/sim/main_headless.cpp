// EconSpace — headless galaxy server (track M, M3).
//
// Runs the SAME simulation as the game, but without rendering/window/input: real
// agents (NpcShip) move and fight, the spawn director holds the population, and
// macrodynamics change security/economy/territory control. This is the authoritative
// server — no client (Game) sits on top yet, but the logic is one (Simulation).
//
// Run: econserver [ticks]   (default 3600 ticks = 60 s at SIM_DT=1/60).

#include "sim/Auth.h"
#include "sim/Protocol.h"
#include "sim/SaveSchema.h"
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

#include <nlohmann/json.hpp>

#include <chrono>
#include <fstream>
#include <csignal>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <map>
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
static void SetupHostSim(Simulation& sim, const std::string& dataDir,
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
    const Save::Result world = worldPath.empty() ? Save::Result::Missing : sim.LoadWorld(worldPath);
    if (world == Save::Result::TooNew)
    {
        // Refusing to start beats starting a different galaxy: the checkpoint timer would
        // write this build's idea of the world over the one that is there (#20).
        fprintf(stderr,
                "FATAL: %s was written by a newer build (this one speaks world schema v%d).\n"
                "Move it aside or upgrade the server; it has NOT been modified.\n",
                worldPath.c_str(), Save::WORLD_VERSION);
        exit(1);
    }
    if (world == Save::Result::Ok)
        printf("World restored from %s (t=%.0fs).\n", worldPath.c_str(), sim.Time());
    else
        sim.InitGalaxy();
    sim.MaterializeAllSystems(dataDir + "systems/");
}

// A player in the start system. The server makes one per connection; the self-tests make
// exactly one and fly it themselves.
static ClientSession& SetupHostPlayer(Simulation& sim)
{
    return sim.CreateSession(sim.Universe().startId, Vector2{ 0.0f, 3000.0f },
                             GetShipCatalog()[0].stats);
}

// One client input = ONE player tick. This is critical for client and server
// prediction to match: the client predicts one StepPlayerShip per input, and the
// server must step the same way (otherwise the ack numbers diverge from the step count
// and the ship "jerks"). Applies movement/jump/loot/combat/mining for command c.
// Returns true if the system changed. Account effects are not yet applied on the server
// (M4f); NPCs do not target the player yet.
static bool HostStepPlayer(Simulation& sim, ClientSession& s, const Proto::Command& c, float dt,
                           std::vector<Proto::TradeAck>&                  acks,
                           std::map<std::string, std::vector<FireEvent>>& fires)
{
    bool changed = false;

    // Dock/undock (server-authoritative). While docked we do not step the player's
    // physics (the ship is frozen at the station), but we still handle trade/undock.
    if (c.dock && !s.IsDocked())
        sim.StepPlayerDock(s, *sim.SystemOf(s));
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
                sim.StepPlayerSell(s, *sim.SystemOf(s), c.sellType, c.sellAmount);
            if (r.sold > 0)
                acks.push_back(Proto::TradeAck{ c.sellType, r.sold, r.gross, r.revenue });
        }
        if (c.refitShip >= 0)
            sim.SwitchShip(s, c.refitShip);  // only to a ship this account owns (#5)
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
        std::string dest = sim.JumpGateDestIfNear(s, *sim.SystemOf(s), c.jumpGateId);
        if (!dest.empty())
        {
            sim.ServerEnterSystem(s, dest, s.systemId);  // arrives at the gate back
            changed = true;
        }
    }
    if (c.lootId != 0)
        sim.StepPlayerLoot(s, *sim.SystemOf(s), c.lootId);

    // Player fire: on a shot — a beam event into the snapshot (client draws it blue).
    // Account effects (mission credit/reputation) are not yet applied on the server (3c-ii).
    Simulation::PlayerCombatEvents ev;
    if (sim.StepPlayerFire(s, *sim.SystemOf(s), c.targetId, dt, &ev))
    {
        FireEvent fe;
        fe.from = ev.shotFrom;
        fe.to = ev.shotTo;
        fe.shooterFaction = FactionId::Independent;
        fe.shooterSessionId = s.id;  // "mine" for this player and nobody else
        fires[s.systemId].push_back(fe);
    }
    sim.StepPlayerMining(s, *sim.SystemOf(s), s.account.GetSkills().GetBonus(SkillType::Mining),
                         dt);
    return changed;
}

// Is this player inside something that hides ships? The nebula is the only such thing
// today, but the test asks for the property rather than for the class (#34), so a jammer
// a player builds (#44) would hide them too.
static bool HiddenInCover(Simulation& sim, const ClientSession& s)
{
    const SystemState* st = sim.SystemOf(s);
    if (st == nullptr || !s.ship)
        return false;
    Vector2 pp = s.ship->GetPosition();
    for (const auto& e : st->entities)
    {
        if (!e->Has(Component::Hazard) || !e->GetArchetype()->hazardHidesShips)
            continue;
        // radius 0 in the archetype means "as large as the object itself".
        float r =
            e->GetArchetype()->hazardRadius > 0.0f ? e->GetArchetype()->hazardRadius : e->GetSize();
        float dx = pp.x - e->GetPosition().x;
        float dy = pp.y - e->GetPosition().y;
        if (dx * dx + dy * dy <= r * r)
            return true;
    }
    return false;
}

// World step (every system + maintenance) — on the server timer, independent of inputs.
//
// Every system is stepped the same way; the only difference between them is who happens
// to be standing in one (#3). NPCs see those players, shoot at them, and the beams are
// collected per system so each client is told about its own sky. A system with nobody in
// it keeps living, which is the premise of the whole galaxy.
static void HostStepWorld(Simulation& sim, float dt,
                          std::map<std::string, std::vector<FireEvent>>& fires)
{
    // Who is standing where. A docked player is not in space: NPCs cannot see or shoot
    // them, which is what makes a station a refuge.
    std::map<std::string, std::vector<Simulation::PlayerPresence>> present;
    for (auto& kv : sim.Sessions())
    {
        ClientSession& s = kv.second;
        if (!s.ship || s.IsDocked())
            continue;
        Simulation::PlayerPresence p;
        p.session = &s;
        p.ship = (Combatant*)s.ship.get();
        p.hidden = HiddenInCover(sim, s);
        present[s.systemId].push_back(p);
    }

    static const std::vector<Simulation::PlayerPresence> kEmpty;
    for (auto& kv : sim.Systems())
    {
        auto                                           it = present.find(kv.first);
        const std::vector<Simulation::PlayerPresence>& players =
            (it == present.end()) ? kEmpty : it->second;
        sim.StepSystemAgents(kv.second, players, players.empty() ? nullptr : &fires[kv.first], dt);
        kv.second.market.Update(dt);  // market price recovery
    }

    for (auto& kv : sim.Sessions())
    {
        ClientSession& s = kv.second;
        if (s.ship && !s.ship->IsAlive())
            sim.ServerRespawnPlayer(s);

        // Standing orders execute here rather than in the input path: an agent issues one
        // order and then sends nothing, so if the order did not drive its own ticks the
        // ship would simply sit there.
        if (SystemState* st = sim.SystemOf(s))
            sim.StepPlayerOrder(s, *st, dt);

        // Account passive: piloting xp (in flight) + bounty decay.
        sim.StepPlayerAccountTick(s, dt);
    }

    sim.MaintainWorld(dt);
}

// Receives client inputs from the transport and applies EACH as one player tick
// (HostStepPlayer). lastSeq — the highest processed input number (ack to the client).
// Returns true if the layout must be resent (system change).
static bool HostApplyCommand(const std::string& msg, Simulation& sim, ClientSession& s, float dt,
                             int& lastSeq, std::vector<Proto::TradeAck>& acks,
                             std::map<std::string, std::vector<FireEvent>>& fires)
{
    Proto::Command c;
    if (!Proto::DecodeCommand(msg, c))
        return false;
    if (c.seq > lastSeq)
        lastSeq = c.seq;
    return HostStepPlayer(sim, s, c, dt, acks, fires);
}

static bool HostDrainInputs(ITransport& conn, Simulation& sim, ClientSession& s, float dt,
                            int& lastSeq, std::vector<Proto::TradeAck>& acks,
                            std::map<std::string, std::vector<FireEvent>>& fires)
{
    bool        layoutDirty = false;
    std::string msg;
    while (conn.Poll(msg))
        if (Proto::MessageType(msg) == "cmd" &&
            HostApplyCommand(msg, sim, s, dt, lastSeq, acks, fires))
            layoutDirty = true;
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
// One connected client: the socket, the player it drives, and the bookkeeping that
// belongs to the connection rather than to the player -- what has been acknowledged and
// what has been delivered. A client exists before its player does: until the hello
// arrives the server has a socket and nobody behind it.
struct HostClient
{
    std::unique_ptr<Net::TcpConnection> conn;
    int                                 sessionId = 0;  // 0 until the hello arrives
    std::string                         account;
    int                                 lastSeq = 0;       // last input acked back
    int                                 lastEventSeq = 0;  // last journal entry delivered
    bool                                wasDocked = false;
    bool                                accountReadOnly = false;  // their file is newer (#20)
    bool                                evicted = false;  // displaced by a later login (#105)

    // The login in progress (#106). A name has been claimed and a challenge sent; none of
    // it means anything until the proof arrives.
    std::string pendingAccount;
    std::string nonce;          // this connection's, single use
    std::string challengeSalt;  // the salt the client was told to use
    std::string fileStored;     // what the account file holds, for an account that exists
    bool        isNewAccount = false;
    double      silentFor = 0.0;  // seconds connected without a hello

    // How many player ticks this client may still be granted. One received command is one
    // tick of movement, so without a budget a client that sends faster than the simulation
    // runs simply moves faster than everyone else -- a speed hack, not merely a flood
    // (#14). An honest client sends exactly one per SIM_DT, so it never runs out.
    double tickBudget = 0.0;
    bool   throttleReported = false;
};

// The rate a client may spend ticks at, and how many it may bank. Beyond the burst the
// sender is outrunning the simulation and the extra inputs are refused rather than queued
// -- the client already replays unacknowledged inputs, so refusing is a correction, not a
// loss.
//
// The two numbers bound different things, and only the rate is a cheat gate. Sustained
// speed is what a speed hack needs; a burst is what an honest client produces while its
// window is still opening, and measuring one startup showed the old allowance of eight
// being spent before the first frame was drawn (#115).
//
// So the burst is a whole second of ticks. Someone who saves it up and spends it at once
// gains one second of movement, once, and is then held to real time forever -- which is
// not an exploit worth throttling every real player to prevent.
static const double TICKS_PER_SECOND = 1.0 / (double)Sim::SIM_DT;
static const double MAX_TICK_BURST = TICKS_PER_SECOND;

// How long a socket may stay connected without saying who it is. A client that opens a
// connection and never introduces itself is not a player; without a limit, sockets like
// that accumulate for as long as the server runs.
static const double HELLO_TIMEOUT = 10.0;

// A name that is safe both as an identity and as a file name. Rejecting is better than
// mangling: two names differing only in punctuation would otherwise share one account
// file, which is one player spending another's money.
static bool ValidAccountName(const std::string& n)
{
    if (n.empty() || n.size() > 32)
        return false;
    for (char c : n)
        if (!std::isalnum((unsigned char)c) && c != '_' && c != '-')
            return false;
    return true;
}

// What an account file says its secret is, if it says anything. False for a file that is
// not there and for one written before secrets existed (#106) -- in both cases the next
// login sets it, which is a migration hole that closes itself as accounts are used.
static bool ReadStoredAuth(const std::string& path, std::string& salt, std::string& stored)
{
    std::ifstream in(path);
    if (!in.is_open())
        return false;
    nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("auth") || !j["auth"].is_object())
        return false;
    salt = j["auth"].value("salt", std::string());
    stored = j["auth"].value("stored", std::string());
    return !salt.empty() && !stored.empty();
}

// Progress is stored per account name (#3). There was one account.json when there could
// only be one player; with two connected it would be one file two people write.
static std::string AccountPath(const std::string& account)
{
    return std::string(GetApplicationDirectory()) + "account_" + account + ".json";
}

// The same beams, told to one player. Whether a shot is "mine" or "at me" is a fact about
// the recipient, not about the shot, so it is written here rather than where it happened.
static std::vector<FireEvent> FiresFor(const std::vector<FireEvent>& all, int sessionId)
{
    std::vector<FireEvent> out = all;
    for (FireEvent& f : out)
    {
        f.fromPlayer = (f.shooterSessionId != 0 && f.shooterSessionId == sessionId);
        f.targetIsPlayer = (f.targetSessionId != 0 && f.targetSessionId == sessionId);
    }
    return out;
}

// What the server is actually sending, because #16 asks for numbers before cuts. Printed
// periodically while anyone is connected: an optimization argued from a guess is how the
// wrong thing gets optimized.
struct NetStats
{
    double snapshots = 0.0;  // bytes
    double layouts = 0.0;
    double galaxies = 0.0;
    int    snapCount = 0;
    double window = 0.0;  // seconds since the last report

    void Report(size_t clients)
    {
        const double total = snapshots + layouts + galaxies;
        if (window <= 0.0 || total <= 0.0)
            return;
        const double perSec = total / window;
        printf("net: %zu client(s), %.1f KB/s out (%.1f KB/s each) -- snapshots %.0f%%, "
               "galaxy %.0f%%, layout %.0f%%; %.0f B per snapshot\n",
               clients, perSec / 1024.0, clients ? perSec / 1024.0 / (double)clients : 0.0,
               100.0 * snapshots / total, 100.0 * galaxies / total, 100.0 * layouts / total,
               snapCount ? snapshots / snapCount : 0.0);
        snapshots = layouts = galaxies = 0.0;
        snapCount = 0;
        window = 0.0;
    }
};

static const double NET_REPORT_INTERVAL = 10.0;

// How often each client is sent a snapshot. Not once per loop iteration, which is what it
// used to be -- that made the send rate an accident of how fast the loop happened to spin,
// and it was spinning far faster than anything could be seen (#16).
//
// The client renders the world 100 ms in the past and interpolates between the two
// snapshots around that time, precisely so updates can be sparse. Twenty a second puts
// them 50 ms apart, comfortably inside that window. The simulation still runs at 60 Hz;
// what changed is how often the result is described, not how often it is computed.
static const double SNAPSHOT_INTERVAL = 1.0 / 20.0;

static int RunHost(unsigned short port)
{
    std::string dataDir = SIM_DATA_DIR;
    std::string worldPath = std::string(GetApplicationDirectory()) + "world.json";
    Simulation  sim;
    SetupHostSim(sim, dataDir, worldPath);

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
    printf("EconSpace server on port %d. The galaxy runs with or without players; "
           "Ctrl+C to stop.\n",
           port);

    std::vector<HostClient> clients;

    NetStats    net;
    double      snapshotAcc = 0.0;   // time since the last round of snapshots
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

        // Everyone waiting, not one per iteration: two clients arriving in the same
        // millisecond is the ordinary case, not a race to be lost.
        while (std::unique_ptr<Net::TcpConnection> c = listener.Accept())
        {
            HostClient hc;
            hc.conn = std::move(c);
            clients.push_back(std::move(hc));
            printf("Client connected; waiting for hello.\n");
        }

        // Inputs. Each received command is one player tick (1:1 with client prediction),
        // and each client's trade acknowledgments are its own.
        std::map<std::string, std::vector<FireEvent>> fires;  // beams, by system
        std::map<int, std::vector<Proto::TradeAck>>   acks;   // by session
        std::map<int, bool>                           layoutDirty;

        for (HostClient& hc : clients)
        {
            if (!hc.conn)
                continue;
            if (hc.sessionId == 0)
                hc.silentFor += frame;
            hc.tickBudget += frame * TICKS_PER_SECOND;
            if (hc.tickBudget > MAX_TICK_BURST)
                hc.tickBudget = MAX_TICK_BURST;
            std::string msg;
            while (hc.conn->Poll(msg))
            {
                const std::string type = Proto::MessageType(msg);
                if (type == "hello")
                {
                    Proto::Hello h;
                    if (hc.sessionId != 0 || !hc.pendingAccount.empty() ||
                        !Proto::DecodeHello(msg, h) || !ValidAccountName(h.account))
                    {
                        printf("Rejected a hello (bad name, wrong version, or a second one "
                               "on the same connection).\n");
                        // Dropping the socket is the whole answer: a client that cannot
                        // say who it is has nothing to be told.
                        hc.conn.reset();
                        break;
                    }
                    // A name is a claim, not a login (#106). Nothing is created here and
                    // nobody is disturbed: the challenge goes out, and everything that
                    // costs anyone anything waits for the proof.
                    hc.pendingAccount = h.account;
                    hc.nonce = Auth::MakeNonce();

                    Proto::Challenge chal;
                    chal.nonce = hc.nonce;
                    const std::string path = AccountPath(h.account);
                    hc.isNewAccount = !ReadStoredAuth(path, chal.salt, hc.fileStored);
                    if (hc.isNewAccount)
                        chal.salt = Auth::MakeSalt();  // this login will set the secret
                    chal.isNew = hc.isNewAccount;
                    hc.challengeSalt = chal.salt;
                    hc.conn->Send(Proto::EncodeChallenge(chal));
                    continue;
                }
                if (type == "auth")
                {
                    Proto::Auth a;
                    if (hc.sessionId != 0 || hc.pendingAccount.empty() ||
                        !Proto::DecodeAuth(msg, a))
                    {
                        hc.conn.reset();
                        break;
                    }
                    // A new account is taken at its word once -- there is nothing to check
                    // it against yet -- but the proof still has to match the value being
                    // registered, so a client that gets this wrong is caught rather than
                    // creating an account nobody can log into afterwards.
                    const std::string against = hc.isNewAccount ? a.stored : hc.fileStored;
                    const bool        shaped = against.size() == 64;
                    if (!shaped || a.proof != Auth::Proof(hc.nonce, against))
                    {
                        // One guess per connection: a wrong answer ends it, so guessing
                        // costs a fresh socket every time.
                        Proto::Bye bye;
                        bye.reason = hc.isNewAccount ? "Could not create that account"
                                                     : "Wrong secret for that account";
                        hc.conn->Send(Proto::EncodeBye(bye));
                        printf("%s: refused (%s).\n", hc.pendingAccount.c_str(),
                               hc.isNewAccount ? "bad registration" : "wrong secret");
                        hc.conn.reset();
                        break;
                    }
                    const Proto::Hello h{ hc.pendingAccount };

                    // One account, one session (#105). Two connections claiming the same
                    // name used to get a ship each and the whole balance each, and
                    // whichever left last wrote its copy over the other's.
                    //
                    // The newcomer wins, and only once it has proved itself: displacing on
                    // the claim alone would have handed anyone a way to kick anyone (#106).
                    // Between refusing and displacing, this is the one that always lets the
                    // real owner back in after a crash instead of locking them out of their
                    // own account until a dead socket times out.
                    for (HostClient& other : clients)
                    {
                        if (&other == &hc || other.sessionId == 0 || other.account != h.account)
                            continue;
                        if (ClientSession* os = sim.Session(other.sessionId))
                        {
                            // Saved before it is taken away: what they earned up to this
                            // moment is theirs, and the new session loads that file next.
                            if (!other.accountReadOnly)
                                sim.SaveAccount(*os, AccountPath(other.account));
                            sim.DestroySession(other.sessionId);
                        }
                        // Say why before going. Send pumps synchronously, so on any socket
                        // that is not already backed up this reaches them; if it does not,
                        // they see a plain disconnect -- which is all they used to see.
                        Proto::Bye bye;
                        bye.reason = "Signed in from somewhere else";
                        if (other.conn)
                            other.conn->Send(Proto::EncodeBye(bye));
                        other.sessionId = 0;
                        other.evicted = true;
                        printf("%s signed in again; the earlier connection was displaced.\n",
                               h.account.c_str());
                    }

                    ClientSession& s = SetupHostPlayer(sim);
                    hc.sessionId = s.id;
                    hc.account = h.account;
                    s.ship->SetPilotName(h.account);     // what other players see (#4)
                    hc.lastEventSeq = s.LastEventSeq();  // a new session starts from now
                    const Save::Result acct = sim.LoadAccount(s, AccountPath(h.account));
                    if (acct == Save::Result::TooNew)
                    {
                        // They can play, but their file is left exactly as it is. Handing
                        // a player a fresh account and then saving it over the real one is
                        // the failure this whole check exists to prevent (#20).
                        hc.accountReadOnly = true;
                        printf("%s: account file is from a newer build; joining with a "
                               "fresh account and NOT saving over it.\n",
                               h.account.c_str());
                    }
                    else if (acct == Save::Result::Ok)
                        printf("%s joined (money %.0f).\n", h.account.c_str(),
                               s.account.GetMoney());
                    else
                        printf("%s joined, new account (money %.0f).\n", h.account.c_str(),
                               s.account.GetMoney());

                    // Set after the load, not before: an account file with no secret in it
                    // (one written before #106) would otherwise overwrite what was just
                    // registered with nothing.
                    if (hc.isNewAccount)
                    {
                        s.authSalt = hc.challengeSalt;
                        s.authStored = a.stored;
                        if (acct == Save::Result::Ok)
                            printf("%s: an account that had no secret now has one.\n",
                                   h.account.c_str());
                    }
                    hc.wasDocked = s.IsDocked();
                    const std::string lay = Proto::EncodeLayout(sim.BuildLayout(s.systemId));
                    const std::string gal = Proto::EncodeGalaxy(sim.BuildGalaxyState());
                    net.layouts += (double)lay.size();
                    net.galaxies += (double)gal.size();
                    hc.conn->Send(lay);
                    hc.conn->Send(gal);
                    continue;
                }
                // Anything else before the hello is from a client that has not said who it
                // is; there is no player to apply it to.
                ClientSession* s = sim.Session(hc.sessionId);
                if (type != "cmd" || s == nullptr)
                    continue;
                if (hc.tickBudget < 1.0)
                {
                    // Not acknowledged either: the client replays what the server has not
                    // confirmed, so the input comes back rather than being lost.
                    if (!hc.throttleReported)
                    {
                        hc.throttleReported = true;
                        printf("%s is sending commands faster than the simulation runs; "
                               "the extra ones are ignored.\n",
                               hc.account.c_str());
                    }
                    continue;
                }
                hc.tickBudget -= 1.0;
                if (HostApplyCommand(msg, sim, *s, dt, hc.lastSeq, acks[hc.sessionId], fires))
                    layoutDirty[hc.sessionId] = true;
            }

            // Account checkpoint on a dock-state change: on docking -- commit what was
            // earned in flight (loot/bounty), on undocking -- trade and mission hand-ins.
            if (ClientSession* s = sim.Session(hc.sessionId))
                if (s->IsDocked() != hc.wasDocked)
                {
                    hc.wasDocked = s->IsDocked();
                    if (!hc.accountReadOnly)
                        sim.SaveAccount(*s, AccountPath(hc.account));
                }
        }

        // World (NPCs): on the server timer, independent of inputs and of whether anyone
        // is watching.
        while (acc >= dt)
        {
            HostStepWorld(sim, dt, fires);
            acc -= dt;
        }

        snapshotAcc += frame;
        const bool sendSnapshots = snapshotAcc >= SNAPSHOT_INTERVAL;
        if (sendSnapshots)
            snapshotAcc = 0.0;

        for (HostClient& hc : clients)
        {
            ClientSession* s = sim.Session(hc.sessionId);
            if (s == nullptr)
                continue;
            // A layout is not on the snapshot clock: it answers "you are somewhere else
            // now", and waiting 50 ms to say so would draw the old system for a moment.
            if (layoutDirty[hc.sessionId])
            {
                const std::string lay = Proto::EncodeLayout(sim.BuildLayout(s->systemId));
                net.layouts += (double)lay.size();
                hc.conn->Send(lay);
            }
            if (!sendSnapshots)
                continue;
            Proto::Snapshot snap = sim.BuildSnapshot(*s, s->systemId);
            snap.player.lastInput = hc.lastSeq;  // ack for client reconciliation
            snap.tradeAcks = std::move(acks[hc.sessionId]);
            snap.fires = FiresFor(fires[s->systemId], s->id);
            // Everything this player has not seen yet; the client acknowledges by seq.
            snap.events = s->EventsSince(hc.lastEventSeq);
            if (!snap.events.empty())
                hc.lastEventSeq = snap.events.back().seq;
            const std::string enc = Proto::EncodeSnapshot(snap);
            net.snapshots += (double)enc.size();
            net.snapCount++;
            hc.conn->Send(enc);
        }

        // Galaxy snapshot -- rarely (once a second): the statistics change slowly and the
        // message is large (all systems). The client's galaxy map lives off it.
        galaxyAcc += frame;
        if (galaxyAcc >= 1.0)
        {
            galaxyAcc = 0.0;
            if (!clients.empty())
            {
                const std::string g = Proto::EncodeGalaxy(sim.BuildGalaxyState());
                for (HostClient& hc : clients)
                    if (hc.sessionId != 0)
                    {
                        net.galaxies += (double)g.size();
                        hc.conn->Send(g);
                    }
            }
        }

        net.window += frame;
        if (net.window >= NET_REPORT_INTERVAL && !clients.empty())
            net.Report(clients.size());

        // World checkpoint. The galaxy drifts continuously (security, prosperity,
        // territory control), so unlike an account there is no natural commit point to
        // hang this on: a timer is what keeps a crash from costing more than a minute.
        worldSaveAcc += frame;
        if (worldSaveAcc >= WORLD_SAVE_INTERVAL)
        {
            worldSaveAcc = 0.0;
            sim.SaveWorld(worldPath);
        }

        // A disconnect ends one session, not the server and not anyone else's game.
        for (size_t i = 0; i < clients.size();)
        {
            HostClient& hc = clients[i];
            if (hc.conn && hc.conn->Alive() && !hc.evicted &&
                (hc.sessionId != 0 || hc.silentFor < HELLO_TIMEOUT))
            {
                i++;
                continue;
            }
            if (ClientSession* s = sim.Session(hc.sessionId))
            {
                if (hc.accountReadOnly)
                    printf("%s left. Account NOT saved: their file is from a newer build.\n",
                           hc.account.c_str());
                else
                {
                    sim.SaveAccount(*s, AccountPath(hc.account));
                    printf("%s left. Account saved (money %.0f); galaxy still running.\n",
                           hc.account.c_str(), s->account.GetMoney());
                }
                sim.DestroySession(hc.sessionId);
            }
            else if (hc.evicted)
            {
                printf("The displaced connection for %s closed.\n", hc.account.c_str());
            }
            else
            {
                printf("A client left before saying hello.\n");
            }
            clients.erase(clients.begin() + (long)i);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }

    for (HostClient& hc : clients)
        if (ClientSession* s = sim.Session(hc.sessionId))
            if (!hc.accountReadOnly)
                sim.SaveAccount(*s, AccountPath(hc.account));
    sim.SaveWorld(worldPath);
    printf("\nShutting down. World and accounts saved.\n");
    Net::Shutdown();
    return 0;
}

// Host smoke test (no sockets): runs a host tick through LocalTransport. The client
// sends a thrust command, the server steps the world and sends layout+snapshot; we
// check that the layout arrived and the player moved. Run: econserver hosttest.
static int HostSelftest()
{
    std::string dataDir = SIM_DATA_DIR;
    Simulation  sim;
    SetupHostSim(sim, dataDir);
    ClientSession& s = SetupHostPlayer(sim);

    LocalTransport link;
    link.Server().Send(Proto::EncodeLayout(sim.BuildLayout(s.systemId)));

    Proto::Command thrust;
    thrust.thrust = true;
    link.Client().Send(Proto::EncodeCommand(thrust));

    Vector2 startPos = s.ship->GetPosition();

    int         lastSeq = 0;
    const float dt = 1.0f / 60.0f;
    for (int i = 0; i < 120; i++)  // ~2 s of simulation
    {
        thrust.seq = i + 1;  // client: numbered input
        link.Client().Send(Proto::EncodeCommand(thrust));
        std::vector<Proto::TradeAck>                  acks;
        std::map<std::string, std::vector<FireEvent>> fires;
        HostDrainInputs(link.Server(), sim, s, dt, lastSeq, acks, fires);  // 1 input=1 tick
        HostStepWorld(sim, dt, fires);                                     // world
        Proto::Snapshot snap = sim.BuildSnapshot(s, s.systemId);
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
    Vector2 endPos = s.ship->GetPosition();
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
    ClientSession& as =
        a.CreateSession(std::string(), Vector2{ 0.0f, 0.0f }, GetShipCatalog()[0].stats);
    as.account.SetMoney(4242.0);
    as.account.SetReputation(FactionId::Pirates, -7.5f);
    as.account.SetBounty(FactionId::TradersGuild, 300.0);
    as.account.GetSkills().SetXp(SkillType::Mining, 555.0f);
    // What a returning player expects to find (#49): the hold as they left it, and the
    // ship where they left it rather than back at the start.
    as.ship->AddCargo(AllResourceTypes()[0], 9);
    as.ship->Teleport(Vector2{ 777.0f, -333.0f });
    a.SaveAccount(as, path);

    Simulation     b;  // clean account (money 500) — check that the load overwrote it
    ClientSession& bs =
        b.CreateSession(std::string(), Vector2{ 0.0f, 0.0f }, GetShipCatalog()[0].stats);
    bool loaded = b.LoadAccount(bs, path) == Save::Result::Ok;
    bool money = approx(bs.account.GetMoney(), 4242.0);
    bool rep = approx(bs.account.GetReputation(FactionId::Pirates), -7.5);
    bool bounty = approx(bs.account.GetBounty(FactionId::TradersGuild), 300.0);
    bool skill = approx(bs.account.GetSkills().GetXp(SkillType::Mining), 555.0);
    bool cargo = bs.ship->GetCargoAmount(AllResourceTypes()[0]) == 9;
    // No galaxy is loaded here, so the saved system id is not one this simulation has and
    // the position is deliberately NOT restored -- putting a ship in a system that does
    // not exist is worse than putting it back at the start. The doctest suite covers the
    // restored case, where a galaxy is loaded.
    bool placeSafe = bs.ship->GetPosition().x == 0.0f && bs.ship->GetPosition().y == 0.0f;
    std::remove(path.c_str());

    bool ok = loaded && money && rep && bounty && skill && cargo && placeSafe;
    printf("Account selftest: load %s, money %s, rep %s, bounty %s, skill %s, cargo %s, "
           "place %s => %s\n",
           loaded ? "OK" : "FAIL", money ? "OK" : "FAIL", rep ? "OK" : "FAIL",
           bounty ? "OK" : "FAIL", skill ? "OK" : "FAIL", cargo ? "OK" : "FAIL",
           placeSafe ? "OK" : "FAIL", ok ? "PASS" : "FAIL");
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
        a.MaintainWorld(dt);
    bool ticked = approx(a.Time(), 2.0);

    a.Systems()[sid].agg.security = 0.25f;
    a.Systems()[sid].agg.prosperity = 0.75f;
    a.Systems()[sid].agg.pirates = 7.0f;
    a.Systems()[sid].agg.controller = FactionId::Pirates;
    a.SaveWorld(path);

    Simulation b;  // fresh galaxy: the load must overwrite the defaults
    b.LoadUniverse(dataDir + "universe.json");
    bool loaded = b.LoadWorld(path) == Save::Result::Ok;
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
    std::string dataDir = SIM_DATA_DIR;
    Simulation  sim;
    SetupHostSim(sim, dataDir);
    ClientSession& s = SetupHostPlayer(sim);

    const float dt = 1.0f / 60.0f;
    auto        run = [&sim, &s, dt](int maxTicks)
    {
        for (int i = 0; i < maxTicks && s.HasRunningOrder(); i++)
        {
            sim.StepPlayerOrder(s, *sim.SystemOf(s), dt);
            sim.MaintainWorld(dt);
        }
        return !s.HasRunningOrder();
    };

    // 1) Fly to a point and stop there.
    Vector2       start = s.ship->GetPosition();
    Orders::Order move;
    move.kind = Orders::Kind::MoveTo;
    move.point = { start.x + 1200.0f, start.y };
    move.stopDist = 150.0f;
    int   id = sim.GiveOrder(s, move);
    bool  moveFinished = run(60 * 120);  // up to two simulated minutes
    float dx = s.ship->GetPosition().x - move.point.x;
    float dy = s.ship->GetPosition().y - move.point.y;
    bool  arrived = moveFinished && s.orderStatus == Orders::Status::Done &&
                    std::sqrt(dx * dx + dy * dy) <= move.stopDist * 1.5f;
    bool  idOk = id > 0;

    // 2) An order naming something that is not here must fail, not hang.
    Orders::Order bogus;
    bogus.kind = Orders::Kind::Dock;
    bogus.targetId = 999999;
    sim.GiveOrder(s, bogus);
    sim.StepPlayerOrder(s, *sim.SystemOf(s), dt);
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
    sim.StepPlayerOrder(s, *sim.SystemOf(s), dt);
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
            sim.StepSystemAgents(kv.second, {}, nullptr, dt);
        sim.MaintainWorld(dt);

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
