// Game — the client's mirror of server state.
//
// Everything that turns what arrives on the wire into something drawable: snapshots and
// layouts in, proxy entities and a reconciled prediction out. The client owns no
// authoritative state, so this is the whole of what it knows about the world.
//
// Part of the Game class; see Game.cpp.
#include "core/Game.h"
#include "sim/PlayerStep.h"

#include "core/World.h"
#include "core/WorldLoader.h"
#include "entities/Star.h"
#include "entities/Planet.h"
#include "entities/Station.h"
#include "entities/AsteroidField.h"
#include "entities/NpcShip.h"
#include "entities/Nebula.h"
#include "entities/Derelict.h"
#include "entities/JumpGate.h"
#include "economy/Resource.h"
#include "ui/Button.h"
#include "ui/UiTheme.h"
#include "ui/Window.h"
#include "render/Textures.h"
#include "raymath.h"
#include <nlohmann/json.hpp>
#include <cmath>
#include <string>
#include <algorithm>
#include <fstream>

void Game::BuildNetworkBeams()
{
    beams_.clear();
    for (const FireEvent& f : snapshot_.fires)
    {
        Color c = f.fromPlayer       ? SKYBLUE
                  : f.targetIsPlayer ? ORANGE
                                     : Fade(FactionColor(f.shooterFaction), 0.85f);
        beams_.push_back({ f.from, f.to, c });
    }
}

// Network: credit sales revenue from the server's acknowledgements (tradeAcks). The server
// authoritatively computed the gross revenue (price slippage); the client applies account
// effects: trade-skill and reputation multipliers, XP, reputation gain with the faction.
void Game::ApplyTradeAcks(const Proto::Snapshot& s)
{
    // M4f: the account is server-authoritative — the server already credited the revenue to
    // account_, the client only shows the result (money is updated by the account mirror from the
    // snapshot).
    for (const Proto::TradeAck& a : s.tradeAcks)
    {
        if (a.sold <= 0)
            continue;
        FlashMessage(TextFormat("Sold %d %s  +%.0f cr", a.sold,
                                ResourceName((ResourceType)a.type).c_str(), a.revenue));
    }
}

// Makes system id active. fromId — the system we jumped from (empty at
// startup): the arrival point is chosen at the gate leading back there.
// fromId is taken BY VALUE: it's called with sim_.ActiveId(), which is overwritten
// below, so a reference would become dangling.
//
// L1: systems are persistent. First visit — load the static objects and
// populate NPCs; repeat visit — just activate the saved state (objects and
// NPCs in their places, as the player left them).
// cppcheck-suppress passedByValue ; fromId is intentionally by value (see above)
Entity* Game::FindEntityById(int id) const
{
    if (id == 0)
        return nullptr;
    for (const auto& e : clientWorld_)
        if (e->GetId() == id)
            return e.get();
    return nullptr;
}

// Builds the active-system snapshot: the world is built by the server (Simulation::BuildSnapshot),
// the player state is added by the client (the player ship is still on the client, until M4f).
// Client: receives a new system's layout — remembers the static descriptions by id and
// resets the proxies (they'll be rebuilt from the new layout + snapshot).
void Game::ApplyLayout(const Proto::SystemLayout& lay)
{
    layoutById_.clear();
    for (const Proto::EntityLayout& el : lay.entities)
        layoutById_[el.id] = el;
    clientWorld_.clear();
    snapBuffer_.clear();  // another system's interpolation history isn't needed

    // Everything that pointed into the old system dies with clientWorld_. selected_ and
    // the station/field pointers must be dropped in the same breath, or they dangle and
    // are dereferenced on the very next frame (HandleInput reads selected_->GetId()).
    selected_ = nullptr;
    nearbyStation_ = nullptr;
    miningBeamField_ = nullptr;
    dockedStation_ = nullptr;  // the snapshot re-establishes docking if we are docked
    beams_.clear();

    // The view belongs to the old system's coordinates: re-center the radar, and snap the
    // camera once the new position arrives instead of sliding across the gap.
    radarInit_ = false;
    cameraSnap_ = true;
}

