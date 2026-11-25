// The server's self-tests (#354): `econserver hosttest | accttest | worldtest | ordertest |
// opstest`. Smoke tests that run the host loop, a save, a world or the admin channel end to
// end; the rules themselves are tested where they live, in tests/.

#include "sim/SelfTests.h"

#include "sim/HostLoop.h"

#include "sim/AccessList.h"
#include "sim/Auth.h"
#include "sim/Chat.h"
#include "sim/Compat.h"
#include "sim/HostRules.h"
#include "sim/Login.h"
#include "sim/MacroBench.h"
#include "sim/Protocol.h"
#include "sim/SaveFile.h"
#include "sim/SaveSchema.h"
#include "sim/ServerConfig.h"
#include "sim/ServerLog.h"
#include "sim/ServerOps.h"
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
#include "core/Version.h"
#include "core/Faction.h"
#include "economy/Resource.h"
#include "entities/AsteroidField.h"
#include "entities/Combatant.h"
#include "entities/Nebula.h"
#include "entities/NpcShip.h"
#include "entities/Ship.h"
#include "entities/ShipType.h"
#include "entities/Station.h"
#include "entities/Structure.h"
#include "missions/Mission.h"
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
#include <iterator>
#include <mutex>
#include <sstream>

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
int HostSelftest()
{
    std::string dataDir = DataDir::Path();
    Simulation  sim;
    SetupHostSim(sim, dataDir);
    ClientSession& s = SetupHostPlayer(sim);

    LocalTransport link;
    s.accountName = "selftest";
    link.Server().Send(LayoutFor(sim, s.systemId, s));
    // What anyone else in the system is sent, as RunHost cuts it (#340).
    Proto::LayoutMirror stranger;
    {
        Proto::SystemLayout lay;
        ClientSession       other;
        other.accountName = "stranger";
        if (Proto::DecodeLayout(LayoutFor(sim, s.systemId, other), lay))
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
    // The start is a faction's space: building there takes its licence (#431 slice 6).
    sim.GrantLicence(s.accountName, s.systemId);
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
int HostNameSelftest()
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
            link.Server().Send(Proto::EncodeUniverse(sim.KnownUniverseFor(s)));
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
    const SystemState* keptIn = again.SystemById(entry);
    const bool         kept = keptIn != nullptr && keptIn->agg.givenName == "Selftest Haven";
    std::remove(path.c_str());

    printf("Name selftest: in-flight %s, refusal-said %s (\"%s\"), named-for-all %s, "
           "kept %s\n",
           inFlight ? "OK" : "FAIL", refusedSaid ? "OK" : "FAIL", refusal.c_str(),
           named ? "OK" : "FAIL", kept ? "OK" : "FAIL");
    return (inFlight && refusedSaid && named && kept) ? 0 : 1;
}

// Account persistence smoke test (no network, M4f-3): write the account to a file and
// read it into a clean simulation, compare the fields. Run: econserver accttest.
int AccountSelftest()
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
int WorldSelftest()
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

    // What every open region's NPCs would fly (#464): shares that add up, for each region.
    bool styles = true;
    for (const Simulation::RegionOpening& o : a.OpeningLog())
    {
        const Ships::Style s = a.RegionStyle(o.slot);
        float              families = 0.0f, lines = 0.0f;
        std::string        said;
        for (const auto& [id, share] : s.families)
        {
            families += share;
            said += " " + id + " " + std::to_string((int)std::lround(100.0f * share)) + "%";
        }
        for (const auto& [id, share] : s.lines)
            lines += share;
        printf("  region %d (%s):%s, %s hulls\n", o.slot, a.RegionCulture(o.slot).c_str(),
               said.c_str(), s.LeadingLine().c_str());
        styles = styles && approx(families, 1.0) && approx(lines, 1.0);
    }

    bool ok = ticked && loaded && time && sec && prosp && pir && ctrl && kept && styles;
    printf("World selftest: clock %s, load %s, time %s, security %s, prosperity %s, "
           "pirates %s, controller %s, built/removed kept %s, region styles %s => %s\n",
           ticked ? "OK" : "FAIL", loaded ? "OK" : "FAIL", time ? "OK" : "FAIL",
           sec ? "OK" : "FAIL", prosp ? "OK" : "FAIL", pir ? "OK" : "FAIL", ctrl ? "OK" : "FAIL",
           kept ? "OK" : "FAIL", styles ? "OK" : "FAIL", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

// Standing-order smoke test (no network): give the ship an order, tick the server the way
// the host loop does, and check it finishes on its own. This is the property agent play
// rests on -- one order, no further input, the server does the flying.
// Run: econserver ordertest.
int OrderSelftest()
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
    std::vector<std::string> self = sim.PlanRoute(startSys, startSys);
    bool                     selfRoute = self.size() == 1 && self[0] == startSys;

    // Every other system must be reachable from the start, and the path must begin and
    // end where it was asked to.
    bool allReachable = true;
    for (const WorldLoader::SystemInfo& si : sim.Universe().systems)
    {
        // The start's own centre: civilisations are apart until their frontiers meet (S2).
        if (Gen::SlotOf(si.id) != Gen::SlotOf(startSys))
            continue;
        std::vector<std::string> path = sim.PlanRoute(startSys, si.id);
        if (path.empty() || path.front() != startSys || path.back() != si.id)
            allReachable = false;
    }
    bool noRoute = sim.PlanRoute(startSys, "nowhere").empty();

    // Planning around danger must still produce a valid path, not give up.
    bool safeRouteOk = true;
    for (const WorldLoader::SystemInfo& si : sim.Universe().systems)
    {
        // The start's own centre: civilisations are apart until their frontiers meet (S2).
        if (Gen::SlotOf(si.id) != Gen::SlotOf(startSys))
            continue;
        std::vector<std::string> path = sim.PlanRoute(startSys, si.id, Route::Preference::Safer);
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

    // 7) Docked, over the wire as an agent sends it: an undock order runs, and any other order
    // is refused with the reason instead of being dropped (an agent that docked stayed).
    bool dockedOrders = false;
    for (const auto& e : sim.SystemOf(s)->entities)
        if (e->GetKind() == EntityKind::Station && !dockedOrders)
        {
            s.ship->Teleport(e->GetPosition());
            if (sim.StepPlayerDock(s, *sim.SystemOf(s)) == 0)
                continue;
            std::vector<Proto::TradeAck>                  acks;
            std::map<std::string, std::vector<FireEvent>> fires;
            Proto::Command                                go;
            go.orderKind = (int)Orders::Kind::MoveTo;
            go.orderPoint = { 0.0f, 0.0f };
            sim.ApplyCommand(s, go, dt, acks, fires);
            const bool     refused = s.orderStatus == Orders::Status::Failed &&
                                     s.orderDetail == "docked; undock first" && s.IsDocked();
            Proto::Command leave;
            leave.orderKind = (int)Orders::Kind::Undock;
            sim.ApplyCommand(s, leave, dt, acks, fires);
            run(10);
            dockedOrders = refused && !s.IsDocked() && s.orderStatus == Orders::Status::Done;
        }

    bool ok = idOk && arrived && rejected && aborted && selfRoute && allReachable && noRoute &&
              safeRouteOk && routeRejected && journalled && journalCursor && journalHistory &&
              dockedOrders;
    printf("Order selftest: id %s, move %s, bad-target %s, abort %s, route-self %s, "
           "route-all %s, route-none %s, route-safe %s, route-reject %s, journal %s, "
           "docked %s => %s\n",
           idOk ? "OK" : "FAIL", arrived ? "OK" : "FAIL", rejected ? "OK" : "FAIL",
           aborted ? "OK" : "FAIL", selfRoute ? "OK" : "FAIL", allReachable ? "OK" : "FAIL",
           noRoute ? "OK" : "FAIL", safeRouteOk ? "OK" : "FAIL", routeRejected ? "OK" : "FAIL",
           (journalled && journalCursor && journalHistory) ? "OK" : "FAIL",
           dockedOrders ? "OK" : "FAIL", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

// Server operations smoke test (#400): a world directory made by init, a real server on it
// (loopback, a port the system picks), and the console commands given the way an admin on a
// headless box gives them -- through `econserver admin`'s channel -- ending with `shutdown`,
// after which everything must be saved and the server must have returned 0.
// Run: econserver opstest.
int OpsSelftest()
{
    namespace fs = std::filesystem;
    const auto        stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string dir =
        (fs::temp_directory_path() / ("econserver_ops_" + std::to_string(stamp))).generic_string() +
        "/";
    Host::ServerConfig cfg;
    cfg.port = 0;
    cfg.seed = 7;
    cfg.registration = Host::Registration::Open;
    std::string error;
    if (!Host::InitWorldDir(dir, cfg, error))
    {
        fprintf(stderr, "opstest: init FAIL: %s\n", error.c_str());
        return 1;
    }
    std::map<std::string, std::string> said;
    bool                               answered = true;
    std::thread                        admin(
        [&]()
        {
            // Until the server is up, nothing takes a command; it is asked again.
            std::string status;
            bool        up = false;
            for (int i = 0; i < 240 && !up; i++)
                if (!(up = Host::AdminRequest(dir, "status", status, 1.0, 30.0)))
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
            said["status"] = status;
            answered = up;
            for (const char* c :
                 { "who", "save", "backup", "kick nobody", "shutdown 600", "shutdown cancel" })
            {
                std::string a;
                answered = Host::AdminRequest(dir, c, a, 5.0, 60.0) && answered;
                said[c] = a;
            }
#if defined(_WIN32)
            std::string a;
            answered = Host::AdminRequest(dir, "shutdown", a, 5.0, 60.0) && answered;
            said["shutdown"] = a;
#else
            // What systemd's stop sends: the same clean stop as the command.
            said["shutdown"] = "Shutting down now";
            std::raise(SIGTERM);
#endif
            if (!up)
                Host::RequestStop();
        });
    const int rc = RunHost(cfg, dir);
    admin.join();
    Log::SetStamped(false);

    bool ok = true;
    auto check = [&ok](bool cond, const char* what)
    {
        if (!cond)
        {
            fprintf(stderr, "opstest: FAIL: %s\n", what);
            ok = false;
        }
    };
    const auto has = [&said](const char* cmd, const char* text)
    { return said[cmd].find(text) != std::string::npos; };
    check(rc == 0, "the server returned 0 after `shutdown`");
    check(answered, "every command was answered");
    check(has("status", "Up ") && has("status", "Players: 0 in play"), "status says how it is");
    check(has("who", "0 pilot(s) in play"), "who lists nobody");
    check(has("save", "Saved the world"), "save saves");
    check(has("backup", "Backup: "), "backup makes a copy");
    check(has("kick nobody", "Nobody called 'nobody'"), "kick names who is not there");
    check(has("shutdown 600", "Shutting down in 10 min"), "a shutdown can be put off");
    check(has("shutdown cancel", "Shutdown cancelled"), "...and cancelled");
    check(has("shutdown", "Shutting down now"), "shutdown answers before it stops");
    std::error_code ec;
    check(fs::exists(dir + "world.json", ec), "world.json is saved");
    check(fs::exists(dir + "corps.json", ec), "corps.json is saved");
    bool backedUp = false, logged = false;
    for (fs::directory_iterator it(dir + "backups", ec), end; !ec && it != end; it.increment(ec))
        backedUp = backedUp || fs::exists(it->path() / "world.json") ||
                   fs::exists(it->path() / "server.json");
    for (fs::directory_iterator it(dir + "logs", ec), end; !ec && it != end; it.increment(ec))
    {
        std::ifstream     in(it->path());
        const std::string text((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        logged = logged || text.find("Shut down cleanly") != std::string::npos;
    }
    check(backedUp, "backups/<time>/ holds the world");
    check(logged, "logs/server-<date>.log has the whole run, to the clean stop");
    fs::remove_all(dir, ec);
    printf("opstest: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
