// What the client is told: the per-tick snapshot, the once-per-system layout and the
// galaxy overview. One translation unit of Simulation (#17).
//
// Nothing here decides anything -- it only translates authoritative state into wire
// messages. Keeping it apart is what makes a protocol change reviewable on its own.

#include "sim/Simulation.h"
#include "sim/ClientSession.h"

#include "core/World.h"
#include "entities/AsteroidField.h"
#include "entities/Derelict.h"
#include "entities/JumpGate.h"
#include "entities/Nebula.h"
#include "entities/NpcShip.h"
#include "entities/Planet.h"
#include "entities/Ship.h"
#include "entities/Star.h"
#include "entities/Station.h"
#include "entities/Structure.h"

#include <cmath>

// World snapshot of a system for the client: each entity -> id/kind/position/size and
// (for NPCs) faction/role/heading/hull fraction. The player and shots are added by the client.
Proto::Snapshot Simulation::BuildSnapshot(const ClientSession& s, const std::string& systemId) const
{
    Proto::Snapshot snap;
    snap.systemId = systemId;
    snap.time = time_;
    auto it = systems_.find(systemId);
    if (it == systems_.end())
        return snap;

    for (const auto& e : it->second.entities)
    {
        // Anything that cannot move is already fully described by the layout the client
        // was sent on entry, so it is not described again here (#97). Planets orbit, so
        // they stay. What this costs is that a static object which changed mid-session is
        // invisible until the client re-enters the system -- which was already true of
        // its name and size, and is #38.
        // A satellite moves with its planet (#210), so it stays too.
        if (!e->GetOrbit())
            switch (e->GetKind())
            {
                case EntityKind::Star:
                case EntityKind::Station:
                case EntityKind::Field:
                case EntityKind::Gate:
                case EntityKind::Nebula:
                case EntityKind::Derelict:
                case EntityKind::Structure: continue;
                default: break;
            }

        Proto::EntitySnapshot es;
        es.id = e->GetId();
        es.pos = e->GetPosition();

        // Size and name are left out on purpose for anything the SystemLayout describes
        // (#16). A station's name and radius do not change, the client was told them on
        // entry, and re-sending them thirty times a second is the largest single thing
        // this message was spending its bytes on. Proto::CompleteFromLayout puts them
        // back on the receiving side, so nothing downstream knows the difference.
        const bool inLayout = e->GetKind() != EntityKind::Npc;
        if (!inLayout)
        {
            es.size = e->GetSize();
            es.name = e->GetName();
        }

        // The wire kind IS the world kind — one enum, so this is a copy, not a translation.
        // Only the extra per-kind fields need a case.
        es.kind = e->GetKind();
        switch (e->GetKind())
        {
            case EntityKind::Npc:
            {
                const NpcShip* n = static_cast<const NpcShip*>(e.get());
                es.faction = n->GetFaction();
                es.role = (int)n->GetRole();
                es.heading = n->GetHeading();
                es.hullFrac = n->GetMaxHull() > 0.0f ? n->GetHull() / n->GetMaxHull() : 0.0f;
                break;
            }
            // Station faction and field ore are in the layout too, and neither changes
            // while a client is in the system (#16, #38).
            case EntityKind::Station: break;
            case EntityKind::Field: break;
            default: break;
        }

        snap.entities.push_back(es);
    }

    // System market prices (for the client's station screen) — AllResourceTypes order.
    for (ResourceType rt : AllResourceTypes())
        snap.marketPrices.push_back((float)it->second.market.GetPrice(rt));

    // Physical view of the player's ship. Everything here is authoritative: the client
    // predicts its own movement between snapshots and is corrected by this.
    if (s.ship)
    {
        Proto::PlayerView& p = snap.player;
        p.pos = s.ship->GetPosition();
        p.vel = s.ship->GetVelocity();
        p.heading = s.ship->GetHeading();
        p.hull = s.ship->GetHull();
        p.maxHull = s.ship->GetMaxHull();
        p.shields = s.ship->GetShields();
        p.maxShields = s.ship->GetMaxShields();
        p.ownedShips = s.ownedShips;
        p.shipIndex = s.currentShip;
        p.cargoUsed = s.ship->GetCargoUsed();
        p.cargoCap = s.ship->GetCargoCapacity();
        p.warpPhase = (int)s.ship->GetWarpPhase();
        p.warpAlign = s.ship->GetWarpAlignTimer();
        p.warpTarget = s.ship->GetWarpTarget();
        p.warpDrop = s.ship->GetWarpDrop();
        p.warpViaSet = s.ship->HasWarpVia();
        p.warpVia = s.ship->GetWarpVia();
        p.autopilot = s.ship->IsAutopilotOn();
        p.apTarget = s.ship->GetAutopilotTarget();
        p.apStop = s.ship->GetAutopilotStopDistance();
        const HoldMode hm = s.ship->GetHoldMode();
        p.holdMode = hm == HoldMode::Orbit    ? 1
                     : hm == HoldMode::Keep   ? 2
                     : hm == HoldMode::Follow ? 3
                                              : 0;
        p.holdTargetId = s.ship->GetHoldTargetId();
        p.holdRange = s.ship->GetHoldRange();
        p.docked = (s.dockedStationId != 0);
        p.dockedStationId = s.dockedStationId;
        p.stabilizer = s.ship->IsStabilizerOn();
        p.mining = s.ship->IsMiningOn();
        p.weaponOn = s.weaponOn;
        p.orderKind = (int)s.order.kind;
        p.orderStatus = (int)s.orderStatus;
        p.orderId = s.orderId;
        p.orderDetail = s.orderDetail;
        for (ResourceType rt : AllResourceTypes())
            p.cargoByType.push_back(s.ship->GetCargoAmount(rt));

        // Player account (server-authoritative, M4f): the client shows it as a mirror.
        p.money = s.account.GetMoney();
        for (int i = 0; i < 4; i++)
        {
            p.reputation.push_back(s.account.GetReputation((FactionId)i));
            p.bounty.push_back(s.account.GetBounty((FactionId)i));
        }
        p.skillXp = { (float)s.account.GetSkills().GetXp(SkillType::Piloting),
                      (float)s.account.GetSkills().GetXp(SkillType::Mining),
                      (float)s.account.GetSkills().GetXp(SkillType::Trading) };
    }

    // Missions (M4f-2): the board — only when docked, active ones — always. completable
    // is computed by the server (account/cargo/docking are authoritative).
    auto toView = [this, &s](const Mission& m)
    {
        Proto::MissionView v;
        v.type = (int)m.type;
        v.faction = (int)m.faction;
        v.title = m.title;
        v.description = m.description;
        v.giverStationId = m.giverStationId;
        v.destStationId = m.destStationId;
        v.resource = (int)m.resource;
        v.targetCount = m.targetCount;
        v.progress = m.progress;
        v.rewardMoney = m.rewardMoney;
        v.rewardRep = m.rewardRep;
        v.completable = MissionCompletableNow(s, m);
        return v;
    };
    if (s.IsDocked())
        for (const Mission& m : s.missions.Offers())
            snap.missionOffers.push_back(toView(m));
    for (const Mission& m : s.missions.Active())
        snap.missionActive.push_back(toView(m));

    // Other players standing in this system (#4). Their ships belong to sessions rather
    // than to the system, so they are appended here rather than found in the loop above.
    // The recipient is left out on purpose: the client predicts its own ship, and a proxy
    // of it would fight the prediction for the wheel.
    for (const auto& kv : sessions_)
    {
        const ClientSession& other = kv.second;
        if (other.id == s.id || !other.ship || other.systemId != systemId || other.IsDocked())
            continue;  // docked is not "in space": a station is cover from being seen

        Proto::EntitySnapshot es;
        es.id = other.ship->GetId();
        es.kind = EntityKind::PlayerShip;
        es.pos = other.ship->GetPosition();
        es.heading = other.ship->GetHeading();
        es.size = other.ship->GetSize();
        es.name = other.ship->GetName();
        es.hullFrac = other.ship->GetMaxHull() > 0.0f
                          ? other.ship->GetHull() / other.ship->GetMaxHull()
                          : 1.0f;
        snap.entities.push_back(es);
    }

    return snap;
}