void Game::BuildClientSnapshot()
{
    // Client side: parse incoming transport messages by type. Layout
    // (entering a system) is applied immediately; several snapshots may arrive — for rendering
    // we take the last (intermediate ones are stale), but one-shot trade acks are applied from
    // EACH (they must not be lost when discarding intermediate snapshots).
    std::string     msg;
    bool            gotSnap = false;
    Proto::Snapshot incoming;
    while (clientLink_->Poll(msg))
    {
        // A version mismatch would otherwise be invisible: every Decode* rejects the
        // message, the client keeps rendering the last snapshot, and the game looks
        // frozen for no stated reason. Say it once, plainly.
        int ver = Proto::MessageVersion(msg);
        if (ver != Proto::PROTO_VERSION)
        {
            if (!protocolMismatchReported_)
            {
                protocolMismatchReported_ = true;
                TraceLog(LOG_ERROR, "Protocol mismatch: server speaks v%d, this client v%d", ver,
                         Proto::PROTO_VERSION);
                FlashMessage(TextFormat("Server protocol v%d, client v%d — update needed", ver,
                                        Proto::PROTO_VERSION));
            }
            continue;
        }

        std::string type = Proto::MessageType(msg);
        if (type == "layout")
        {
            Proto::SystemLayout lay;
            if (Proto::DecodeLayout(msg, lay))
                ApplyLayout(lay);
        }
        else if (type == "galaxy")
        {
            Proto::DecodeGalaxy(msg, galaxyState_);  // per-system stats for the map
        }
        else if (type == "snap")
        {
            Proto::Snapshot s;
            if (!Proto::DecodeSnapshot(msg, s))
                continue;
            ApplyTradeAcks(s);                   // credit sales revenue (client account)
            for (const Ev::Event& e : s.events)  // server journal (#29)
                FlashMessage(e.text);
            incoming = std::move(s);
            gotSnap = true;
        }
    }
    if (gotSnap)
    {
        snapshot_ = std::move(incoming);
        // A buffer of snapshots with arrival timestamps — for interpolating non-own
        // entities (entity interpolation, Gambetta). We draw them "in the past", smoothing
        // out snapshot jitter. The own ship is NOT touched by interpolation (prediction).
        snapBuffer_.push_back({ GetTime(), snapshot_.entities });
        double cutoff = GetTime() - 0.5;  // keep ~0.5 s of history
        while (snapBuffer_.size() > 2 && snapBuffer_.front().t < cutoff)
            snapBuffer_.pop_front();
        if (snapBuffer_.size() > 120)
            snapBuffer_.pop_front();
    }

    Proto::PlayerView& p = snapshot_.player;
    {
        // RECONCILIATION (Gambetta). The server sent the authoritative state and the
        // sequence number of the last processed input (lastInput). We drop the acked inputs,
        // reset the ship to the server state, and REPLAY the remaining (unacked) inputs — the
        // result matches the current prediction, without snapping back to a stale position.
        // Hull/shields come from the server (combat is server-side).
        pendingInputs_.erase(std::remove_if(pendingInputs_.begin(), pendingInputs_.end(),
                                            [&](const Proto::Command& c)
                                            { return c.seq <= p.lastInput; }),
                             pendingInputs_.end());
        playerShip_->ApplyView(p.pos, p.heading, p.vel, p.hull, p.shields);
        // Warp/AP are server-authoritative: we mirror the server's warp scale and don't keep
        // our own. Applied BEFORE replay so unacked orders (which the server hasn't seen yet)
        // correctly "carry through" via prediction over the authoritative state.
        playerShip_->ApplyNavView(p.warpPhase, p.warpAlign, p.warpTarget, p.warpDrop, p.autopilot,
                                  p.apTarget, p.apStop);
        // Toggles (stabilizer/mining) are server-authoritative: restore from the snapshot
        // BEFORE replay, otherwise unacked toggle commands would flicker during replay
        // (like warp). Unacked toggles "carry through" via prediction below.
        playerShip_->SetStabilizerOn(p.stabilizer);
        playerShip_->SetMiningOn(p.mining);
        for (const Proto::Command& c : pendingInputs_)
            Sim::StepPlayerShip(*playerShip_, c, 1.0f, SIM_DT);
        // Weapon state is server-owned; the local flag is only an optimistic echo of the
        // toggle we sent, corrected here the same way the ship's position is.
        weaponOn_ = p.weaponOn;

        // The account is a MIRROR of the server (M4f): money/reputation/wanted/skills arrive in
        // the snapshot, the client only displays them (mutations moved to the server via
        // commands/Step).
        player_.SetMoney(p.money);
        for (int i = 0; i < 4 && i < (int)p.reputation.size(); i++)
            player_.SetReputation((FactionId)i, p.reputation[i]);
        for (int i = 0; i < 4 && i < (int)p.bounty.size(); i++)
            player_.SetBounty((FactionId)i, p.bounty[i]);
        if (p.skillXp.size() >= 3)
        {
            player_.GetSkills().SetXp(SkillType::Piloting, p.skillXp[0]);
            player_.GetSkills().SetXp(SkillType::Mining, p.skillXp[1]);
            player_.GetSkills().SetXp(SkillType::Trading, p.skillXp[2]);
        }

        // Missions are a MIRROR of the server (M4f-2): the board/active ones arrive in the
        // snapshot, the client only displays them (accept/turn in — via commands).
        auto toMission = [](const Proto::MissionView& v)
        {
            Mission m;
            m.type = (MissionType)v.type;
            m.faction = (FactionId)v.faction;
            m.title = v.title;
            m.description = v.description;
            m.giverStationId = v.giverStationId;
            m.destStationId = v.destStationId;
            m.resource = (ResourceType)v.resource;
            m.targetCount = v.targetCount;
            m.progress = v.progress;
            m.rewardMoney = v.rewardMoney;
            m.rewardRep = v.rewardRep;
            m.completable = v.completable;
            return m;
        };
        std::vector<Mission> offers, active;
        for (const Proto::MissionView& v : snapshot_.missionOffers)
            offers.push_back(toMission(v));
        for (const Proto::MissionView& v : snapshot_.missionActive)
            active.push_back(toMission(v));
        missions_.SetMirror(std::move(offers), std::move(active));

        // Server-authoritative docking: enter/leave station mode by snapshot.
        // We take the station from the proxy by id (at docking time the client was in flight, so a
        // proxy exists). Money/reputation/missions — the client account (missions over the network
        // — later).
        if (p.docked && mode_ == GameMode::Flying)
        {
            if (Station* s = StationById(p.dockedStationId))
            {
                mode_ = GameMode::Docked;
                dockedStation_ = s;
                FlashMessage(TextFormat("Docked at %s", s->GetName().c_str()));
            }
        }
        else if (!p.docked && mode_ == GameMode::Docked)
        {
            mode_ = GameMode::Flying;
            dockedStation_ = nullptr;
        }
    }
}

