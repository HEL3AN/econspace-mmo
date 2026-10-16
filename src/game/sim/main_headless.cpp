// EconSpace — headless galaxy server (track M, M3).
//
// Runs the SAME simulation as the game, but without rendering/window/input: real
// agents (NpcShip) move and fight, the spawn director holds the population, and
// macrodynamics change security/economy/territory control. This is the authoritative
// server — no client (Game) sits on top yet, but the logic is one (Simulation).
//
// Run: econserver [ticks]   (default 3600 ticks = 60 s at SIM_DT=1/60).
// Every mode takes --data <dir>; without it data/ is looked for beside the exe (DataDir.h).

#include "sim/AccessList.h"
#include "sim/Auth.h"
#include "sim/HostRules.h"
#include "sim/MacroBench.h"
#include "sim/Protocol.h"
#include "sim/SaveFile.h"
#include "sim/SaveSchema.h"
#include "sim/ServerLog.h"
#include "sim/WorldRules.h"
#include "gen/Galaxy.h"
#include "gen/Pack.h"
#include "gen/Pins.h"
#include "gen/Region.h"
#include "sim/Simulation.h"
#include "sim/ClientSession.h"
#include "net/Tcp.h"
#include "net/Transport.h"
#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/DataDir.h"
#include "core/Faction.h"
#include "economy/Resource.h"
#include "entities/Combatant.h"
#include "entities/Nebula.h"
#include "entities/NpcShip.h"
#include "entities/Ship.h"
#include "entities/ShipType.h"
#include "entities/Station.h"
#include "entities/Structure.h"
#include "raylib.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <csignal>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <set>
#include <memory>
#include <string>
#include <random>
#include <thread>
#include <iostream>
#include <mutex>
#include <sstream>

// The designs players' ships fly (#279 fitting) live beside the world they are flown in.
static std::string DesignsPath(const std::string& worldPath)
{
    const size_t slash = worldPath.find_last_of("/\\");
    return (slash == std::string::npos ? std::string() : worldPath.substr(0, slash + 1)) +
           "designs.json";
}

// Where admin packs are read from (#342 S7): data/packs/ unless `host --packs DIR` says.
static std::string g_packsDir;

// The packs in `dir`, as the simulation holds them; every problem a line in `problems`.
static std::vector<std::shared_ptr<const Gen::Pack>> ReadPacks(const std::string&        dir,
                                                               std::vector<std::string>& problems)
{
    std::vector<std::shared_ptr<const Gen::Pack>> out;
    for (Gen::Pack& p : Gen::LoadPacks(dir, problems))
        out.push_back(std::make_shared<const Gen::Pack>(std::move(p)));
    return out;
}

// How often the galaxy is written to world.json while the server runs (seconds).
static const double WORLD_SAVE_INTERVAL = 60.0;

// Set up the authoritative simulation for the server host: generate the galaxy, restore
// what the save says about it, materialize all systems.
//
// worldPath non-empty: restore the galaxy from it if the file exists. LoadWorld does its
// own Reset+InitGalaxy and restores only the aggregates (population, security, prosperity,
// controllers) plus simulation time, and holds what players changed in the static layer
// (#38) until materialization, which builds every system from data and replays the changes
// on top -- so it happens afterwards either way. An empty worldPath keeps a run ephemeral (the
// self-tests).
//
// Nothing hand-written is needed (#342 S2): the galaxy is `centres` civilisation centres and
// a frontier region beside each, all from `newSeed` -- or from what the saved world says it
// was made with, which wins over both.
static void SetupHostSim(Simulation& sim, const std::string& dataDir,
                         const std::string& worldPath = std::string(), uint64_t newSeed = 1,
                         int centres = Gen::DEFAULT_CENTRES, int maxRegions = 0, int maxSystems = 0)
{
    SetRandomSeed(0xC0FFEEu);
    // Fatal like the archetypes below: a world whose factions did not read would be a world
    // of four default factions that never reach out, served as if it were this one (#357).
    if (!Factions::Load(dataDir + "factions.json"))
    {
        Log::Errorf("FATAL: %s\n", Factions::Error().c_str());
        exit(1);
    }
    // What markets trade (#343), before anything is priced or carried.
    if (!Goods::Load(dataDir + "goods.json"))
    {
        fprintf(stderr, "FATAL: %s\n", Goods::Error().c_str());
        exit(1);
    }
    // Must precede materialization: every entity constructor looks itself up here, and a
    // world whose objects have no components is a world where nothing can be docked with.
    //
    // So a registry that will not load stops the server rather than serving an undockable,
    // invisible world to everyone who connects (#191).
    if (!Archetypes::Load(dataDir + "archetypes.json"))
    {
        Log::Errorf("FATAL: %s\n", Archetypes::Error().c_str());
        exit(1);
    }
    // After the archetypes, which every blueprint is checked against. Fatal for the same
    // reason: a blueprint that does not read is a structure that costs nothing (#39).
    if (!Blueprints::Load(dataDir + "blueprints.json"))
    {
        Log::Errorf("FATAL: %s\n", Blueprints::Error().c_str());
        exit(1);
    }

    // ...and how it grows (#342 S3), fatal for the same reason: a world whose tide did not read
    // would grow by rules nobody chose.
    if (!WorldRules::Load(dataDir + "world_rules.json"))
    {
        Log::Errorf("FATAL: %s\n", WorldRules::Error().c_str());
        exit(1);
    }

    // A saved world says which seed made it, under which rules, with how many centres, and
    // which regions have opened since; a new one takes `newSeed` and `centres`. Each region
    // opened is sealed in regions/ beside it (S3), so a build with other region rules goes on
    // with it -- unless a region is not sealed, or the skeleton's rules changed: the same seed
    // read by other rules is another galaxy.
    uint64_t                seed = newSeed;
    Simulation::WorldHeader header;
    const bool saved = !worldPath.empty() && Simulation::ReadWorldHeader(worldPath, header);

    // Admin packs (#342 S7), read whole and every problem named. A new world is not made from
    // a bad one -- it does not exist yet, so nothing is lost by stopping -- while a saved world
    // has its packs sealed and is only told.
    const std::string        packsDir = g_packsDir.empty() ? dataDir + "packs/" : g_packsDir;
    std::vector<std::string> packProblems;
    auto                     packs = ReadPacks(packsDir, packProblems);
    if (!packProblems.empty() && !saved)
    {
        fprintf(stderr, "FATAL: the admin packs in %s have problems; no world is made:\n",
                packsDir.c_str());
        for (const std::string& p : packProblems)
            fprintf(stderr, "  %s\n", p.c_str());
        exit(1);
    }
    for (const std::string& p : packProblems)
        fprintf(stderr, "Pack: %s -- not read\n", p.c_str());
    sim.SetPacks(std::move(packs), saved ? header.packs : std::vector<Gen::Woven>{});
    const std::string sealDir = worldPath.empty() ? std::string() : Host::RegionsDir(worldPath);
    sim.SetSealDir(sealDir);
    if (saved)
    {
        const std::string why = Host::WorldRulesRefusal(header, sealDir);
        if (!why.empty())
        {
            // Accounts are kept: one whose system the new rules do not have chooses a start
            // again (#342), so moving the world aside costs the galaxy and nothing else.
            Log::Errorf("FATAL: %s: %s\n", worldPath.c_str(), why.c_str());
            exit(1);
        }
        if (header.generator != Gen::GENERATOR_VERSION)
            Log::Printf("World made under region rules v%d: its %d regions are sealed, and regions "
                        "that open from now on follow v%d.\n",
                        header.generator, (int)header.log.size(), Gen::GENERATOR_VERSION);
        seed = header.seed;
        if (header.centres >= 1)
            centres = header.centres;
        // The cap is the admin's: what they ask for now, else what the world ran with.
        if (maxRegions <= 0)
            maxRegions = header.maxRegions;
        if (maxSystems <= 0)
            maxSystems = header.maxSystems;
    }
    Gen::WorldParams params;
    params.seed = seed;
    params.centres = std::max(1, std::min(centres, Gen::MAX_CENTRES));
    // The skeleton is laid out up to the cap. A slot is where it is whatever the cap, so a
    // larger one only adds outer slots.
    params.cap = std::max(Gen::DEFAULT_REGION_CAP, maxRegions);
    params.owners = Factions::MajorIds();
    params.pirates = Factions::Id(Factions::Pirates());
    params.unaffiliated = Factions::Id(Factions::Unaffiliated());
    if (!sim.OpenGalaxy(params, dataDir,
                        saved ? header.log : std::vector<Simulation::RegionOpening>{}))
    {
        Log::Errorf("FATAL: seed %llu made a galaxy with nowhere to start.\n",
                    (unsigned long long)seed);
        exit(1);
    }
    if (!saved && !sim.PackProblems().empty())
    {
        fprintf(stderr,
                "FATAL: the admin packs could not be woven into a new world of seed "
                "%llu; no world is made:\n",
                (unsigned long long)seed);
        for (const std::string& p : sim.PackProblems())
            fprintf(stderr, "  %s\n", p.c_str());
        exit(1);
    }
    for (const Gen::Woven& w : sim.WovenPacks())
        printf("  pack: %s, %s %s%s\n", w.id.c_str(), w.kind == "region" ? "region" : "into region",
               std::to_string(w.slot).c_str(), w.system.empty() ? "" : (" as " + w.system).c_str());
    for (const std::string& change : sim.PackChanges())
        fprintf(stderr, "Pack: %s\n", change.c_str());
    sim.SetGrowthCap(maxRegions, maxSystems);
    Log::Printf("Galaxy: seed %llu, %d civilisation centre(s), %d regions, %d systems (the cap: %d "
                "regions, %d systems).\n",
                (unsigned long long)seed, sim.CentreCount(), (int)sim.OpenSlots().size(),
                (int)sim.Universe().systems.size(), sim.MaxRegions(), sim.MaxSystems());
    for (const Simulation::StartPoint& sp : sim.Starts())
        Log::Printf("  start: %s (%s), %s's\n", sp.systemName.c_str(), sp.station.c_str(),
                    sp.owner.c_str());

    sim.Seed(0xC0FFEEu);
    // A world from a newer build, or a damaged one, stops the server rather than being
    // replaced by a new galaxy at the first checkpoint (#20, #341).
    std::string why;
    switch (Host::OpenWorld(sim, worldPath, why))
    {
        case Host::WorldStart::Restored:
            Log::Printf("World restored from %s (t=%.0fs).\n", worldPath.c_str(), sim.Time());
            break;
        case Host::WorldStart::Fresh: break;
        case Host::WorldStart::Refused: Log::Errorf("FATAL: %s\n", why.c_str()); exit(1);
    }
    // Before any account loads: an account names its ships' designs and nothing more. A
    // file from a later build, or a damaged one, is refused like the world's, for the same
    // reason -- the next checkpoint would write this build's reading of it back (#20, #341).
    if (!worldPath.empty())
    {
        const Save::Result designs = sim.LoadDesigns(DesignsPath(worldPath));
        if (designs == Save::Result::TooNew || designs == Save::Result::Corrupt)
        {
            Log::Errorf("FATAL: %s is %s (this build speaks designs schema v%d).\n"
                        "Move it aside or upgrade the server; it has NOT been modified.\n",
                        DesignsPath(worldPath).c_str(),
                        designs == Save::Result::TooNew ? "from a newer build" : "damaged",
                        Save::DESIGNS_VERSION);
            exit(1);
        }
    }
    sim.MaterializeAllSystems(dataDir + "systems/");
}