// One object of the static layer as the wire describes it. False for anything that is not
// in the layout at all -- an NPC, a player's ship. Shared by the whole layout and by a
// delta, so an object a delta announces is described exactly as a later layout would.
static bool DescribeStatic(const Entity& e, Proto::EntityLayout& el)
{
    el.id = e.GetId();
    el.pos = e.GetPosition();
    el.size = e.GetSize();
    el.color = e.GetColor();
    el.name = e.GetName();
    el.archetype = e.GetArchetype() != nullptr ? e.GetArchetype()->id : std::string();
    el.owner = e.GetOwner();

    // No `default:` on purpose, unlike the snapshot above. There the kind is copied
    // wholesale and the cases only add optional fields; here every kind has to be
    // decided in or out of the static layout, and a new one silently dropped would be
    // an entity the client never draws. Let the compiler ask the question.
    el.kind = e.GetKind();
    switch (e.GetKind())
    {
        case EntityKind::Star: el.subType = (int)static_cast<const Star&>(e).GetStarType(); break;
        case EntityKind::Planet:
        {
            const Planet& p = static_cast<const Planet&>(e);
            el.subType = (int)p.GetPlanetType();
            el.orbitRadius = p.GetOrbitRadius();
            el.resource = (int)p.GetDeposit();
            break;
        }
        case EntityKind::Station:
        {
            const Station& st = static_cast<const Station&>(e);
            el.faction = st.GetFaction();
            el.subType = (int)st.GetRole();
            break;
        }
        case EntityKind::Field:
            el.resource = (int)static_cast<const AsteroidField&>(e).GetResource();
            break;
        case EntityKind::Gate: el.dest = static_cast<const JumpGate&>(e).GetDestination(); break;
        case EntityKind::Nebula: break;
        case EntityKind::Derelict:
        {
            // The name and the state apart (#38): a client rebuilding the wreck sets it
            // searched itself, and the name it was given must not say so already.
            const Derelict& d = static_cast<const Derelict&>(e);
            el.name = d.GetBaseName();
            el.reward = d.GetReward();
            el.looted = d.IsLooted();
            break;
        }
        case EntityKind::Structure:
        {
            // What it is or will be, and when: a client wears the site's look until then and
            // needs no word from the server when the moment passes (#39).
            const Structure& t = static_cast<const Structure&>(e);
            el.archetype = t.GetBuilds();
            el.startedAt = t.IsBuilding() ? t.GetStartedAt() : 0.0;
            el.completesAt = t.GetCompletesAt();
            el.expiresAt = t.GetExpiresAt();
            break;
        }
        case EntityKind::Npc:         // dynamic: created by the client from the snapshot
        case EntityKind::PlayerShip:  // never a world entity
        case EntityKind::Unknown: return false;
    }
    return true;
}