// Builds an engine proxy entity from a static layout description (star/
// planet/station/field/gate/nebula/derelict). The position is refined by the snapshot.
std::unique_ptr<Entity> Game::MakeProxyFromLayout(const Proto::EntityLayout& el)
{
    std::unique_ptr<Entity> e;
    switch (el.kind)
    {
        case Proto::EntityKind::Star:
            e = std::make_unique<Star>(el.pos, el.size, (StarType)el.subType);
            break;
        case Proto::EntityKind::Planet:
            e = std::make_unique<Planet>(el.orbitRadius, 0.0f, 0.0f, el.size, el.color,
                                         (ResourceType)el.resource, (PlanetType)el.subType);
            break;
        case Proto::EntityKind::Station:
            e = std::make_unique<Station>(el.pos, el.size, el.name, el.faction,
                                          (StationRole)el.subType);
            break;
        case Proto::EntityKind::Field:
            e = std::make_unique<AsteroidField>(el.pos, el.size, el.name, (ResourceType)el.resource,
                                                1000);
            break;
        case Proto::EntityKind::Gate:
            e = std::make_unique<JumpGate>(el.pos, el.size, el.name, el.dest);
            break;
        case Proto::EntityKind::Nebula:
            e = std::make_unique<Nebula>(el.pos, el.size, el.name);
            break;
        case Proto::EntityKind::Derelict:
            e = std::make_unique<Derelict>(el.pos, el.size, el.name, el.reward);
            break;
        default: return nullptr;
    }
    if (e)
    {
        e->SetId(el.id);
        e->SetPosition(el.pos);
    }
    return e;
}

// Find a snapshot entity by id (for buffer-based interpolation).
static const Proto::EntitySnapshot* FindEnt(const std::vector<Proto::EntitySnapshot>& v, int id)
{
    for (const Proto::EntitySnapshot& e : v)
        if (e.id == id)
            return &e;
    return nullptr;
}

// Angle interpolation along the shortest arc.
static float LerpAngleShort(float a, float b, float t)
{
    float d = b - a;
    while (d > PI)
        d -= 2.0f * PI;
    while (d < -PI)
        d += 2.0f * PI;
    return a + d * t;
}