// A player at a start (#342 S2), the first one unless told which. The server makes one per
// connection once the player has a place; the self-tests make exactly one and fly it.
static ClientSession& SetupHostPlayer(Simulation& sim, std::string start = std::string())
{
    if (start.empty())
        start = sim.Starts().front().systemId;
    return sim.CreateSession(start, sim.SafeArrival(start), GetShipCatalog()[0].stats);
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
    // physics (the ship is carried by the station, #298), but we still handle trade/undock.
    if (c.dock && !s.IsDocked())
        sim.StepPlayerDock(s, *sim.SystemOf(s));
    if (c.undock)
        sim.StepPlayerUndock(s);

    // Debug (F1): credit money to the server account -- only on a server started with --dev
    // (#341). Anyone who could open a socket used to be able to print money.
    if (c.debugMoney && !sim.GrantDebugMoney(s))
        s.RecordEvent(Ev::Kind::Notice, "Debug money refused: this server is not in dev mode");

    // Naming the system the ship is in (#145), as its discoverer. Not a station verb: the
    // map offers it in flight, and filed under the docked ones it was dropped without a
    // word for everyone not at a station (#308). The server says why it refuses, in the
    // journal; an accepted name reaches everyone in the galaxy index.
    if (!c.nameSystem.empty())
        sim.NameSystem(s, c.nameSystem);

    // A refit (#279 fitting). Not under the docked verbs below: asked for in flight, it is
    // refused with a reason rather than dropped without a word.
    if (c.refit)
        sim.RefitShip(s, c.refitLines);
    // The design library (#279 step 3): saved, renamed and deleted anywhere; applied as a
    // refit is, which says why when it is not at a shipyard.
    if (!c.saveDesign.empty())
        sim.SaveDesign(s, c.saveDesign, c.saveLines);
    if (!c.renameDesign.empty())
        sim.RenameDesign(s, c.renameDesign, c.renameTo);
    if (!c.deleteDesign.empty())
        sim.DeleteDesign(s, c.deleteDesign);
    if (!c.applyDesign.empty())
        sim.ApplyDesign(s, c.applyDesign);

    // Station trade/hangar: sell, refit, buy, pay off bounty. Account effects
    // (money/skill/reputation/bounty) are applied by the core in account_ (server-authoritative).
    if (s.IsDocked())
    {
        if (c.sellType >= 0 && c.sellAmount > 0)
        {
            Simulation::PlayerSellResult r =
                sim.StepPlayerSell(s, *sim.SystemOf(s), c.sellType, c.sellAmount);
            if (r.sold > 0)
                acks.push_back(
                    Proto::TradeAck{ c.sellType, r.sold, r.gross, r.revenue, 0, 0.0, r.fee });
        }
        if (c.buyType >= 0 && c.buyAmount > 0)  // a refusal says why, in the journal (#343)
        {
            const Simulation::PlayerBuyResult r =
                sim.StepPlayerBuy(s, *sim.SystemOf(s), c.buyType, c.buyAmount);
            if (r.bought > 0)
                acks.push_back(Proto::TradeAck{ c.buyType, 0, 0.0, 0.0, r.bought, r.cost, r.fee });
        }
        if (c.refitShip >= 0)
            sim.SwitchShip(s, c.refitShip);  // only to a ship this account owns (#5)
        if (c.buyShip >= 0)
            sim.BuyShip(s, c.buyShip);  // purchase (deducts money)
        FactionId bountyTo;
        if (!c.payBountyFaction.empty() && Factions::Find(c.payBountyFaction, bountyTo))
            sim.PayBounty(s, bountyTo);  // clear bounty
        if (c.acceptOffer >= 0)
            sim.AcceptMission(s, c.acceptOffer);  // accept a mission from the board
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
        o.avoidDanger = c.orderAvoidDanger;
        o.blueprint = c.orderBlueprint;
        o.name = c.orderName;
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
        else
            // Too heavy for an unstable mouth (#342 S4): said, since nothing else would show it.
            for (const auto& e : sim.SystemOf(s)->entities)
                if (e->GetId() == c.jumpGateId)
                {
                    const std::string barred = sim.MouthProblem(s, *e);
                    if (!barred.empty())
                        s.RecordEvent(Ev::Kind::Notice, "Cannot jump: " + barred);
                }
    }
    if (c.lootId != 0)
        sim.StepPlayerLoot(s, *sim.SystemOf(s), c.lootId);
    // A site, laid down from space (#39); the answer is in the journal either way.
    if (!c.deploy.empty())
        sim.Deploy(s, c.deploy, c.deployPos, c.deployName);
    // ...and taken apart by its owner, the same way.
    if (c.dismantleId != 0)
        sim.Dismantle(s, c.dismantleId);
    // ...and told whom to fire at (#340).
    if (c.defenceId != 0)
        sim.SetDefence(s, c.defenceId, c.defence);

    // Player fire: on a shot — a beam event into the snapshot (client draws it blue).
    // Account effects (mission credit/reputation) are not yet applied on the server (3c-ii).
    Simulation::PlayerCombatEvents ev;
    s.selectedId = c.targetId;  // what it is told about in full, wherever it is (#349)
    if (sim.StepPlayerFire(s, *sim.SystemOf(s), c.targetId, dt, &ev))
    {
        FireEvent fe;
        fe.from = ev.shotFrom;
        fe.to = ev.shotTo;
        fe.shooterFaction = Factions::Unaffiliated();
        fe.shooterSessionId = s.id;  // "mine" for this player and nobody else
        fires[s.systemId].push_back(fe);
    }
    sim.StepPlayerMining(s, *sim.SystemOf(s), s.account.GetSkills().GetBonus(SkillType::Mining),
                         dt);
    return changed;
}