Proto::SystemLayout Simulation::BuildLayout(const std::string& systemId) const
{
    Proto::SystemLayout lay;
    lay.systemId = systemId;
    auto it = systems_.find(systemId);
    if (it == systems_.end())
        return lay;

    lay.rev = it->second.layoutRev;
    for (const auto& e : it->second.entities)
    {
        Proto::EntityLayout el;
        if (DescribeStatic(*e, el))
            lay.entities.push_back(std::move(el));
    }
    return lay;
}

std::vector<Proto::LayoutDelta> Simulation::TakeLayoutDeltas()
{
    std::vector<Proto::LayoutDelta> out;
    for (auto& kv : systems_)
    {
        SystemState& st = kv.second;
        if (st.pendingAdded.empty() && st.pendingChanged.empty() && st.pendingRemoved.empty())
            continue;

        Proto::LayoutDelta d;
        d.systemId = kv.first;
        d.rev = st.layoutRev;
        // Described as they are now, not as they were when the change was recorded: two
        // changes to one object since the last delta are one entry, and an object added and
        // removed again in between is only a removal.
        for (const auto& e : st.entities)
        {
            const int  id = e->GetId();
            const bool added = st.pendingAdded.count(id) > 0;
            if (!added && st.pendingChanged.count(id) == 0)
                continue;
            Proto::EntityLayout el;
            if (DescribeStatic(*e, el))
                (added ? d.added : d.changed).push_back(std::move(el));
        }
        d.removed.assign(st.pendingRemoved.begin(), st.pendingRemoved.end());

        st.pendingAdded.clear();
        st.pendingChanged.clear();
        st.pendingRemoved.clear();
        out.push_back(std::move(d));
    }
    return out;
}

Proto::GalaxyState Simulation::BuildGalaxyState()
{
    Proto::GalaxyState gs;
    for (auto& kv : systems_)
    {
        RecountAgg(kv.second);  // refresh the population from live entities
        const SystemAggregate& a = kv.second.agg;
        if (!a.visited)
            continue;  // nobody knows how a system nobody has seen is doing (#144)
        Proto::GalaxySystemStat g;
        g.id = kv.first;
        g.security = a.security;
        g.pirates = (int)(a.pirates + 0.5f);
        g.prosperity = a.prosperity;
        g.controller = a.controller;
        gs.systems.push_back(std::move(g));
    }
    gs.events = events_;
    return gs;
}
