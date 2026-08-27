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

#include <cmath>

// World snapshot of a system for the client: each entity -> id/kind/position/size and
// (for NPCs) faction/role/heading/hull fraction. The player and shots are added by the client.
Proto::Snapshot Simulation::BuildSnapshot(const ClientSession& s, const std::string& systemId) const
{
    Proto::Snapshot snap;
    snap.systemId = systemId;
    auto it = systems_.find(systemId);
    if (it == systems_.end())
        return snap;

    for (const auto& e : it->second.entities)
    {
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

    // Physical view of the player ship (for the networked client — authoritative on the
    // server; in single-player the client augments the view with its own weaponOn/docked/nearby
    // fields).
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
        p.autopilot = s.ship->IsAutopilotOn();
        p.apTarget = s.ship->GetAutopilotTarget();
        p.apStop = s.ship->GetAutopilotStopDistance();
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

Proto::SystemLayout Simulation::BuildLayout(const std::string& systemId) const
{
    Proto::SystemLayout lay;
    lay.systemId = systemId;
    auto it = systems_.find(systemId);
    if (it == systems_.end())
        return lay;

    for (const auto& e : it->second.entities)
    {
        // NPCs — dynamics (created by the client from the snapshot), not in the static layout.
        if (e->GetKind() == EntityKind::Npc)
            continue;

        Proto::EntityLayout el;
        el.id = e->GetId();
        el.pos = e->GetPosition();
        el.size = e->GetSize();
        el.color = e->GetColor();
        el.name = e->GetName();

        // No `default:` on purpose, unlike the snapshot above. There the kind is copied
        // wholesale and the cases only add optional fields; here every kind has to be
        // decided in or out of the static layout, and a new one silently dropped would be
        // an entity the client never draws. Let the compiler ask the question.
        el.kind = e->GetKind();
        switch (e->GetKind())
        {
            case EntityKind::Star:
                el.subType = (int)static_cast<const Star*>(e.get())->GetStarType();
                break;
            case EntityKind::Planet:
            {
                const Planet* p = static_cast<const Planet*>(e.get());
                el.subType = (int)p->GetPlanetType();
                el.orbitRadius = p->GetOrbitRadius();
                el.resource = (int)p->GetDeposit();
                break;
            }
            case EntityKind::Station:
            {
                const Station* st = static_cast<const Station*>(e.get());
                el.faction = st->GetFaction();
                el.subType = (int)st->GetRole();
                break;
            }
            case EntityKind::Field:
                el.resource = (int)static_cast<const AsteroidField*>(e.get())->GetResource();
                break;
            case EntityKind::Gate:
                el.dest = static_cast<const JumpGate*>(e.get())->GetDestination();
                break;
            case EntityKind::Nebula: break;
            case EntityKind::Derelict:
                el.reward = static_cast<const Derelict*>(e.get())->GetReward();
                break;
            case EntityKind::Npc:         // dynamic — skipped above, listed so the switch is total
            case EntityKind::PlayerShip:  // never a world entity
            case EntityKind::Unknown: continue;
        }

        lay.entities.push_back(el);
    }
    return lay;
}

Proto::GalaxyState Simulation::BuildGalaxyState()
{
    Proto::GalaxyState gs;
    for (auto& kv : systems_)
    {
        RecountAgg(kv.second);  // refresh the population from live entities
        const SystemAggregate&  a = kv.second.agg;
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
