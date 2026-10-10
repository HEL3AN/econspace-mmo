// Game — the client's mirror of server state.
//
// Everything that turns what arrives on the wire into something drawable: snapshots and
// layouts in, proxy entities and a reconciled prediction out. The client owns no
// authoritative state, so this is the whole of what it knows about the world.
//
// Part of the Game class; see Game.cpp.
#include "core/Game.h"
#include "core/Archetype.h"
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
#include "entities/Structure.h"
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
#include <set>

void Game::BuildNetworkBeams()
{
    beams_.clear();
    for (const FireEvent& f : snapshot_.fires)
    {
        // Relative to the one looking, which is what may colour the world: your own fire and
        // fire at you. Everyone else's is one colour, not their faction's (#117).
        Color c = f.fromPlayer       ? SKYBLUE
                  : f.targetIsPlayer ? ORANGE
                                     : Fade(Color{ 255, 220, 170, 255 }, 0.7f);
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

// The proxy for an entity the server has told us about, or nullptr.
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
    layout_.Reset(lay);
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

// Client: the system we are in changed while we were in it (#38). The layout takes the
// change, and every proxy it touches is rebuilt from the new description at once -- a
// removed one simply is not. The raw pointers into the old proxies are re-pointed by id in
// the same breath, as ApplyLayout clears them all, or they would dangle on the next frame.
void Game::ApplyLayoutDelta(const Proto::LayoutDelta& d)
{
    if (!layout_.Apply(d))
        return;  // another system's, or already in the layout we were sent

    std::set<int> touched(d.removed.begin(), d.removed.end());
    for (const Proto::EntityLayout& el : d.changed)
        touched.insert(el.id);
    for (const Proto::EntityLayout& el : d.added)
        touched.insert(el.id);  // a client that had it from a layout rebuilds it as described

    // Held by id across the rebuild: a wreck searched by somebody else is no reason to lose
    // the target you were flying to.
    auto      idOf = [](const Entity* e) { return e != nullptr ? e->GetId() : 0; };
    const int selectedId = idOf(selected_);
    const int nearbyId = idOf(nearbyStation_);
    const int dockedId = idOf(dockedStation_);
    const int miningId = idOf(miningBeamField_);

    clientWorld_.erase(std::remove_if(clientWorld_.begin(), clientWorld_.end(),
                                      [&touched](const std::unique_ptr<Entity>& e)
                                      { return touched.count(e->GetId()) > 0; }),
                       clientWorld_.end());
    for (int id : touched)
    {
        auto it = layout_.byId.find(id);
        if (it == layout_.byId.end())
            continue;  // removed
        if (std::unique_ptr<Entity> p = MakeProxyFromLayout(it->second))
            clientWorld_.push_back(std::move(p));
    }

    // The last snapshot was completed against the old layout. Brought up to date as well,
    // or the reconcile that runs before the next one would put a removed object back and
    // drop an added one for a frame.
    snapshot_.entities.erase(
        std::remove_if(snapshot_.entities.begin(), snapshot_.entities.end(),
                       [this, &touched](const Proto::EntitySnapshot& e)
                       { return touched.count(e.id) > 0 && layout_.byId.count(e.id) == 0; }),
        snapshot_.entities.end());
    Proto::CompleteFromLayout(snapshot_, layout_.byId);

    selected_ = FindEntityById(selectedId);
    nearbyStation_ = StationById(nearbyId);
    Entity* field = FindEntityById(miningId);
    miningBeamField_ = field != nullptr && field->GetKind() == EntityKind::Field
                           ? static_cast<AsteroidField*>(field)
                           : nullptr;
    dockedStation_ = StationById(dockedId);
    if (dockedId != 0 && dockedStation_ == nullptr)
        mode_ = GameMode::Flying;  // the station is gone; the snapshot says the same

    for (const Proto::EntityLayout& el : d.added)
        if (!el.owner.empty())
            FlashMessage(TextFormat("%s built %s", el.owner.c_str(), el.name.c_str()));
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
                FlashMessage(TextFormat("Server protocol v%d, client v%d: update needed", ver,
                                        Proto::PROTO_VERSION));
            }
            continue;
        }

        std::string type = Proto::MessageType(msg);
        if (type == "universe")
        {
            // The galaxy index (#206): replaced whole, because it is sent whole -- at login
            // and again whenever the server's index changes.
            WorldLoader::Universe u;
            if (Proto::DecodeUniverse(msg, u))
                universe_ = std::move(u);
        }
        else if (type == "layout")
        {
            Proto::SystemLayout lay;
            if (Proto::DecodeLayout(msg, lay))
                ApplyLayout(lay);
        }
        else if (type == "ldelta")
        {
            Proto::LayoutDelta d;
            if (Proto::DecodeLayoutDelta(msg, d))
                ApplyLayoutDelta(d);
        }
        else if (type == "galaxy")
        {
            Proto::DecodeGalaxy(msg, galaxyState_);  // per-system stats for the map
        }
        else if (type == "bye")
        {
            // Said out loud rather than left as a silent disconnect: being displaced from
            // your own account looks exactly like a crash otherwise (#105).
            Proto::Bye b;
            if (Proto::DecodeBye(msg, b))
            {
                TraceLog(LOG_WARNING, "Server ended the session: %s", b.reason.c_str());
                FlashMessage(b.reason);
            }
        }
        else if (type == "snap")
        {
            Proto::Snapshot s;
            if (!Proto::DecodeSnapshot(msg, s))
                continue;
            // The static half of each entity is not on the wire; it is in the layout we
            // were sent on entry (#16).
            Proto::CompleteFromLayout(s, layout_.byId);
            ApplyTradeAcks(s);                   // say what was sold; the money is the server's
            for (const Ev::Event& e : s.events)  // server journal (#29)
                FlashMessage(e.text);
            // Every snapshot is a sample of where things were, so every one goes into the
            // interpolation buffer -- including the ones that arrived in the same frame as a
            // newer one and are otherwise dropped. Each is stamped with the server's time,
            // not the time it happened to arrive: arrivals bunch and stretch with the
            // network, and interpolating by them made everything nearby speed up and slow
            // down -- a smoothed hitch every couple of seconds.
            snapBuffer_.push_back({ s.time, s.entities });
            incoming = std::move(s);
            gotSnap = true;
        }
    }
    if (gotSnap)
    {
        snapshot_ = std::move(incoming);
        worldClock_.Observe(snapshot_.time, GetTime());
        // A buffer of snapshots with server timestamps — for interpolating non-own
        // entities (entity interpolation, Gambetta). We draw them "in the past", smoothing
        // out snapshot jitter. The own ship is NOT touched by interpolation (prediction).
        // Kept in server time, half a second of it; an older snapshot arriving late is
        // simply out of order and is dropped by the sort below.
        std::stable_sort(snapBuffer_.begin(), snapBuffer_.end(),
                         [](const InterpSnap& x, const InterpSnap& y) { return x.t < y.t; });
        double cutoff = snapBuffer_.back().t - 0.5;  // keep ~0.5 s of history
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
        playerShip_->ApplyNavView(p.warpPhase, p.warpAlign, p.warpTarget, p.warpDrop, p.warpViaSet,
                                  p.warpVia, p.autopilot, p.apTarget, p.apStop, p.holdMode,
                                  p.holdTargetId, p.holdRange);
        // Toggles (stabilizer/mining) are server-authoritative: restore from the snapshot
        // BEFORE replay, otherwise unacked toggle commands would flicker during replay
        // (like warp). Unacked toggles "carry through" via prediction below.
        playerShip_->SetStabilizerOn(p.stabilizer);
        playerShip_->SetMiningOn(p.mining);
        for (const Proto::Command& c : pendingInputs_)
            Sim::StepPlayerShip(*playerShip_, c, 1.0f, SIM_DT, HoldTarget());
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
        // proxy exists). No flash of our own: the server's journal already says "Docked at X",
        // and saying it here as well showed it twice (#227).
        if (p.docked && mode_ == GameMode::Flying)
        {
            if (Station* s = StationById(p.dockedStationId))
            {
                mode_ = GameMode::Docked;
                dockedStation_ = s;
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
        {
            auto p = std::make_unique<Planet>(el.orbitRadius, 0.0f, 0.0f, el.size, el.color,
                                              (ResourceType)el.resource, (PlanetType)el.subType);
            p->SetName(el.name);  // the server's name for it (#259)
            e = std::move(p);
            break;
        }
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
        {
            auto d = std::make_unique<Derelict>(el.pos, el.size, el.name, el.reward);
            if (el.looted)
                d->SetLooted();  // dimmer, and no longer offering to be investigated (#38)
            e = std::move(d);
            break;
        }
        case Proto::EntityKind::Structure:
        {
            // Built as what it will be, then dressed as a site until its moment (#39). The
            // server says when that moment passed with a delta; the look follows it.
            auto t = std::make_unique<Structure>(el.pos, el.size, el.name, el.archetype);
            t->StartBuilding(el.startedAt, el.completesAt);
            t->SetExpiresAt(el.expiresAt);
            e = std::move(t);
            break;
        }
        default: return nullptr;
    }
    if (e)
    {
        e->SetId(el.id);
        e->SetPosition(el.pos);
        e->SetOwner(el.owner);
        // The archetype the server built it as (#195), which may not be its kind's usual
        // one: a leviathan among the derelicts (#142, #211). Without this the client would
        // draw every rare find as the ordinary thing of its kind.
        if (el.kind != Proto::EntityKind::Structure && !el.archetype.empty() &&
            e->GetArchetype() != nullptr && e->GetArchetype()->id != el.archetype)
        {
            const Archetype* a = Archetypes::Find(el.archetype);
            if (a != nullptr && a->kind == e->GetKind())
                e->SetArchetype(el.archetype);
        }
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
// non-own entities: ENTITY INTERPOLATION -- drawn "in the past", interpolating between two
// buffered snapshots, which is what lets the server describe the world twenty times a
// second instead of sixty (#16). The world comes entirely from the network; there is no
// local simulation to peek into.
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
                auto itl = layout_.byId.find(es.id);
                if (itl != layout_.byId.end())
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
        // Two snapshot intervals in the past, on the server's clock (#192), eased so it
        // never runs backwards or jumps with one late packet.
        double            rt = worldClock_.Now(GetTime()) - 0.1;
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

// What this account owns, as the server last said (#5). Not remembered locally: a client
// that kept its own list would keep showing a ship the server refused to sell it.
bool Game::OwnsShip(int catalogIndex) const
{
    for (int i : snapshot_.player.ownedShips)
        if (i == catalogIndex)
            return true;
    return catalogIndex == 0;  // before the first snapshot, only the starter
}

// See Game.h: the position of whatever the ship is holding station on, from this client's
// own proxies. The server answers the same question from the live entity, and the two
// differ by the render delay -- deliberately, and only for a control loop that cannot see
// the difference (#157).
//
// The velocity, which a follow flies (#298), is the difference between the two newest
// snapshots that have the object: what the server said it did, rather than a derivative of
// the interpolation, which is piecewise and steps at every snapshot.
const Sim::HoldTarget* Game::HoldTarget() const
{
    if (!playerShip_ || playerShip_->GetHoldMode() == HoldMode::None)
        return nullptr;
    const int id = playerShip_->GetHoldTargetId();
    if (id == 0)
        return nullptr;
    for (const auto& e : clientWorld_)
        if (e->GetId() == id)
        {
            holdTarget_.pos = e->GetPosition();
            holdTarget_.vel = { 0.0f, 0.0f };
            const Proto::EntitySnapshot* newer = nullptr;
            double                       newerT = 0.0;
            for (auto it = snapBuffer_.rbegin(); it != snapBuffer_.rend(); ++it)
            {
                const Proto::EntitySnapshot* es = FindEnt(it->ents, id);
                if (es == nullptr)
                    continue;
                if (newer == nullptr)
                {
                    newer = es;
                    newerT = it->t;
                    continue;
                }
                const float span = (float)(newerT - it->t);
                if (span > 0.0f)
                    holdTarget_.vel = { (newer->pos.x - es->pos.x) / span,
                                        (newer->pos.y - es->pos.y) / span };
                break;
            }
            return &holdTarget_;
        }
    return nullptr;
}