// Reconciles the client's proxy world from the received data: statics are built from the layout
// (by id), dynamics (NPCs) from the snapshot fields; vanished ones are removed. Positions of
// non-own entities: single-player directly from the fresh snapshot; over the network — ENTITY
// INTERPOLATION (drawn "in the past", interpolating between two buffered snapshots) for
// smoothness. The client does NOT peek into the live sim_ (the world comes entirely from the
// network).
void Game::ReconcileClientWorld()
{
    // 1) Presence: create new proxies from the latest snapshot. Single-player we set the
    // position right away (the local snapshot is fresh every frame — no interpolation needed).
    for (const Proto::EntitySnapshot& es : snapshot_.entities)
    {
        Entity* proxy = nullptr;
        for (auto& e : clientWorld_)
            if (e->GetId() == es.id)
            {
                proxy = e.get();
                break;
            }

        if (proxy == nullptr)
        {
            std::unique_ptr<Entity> p;
            if (es.kind == Proto::EntityKind::PlayerShip)
            {
                // Another player (#4). A Ship rather than an NpcShip, so it describes
                // itself with the player glyph and carries the pilot's name; its stats do
                // not matter here -- everything drawn about it comes from the snapshot.
                auto other = std::make_unique<Ship>(es.pos, GetShipCatalog()[0].stats);
                other->SetHeading(es.heading);
                other->SetHull(es.hullFrac * other->GetMaxHull());
                other->SetPilotName(es.name);
                p = std::move(other);
            }
            else if (es.kind == Proto::EntityKind::Npc)
            {
                // NPC — snapshot dynamics: built from faction/role, hull for the indicator.
                auto n = std::make_unique<NpcShip>(es.pos, es.faction, (NpcRole)es.role,
                                                   std::vector<Vector2>{});
                n->SetHeading(es.heading);
                n->SetHull(es.hullFrac * n->GetMaxHull());
                p = std::move(n);
            }
            else
            {
                // Statics — from the system layout by id.
                auto itl = layoutById_.find(es.id);
                if (itl != layoutById_.end())
                    p = MakeProxyFromLayout(itl->second);
            }
            if (!p)
                continue;
            p->SetId(es.id);
            p->SetPosition(es.pos);
            clientWorld_.push_back(std::move(p));
            proxy = clientWorld_.back().get();
        }

        // Damage is not only a number in the target panel: the glyph dims with the hull
        // (#35). Refreshing it every snapshot rather than only at creation is what makes
        // a fight visible -- otherwise a ship stays as bright as it was when it appeared.
        if (proxy->GetKind() == EntityKind::Npc)
        {
            NpcShip* n = static_cast<NpcShip*>(proxy);
            n->SetHull(es.hullFrac * n->GetMaxHull());
        }
        else if (proxy->GetKind() == EntityKind::PlayerShip)
        {
            Ship* sh = static_cast<Ship*>(proxy);
            sh->SetHull(es.hullFrac * sh->GetMaxHull());
        }
    }

    // 2) Positions of non-own entities are taken "from the past", interpolating between two
    // buffer snapshots around renderTime. Smooths out snapshot jitter (the own
    // ship runs on prediction — it's not in clientWorld_).
    {
        double            rt = GetTime() - 0.1;  // render delay ~100 ms
        const InterpSnap* a = nullptr;
        const InterpSnap* b = nullptr;
        for (const InterpSnap& s : snapBuffer_)
        {
            if (s.t <= rt)
                a = &s;
            else
            {
                b = &s;
                break;
            }
        }
        for (auto& e : clientWorld_)
        {
            int                          id = e->GetId();
            const Proto::EntitySnapshot* ea = a ? FindEnt(a->ents, id) : nullptr;
            const Proto::EntitySnapshot* eb = b ? FindEnt(b->ents, id) : nullptr;
            Vector2                      pos;
            float                        heading;
            if (ea && eb && b->t > a->t)
            {
                float al = (float)((rt - a->t) / (b->t - a->t));
                pos = Vector2Lerp(ea->pos, eb->pos, al);
                heading = LerpAngleShort(ea->heading, eb->heading, al);
            }
            else if (eb)
            {
                pos = eb->pos;
                heading = eb->heading;
            }
            else if (ea)
            {
                pos = ea->pos;
                heading = ea->heading;
            }
            else  // not in the buffer (just created) — take from the fresh snapshot
            {
                const Proto::EntitySnapshot* cur = FindEnt(snapshot_.entities, id);
                if (cur == nullptr)
                    continue;
                pos = cur->pos;
                heading = cur->heading;
            }
            e->SetPosition(pos);
            if (e->GetKind() == EntityKind::Npc)
                static_cast<NpcShip*>(e.get())->SetHeading(heading);
            else if (e->GetKind() == EntityKind::PlayerShip)
                static_cast<Ship*>(e.get())->SetHeading(heading);  // other players turn too
        }
    }

    // 3) Remove proxies not present in the latest snapshot (the object vanished/was destroyed).
    // If the selected target vanished — clear the selection (otherwise selected_ would dangle on
    // a freed proxy: the ring would be drawn from dead memory, and cmd_.targetId too).
    clientWorld_.erase(std::remove_if(clientWorld_.begin(), clientWorld_.end(),
                                      [this](const std::unique_ptr<Entity>& e)
                                      {
                                          for (const auto& es : snapshot_.entities)
                                              if (es.id == e->GetId())
                                                  return false;
                                          if (selected_ == e.get())
                                              selected_ = nullptr;
                                          return true;
                                      }),
                       clientWorld_.end());
}

// Station by stable id (missions store an id, not a pointer). Searches the same place
// as FindEntityById: the clientWorld_ proxies built from the server's layout.