// World step (every system + maintenance) — on the server timer, independent of inputs.
//
// Every system is stepped the same way; the only difference between them is who happens
// to be standing in one (#3). NPCs see those players, shoot at them, and the beams are
// collected per system so each client is told about its own sky. A system with nobody in
// it keeps living, which is the premise of the whole galaxy -- once nobody has been there
// for a minute, as numbers in the coarse pass rather than as ships stepped here (#295).
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
        p.hidden = sim.InCover(s);  // a nebula, say: hidden from NPCs and players alike
        present[s.systemId].push_back(p);
    }

    static const std::vector<Simulation::PlayerPresence> kEmpty;
    for (auto& kv : sim.Systems())
    {
        // A cold system has no ships, and its prices recover in the coarse pass (#295).
        if (!Simulation::IsHot(kv.second))
            continue;
        auto                                           it = present.find(kv.first);
        const std::vector<Simulation::PlayerPresence>& players =
            (it == present.end()) ? kEmpty : it->second;
        sim.StepSystemAgents(kv.second, players, players.empty() ? nullptr : &fires[kv.first], dt);
    }

    for (auto& kv : sim.Sessions())
    {
        ClientSession& s = kv.second;
        if (s.ship && !s.ship->IsAlive())
            sim.ServerRespawnPlayer(s);

        // After the world moved: a docked ship goes round with its station, and a hold
        // target's velocity is measured for a follow (#298).
        sim.StepPlayerAttachment(s, dt);

        // Standing orders execute here rather than in the input path: an agent issues one
        // order and then sends nothing, so if the order did not drive its own ticks the
        // ship would simply sit there.
        if (SystemState* st = sim.SystemOf(s))
            sim.StepPlayerOrder(s, *st, dt);
        for (const ClientSession::Shot& shot : s.orderShots)
        {
            FireEvent fe;
            fe.from = shot.from;
            fe.to = shot.to;
            fe.shooterFaction = Factions::Unaffiliated();
            fe.shooterSessionId = s.id;
            fires[s.systemId].push_back(fe);
        }
        s.orderShots.clear();

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
    // The system whose layout this client was last sent. Any way of arriving somewhere
    // else -- a command, a standing order's route, a respawn -- makes it differ, and that
    // is what sends the new one: an agent that travelled by order used to keep the old
    // system's stations in its picture of the new one.
    std::string                         layoutSystem;
    std::set<std::string>               sentDesigns;       // DesignDefs this client has (#279)
    int                                 factionsSent = 0;  // registry size last sent (#342 S5)
    std::unique_ptr<Net::TcpConnection> conn;
    int                                 sessionId = 0;  // 0 until the hello arrives
    std::string                         account;
    int                                 lastSeq = 0;       // last input acked back
    int                                 lastEventSeq = 0;  // last journal entry delivered
    bool                                wasDocked = false;
    bool                                accountReadOnly = false;  // their file is newer (#20)
    bool                                evicted = false;  // displaced by a later login (#105)
    // The connection is gone and the ship is still in space, for Simulation::LINGER_SECONDS
    // (#341). The session is saved and ended when that runs out, or when its owner logs in
    // again -- which displaces it like any other earlier connection.
    bool lingering = false;
    // The snapshots this client was sent and the newest it acknowledged (#349): what the next
    // one is a delta against.
    Proto::SnapshotSender snapOut;

    // The login in progress (#106). A name has been claimed and a challenge sent; none of
    // it means anything until the proof arrives.
    std::string pendingAccount;
    std::string nonce;          // this connection's, single use
    std::string challengeSalt;  // the salt the client was told to use
    std::string fileStored;     // what the account file holds, for an account that exists
    bool        isNewAccount = false;
    double      silentFor = 0.0;  // seconds connected without a hello
    // Logged in, and choosing a start (#342 S2): no session until the answer comes.
    bool        awaitingStart = false;
    std::string authStored;  // a new account's secret, kept until there is a session for it
    // Taken off the server by its operator -- a ban, or off the allowlist (#400) -- and told
    // this; the connection is gone and the session saved and ended.
    std::string removedFor;

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

// How long a client that has said who it is may take to answer the challenge. Longer than
// the hello limit because answering is real work the server asked for: the secret is
// stretched a hundred thousand times (sim/Auth.cpp), which takes seconds on a slow or
// loaded machine. Holding both to ten seconds dropped a client mid-login while a second
// one was hashing next to it on the same CPU -- and it looked like a lost message.
static const double LOGIN_TIMEOUT = 30.0;

// How long a player may look at the starts before choosing (#342 S2): a person reading
// them, not a handshake, so minutes.
static const double START_TIMEOUT = 600.0;

static double LoginLimit(const HostClient& hc)
{
    if (hc.awaitingStart)
        return START_TIMEOUT;
    return hc.pendingAccount.empty() ? HELLO_TIMEOUT : LOGIN_TIMEOUT;
}

// Progress is stored per account name (#3). There was one account.json when there could
// only be one player; with two connected it would be one file two people write. `account`
// is canonical -- lower case (#341). `dir` is the world's accounts folder (#392).
static std::string AccountPath(const std::string& dir, const std::string& account)
{
    return Host::AccountFile(dir, account);
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
    double window = 0.0;    // seconds since the last report
    double reported = 0.0;  // bytes in the reports so far, for the metrics line (#400)

    // Everything sent since the server started.
    double Sent() const { return reported + snapshots + layouts + galaxies; }

    void Report(size_t clients)
    {
        const double total = snapshots + layouts + galaxies;
        if (window <= 0.0 || total <= 0.0)
            return;
        reported += total;
        const double perSec = total / window;
        Log::Printf("net: %zu client(s), %.1f KB/s out (%.1f KB/s each) -- snapshots %.0f%%, "
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
// How often the metrics line is written (#400).
static const double METRICS_INTERVAL = 60.0;

// What changed in the static layer since the last call, to everyone whose picture of that
// system it changes (#38). Called once per loop, after every input and every world tick
// that could have changed anything and before any snapshot is built: deltas and snapshots
// share one ordered connection, so a snapshot can never arrive ahead of the change it
// reflects. Nobody in a system means nobody is told -- whoever arrives later is sent the
// whole layout, which already has it. Counted as layout traffic.
//
// What only an owner may read is cut for everyone else (#340): one message for the system,
// and one of their own for a client whose structure's allow-list the delta carries.
static void SendLayoutDeltas(std::vector<HostClient>& clients, Simulation& sim, NetStats& net)
{
    for (const Proto::LayoutDelta& d : sim.TakeLayoutDeltas())
    {
        Proto::LayoutDelta shared = d;
        const bool         cut = Proto::ForViewer(shared, std::string());
        const std::string  msg = Proto::EncodeLayoutDelta(shared);
        for (HostClient& hc : clients)
            if (hc.sessionId != 0 && hc.conn && hc.layoutSystem == d.systemId)
            {
                std::string theirs = msg;
                if (cut && Proto::OwnsPrivate(d, hc.account))
                {
                    Proto::LayoutDelta own = d;
                    Proto::ForViewer(own, hc.account);
                    theirs = Proto::EncodeLayoutDelta(own);
                }
                net.layouts += (double)theirs.size();
                hc.conn->Send(theirs);
            }
    }
}

// The whole layout of a system, as `account` may read it (#340).
static std::string LayoutFor(const Simulation& sim, const std::string& systemId,
                             const std::string& account)
{
    Proto::SystemLayout lay = sim.BuildLayout(systemId);
    Proto::ForViewer(lay, account);
    return Proto::EncodeLayout(lay);
}

// The galaxy index goes to one client: at login, before its first layout, so by the time
// it is told which system it is in it already knows what that system is called (#206).
// Counted with the galaxy statistics -- it is the same kind of message, only rarer.
static void SendUniverse(HostClient& hc, const Simulation& sim, NetStats& net)
{
    // The faction registry before it, when it has grown since this client was last sent it
    // (#342 S5): the index names a native faction as the owner of its systems. It only grows,
    // so its size says whether it changed.
    if (hc.factionsSent != Factions::Count())
    {
        const std::string f = Proto::EncodeFactions(Proto::RegistryMessage());
        net.galaxies += (double)f.size();
        hc.conn->Send(f);
        hc.factionsSent = Factions::Count();
    }
    const std::string u = Proto::EncodeUniverse(sim.KnownUniverse());  // what is charted (#144)
    net.galaxies += (double)u.size();
    hc.conn->Send(u);
}

// ...and to everyone logged in, whenever the index changes: when a first ship charts a
// system (#144). Knowledge is shared, so everyone gets the same index.
static void BroadcastUniverse(std::vector<HostClient>& clients, const Simulation& sim,
                              NetStats& net)
{
    for (HostClient& hc : clients)
        if (hc.sessionId != 0 && hc.conn)
            SendUniverse(hc, sim, net);
}

// A logged-in player into the game (#3, #342 S2): a session at `start` -- or wherever their
// account says they were, which LoadAccount puts them back to -- then the index, the layout
// and the galaxy, in that order.
static void JoinPlayer(Simulation& sim, HostClient& hc, const std::string& accountsDir,
                       const std::string& start, NetStats& net)
{
    ClientSession& s = SetupHostPlayer(sim, start);
    hc.sessionId = s.id;
    s.accountName = hc.account;          // what they build is theirs (#39)
    s.ship->SetPilotName(hc.account);    // what other players see (#4)
    hc.lastEventSeq = s.LastEventSeq();  // a new session starts from now
    const Save::Result acct = sim.LoadAccount(s, AccountPath(accountsDir, hc.account));
    if (acct == Save::Result::TooNew)
    {
        // They can play, but their file is left exactly as it is. Handing
        // a player a fresh account and then saving it over the real one is
        // the failure this whole check exists to prevent (#20).
        hc.accountReadOnly = true;
        Log::Printf("%s: account file is from a newer build; joining with a "
                    "fresh account and NOT saving over it.\n",
                    hc.account.c_str());
    }
    else if (acct == Save::Result::Corrupt)
    {
        // Damaged since the hello was answered: the same rule, never
        // written over (#341).
        hc.accountReadOnly = true;
        Log::Printf("%s: account file is damaged; joining with a fresh account "
                    "and NOT saving over it.\n",
                    hc.account.c_str());
    }
    else if (acct == Save::Result::Ok)
        Log::Printf("%s joined (money %.0f).\n", hc.account.c_str(), s.account.GetMoney());
    else
        Log::Printf("%s joined, new account (money %.0f).\n", hc.account.c_str(),
                    s.account.GetMoney());

    // Set after the load, not before: an account file with no secret in it
    // (one written before #106) would otherwise overwrite what was just
    // registered with nothing.
    if (hc.isNewAccount)
    {
        s.authSalt = hc.challengeSalt;
        s.authStored = hc.authStored;
        if (acct == Save::Result::Ok)
            Log::Printf("%s: an account that had no secret now has one.\n", hc.account.c_str());
    }
    hc.wasDocked = s.IsDocked();
    SendUniverse(hc, sim, net);  // the index first: the layout names a system
    const std::string lay = LayoutFor(sim, s.systemId, hc.account);
    const std::string gal = Proto::EncodeGalaxy(sim.BuildGalaxyState());
    net.layouts += (double)lay.size();
    net.galaxies += (double)gal.size();
    hc.conn->Send(lay);
    hc.layoutSystem = s.systemId;
    hc.conn->Send(gal);
}

// How often each client is sent a snapshot. Not once per loop iteration, which is what it
// used to be -- that made the send rate an accident of how fast the loop happened to spin,
// and it was spinning far faster than anything could be seen (#16).
//
// The client renders the world 100 ms in the past and interpolates between the two
// snapshots around that time, precisely so updates can be sparse. Twenty a second puts
// them 50 ms apart, comfortably inside that window. The simulation still runs at 60 Hz;
// what changed is how often the result is described, not how often it is computed.
static const double SNAPSHOT_INTERVAL = 1.0 / 20.0;

// Opens skeleton slot `slot` the way the tide will (#342): from an open neighbour, every slot
// on the way opened first -- along the shortest path through the skeleton from what is open,
// lowest slots first among equals. What is in a region does not depend on this; only where
// its mouth is does.
static bool OpenSlot(Simulation& sim, int slot, const std::string& dataDir)
{
    const Gen::GalaxySkeleton& g = sim.Skeleton();
    if (sim.OpenSlots().count(slot) != 0)
        return true;
    if (slot < 0 || slot >= (int)g.slots.size())
        return false;
    std::map<int, int> cameFrom;
    std::deque<int>    todo;
    for (int open : sim.OpenSlots())
    {
        cameFrom[open] = -1;
        todo.push_back(open);
    }
    while (!todo.empty() && cameFrom.count(slot) == 0)
    {
        const int at = todo.front();
        todo.pop_front();
        for (int n : g.slots[(size_t)at].neighbours)
            if (cameFrom.count(n) == 0)
            {
                cameFrom[n] = at;
                todo.push_back(n);
            }
    }
    if (cameFrom.count(slot) == 0)
        return false;
    std::vector<int> path;
    for (int at = slot; cameFrom[at] >= 0; at = cameFrom[at])
        path.push_back(at);
    for (auto it = path.rbegin(); it != path.rend(); ++it)
        if (!sim.OpenSlotFrom(*it, cameFrom[*it], dataDir))
            return false;
    return true;
}

// One open region as JSON: its index entries, its links and every system document, exactly
// what the server would build.
static nlohmann::json RegionJson(const Simulation& sim, int slot)
{
    nlohmann::json out;
    out["systems"] = nlohmann::json::array();
    for (const auto& info : sim.Universe().systems)
        if (Gen::SlotOf(info.id) == slot && sim.RegionDocuments().count(info.id))
            out["systems"].push_back({ { "id", info.id },
                                       { "name", info.name },
                                       { "map", { info.mapPos.x, info.mapPos.y } },
                                       { "security", info.security },
                                       { "owner", info.owner } });
    out["links"] = nlohmann::json::array();
    for (const auto& l : sim.Universe().links)
        if (Gen::SlotOf(l.a) == slot || Gen::SlotOf(l.b) == slot)
            out["links"].push_back({ l.a, l.b });
    out["documents"] = nlohmann::json::object();
    for (const auto& kv : sim.RegionDocuments())
        if (Gen::SlotOf(kv.first) == slot)
            out["documents"][kv.first] = nlohmann::json::parse(kv.second);
    return out;
}

// The galaxy a seed makes, as JSON on stdout: the skeleton of `cap` slots with `centres`
// civilisation centres, and the regions a new world opens with -- every centre and the
// frontier beside each -- plus those of `slots`, each opened as OpenSlot does. Nothing
// generated is ever saved -- it is remade from the seed -- so this is how a developer or a
// bot finds where something is without flying there: `econserver galaxydump 8 | jq ...`.
// `regiondump` prints one region (the first centre unless a slot is named) in the shape it
// always had.
static int GalaxyDump(uint64_t seed, int cap, int centres, std::vector<int> slots, bool skeleton)
{
    SetTraceLogLevel(LOG_NONE);  // stdout is the JSON
    std::string dataDir = DataDir::Path();
    Factions::Load(dataDir + "factions.json");  // who owns the centres
    for (int s : slots)
        cap = std::max(cap, s + 1);  // a slot is where it is whatever the cap
    Gen::WorldParams params;
    params.seed = seed;
    params.cap = cap;
    params.centres = std::max(1, std::min(centres, Gen::MAX_CENTRES));
    params.owners = Factions::MajorIds();
    params.pirates = Factions::Id(Factions::Pirates());
    params.unaffiliated = Factions::Id(Factions::Unaffiliated());
    Simulation sim;
    if (!sim.OpenGalaxy(params, dataDir))
    {
        fprintf(stderr, "galaxydump: %d slots have no room for a centre\n", cap);
        return 1;
    }
    if (!skeleton && slots.empty())
        slots.push_back(sim.Skeleton().centres.front());
    for (int s : slots)
        if (!OpenSlot(sim, s, dataDir))
        {
            fprintf(stderr, "galaxydump: slot %d could not be opened\n", s);
            return 1;
        }
    nlohmann::json out;
    out["seed"] = seed;
    out["generator"] = Gen::GENERATOR_VERSION;
    if (!skeleton)
    {
        // regiondump: one region, as before slots
        const nlohmann::json r = RegionJson(sim, slots.front());
        out["slot"] = slots.front();
        out["systems"] = r["systems"];
        out["documents"] = r["documents"];
    }
    else
    {
        out["skeleton"] = Gen::SkeletonJson(sim.Skeleton());
        out["starts"] = nlohmann::json::array();
        for (const Simulation::StartPoint& sp : sim.Starts())
            out["starts"].push_back({ { "system", sp.systemId },
                                      { "name", sp.systemName },
                                      { "station", sp.station },
                                      { "owner", sp.owner } });
        out["regions"] = nlohmann::json::object();
        for (int s : sim.OpenSlots())
            out["regions"][std::to_string(s)] = RegionJson(sim, s);
    }
    printf("%s\n", out.dump(2).c_str());
    return 0;
}

// How the tide stands, region by region (#342 S3): what the console's `tide` and the batch
// mode's hourly line print.
static void PrintTide(const Simulation& sim, const char* indent)
{
    Log::Printf("%stide (threshold %.0f, %d of %d regions, %d young mouth(s) of %d%s%s):", indent,
                sim.TideThreshold(), (int)sim.OpenSlots().size(), sim.MaxRegions(),
                sim.YoungMouths(), sim.MouthsAllowed(), sim.GrowthPaused() ? ", PAUSED" : "",
                sim.AtCap() ? ", AT CAP" : "");
    for (const auto& kv : sim.Tides())
        if (sim.IsEdgeRegion(kv.first))
            Log::Printf(" R%d %.0f", kv.first, kv.second.tide);
    if (sim.SurplusTide() > 0.0)
        Log::Printf("  surplus %.0f", sim.SurplusTide());
    // The mouths that are not simply open (#342 S4), and what the archive holds.
    Log::Printf("  | mouths:");
    for (const Simulation::RegionOpening& o : sim.OpeningLog())
        if (o.parent >= 0 && o.state != "open" && o.state != "returned")
        {
            Log::Printf(" R%d %s", o.slot, o.state.c_str());
            if (o.state == "unstable")
                Log::Printf(" (%.1f h left)", (o.closesAt - sim.Time()) / 3600.0);
            else if (o.state == "anchored")
                Log::Printf(" (%s)", o.anchoredBy.c_str());
        }
    Log::Printf("  | archive: %d\n", (int)sim.Archives().size());
}

// What an opening is, in a line: where, from where, and why.
static void PrintOpening(const Simulation& sim, int slot, const char* indent)
{
    const Simulation::RegionOpening* o = sim.OpeningOf(slot);
    if (o != nullptr)
        Log::Printf("%sregion %d opened from region %d (%s), by %s%s%s; %s%s; %d systems in all\n",
                    indent, o->slot, o->parent, o->mouth.c_str(), o->by.c_str(),
                    o->why.empty() ? "" : ": ", o->why.c_str(), o->state.c_str(),
                    o->archive >= 0 ? (", back from archive " + std::to_string(o->archive)).c_str()
                                    : "",
                    (int)sim.Universe().systems.size());
}

// The admin's console (#342 S3): lines typed on the server's standard input, read on a thread
// of their own so the loop never waits for one, and handed over under a lock. With no console
// -- input from /dev/null -- the thread finds the end at once and is gone.
struct AdminConsole
{
    std::mutex              lock;
    std::deque<std::string> lines;

    void Start()
    {
        std::thread(
            [this]()
            {
                std::string line;
                while (std::getline(std::cin, line))
                {
                    std::lock_guard<std::mutex> g(lock);
                    lines.push_back(line);
                }
            })
            .detach();
    }
    bool Take(std::string& line)
    {
        std::lock_guard<std::mutex> g(lock);
        if (lines.empty())
            return false;
        line = std::move(lines.front());
        lines.pop_front();
        return true;
    }
};

// Who may come in (#400), as the console shows it: whether the list is checked, who is on it,
// who is banned, and any line of the files that was not a name.
static void PrintAccess(const Host::AccessList& access)
{
    Log::Printf("Access: %s; %d on %s, %d banned in %s.\n",
                access.ListChecked() ? "by allowlist" : "registration open",
                (int)access.Allowed().size(), access.AllowlistPath().c_str(),
                (int)access.Bans().size(), access.BansPath().c_str());
    std::string names;
    for (const std::string& n : access.Allowed())
        names += " " + n;
    if (!names.empty())
        Log::Printf("  allowed:%s\n", names.c_str());
    for (const auto& kv : access.Bans())
        Log::Printf("  banned: %s%s%s\n", kv.first.c_str(), kv.second.empty() ? "" : " -- ",
                    kv.second.c_str());
    for (const std::string& p : access.Problems())
        Log::Printf("  ignored: %s\n", p.c_str());
}

// One console line, applied: `tide`, `open [region]`, `drift`, `pause`, `resume`; `allow`,
// `deny`, `ban`, `unban`, `access`, `reload` (#400); anything else is `help`.
static void AdminCommand(Simulation& sim, Host::AccessList& access, const std::string& line)
{
    std::istringstream in(line);
    std::string        verb;
    in >> verb;
    if (verb.empty())
        return;
    if (verb == "tide")
        PrintTide(sim, "");
    else if (verb == "open")
    {
        int from = -1;
        in >> from;
        std::string why;
        const int   slot = sim.OpenMouthNow(from, why);
        if (slot < 0)
            Log::Printf("No mouth opened: %s.\n", why.c_str());
    }
    else if (verb == "drift")
    {
        int slot = -1;
        in >> slot;
        std::string why;
        if (sim.ArchiveRegionNow(slot, why) < 0)
            Log::Printf("Region %d did not drift away: %s.\n", slot, why.c_str());
    }
    else if (verb == "allow" || verb == "deny" || verb == "ban" || verb == "unban")
    {
        // Whoever is now refused is taken off the server by the loop, which sees the list's
        // generation change.
        std::string name, reason;
        in >> name;
        std::getline(in, reason);
        bool ok = false;
        if (name.empty())
            Log::Printf("Usage: %s <name>%s\n", verb.c_str(), verb == "ban" ? " [reason]" : "");
        else if (verb == "allow")
            Log::Printf("%s.\n", access.Allow(name, ok).c_str());
        else if (verb == "deny")
            Log::Printf("%s.\n", access.Deny(name, ok).c_str());
        else if (verb == "ban")
            Log::Printf("%s.\n", access.Ban(name, reason, ok).c_str());
        else
            Log::Printf("%s.\n", access.Unban(name, ok).c_str());
    }
    else if (verb == "access")
        PrintAccess(access);
    else if (verb == "reload")
    {
        access.Reload();
        PrintAccess(access);
    }
    else if (verb == "pause" || verb == "resume")
    {
        sim.PauseGrowth(verb == "pause");
        Log::Printf("Growth %s.\n",
                    verb == "pause" ? "paused: no tide gathers and no mouth opens" : "resumed");
    }
    else
        Log::Printf("Commands: tide | open [region] (a mouth now, from the region named or the one "
                    "nearest to breaking) | drift <region> (into the archive now) | pause | resume "
                    "(growth) | allow <name> | deny <name> | ban <name> [reason] | unban <name> | "
                    "access (who may come in) | reload (allowlist.txt and bans.txt). With the "
                    "server stopped: econserver regenerate <region>.\n");
}

// raylib's own warnings (a region that could not be sealed, a pin not applied) into the
// server's log, stamped like everything else; below a warning it has nothing to say here.
static void RaylibToLog(int level, const char* fmt, va_list args)
{
    if (level < LOG_WARNING)
        return;
    std::string line = std::string(level >= LOG_ERROR ? "ERROR: " : "WARNING: ") + fmt + "\n";
    Log::VPrintf(true, line.c_str(), args);
}

static int RunHost(unsigned short port, bool isPublic, bool openRegistration, uint64_t newSeed,
                   bool dev, int centres, bool randomStarts, int maxRegions, int maxSystems)
{
    // Everything said from here on goes to stderr with the time on it (#400).
    Log::SetStamped(true);
    SetTraceLogCallback(RaylibToLog);
    std::string dataDir = DataDir::Path();
    std::string worldPath = std::string(GetApplicationDirectory()) + "world.json";
    Simulation  sim;
    SetupHostSim(sim, dataDir, worldPath, newSeed, centres, maxRegions, maxSystems);

    // Accounts live with the world (#392): in accounts/ beside world.json. Until then they
    // were beside the executable -- which is where world.json is, so the files already
    // there are this world's and move in once. One already in accounts/ is never written
    // over; the old copy is left and named.
    const std::string accountsDir = Host::AccountsDir(worldPath);
    {
        const Host::AccountMove move = Host::MoveAccounts(GetApplicationDirectory(), accountsDir);
        if (!move.error.empty())
        {
            Log::Errorf("FATAL: %s: no account could be saved.\n", move.error.c_str());
            return 1;
        }
        if (move.moved > 0)
            Log::Printf("Accounts: %d file(s) moved into %s.\n", move.moved, accountsDir.c_str());
        for (const std::string& kept : move.kept)
            Log::Printf(
                "Accounts: %s was left where it is: %s already has one by that name, which is "
                "the one in use.\n",
                kept.c_str(), accountsDir.c_str());
    }
    // ...and an account knows which world it was saved in, so one from another world -- a
    // world.json moved aside, its accounts left -- is offered a start rather than a place
    // in a system that only shares a name with its own. A world from before has no
    // identity: it is given one now, and written at once, so no account is stamped with an
    // identity a crash could lose.
    if (sim.WorldId().empty())
    {
        sim.SetWorldId(Host::NewWorldId());
        if (!sim.SaveWorld(worldPath))
        {
            Log::Errorf("FATAL: %s could not be written.\n", worldPath.c_str());
            return 1;
        }
    }
    // Who may come in (#400): allowlist.txt and bans.txt beside world.json.
    Host::AccessList access;
    {
        const size_t      slash = worldPath.find_last_of("/\\");
        const std::string worldDir =
            slash == std::string::npos ? std::string() : worldPath.substr(0, slash + 1);
        std::error_code ec;
        const bool      listExists = std::filesystem::exists(worldDir + "allowlist.txt", ec);
        access.Open(worldDir,
                    Host::AccessList::ListRequired(isPublic, openRegistration, listExists));
        PrintAccess(access);
        if (access.ListChecked() && access.Allowed().empty())
            Log::Printf("Nobody can log in yet: add names with `allow <name>` here or to %s, or "
                        "start with --open to let anyone register.\n",
                        access.AllowlistPath().c_str());
    }
    int accessSeen = access.Generation();

    AdminConsole console;
    console.Start();
    if (randomStarts)
        Log::Printf("Starts are dealt at random; players do not choose.\n");
    // Which start a player is dealt: not the generator's, so not from its keys.
    std::mt19937_64 dealer(((uint64_t)std::random_device{}() << 32) ^ std::random_device{}());
    auto            roll = [&dealer]() { return (uint64_t)dealer(); };
    sim.SetDevCommands(dev);
    if (dev)
        Log::Printf("Dev mode: debug commands (F1's money) are honoured for every client.\n");

    if (!Net::Startup())
    {
        Log::Printf("host: WSAStartup FAIL\n");
        return 1;
    }
    Net::TcpListener listener;
    if (!listener.Listen(port, isPublic ? Net::TcpListener::Exposure::Public
                                        : Net::TcpListener::Exposure::Loopback))
    {
        Log::Printf("host: listen on port %d FAIL\n", port);
        Net::Shutdown();
        return 1;
    }
    std::signal(SIGINT, OnInterrupt);
    std::signal(SIGTERM, OnInterrupt);
    // The port actually listened on: `host 0` lets the system choose a free one, and this
    // line is where a script reads which (#306).
    Log::Printf("EconSpace server on port %d, %s. The galaxy runs with or without players; "
                "Ctrl+C to stop.\n",
                listener.Port(),
                isPublic ? "open to other machines"
                         : "this machine only (add --public to let other machines connect)");

    using clock = std::chrono::steady_clock;
    std::vector<HostClient> clients;
    // Connection, registration and login limits (#341). Its clock is the server's uptime.
    Host::Gate gate;
    const auto started = clock::now();
    double     gateExpireAcc = 0.0;
    int        refusedConnections = 0;  // since the last line about them
    double     refusalLogAcc = 0.0;

    NetStats     net;
    double       snapshotAcc = 0.0;   // time since the last round of snapshots
    double       worldSaveAcc = 0.0;  // world checkpoint timer
    const float  dt = 1.0f / 60.0f;
    auto         prev = clock::now();
    double       acc = 0.0;
    double       galaxyAcc = 0.0;  // galaxy-snapshot broadcast timer (map statistics)
    std::string  lastGalaxy;       // what the last broadcast said
    Log::Metrics metrics;          // the once-a-minute line (#400)
    double       metricsAcc = 0.0;
    double       accessCheckAcc = 0.0;  // allowlist.txt and bans.txt looked at (#400)

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
        const double uptime = std::chrono::duration<double>(now - started).count();
        while (std::unique_ptr<Net::TcpConnection> c = listener.Accept())
        {
            c->SetLimits(Net::TcpConnection::SERVER_MAX_FRAME_BYTES,
                         Net::TcpConnection::SERVER_MAX_SEND_BACKLOG);
            int total = 0, same = 0;
            for (const HostClient& other : clients)
                if (other.conn && other.conn->Alive())
                {
                    total++;
                    same += other.conn->PeerAddress() == c->PeerAddress() ? 1 : 0;
                }
            const std::string refusal = gate.RefuseConnection(total, same);
            if (!refusal.empty())
            {
                // Told why, then closed: a full server is not a crashed one.
                Proto::Bye bye;
                bye.reason = refusal;
                c->Send(Proto::EncodeBye(bye));
                refusedConnections++;
                continue;
            }
            HostClient hc;
            hc.conn = std::move(c);
            clients.push_back(std::move(hc));
            Log::Printf("Client connected; waiting for hello.\n");
        }
        // One line now and then, not one per refusal: a flood would otherwise be a log flood.
        refusalLogAcc += frame;
        if (refusedConnections > 0 && refusalLogAcc >= 10.0)
        {
            Log::Printf("Refused %d connection(s) over the limits (%d in all, %d per address).\n",
                        refusedConnections, gate.GetLimits().maxClients,
                        gate.GetLimits().maxPerAddress);
            refusedConnections = 0;
            refusalLogAcc = 0.0;
        }
        gateExpireAcc += frame;
        if (gateExpireAcc >= 60.0)
        {
            gateExpireAcc = 0.0;
            gate.Expire(uptime);
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
                    const int    version = Proto::MessageVersion(msg);
                    if (hc.sessionId == 0 && hc.pendingAccount.empty() &&
                        version != Proto::PROTO_VERSION)
                    {
                        // Told, with both numbers (#341): dropped in silence, a client one
                        // version behind looked exactly like a server that was down.
                        Proto::Bye bye;
                        bye.reason = Host::VersionRefusal(version);
                        hc.conn->Send(Proto::EncodeBye(bye));
                        Log::Printf("Rejected a hello from a client speaking protocol v%d (this "
                                    "server: v%d).\n",
                                    version, Proto::PROTO_VERSION);
                        hc.conn.reset();
                        break;
                    }
                    if (hc.sessionId != 0 || !hc.pendingAccount.empty() ||
                        !Proto::DecodeHello(msg, h) ||
                        (h.account = Host::CanonicalAccount(h.account)).empty())
                    {
                        Log::Printf("Rejected a hello (not a name, or a second one "
                                    "on the same connection).\n");
                        // Dropping the socket is the whole answer: a client that cannot
                        // say who it is has nothing to be told.
                        hc.conn.reset();
                        break;
                    }
                    // Not on the list, or banned (#400): told why, before a challenge -- so a
                    // name that may not come in cannot create its account either.
                    if (const std::string why = access.Refusal(h.account); !why.empty())
                    {
                        Proto::Bye bye;
                        bye.reason = why;
                        hc.conn->Send(Proto::EncodeBye(bye));
                        Log::Printf("%s: refused from %s -- %s.\n", h.account.c_str(),
                                    hc.conn->PeerAddress().c_str(), why.c_str());
                        hc.pendingAccount = h.account;
                        hc.conn.reset();
                        break;
                    }
                    // Failed logins slow the next one down (#341): a wrong answer already
                    // costs a socket, and now a wait that doubles each time.
                    const std::string& from = hc.conn->PeerAddress();
                    if (const double wait = gate.LoginWait(from, h.account, uptime); wait > 0.0)
                    {
                        Proto::Bye bye;
                        bye.reason = "Too many failed logins for that account; try again in " +
                                     std::to_string((int)std::ceil(wait)) + " s";
                        hc.conn->Send(Proto::EncodeBye(bye));
                        Log::Printf("%s: refused -- %.0f s of backoff left for %s.\n",
                                    h.account.c_str(), wait, from.c_str());
                        hc.pendingAccount = h.account;
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
                    const std::string  path = AccountPath(accountsDir, h.account);
                    const Save::Result file = Host::ReadAccountAuth(path, chal.salt, hc.fileStored);
                    // No file, or one written before secrets existed (#106): the next login
                    // sets it, a migration hole that closes itself as accounts are used.
                    hc.isNewAccount = chal.salt.empty() || hc.fileStored.empty();
                    // A name that is playing is an account, file or not (#201). A new
                    // account has no file until its first save, and a second login in that
                    // window used to register a secret of its own and displace the owner.
                    // The live session holds what the account is, so the newcomer has to
                    // answer against that.
                    if (hc.isNewAccount)
                        for (const HostClient& other : clients)
                        {
                            const ClientSession* os =
                                other.sessionId != 0 && other.account == h.account
                                    ? sim.Session(other.sessionId)
                                    : nullptr;
                            if (os != nullptr && os->authStored.size() == 64)
                            {
                                chal.salt = os->authSalt;
                                hc.fileStored = os->authStored;
                                hc.isNewAccount = false;
                                break;
                            }
                            // ...and so is one still choosing where to start (#342 S2): it
                            // has proved a secret and has no session to hold it yet.
                            if (other.awaitingStart && other.account == h.account &&
                                other.isNewAccount && other.authStored.size() == 64)
                            {
                                chal.salt = other.challengeSalt;
                                hc.fileStored = other.authStored;
                                hc.isNewAccount = false;
                                break;
                            }
                        }
                    if (hc.isNewAccount && file == Save::Result::Corrupt)
                    {
                        // A damaged file is an account that exists (#341). Read as a free
                        // name, it went to whoever logged in under it first; the owner has
                        // to have it restored from its backup instead.
                        Proto::Bye bye;
                        bye.reason = "That account's file is damaged; ask the server's "
                                     "operator to restore it";
                        hc.conn->Send(Proto::EncodeBye(bye));
                        Log::Printf("%s: refused -- %s is damaged (the previous save is %s).\n",
                                    h.account.c_str(), path.c_str(),
                                    Save::BackupPath(path, 1).c_str());
                        hc.conn.reset();
                        break;
                    }
                    if (hc.isNewAccount && !gate.MayRegister(from, uptime))
                    {
                        // Accounts are free; a script that makes them by the thousand is
                        // what makes them cost something (#341).
                        Proto::Bye bye;
                        bye.reason = "Too many new accounts from your address; try again later";
                        hc.conn->Send(Proto::EncodeBye(bye));
                        Log::Printf("%s: refused -- %s has made its new accounts for the hour.\n",
                                    h.account.c_str(), from.c_str());
                        hc.conn.reset();
                        break;
                    }
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
                        Log::Printf("%s: refused (%s).\n", hc.pendingAccount.c_str(),
                                    hc.isNewAccount ? "bad registration" : "wrong secret");
                        gate.LoginFailed(hc.conn->PeerAddress(), hc.pendingAccount, uptime);
                        hc.conn.reset();
                        break;
                    }
                    const Proto::Hello h{ hc.pendingAccount };
                    gate.LoginSucceeded(hc.conn->PeerAddress(), h.account);
                    if (hc.isNewAccount)
                        gate.Registered(hc.conn->PeerAddress(), uptime);

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
                        if (&other == &hc || other.account != h.account ||
                            (other.sessionId == 0 && !other.awaitingStart))
                            continue;
                        other.awaitingStart = false;
                        if (ClientSession* os = sim.Session(other.sessionId))
                        {
                            // Saved before it is taken away: what they earned up to this
                            // moment is theirs, and the new session loads that file next.
                            if (!other.accountReadOnly)
                                sim.SaveAccount(*os, AccountPath(accountsDir, other.account));
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
                        if (other.lingering)
                            Log::Printf("%s is back; the ship left in space is flown again.\n",
                                        h.account.c_str());
                        else
                            Log::Printf(
                                "%s signed in again; the earlier connection was displaced.\n",
                                h.account.c_str());
                    }

                    // Where they begin (#342 S2): where they left off, if this galaxy still
                    // has it; otherwise a start -- theirs to choose unless the server deals.
                    hc.account = h.account;
                    hc.authStored = a.stored;
                    if (!Host::NeedsStart(sim, AccountPath(accountsDir, h.account)))
                        JoinPlayer(sim, hc, accountsDir, std::string(), net);
                    else if (randomStarts)
                        JoinPlayer(sim, hc, accountsDir,
                                   Host::PickStart(sim, std::string(), true, roll()), net);
                    else
                    {
                        hc.conn->Send(Proto::EncodeStarts(Host::StartsMessage(sim, randomStarts)));
                        hc.awaitingStart = true;
                        hc.silentFor = 0.0;
                        Log::Printf("%s: choosing where to start.\n", h.account.c_str());
                    }
                    continue;
                }
                if (type == "start")
                {
                    // The answer to the starts (#342 S2), once, and only when asked for.
                    Proto::ChooseStart choice;
                    if (!hc.awaitingStart || !Proto::DecodeChooseStart(msg, choice))
                        continue;
                    hc.awaitingStart = false;
                    JoinPlayer(sim, hc, accountsDir,
                               Host::PickStart(sim, choice.system, randomStarts, roll()), net);
                    continue;
                }
                // Anything else before the hello is from a client that has not said who it
                // is; there is no player to apply it to.
                ClientSession* s = sim.Session(hc.sessionId);
                // An acknowledgement is not a tick of anything, so it is not on the budget.
                if (type == "ack")
                {
                    Proto::Ack a;
                    if (s != nullptr && Proto::DecodeAck(msg, a))
                        hc.snapOut.Acknowledge(a.snapshot);
                    continue;
                }
                if (type != "cmd" || s == nullptr)
                    continue;
                if (hc.tickBudget < 1.0)
                {
                    // Not acknowledged either: the client replays what the server has not
                    // confirmed, so the input comes back rather than being lost.
                    if (!hc.throttleReported)
                    {
                        hc.throttleReported = true;
                        Log::Printf("%s is sending commands faster than the simulation runs; "
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
                        sim.SaveAccount(*s, AccountPath(accountsDir, hc.account));
                }
        }

        // World (NPCs): on the server timer, independent of inputs and of whether anyone
        // is watching.
        while (acc >= dt)
        {
            const auto tickStart = clock::now();
            HostStepWorld(sim, dt, fires);
            metrics.Tick(
                std::chrono::duration<double, std::milli>(clock::now() - tickStart).count());
            acc -= dt;
        }
        for (std::string line; console.Take(line);)
            AdminCommand(sim, access, line);

        // The lists edited by hand are read again (#400); then whoever they, or the console,
        // now refuse is taken off the server -- in play, logging in or choosing a start.
        accessCheckAcc += frame;
        if (accessCheckAcc >= 2.0)
        {
            accessCheckAcc = 0.0;
            if (access.ReloadIfChanged())
                Log::Printf("Access: %s or %s changed; read again (%d allowed, %d banned).\n",
                            access.AllowlistPath().c_str(), access.BansPath().c_str(),
                            (int)access.Allowed().size(), (int)access.Bans().size());
        }
        if (access.Generation() != accessSeen)
        {
            accessSeen = access.Generation();
            for (HostClient& hc : clients)
            {
                const std::string& name = hc.account.empty() ? hc.pendingAccount : hc.account;
                const std::string  why =
                    name.empty() || !hc.removedFor.empty() ? std::string() : access.Refusal(name);
                if (why.empty() || (!hc.conn && !hc.lingering))
                    continue;
                if (ClientSession* s = sim.Session(hc.sessionId))
                {
                    if (!hc.accountReadOnly)
                        sim.SaveAccount(*s, AccountPath(accountsDir, hc.account));
                    sim.DestroySession(hc.sessionId);
                }
                if (hc.conn)
                {
                    Proto::Bye bye;
                    bye.reason = why;
                    hc.conn->Send(Proto::EncodeBye(bye));
                    hc.conn.reset();
                }
                hc.sessionId = 0;
                hc.awaitingStart = false;
                hc.lingering = false;
                hc.removedFor = why;
            }
        }

        // A region opened (#342 S3): written down at once -- its log entry is what makes the
        // sealed file part of this world -- and everyone is sent the index that has it, and
        // the galaxy, before the delta that adds the wormhole names it and before any snapshot.
        // ...and so is a mouth collapsing or reopening, a region drifting away or coming back
        // (S4): the routes everyone plans by have changed.
        const std::vector<int> opened = sim.TakeOpenedRegions();
        const bool             reshaped = sim.TakeTopologyChanged();
        if (!opened.empty() || reshaped)
        {

            for (int slot : opened)
                PrintOpening(sim, slot, "");
            sim.SaveWorld(worldPath);
            sim.SaveDesigns(DesignsPath(worldPath));
            worldSaveAcc = 0.0;
            BroadcastUniverse(clients, sim, net);
            const std::string g = Proto::EncodeGalaxy(sim.BuildGalaxyState());
            lastGalaxy = g;
            for (HostClient& hc : clients)
                if (hc.sessionId != 0 && hc.conn)
                {
                    net.galaxies += (double)g.size();
                    hc.conn->Send(g);
                }
        }

        // Before the snapshots below, never after (#38).
        SendLayoutDeltas(clients, sim, net);

        snapshotAcc += frame;
        const bool sendSnapshots = snapshotAcc >= SNAPSHOT_INTERVAL;
        // The interval is taken off, not the whole of it (#341): zeroing threw away the part
        // of a frame past the deadline, and with frames of 4 ms and up that came to 16
        // snapshots a second rather than 20. A long stall still sends one round, not a burst.
        if (sendSnapshots)
            snapshotAcc = std::min(snapshotAcc - SNAPSHOT_INTERVAL, SNAPSHOT_INTERVAL);

        for (HostClient& hc : clients)
        {
            ClientSession* s = sim.Session(hc.sessionId);
            // A connection dropped this frame -- a second hello, a malformed message --
            // still has its session until the cleanup below; it has no socket (#202).
            if (s == nullptr || !hc.conn)
                continue;
            // A layout is not on the snapshot clock: it answers "you are somewhere else
            // now", and waiting 50 ms to say so would draw the old system for a moment.
            if (layoutDirty[hc.sessionId] || s->systemId != hc.layoutSystem)
            {
                const std::string lay = LayoutFor(sim, s->systemId, hc.account);
                net.layouts += (double)lay.size();
                hc.conn->Send(lay);
                hc.layoutSystem = s->systemId;
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
            // What a design the snapshot names is, before the snapshot names it (#279).
            // Counted as layout: it is static description, sent once.
            for (const Proto::DesignDef& def : sim.DesignDefsFor(snap, hc.sentDesigns))
            {
                const std::string d = Proto::EncodeDesignDef(def);
                net.layouts += (double)d.size();
                hc.conn->Send(d);
            }
            // Only what changed since the snapshot this client last acknowledged, and only the
            // neighbourhood in full (#349). The mission board and log are part of it, so they
            // go out when they change rather than twenty times a second (#341).
            const std::string enc = hc.snapOut.Encode(snap, sim.InterestOf(*s));
            net.snapshots += (double)enc.size();
            net.snapCount++;
            hc.conn->Send(enc);
        }

        // Galaxy snapshot -- rarely (once a second): the statistics change slowly and the
        // message is large (all systems). The client's galaxy map lives off it.
        galaxyAcc += frame;
        if (sim.TakeChartsChanged())
        {
            BroadcastUniverse(clients, sim, net);
            // A system charted or named is a claim made once, for good: it is written now
            // rather than at the next checkpoint, so a crash cannot take it back (#308).
            // It happens a handful of times in a session, so the write costs nothing.
            sim.SaveWorld(worldPath);
            sim.SaveDesigns(DesignsPath(worldPath));
            worldSaveAcc = 0.0;
            galaxyAcc = 1.0;  // and the statistics that now include it, straight away
        }
        // A design filed by a refit is written at once (#279): the ship that flies it is
        // paid for, and a crash before the checkpoint would leave it naming nothing.
        if (sim.TakeDesignsChanged())
            sim.SaveDesigns(DesignsPath(worldPath));
        if (galaxyAcc >= 1.0)
        {
            galaxyAcc = 0.0;
            // Only when it changed (#341): a client is sent the whole map at login, and a
            // galaxy that has not moved in a second is the same map again.
            const std::string g =
                clients.empty() ? std::string() : Proto::EncodeGalaxy(sim.BuildGalaxyState());
            if (!g.empty() && g != lastGalaxy)
            {
                lastGalaxy = g;
                for (HostClient& hc : clients)
                    if (hc.sessionId != 0 && hc.conn)
                    {
                        net.galaxies += (double)g.size();
                        hc.conn->Send(g);
                    }
            }
        }

        net.window += frame;
        if (net.window >= NET_REPORT_INTERVAL && !clients.empty())
            net.Report(clients.size());

        // How the server is doing, once a minute whether or not anyone is on (#400).
        metricsAcc += frame;
        if (metricsAcc >= METRICS_INTERVAL)
        {
            int hot = 0;
            for (const auto& kv : sim.Systems())
                hot += Simulation::IsHot(kv.second) ? 1 : 0;
            metrics.SetSent(net.Sent());
            Log::Printf(
                "%s\n",
                metrics.Line((int)sim.Sessions().size(), (int)clients.size(), hot, metricsAcc)
                    .c_str());
            metricsAcc = 0.0;
        }

        // World checkpoint. The galaxy drifts continuously (security, prosperity,
        // territory control), so unlike an account there is no natural commit point to
        // hang this on: a timer is what keeps a crash from costing more than a minute.
        worldSaveAcc += frame;
        if (worldSaveAcc >= WORLD_SAVE_INTERVAL)
        {
            worldSaveAcc = 0.0;
            sim.SaveWorld(worldPath);
            sim.SaveDesigns(DesignsPath(worldPath));
        }

        // A disconnect ends one session, not the server and not anyone else's game -- after
        // the ship has stayed in space a while (#341), so leaving is not a way out of a fight.
        const std::vector<int> lingerOver = sim.StepLinger((float)frame);
        for (size_t i = 0; i < clients.size();)
        {
            HostClient& hc = clients[i];
            if (!hc.removedFor.empty())
            {
                Log::Printf("%s was taken off the server: %s.\n",
                            (hc.account.empty() ? hc.pendingAccount : hc.account).c_str(),
                            hc.removedFor.c_str());
                clients.erase(clients.begin() + (long)i);
                continue;
            }
            if (hc.lingering && !hc.evicted)
            {
                if (std::find(lingerOver.begin(), lingerOver.end(), hc.sessionId) ==
                    lingerOver.end())
                {
                    i++;
                    continue;
                }
                Log::Printf("%s's ship has left space.\n", hc.account.c_str());
            }
            else if (hc.conn && hc.conn->Alive() && !hc.evicted &&
                     (hc.sessionId != 0 || hc.silentFor < LoginLimit(hc)))
            {
                i++;
                continue;
            }
            else if (!hc.evicted && sim.Session(hc.sessionId) != nullptr &&
                     sim.BeginLinger(*sim.Session(hc.sessionId)))
            {
                hc.lingering = true;
                hc.conn.reset();
                Log::Printf("%s's connection closed; the ship stays in space for %.0f s.\n",
                            hc.account.c_str(), Simulation::LINGER_SECONDS);
                i++;
                continue;
            }
            if (ClientSession* s = sim.Session(hc.sessionId))
            {
                if (hc.accountReadOnly)
                    Log::Printf("%s left. Account NOT saved: their file is from a newer build.\n",
                                hc.account.c_str());
                else
                {
                    sim.SaveAccount(*s, AccountPath(accountsDir, hc.account));
                    Log::Printf("%s left. Account saved (money %.0f); galaxy still running.\n",
                                hc.account.c_str(), s->account.GetMoney());
                }
                sim.DestroySession(hc.sessionId);
            }
            else if (hc.evicted)
            {
                Log::Printf("The displaced connection for %s closed.\n", hc.account.c_str());
            }
            else if (hc.conn && hc.conn->Alive())
            {
                // Still connected, but not logged in within the limit. Which half it got
                // through is what tells a client that never spoke from a lost answer.
                if (hc.awaitingStart)
                    Log::Printf("%s chose no start within %.0f s; dropped.\n", hc.account.c_str(),
                                START_TIMEOUT);
                else if (hc.pendingAccount.empty())
                    Log::Printf("A client said nothing for %.0f s and was dropped.\n",
                                HELLO_TIMEOUT);
                else
                    Log::Printf("%s: no answer to the login challenge within %.0f s; dropped.\n",
                                hc.pendingAccount.c_str(), LOGIN_TIMEOUT);
            }
            else
            {
                Log::Printf("A client left before logging in%s%s (%s).\n",
                            hc.pendingAccount.empty() ? "" : " as ", hc.pendingAccount.c_str(),
                            hc.conn ? hc.conn->CloseReason().c_str() : "refused");
            }
            clients.erase(clients.begin() + (long)i);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }

    for (HostClient& hc : clients)
        if (ClientSession* s = sim.Session(hc.sessionId))
            if (!hc.accountReadOnly)
                sim.SaveAccount(*s, AccountPath(accountsDir, hc.account));
    sim.SaveWorld(worldPath);
    sim.SaveDesigns(DesignsPath(worldPath));
    Log::Printf("\nShutting down. World and accounts saved.\n");
    Net::Shutdown();
    return 0;
}

// A save a smoke test writes and reads back. It goes under the system temp directory with a
// name of its own, and is removed however the test ends, an early return included -- never
// left beside the executable, where a real world.json and account files live.
class ScratchFile
{
public:
    explicit ScratchFile(const char* name)
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = (std::filesystem::temp_directory_path() /
                 ("econserver_" + std::string(name) + "_" + std::to_string(stamp) + ".json"))
                    .generic_string();
    }
    ~ScratchFile()
    {
        // ...with the copies a save keeps beside it (#341).
        std::remove(path_.c_str());
        std::remove((path_ + ".tmp").c_str());
        for (int n = 1; n <= Simulation::WORLD_BACKUPS; n++)
            std::remove(Save::BackupPath(path_, n).c_str());
    }
    ScratchFile(const ScratchFile&) = delete;
    ScratchFile& operator=(const ScratchFile&) = delete;

    const std::string& Path() const { return path_; }

private:
    std::string path_;
};

// Host smoke test (no sockets): runs a host tick through LocalTransport. The client
// sends a thrust command, the server steps the world and sends layout+snapshot; we
// check that the layout arrived and the player moved. Run: econserver hosttest.
static int HostSelftest()
{
    std::string dataDir = DataDir::Path();
    Simulation  sim;
    SetupHostSim(sim, dataDir);
    ClientSession& s = SetupHostPlayer(sim);

    LocalTransport link;
    s.accountName = "selftest";
    link.Server().Send(LayoutFor(sim, s.systemId, s.accountName));
    // What anyone else in the system is sent, as RunHost cuts it (#340).
    Proto::LayoutMirror stranger;
    {
        Proto::SystemLayout lay;
        if (Proto::DecodeLayout(LayoutFor(sim, s.systemId, "stranger"), lay))
            stranger.Reset(lay);
    }

    Proto::Command thrust;
    thrust.thrust = true;
    link.Client().Send(Proto::EncodeCommand(thrust));

    Vector2 startPos = s.ship->GetPosition();

    // A structure appears a third of the way in and is gone again two thirds in (#38): the
    // world changing while someone watches. What the server held when it built each
    // snapshot is recorded, so the client side can check it never saw anything else.
    int               builtId = 0;
    std::vector<bool> serverHad;
    // And a player lays one down through the wire (#39): a command, the server's rules, a
    // delta back. The hold is stocked and the name set as a login would set it.
    s.ship->AddCargo(ResourceType(), 10);
    bool tryDeploy = Blueprints::Find("buoy") != nullptr;
    // ...and an armed one, whose rules of engagement are set through the wire too (#340): the
    // owner is sent its allow-list and nobody else is.
    const Blueprint* battery = Blueprints::Find("battery");
    if (battery != nullptr)
        for (const auto& cost : battery->cost)
            s.ship->AddCargo(cost.first, cost.second);
    int batteryId = 0;
    // Somewhere the rules allow, found by asking them: the ship starts beside a station that
    // orbits a planet, and nothing may stand in a satellite's path. The ship is put there
    // first, as if it had flown -- this tests the wire, not travel.
    auto placeFor = [&](const Blueprint& bp, Proto::Command& c)
    {
        const Vector2 from = s.ship->GetPosition();
        for (int k = 0; k < 400 && c.deploy.empty(); k++)
        {
            const Vector2 p = { 150000.0f + 9100.0f * (float)(k % 20),
                                -90000.0f + 9300.0f * (float)(k / 20) };
            s.ship->Teleport(p);
            const Vector2 near = { p.x + bp.reach * 0.5f, p.y };
            if (sim.PlacementProblem(s, bp, near).empty())
            {
                c.deploy = bp.id;
                c.deployPos = near;
            }
        }
        if (c.deploy.empty())
            s.ship->Teleport(from);
        return !c.deploy.empty();
    };

    int         lastSeq = 0;
    const float dt = 1.0f / 60.0f;
    for (int i = 0; i < 120; i++)  // ~2 s of simulation
    {
        thrust.seq = i + 1;  // client: numbered input
        Proto::Command c = thrust;
        if (i == 100 && tryDeploy)
            tryDeploy = placeFor(*Blueprints::Find("buoy"), c);
        if (i == 104 && battery != nullptr && !placeFor(*battery, c))
            battery = nullptr;
        if (i == 108 && battery != nullptr)
        {
            for (const auto& kv : sim.SystemById(s.systemId)->entities)
                if (kv->GetKind() == EntityKind::Structure && kv->GetOwner() == s.accountName &&
                    static_cast<const Structure&>(*kv).GetBlueprintId() == battery->id)
                    batteryId = kv->GetId();
            c.defenceId = batteryId;
            c.defence.engage = Engage::Strangers;
            c.defence.players = { "friend" };
        }
        link.Client().Send(Proto::EncodeCommand(c));
        std::vector<Proto::TradeAck>                  acks;
        std::map<std::string, std::vector<FireEvent>> fires;
        HostDrainInputs(link.Server(), sim, s, dt, lastSeq, acks, fires);  // 1 input=1 tick
        HostStepWorld(sim, dt, fires);                                     // world
        if (i == 40)
            builtId = sim.AddStatic(
                s.systemId,
                std::make_unique<Station>(Vector2{ 900.0f, 900.0f }, 60.0f, "Selftest Depot",
                                          Factions::Unaffiliated(), StationRole::TradeHub),
                "selftest");
        if (i == 80)
            sim.RemoveStatic(s.systemId, builtId);
        // As RunHost does it: the deltas first, then the snapshot (#38), each cut for whoever
        // it goes to (#340).
        for (const Proto::LayoutDelta& d : sim.TakeLayoutDeltas())
            if (d.systemId == s.systemId)
            {
                Proto::LayoutDelta own = d, shared = d;
                Proto::ForViewer(own, s.accountName);
                Proto::ForViewer(shared, "stranger");
                link.Server().Send(Proto::EncodeLayoutDelta(own));
                Proto::LayoutDelta heard;
                if (Proto::DecodeLayoutDelta(Proto::EncodeLayoutDelta(shared), heard))
                    stranger.Apply(heard);
            }
        Proto::Snapshot snap = sim.BuildSnapshot(s, s.systemId);
        snap.player.lastInput = lastSeq;
        link.Server().Send(Proto::EncodeSnapshot(snap));
        serverHad.push_back(i >= 40 && i < 80);
    }

    bool                gotLayout = false, gotSnap = false, worldAgreed = builtId != 0;
    bool                deployed = false;
    Proto::Snapshot     lastSnap;
    Proto::LayoutMirror mirror;
    size_t              snapIndex = 0;
    std::string         msg;
    while (link.Client().Poll(msg))
    {
        std::string         t = Proto::MessageType(msg);
        Proto::SystemLayout lay;
        Proto::LayoutDelta  d;
        if (t == "layout" && Proto::DecodeLayout(msg, lay))
        {
            mirror.Reset(lay);
            gotLayout = true;
        }
        else if (t == "ldelta" && Proto::DecodeLayoutDelta(msg, d))
            mirror.Apply(d);
        else if (t == "snap" && Proto::DecodeSnapshot(msg, lastSnap))
        {
            gotSnap = true;
            Proto::CompleteFromLayout(lastSnap, mirror.byId);
            bool seen = false;
            for (const Proto::EntitySnapshot& e : lastSnap.entities)
                seen = seen || (e.id == builtId);
            if (snapIndex >= serverHad.size() || seen != serverHad[snapIndex])
                worldAgreed = false;
            snapIndex++;
        }
    }
    for (const auto& kv : mirror.byId)
        deployed = deployed || (kv.second.kind == EntityKind::Structure &&
                                kv.second.owner == "selftest" && kv.second.completesAt > 0.0);
    // The battery's rules reached its owner whole, and everyone else without the list.
    bool rules = false;
    if (batteryId != 0 && mirror.byId.count(batteryId) && stranger.byId.count(batteryId))
    {
        const Defence& own = mirror.byId.at(batteryId).defence;
        const Defence& seen = stranger.byId.at(batteryId).defence;
        rules = own.engage == Engage::Strangers &&
                own.players == std::vector<std::string>{ "friend" } &&
                seen.engage == Engage::Strangers && seen.players.empty();
    }
    Vector2 endPos = s.ship->GetPosition();
    bool    moved = (endPos.x != startPos.x || endPos.y != startPos.y);
    bool    snapHasWorld = gotSnap && !lastSnap.entities.empty();

    printf("Host selftest: layout %s, snapshot %s, player-moved %s, world-changes %s, "
           "deploy %s, battery rules %s\n",
           gotLayout ? "OK" : "FAIL", snapHasWorld ? "OK" : "FAIL", moved ? "OK" : "FAIL",
           worldAgreed ? "OK" : "FAIL", deployed ? "OK" : "FAIL", rules ? "OK" : "FAIL");
    return (gotLayout && snapHasWorld && moved && worldAgreed && deployed && rules) ? 0 : 1;
}

// Naming through the wire (#308): a pilot in flight -- not at a station -- names the system
// it discovered. A refused name comes back with its reason in the journal the snapshot
// carries; an accepted one comes back in the galaxy index and survives a save and a load.
// Run as part of: econserver hosttest.
static int HostNameSelftest()
{
    const std::string dataDir = DataDir::Path();
    Simulation        sim;
    SetupHostSim(sim, dataDir);
    ClientSession& s = SetupHostPlayer(sim);
    // The first system beyond a centre's wormhole, and the centre system it opens from: only
    // the frontier is named by its discoverer (#342 S2).
    std::string home, entry;
    for (const auto& l : sim.Universe().links)
        if (entry.empty() && sim.InCentre(l.a) != sim.InCentre(l.b))
        {
            home = sim.InCentre(l.a) ? l.a : l.b;
            entry = sim.InCentre(l.a) ? l.b : l.a;
        }
    if (entry.empty())
    {
        printf("Name selftest: no frontier system next to a centre => FAIL\n");
        return 1;
    }
    s.accountName = "namer";
    s.ship->SetPilotName("namer");          // as a login sets it
    sim.ServerEnterSystem(s, entry, home);  // first in: the discoverer
    sim.TakeChartsChanged();                // charting it is not what is tested

    LocalTransport link;
    int            lastSeq = 0, lastEvent = 0, seq = 0;
    std::string    heard;  // what the journal told the client
    bool           indexSent = false;
    auto           send = [&](const std::string& name)
    {
        Proto::Command c;
        c.seq = ++seq;
        c.nameSystem = name;
        link.Client().Send(Proto::EncodeCommand(c));
        std::vector<Proto::TradeAck>                  acks;
        std::map<std::string, std::vector<FireEvent>> fires;
        HostDrainInputs(link.Server(), sim, s, 1.0f / 60.0f, lastSeq, acks, fires);
        // As RunHost does it: the index when it changed, the journal in the snapshot.
        if (sim.TakeChartsChanged())
        {
            link.Server().Send(Proto::EncodeUniverse(sim.KnownUniverse()));
            indexSent = true;
        }
        Proto::Snapshot snap = sim.BuildSnapshot(s, s.systemId);
        snap.events = s.EventsSince(lastEvent);
        if (!snap.events.empty())
            lastEvent = snap.events.back().seq;
        link.Server().Send(Proto::EncodeSnapshot(snap));
    };
    auto receive = [&](std::string& nameSeen)
    {
        std::string msg;
        while (link.Client().Poll(msg))
        {
            const std::string     t = Proto::MessageType(msg);
            Proto::Snapshot       snap;
            WorldLoader::Universe u;
            if (t == "snap" && Proto::DecodeSnapshot(msg, snap))
                for (const Ev::Event& e : snap.events)
                    heard = e.text;
            else if (t == "factions")
            {
                Proto::FactionList f;
                if (Proto::DecodeFactions(msg, f))
                    Proto::ApplyFactions(f);
            }
            else if (t == "universe" && Proto::DecodeUniverse(msg, u))
                for (const auto& si : u.systems)
                    if (si.id == entry)
                        nameSeen = si.name;
        }
    };

    std::string seen;
    send("9 Lives");  // not a name: refused, and the pilot is told why
    receive(seen);
    const std::string refusal = heard;
    const bool        refusedSaid =
        !indexSent && refusal.rfind("Not named:", 0) == 0 && refusal.size() > 11;
    const bool inFlight = !s.IsDocked();
    send("Selftest Haven");
    receive(seen);
    const bool named = indexSent && seen == "Selftest Haven";

    const ScratchFile scratch("nametest");
    const std::string path = scratch.Path();
    sim.SaveWorld(path);
    Simulation again;
    SetupHostSim(again, dataDir, path);
    bool kept = false;
    for (const auto& si : again.KnownUniverse().systems)
        kept = kept || (si.id == entry && si.name == "Selftest Haven");
    std::remove(path.c_str());

    printf("Name selftest: in-flight %s, refusal-said %s (\"%s\"), named-for-all %s, "
           "kept %s\n",
           inFlight ? "OK" : "FAIL", refusedSaid ? "OK" : "FAIL", refusal.c_str(),
           named ? "OK" : "FAIL", kept ? "OK" : "FAIL");
    return (inFlight && refusedSaid && named && kept) ? 0 : 1;
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
    const ScratchFile scratch("accttest");
    const std::string path = scratch.Path();

    // The registry before any ship: an entity built without it has no archetype, and says
    // so (#227).
    const std::string dataDir = DataDir::Path();
    if (!Factions::Load(dataDir + "factions.json"))
    {
        fprintf(stderr, "FATAL: %s\n", Factions::Error().c_str());
        return 1;
    }
    // What markets trade (#343), before anything is priced or carried.
    if (!Goods::Load(dataDir + "goods.json"))
    {
        fprintf(stderr, "FATAL: %s\n", Goods::Error().c_str());
        return 1;
    }
    if (!Archetypes::Load(dataDir + "archetypes.json"))
    {
        fprintf(stderr, "FATAL: %s\n", Archetypes::Error().c_str());
        return 1;
    }

    // The account belongs to a session, so the test needs one on each side (#3). No
    // world is loaded: persistence must not depend on where the ship happens to be.
    Simulation     a;
    ClientSession& as =
        a.CreateSession(std::string(), Vector2{ 0.0f, 0.0f }, GetShipCatalog()[0].stats);
    as.account.SetMoney(4242.0);
    as.account.SetReputation(Factions::Pirates(), -7.5f);
    as.account.SetBounty(Factions::DefaultPolice(), 300.0);
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
    bool rep = approx(bs.account.GetReputation(Factions::Pirates()), -7.5);
    bool bounty = approx(bs.account.GetBounty(Factions::DefaultPolice()), 300.0);
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
    std::string       dataDir = DataDir::Path();
    const ScratchFile scratch("worldtest");
    const std::string path = scratch.Path();

    Simulation a;
    SetupHostSim(a, dataDir);
    std::string sid = a.Starts().front().systemId;

    // Advance the clock the way the real server does, rather than setting it: this also
    // guards that simulation time moves at all, which it silently did not before #57.
    const float dt = 1.0f / 60.0f;
    for (int i = 0; i < 120; i++)
        a.MaintainWorld(dt);
    bool ticked = approx(a.Time(), 2.0);

    a.Systems()[sid].agg.security = 0.25f;
    a.Systems()[sid].agg.prosperity = 0.75f;
    a.Systems()[sid].agg.pirates = 7.0f;
    a.Systems()[sid].agg.controller = Factions::Pirates();
    a.SaveWorld(path);

    Simulation b;  // the same galaxy, restored: the load must overwrite the defaults
    uint64_t   savedSeed = 0;
    int        savedRules = 0, savedCentres = 0;
    bool       loaded = Simulation::ReadWorldSeed(path, savedSeed, savedRules, &savedCentres) &&
                        savedSeed == 1 && savedRules == Gen::GENERATOR_VERSION &&
                        savedCentres == a.CentreCount();
    SetupHostSim(b, dataDir, path);
    bool haveSys = b.HasSystem(sid);
    bool time = approx(b.Time(), 2.0);
    bool sec = haveSys && approx(b.Systems()[sid].agg.security, 0.25);
    bool prosp = haveSys && approx(b.Systems()[sid].agg.prosperity, 0.75);
    bool pir = haveSys && approx(b.Systems()[sid].agg.pirates, 7.0);
    bool ctrl = haveSys && b.Systems()[sid].agg.controller == Factions::Pirates();
    std::remove(path.c_str());

    // What players changed in the static layer (#38): a station of the world's own taken
    // away and one built, across a save and a start from it the way the server starts.
    bool        kept = false;
    std::string builtKey;
    {
        Simulation before;
        SetupHostSim(before, dataDir);
        Station* gone = nullptr;
        for (auto& e : before.Systems()[sid].entities)
            if (gone == nullptr && e->GetKind() == EntityKind::Station)
                gone = static_cast<Station*>(e.get());
        const std::string goneName = gone != nullptr ? gone->GetName() : std::string();
        const bool        removed = gone != nullptr && before.RemoveStatic(sid, gone->GetId());
        const int         built = before.AddStatic(
            sid,
            std::make_unique<Station>(Vector2{ 4000.0f, 0.0f }, 60.0f, "Selftest Depot",
                                      Factions::DefaultPolice(), StationRole::TradeHub),
            "selftest");
        builtKey = before.StaticKey(sid, built);
        before.SaveWorld(path);

        Simulation after;
        SetupHostSim(after, dataDir, path);
        std::remove(path.c_str());
        bool goneStays = true, builtBack = false;
        for (const auto& e : after.Systems()[sid].entities)
        {
            if (e->GetKind() != EntityKind::Station)
                continue;
            goneStays = goneStays && e->GetName() != goneName;
            builtBack =
                builtBack || (e->GetName() == "Selftest Depot" && e->GetOwner() == "selftest" &&
                              after.StaticKey(sid, e->GetId()) == builtKey);
        }
        kept = removed && built != 0 && goneStays && builtBack;
    }

    bool ok = ticked && loaded && time && sec && prosp && pir && ctrl && kept;
    printf("World selftest: clock %s, load %s, time %s, security %s, prosperity %s, "
           "pirates %s, controller %s, built/removed kept %s => %s\n",
           ticked ? "OK" : "FAIL", loaded ? "OK" : "FAIL", time ? "OK" : "FAIL",
           sec ? "OK" : "FAIL", prosp ? "OK" : "FAIL", pir ? "OK" : "FAIL", ctrl ? "OK" : "FAIL",
           kept ? "OK" : "FAIL", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

// Standing-order smoke test (no network): give the ship an order, tick the server the way
// the host loop does, and check it finishes on its own. This is the property agent play
// rests on -- one order, no further input, the server does the flying.
// Run: econserver ordertest.
static int OrderSelftest()
{
    std::string dataDir = DataDir::Path();
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
    const std::string        startSys = s.systemId;
    std::vector<std::string> self = sim.PlanRoute(startSys, startSys, false);
    bool                     selfRoute = self.size() == 1 && self[0] == startSys;

    // Every other system must be reachable from the start, and the path must begin and
    // end where it was asked to.
    bool allReachable = true;
    for (const WorldLoader::SystemInfo& si : sim.Universe().systems)
    {
        // The start's own centre: civilisations are apart until their frontiers meet (S2).
        if (Gen::SlotOf(si.id) != Gen::SlotOf(startSys))
            continue;
        std::vector<std::string> path = sim.PlanRoute(startSys, si.id, false);
        if (path.empty() || path.front() != startSys || path.back() != si.id)
            allReachable = false;
    }
    bool noRoute = sim.PlanRoute(startSys, "nowhere", false).empty();

    // Planning around danger must still produce a valid path, not give up.
    bool safeRouteOk = true;
    for (const WorldLoader::SystemInfo& si : sim.Universe().systems)
    {
        // The start's own centre: civilisations are apart until their frontiers meet (S2).
        if (Gen::SlotOf(si.id) != Gen::SlotOf(startSys))
            continue;
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

// econserver regenerate <region>, with the server stopped (#342 S3): the region is sealed
// afresh by this build's rules, and what world.json knew of its systems -- their aggregates
// and what was changed in them -- is dropped, so they begin again as the rules make them.
// Refused while a player structure stands in it: that is somebody's, and it stands somewhere
// the new rules may not have.
// econserver validate [path] (#342 S7): the admin packs -- `path` is one pack or a directory
// of them, data/packs/ by default -- and data/pins.json, checked without running a world. A
// pack is read as the server reads it, then woven into a trial world of seed 1, as a new
// world would weave it; each pin is applied to the regions such a world begins with. Every
// problem is one line; the exit status says whether there were any.
static int ValidateCommand(const std::string& path)
{
    namespace fs = std::filesystem;
    const std::string dataDir = DataDir::Path();
    SetTraceLogLevel(LOG_WARNING);
    if (!Factions::Load(dataDir + "factions.json") || !Goods::Load(dataDir + "goods.json") ||
        !Archetypes::Load(dataDir + "archetypes.json") ||
        !Blueprints::Load(dataDir + "blueprints.json"))
    {
        fprintf(stderr, "validate: the registries in %s do not load: %s%s%s%s\n", dataDir.c_str(),
                Factions::Error().c_str(), Goods::Error().c_str(), Archetypes::Error().c_str(),
                Blueprints::Error().c_str());
        return 2;
    }
    const std::string                             target = path.empty() ? dataDir + "packs" : path;
    std::vector<std::string>                      problems;
    std::vector<std::shared_ptr<const Gen::Pack>> packs;
    std::error_code                               ec;
    if (fs::exists(fs::path(target) / "pack.json", ec))
    {
        Gen::Pack p;
        if (Gen::LoadPack(target, p, problems))
            packs.push_back(std::make_shared<const Gen::Pack>(std::move(p)));
    }
    else if (fs::is_directory(target, ec))
        packs = ReadPacks(target, problems);
    else if (!path.empty())
    {
        fprintf(stderr, "validate: %s is neither a pack nor a directory of packs\n",
                target.c_str());
        return 2;
    }
    for (const auto& p : packs)
        printf("pack %s: %s, %d system(s), hash %s\n", p->id.c_str(), p->kind.c_str(),
               (int)p->systems.size(), Gen::PackHashText(p->hash).c_str());

    Gen::WorldParams params;
    params.seed = 1;
    params.centres = Gen::DEFAULT_CENTRES;
    params.owners = Factions::MajorIds();
    params.pirates = Factions::Id(Factions::Pirates());
    params.unaffiliated = Factions::Id(Factions::Unaffiliated());
    // Woven as a new world would weave them -- only when each reads, since a trial of a broken
    // pack says nothing new.
    if (problems.empty() && !packs.empty())
    {
        Simulation trial;
        trial.SetPacks(packs);
        trial.OpenGalaxy(params, dataDir);
        for (const std::string& p : trial.PackProblems())
            problems.push_back("woven into a new world of seed 1: " + p);
        for (const Gen::Woven& w : trial.WovenPacks())
            printf("  woven into a new world of seed 1: %s -> region %d%s\n", w.id.c_str(), w.slot,
                   w.system.empty() ? "" : (", " + w.system).c_str());
    }

    // The pins: each applied to the regions a new world of its seed begins with (seed 1 for
    // one that names none). A pin for a slot that is not open then waits, as it would.
    std::ifstream pinsIn(dataDir + "pins.json");
    if (pinsIn.is_open())
    {
        const nlohmann::json pins = nlohmann::json::parse(pinsIn, nullptr, false);
        if (pins.is_discarded())
            problems.push_back("pins.json: not valid JSON");
        else
        {
            std::set<uint64_t> seeds;
            if (pins.contains("pins") && pins["pins"].is_array())
                for (const nlohmann::json& pin : pins["pins"])
                    seeds.insert(pin.is_object() && pin.contains("seed") &&
                                         pin["seed"].is_number_unsigned()
                                     ? pin["seed"].get<uint64_t>()
                                     : 1u);
            if (seeds.empty())
                seeds.insert(1u);
            for (uint64_t seed : seeds)
            {
                params.seed = seed;
                std::vector<std::string> pinProblems;
                Gen::GenerateWorld(Gen::GenerateGalaxySkeleton(params), [&](Gen::Region& r)
                                   { Gen::ApplyPins(r, pins, seed, pinProblems); });
                for (const std::string& p : pinProblems)
                    problems.push_back("pins.json (seed " + std::to_string(seed) + "): " + p);
            }
        }
    }

    for (const std::string& p : problems)
        printf("problem: %s\n", p.c_str());
    printf("%d pack(s), %d problem(s).\n", (int)packs.size(), (int)problems.size());
    return problems.empty() ? 0 : 1;
}

static int RegenerateCommand(int slot)
{
    const std::string dataDir = DataDir::Path();
    const std::string worldPath = std::string(GetApplicationDirectory()) + "world.json";
    if (!std::filesystem::exists(worldPath))
    {
        fprintf(stderr, "regenerate: there is no world at %s\n", worldPath.c_str());
        return 1;
    }
    Simulation sim;
    SetupHostSim(sim, dataDir, worldPath);
    std::string why;
    if (!sim.RegenerateRegion(slot, why))
    {
        fprintf(stderr, "regenerate: refused -- %s. Nothing has been changed.\n", why.c_str());
        return 1;
    }
    std::ifstream  in(worldPath);
    nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    in.close();
    int dropped = 0;
    for (const char* part : { "galaxy", "changes" })
    {
        if (!j.is_object() || !j.contains(part) || !j[part].is_object())
            continue;
        for (auto it = j[part].begin(); it != j[part].end();)
        {
            if (Gen::SlotOf(it.key()) == slot)
            {
                it = j[part].erase(it);
                dropped++;
            }
            else
                ++it;
        }
    }
    if (!Save::WriteFileAtomic(worldPath, j.dump(2) + "\n", 3))
    {
        fprintf(stderr,
                "regenerate: %s could not be written; the region file is new, so put "
                "its backup back before starting\n",
                worldPath.c_str());
        return 1;
    }
    printf("Region %d sealed afresh under region rules v%d (%s); %d saved entries for its "
           "systems dropped.\n",
           slot, Gen::GENERATOR_VERSION, Simulation::SealedPath(sim.SealDir(), slot).c_str(),
           dropped);
    return 0;
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

    // Every mode reads data/: found once, before any of them, so a copied build runs (#399).
    if (DataDir::Resolve(argc, argv, "econserver").empty())
        return 1;

    // Protocol/TCP regression tests moved to the `tests` target (ctest); here —
    // only server modes and batch simulation.
    if (argc > 1 && std::string(argv[1]) == "hosttest")
    {
        const int host = HostSelftest();
        const int name = HostNameSelftest();
        return host != 0 ? host : name;
    }
    if (argc > 1 && std::string(argv[1]) == "accttest")
        return AccountSelftest();
    if (argc > 1 && std::string(argv[1]) == "worldtest")
        return WorldSelftest();
    if (argc > 1 && std::string(argv[1]) == "ordertest")
        return OrderSelftest();
    // econserver regiondump <seed> [slot]: one region's index entries and documents.
    if (argc > 1 && std::string(argv[1]) == "regiondump")
    {
        std::vector<int> slots;
        if (argc > 3)
            slots.push_back(atoi(argv[3]));
        return GalaxyDump(argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 1,
                          Gen::DEFAULT_REGION_CAP, Gen::DEFAULT_CENTRES, slots, false);
    }
    // econserver galaxydump <seed> [slot ...] [--cap N] [--centres N]: the skeleton of N
    // slots (100 by default) with its centres (3), the regions a new world opens with, and
    // those of the slots named, with every slot opened on the way (#342).
    if (argc > 1 && std::string(argv[1]) == "galaxydump")
    {
        int              cap = Gen::DEFAULT_REGION_CAP, centres = Gen::DEFAULT_CENTRES;
        std::vector<int> slots;
        for (int i = 3; i < argc; i++)
            if (std::string(argv[i]) == "--cap" && i + 1 < argc)
                cap = std::max(1, atoi(argv[++i]));
            else if (std::string(argv[i]) == "--centres" && i + 1 < argc)
                centres = atoi(argv[++i]);
            else
                slots.push_back(atoi(argv[i]));
        return GalaxyDump(argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 1, cap, centres, slots,
                          true);
    }
    // econserver macrobench [N ...] [--seconds S] [--seed X] [--players K] [--hot]: what the
    // galaxy costs to run with a region of N systems and nobody connected, or K players each
    // in a system of their own; --hot keeps every system hot (#295). Without N, 20 100 500
    // 1000.
    if (argc > 1 && std::string(argv[1]) == "macrobench")
    {
        std::vector<int> sizes;
        double           seconds = 120.0;
        uint64_t         seed = 1;
        bool             allHot = false;
        int              players = 0;
        for (int i = 2; i < argc; i++)
            if (std::string(argv[i]) == "--seconds" && i + 1 < argc)
                seconds = atof(argv[++i]);
            else if (std::string(argv[i]) == "--seed" && i + 1 < argc)
                seed = std::strtoull(argv[++i], nullptr, 10);
            else if (std::string(argv[i]) == "--players" && i + 1 < argc)
                players = std::max(0, atoi(argv[++i]));
            else if (std::string(argv[i]) == "--hot")
                allHot = true;
            else if (atoi(argv[i]) > 0)
                sizes.push_back(atoi(argv[i]));
        if (sizes.empty())
            sizes = { 20, 100, 500, 1000 };
        return MacroBench(sizes, seconds, seed, allHot, players);
    }
    if (argc > 1 && std::string(argv[1]) == "validate")
        return ValidateCommand(argc > 2 ? argv[2] : std::string());
    if (argc > 1 && std::string(argv[1]) == "regenerate")
    {
        if (argc < 3)
        {
            fprintf(stderr, "usage: econserver regenerate <region>  (with the server stopped)\n");
            return 2;
        }
        return RegenerateCommand(atoi(argv[2]));
    }
    if (argc > 1 && std::string(argv[1]) == "host")
    {
        unsigned short port = (argc > 2) ? (unsigned short)atoi(argv[2]) : 50800;
        // Loopback unless asked (#187): other machines can connect only with --public.
        bool isPublic = false;
        bool openRegistration = false;
        bool dev = false;
        int  centres = Gen::DEFAULT_CENTRES;
        bool randomStarts = false;
        int  maxRegions = 0, maxSystems = 0;
        // The seed of a NEW galaxy (#140); a saved world keeps its own. Choosing it is the
        // one place a random device belongs -- generating from it is where it must not be.
        uint64_t seed = ((uint64_t)std::random_device{}() << 32) ^ std::random_device{}();
        for (int i = 3; i < argc; i++)
            if (std::string(argv[i]) == "--public")
                isPublic = true;
            // Anyone may register (#400). Without it a --public server lets in only the
            // names in allowlist.txt beside its world.
            else if (std::string(argv[i]) == "--open")
                openRegistration = true;
            else if (std::string(argv[i]) == "--dev")
                dev = true;  // debug commands for everyone (#341); never on a public server
            else if (std::string(argv[i]) == "--seed" && i + 1 < argc)
                seed = std::strtoull(argv[++i], nullptr, 10);
            // A new galaxy's civilisation centres, 1 to 6 (#342 S2); a saved world keeps its own.
            else if (std::string(argv[i]) == "--centres" && i + 1 < argc)
                centres = atoi(argv[++i]);
            // Every new pilot is dealt a start rather than choosing one.
            else if (std::string(argv[i]) == "--random-starts")
                randomStarts = true;
            // The cap (#342 S3): no region opens past this many regions or systems. Saved
            // with the world; given again, it replaces what was saved.
            else if (std::string(argv[i]) == "--max-regions" && i + 1 < argc)
                maxRegions = std::max(1, atoi(argv[++i]));
            else if (std::string(argv[i]) == "--max-systems" && i + 1 < argc)
                maxSystems = std::max(1, atoi(argv[++i]));
            // Admin packs from elsewhere than data/packs/ (#342 S7), woven into a new world.
            else if (std::string(argv[i]) == "--packs" && i + 1 < argc)
                g_packsDir = argv[++i];
        if (centres < 1 || centres > Gen::MAX_CENTRES)
        {
            fprintf(stderr, "--centres must be 1 to %d\n", Gen::MAX_CENTRES);
            return 2;
        }
        return RunHost(port, isPublic, openRegistration, seed, dev, centres, randomStarts,
                       maxRegions, maxSystems);
    }

    std::string dataDir = DataDir::Path();

    // Determinism: fix both the server RNG (Simulation) and the global raylib RNG
    // (NpcShip uses it for patrols/timers) — valid without a window.
    SetRandomSeed(0xC0FFEEu);

    if (!Factions::Load(dataDir + "factions.json"))
    {
        fprintf(stderr, "FATAL: %s\n", Factions::Error().c_str());
        return 1;
    }
    // What markets trade (#343), before anything is priced or carried.
    if (!Goods::Load(dataDir + "goods.json"))
    {
        fprintf(stderr, "FATAL: %s\n", Goods::Error().c_str());
        return 1;
    }
    if (!Archetypes::Load(dataDir + "archetypes.json"))
    {
        fprintf(stderr, "FATAL: %s\n", Archetypes::Error().c_str());
        return 1;
    }
    // ...and blueprints, as the host does: a faction settles from one (#295).
    if (!Blueprints::Load(dataDir + "blueprints.json"))
    {
        fprintf(stderr, "FATAL: %s\n", Blueprints::Error().c_str());
        return 1;
    }

    // econserver <ticks> [--seed N] [--news] [--hot]: the galaxy of seed N (1 unless given);
    // with --news, the galactic news as it happens, stamped, instead of the
    // periodic tables -- how a living world is watched over hours (#231). Nobody is
    // connected, so every system is cold (#295); --hot keeps them all hot as they were
    // before, to compare the two. --players K parks K pilots at the starts, in turn: how the
    // tide answers people (#342 S3).
    int      ticks = (argc > 1) ? atoi(argv[1]) : 3600;
    uint64_t regionSeed = 1;
    bool     newsOnly = false, allHot = false;
    int      players = 0;
    // --max-regions N: the cap, as the host takes it; --rules F: world rules of another file
    // in place of data/world_rules.json -- a day's archive in minutes, say (#342 S4).
    int         maxRegions = 0;
    std::string rulesPath;
    for (int i = 2; i < argc; i++)
        if (std::string(argv[i]) == "--seed" && i + 1 < argc)
            regionSeed = std::strtoull(argv[++i], nullptr, 10);
        else if (std::string(argv[i]) == "--max-regions" && i + 1 < argc)
            maxRegions = std::max(1, atoi(argv[++i]));
        else if (std::string(argv[i]) == "--rules" && i + 1 < argc)
            rulesPath = argv[++i];
        else if (std::string(argv[i]) == "--news")
            newsOnly = true;
        else if (std::string(argv[i]) == "--hot")
            allHot = true;
        else if (std::string(argv[i]) == "--players" && i + 1 < argc)
            players = std::max(0, atoi(argv[++i]));

    Simulation sim;
    sim.SetAllHot(allHot);
    SetupHostSim(sim, dataDir, std::string(), regionSeed, Gen::DEFAULT_CENTRES, maxRegions);
    if (!rulesPath.empty() && !WorldRules::Load(rulesPath))
    {
        fprintf(stderr, "FATAL: %s\n", WorldRules::Error().c_str());
        return 1;
    }

    for (int p = 0; p < players; p++)
        SetupHostPlayer(sim, sim.Starts()[(size_t)p % sim.Starts().size()].systemId);

    const float dt = 1.0f / 60.0f;  // = SIM_DT: server tick as in the game
    long long   seen = 0;           // the history already printed, by seq

    printf("EconSpace headless server — %d systems, %d ticks (%s)\n",
           (int)sim.Universe().systems.size(), ticks,
           allHot ? "every system hot" : "nobody here: every system cold");

    int printEvery = ticks / 20;
    if (printEvery < 1)
        printEvery = 1;

    for (int i = 0; i < ticks; i++)
    {
        // The systems with ships (none, unless --hot) + coarse world maintenance.
        for (auto& kv : sim.Systems())
            sim.StepSystemAgents(kv.second, {}, nullptr, dt);
        sim.MaintainWorld(dt);

        if (newsOnly)
        {
            // The history is numbered (#295): what is new is what follows the last entry
            // printed, surveys included.
            for (const ChronicleEntry& e : sim.Chronicle())
                if (e.seq > seen)
                {
                    printf("  %6.0f min  %s\n", e.time / 60.0, e.text.c_str());
                    seen = e.seq;
                }
            // How the world grows (#342 S3): each region as it opens and why, and the tide
            // every hour.
            for (int slot : sim.TakeOpenedRegions())
                PrintOpening(sim, slot, "             >> ");
            if (i > 0 && i % (int)(3600.0f / dt) == 0)
            {
                printf("  %6.0f min  ", sim.Time() / 60.0);
                PrintTide(sim, "");
            }
        }
        if (!newsOnly && (i % printEvery == 0 || i == ticks - 1))
        {
            printf("\n[tick %d / t=%.1fs]\n", i, i * dt);
            for (auto& kv : sim.Systems())
            {
                sim.RecountAgg(kv.second);  // fresh real numbers for printing, where hot
                const SystemAggregate& a = kv.second.agg;
                printf("  %-10s sec %.2f  pir %2d  pol %2d  trd %2d  econ %3.0f%%  ctrl %s\n",
                       kv.first.c_str(), a.security, (int)a.pirates, (int)a.police, (int)a.traders,
                       a.prosperity * 100.0f, FactionName(a.controller).c_str());
            }
        }
    }

    if (newsOnly)
    {
        printf("\nAfter %.0f minutes:\n", sim.Time() / 60.0);
        for (auto& kv : sim.Systems())
        {
            const SystemAggregate& a = kv.second.agg;
            printf("  %-10s %-14s sec %.2f  presence", kv.first.c_str(),
                   FactionName(a.controller).c_str(), a.security);
            for (FactionId f : Factions::All())
                printf(" %.1f", a.presence[f]);
            printf("\n");
        }
        // Where each faction's outpost stands, and what it stands by (#318).
        printf("\nOutposts:\n");
        for (const auto& kv : sim.Systems())
            for (const auto& e : kv.second.entities)
            {
                if (e->GetKind() != EntityKind::Structure ||
                    !Outposts::IsOutpost(static_cast<const Structure&>(*e).GetBuilds()))
                    continue;
                const Vector2 at = e->GetPosition();
                auto          nearest = [&](EntityKind kind)
                {
                    float d = -1.0f;
                    for (const auto& o : kv.second.entities)
                        if (o->GetKind() == kind)
                        {
                            const float r =
                                std::hypot(o->GetPosition().x - at.x, o->GetPosition().y - at.y) -
                                o->GetSize();
                            d = d < 0.0f ? r : std::min(d, r);
                        }
                    return d;
                };
                // ...and how much of it is left (#295, slice 5): upkeep mends it, decay and
                // sieges wear it down.
                const Structure& t = static_cast<const Structure&>(*e);
                printf("  %-10s %-24s %-20s hull %3.0f%%  r %7.0f  gate %7.0f  belt %7.0f  wreck "
                       "%7.0f%s\n",
                       kv.first.c_str(), e->GetName().c_str(),
                       Outposts::Noun(t.GetBuilds()).c_str(), t.HullFraction(sim.Time()) * 100.0f,
                       std::hypot(at.x, at.y), nearest(EntityKind::Gate),
                       nearest(EntityKind::Field), nearest(EntityKind::Derelict),
                       e->GetOrbit() ? ("  orbits planet " + std::to_string(e->GetOrbit()->planet) +
                                        " at " + std::to_string((int)e->GetOrbit()->radius))
                                           .c_str()
                                     : "");
            }
        return 0;
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
