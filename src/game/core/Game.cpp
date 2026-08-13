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

static const float DOCKING_RANGE = 90.0f;  // margin added to the station radius for docking

// Menu bar (Neocom): strip width and button geometry.
static const float MENU_BAR_W = 46.0f;
static const float MENU_BTN = 36.0f;
static const float MENU_STEP = 46.0f;
static const float MENU_TOP = 12.0f;

// Player combat and mining are now server-side (Simulation::StepPlayerFire/StepPlayerMining);
// the weapon range for rendering the targeting circle is Sim::PLAYER_WEAPON_RANGE.

// Simulation step: fixed, separate from the render rate (determinism,
// server-friendliness). 1/60 matches the target FPS — behaves as before.
static const float SIM_DT = 1.0f / 60.0f;

Game::Game(std::unique_ptr<Net::TcpConnection> conn) : player_(500.0), netConn_(std::move(conn))
{
    // The connection is established by main() before the window opens, so it is
    // always live here — there is no offline mode to degrade into.
    clientLink_ = netConn_.get();

    SetConfigFlags(FLAG_WINDOW_RESIZABLE);  // window can be resized by its edge
    InitWindow(screenWidth_, screenHeight_, "EconSpace");
    SetWindowMinSize(960, 600);
    SetTargetFPS(60);
    Ui::LoadAssets();

    // The authoritative ship lives on the server. This one is the client's prediction of
    // it: the same Ship type, stepped with the same Sim::StepPlayerShip, corrected by
    // every snapshot. Its starting position is a placeholder until the first snapshot.
    playerShip_ = std::make_unique<Ship>(Vector2{ 0.0f, 3000.0f }, GetShipCatalog()[0].stats);

    camera_ = {};
    camera_.target = playerShip_->GetPosition();
    camera_.offset = { screenWidth_ / 2.0f, screenHeight_ / 2.0f };
    camera_.rotation = 0.0f;
    camera_.zoom = 1.0f;

    // Galaxy index and starting system. In a dev build we read the source data/
    // (the same path the editor writes to), otherwise a copy next to the exe.
#ifdef GAME_DATA_DIR
    dataDir_ = GAME_DATA_DIR;
#else
    dataDir_ = std::string(GetApplicationDirectory()) + "data/";
#endif
    Factions::Load(dataDir_ + "factions.json");  // faction properties/relations
    universe_ = WorldLoader::LoadUniverse(dataDir_ + "universe.json");

    // The starting ship (index 0) is already owned by the player.
    ownedShips_.assign(GetShipCatalog().size(), false);
    ownedShips_[0] = true;

    // No system is loaded here: which system we are in, and everything in it, arrives
    // from the server as a SystemLayout followed by snapshots (ApplyLayout).

    SetupWindows();

    // Parallax-background stars: base positions in a large tile, varied depth
    // and brightness. The tile is larger than any window — stars wrap around it.
    for (int i = 0; i < 220; i++)
    {
        BgStar s;
        s.base = { (float)GetRandomValue(0, 2560), (float)GetRandomValue(0, 1440) };
        s.depth = GetRandomValue(15, 70) / 100.0f;  // 0.15..0.70
        s.shade = (unsigned char)GetRandomValue(70, 180);
        bgStars_.push_back(s);
    }
}

Game::~Game()
{
    netConn_.reset();  // close the socket; main() unloads winsock after we're gone
    Tex::Unload();
    Ui::UnloadAssets();
    CloseWindow();
}

void Game::Run()
{
    while (!WindowShouldClose())
    {
        float dt = GetFrameTime();

        // Pick up the current window size (it may have been resized with the mouse).
        screenWidth_ = GetScreenWidth();
        screenHeight_ = GetScreenHeight();
        camera_.offset = { screenWidth_ / 2.0f, screenHeight_ / 2.0f };

        // Debug commands (work in any mode).
        if (IsKeyPressed(KEY_F11))
            ToggleBorderlessWindowed();
        if (IsKeyPressed(KEY_F1))  // account is on the server — credit via command
        {
            Proto::Command dc;
            dc.debugMoney = true;
            clientLink_->Send(Proto::EncodeCommand(dc));
        }
        if (flashTimer_ > 0.0f)
            flashTimer_ -= dt;

        // Input (edge triggers, UI, clicks) — once per frame. Continuous ship
        // control is also set here and read on every simulation step.
        if (mode_ == GameMode::Flying)
            HandleInput(dt);

        // The simulation runs at a fixed step, separate from the render rate.
        // The accumulator is clamped against the "spiral of death" on frame drops.
        // The world itself is computed by econserver; the client only sends commands,
        // predicts its own ship, and draws received snapshots.
        simAccumulator_ += dt;
        if (simAccumulator_ > 0.25f)
            simAccumulator_ = 0.25f;
        while (simAccumulator_ >= SIM_DT)
        {
            if (mode_ == GameMode::Flying)
            {
                // CLIENT-SIDE PREDICTION of the own ship's movement. Number the input,
                // push it into the unacked buffer, send it to the server, and immediately apply the
                // same StepPlayerShip the server uses (pilotBonus=1 as on the server). The snapshot
                // then replays the buffer over the authoritative state (BuildClientSnapshot). The
                // world (NPCs) is not simulated — it arrives via snapshots.
                cmd_.seq = (int)++inputSeq_;
                pendingInputs_.push_back(cmd_);
                if (pendingInputs_.size() > 256)  // guard against growth if the server stalls
                    pendingInputs_.erase(pendingInputs_.begin());
                clientLink_->Send(Proto::EncodeCommand(cmd_));
                Sim::StepPlayerShip(*playerShip_, cmd_, 1.0f, SIM_DT);
                // One-shot intents applied/sent this tick — clear them (axes are held).
                cmd_.toggleStabilizer = cmd_.toggleMining = cmd_.toggleWeapon = false;
                cmd_.dock = cmd_.undock = false;
                cmd_.navMode = 0;
                cmd_.jumpGateId = cmd_.lootId = 0;
            }
            simAccumulator_ -= SIM_DT;
        }

        // Client↔server boundary: the snapshot (world + market + player) comes from
        // econserver and the client picks it up here. We build the snapshot both in
        // flight and while docked; proxy-world reconciliation only in flight.
        BuildClientSnapshot();
        if (mode_ == GameMode::Flying)
            ReconcileClientWorld();

        // The camera follows the ship (position synced from the snapshot). In warp the
        // speed is too high for a smooth catch-up — center hard so the ship doesn't
        // leave the screen.
        if (mode_ == GameMode::Flying)
        {
            if (playerShip_->IsWarping() || cameraSnap_)
            {
                camera_.target = playerShip_->GetPosition();
                cameraSnap_ = false;
            }
            else
            {
                float follow = 1.0f - expf(-8.0f * dt);
                camera_.target = Vector2Lerp(camera_.target, playerShip_->GetPosition(), follow);
            }
            BuildNetworkBeams();  // combat beams — from the snapshot (server computes combat)

            // Mining beam: the server reports mining in the snapshot — draw a beam to the nearest
            // field within mining range (visual only; extraction is server-side). Radius as in the
            // core (40).
            miningBeamField_ = nullptr;
            if (snapshot_.player.mining)
            {
                Vector2 pp = playerShip_->GetPosition();
                for (auto& e : clientWorld_)
                {
                    AsteroidField* f = dynamic_cast<AsteroidField*>(e.get());
                    if (f == nullptr)
                        continue;
                    float dx = f->GetPosition().x - pp.x;
                    float dy = f->GetPosition().y - pp.y;
                    if (sqrtf(dx * dx + dy * dy) <= f->GetSize() + 40.0f)
                    {
                        miningBeamField_ = f;
                        break;
                    }
                }
            }
        }

        BeginDrawing();
        ClearBackground(BLACK);
        if (mode_ == GameMode::Flying)
        {
            DrawWorld();
            DrawHud();
        }
        else
        {
            DrawStationScreen();
        }
        EndDrawing();
    }
}

void Game::HandleInput(float dt)
{
    (void)dt;

    // The context menu is handled first — it sits above the whole UI.
    // We call all handlers explicitly so short-circuit || doesn't skip them.
    bool overMenu = contextMenu_.Update();
    bool overWin = !galaxyMapOpen_ && HandleWindows();  // the map is modal
    bool overBar = HandleMenuBar();
    bool overUi = overMenu || overWin || overBar || galaxyMapOpen_;

    // Combat/mining/docking intents go into the command (applied by the simulation
    // step, accounting for warp etc.), rather than calling ship methods directly.
    if (IsKeyPressed(KEY_X))
        cmd_.toggleStabilizer = true;

    if (IsKeyPressed(KEY_M))
        cmd_.toggleMining = true;

    if (IsKeyPressed(KEY_F))
    {
        cmd_.toggleWeapon = true;
        weaponOn_ = !weaponOn_;  // optimistic: the snapshot confirms it
    }

    if (IsKeyPressed(KEY_T))
        targetWin_->Toggle();

    if (IsKeyPressed(KEY_O))
        overviewWin_->Toggle();

    if (IsKeyPressed(KEY_R))
        radarWin_->Toggle();

    if (IsKeyPressed(KEY_J))
        missionsWin_->Toggle();

    if (IsKeyPressed(KEY_G))
        galaxyMapOpen_ = !galaxyMapOpen_;
    if (galaxyMapOpen_ && IsKeyPressed(KEY_ESCAPE))
        galaxyMapOpen_ = false;

    float wheel = GetMouseWheelMove();
    if (wheel != 0.0f && !overUi)  // over a window, the wheel goes to the window (e.g. radar)
        camera_.zoom = Clamp(camera_.zoom * (1.0f + wheel * 0.12f), 0.04f, 2.5f);

    // Held control axes: W — thrust, S — brake, A/D — turn. Written into the
    // command; the server applies it to the ship in the tick (Simulation::StepPlayerShip).
    cmd_.thrust = IsKeyDown(KEY_W) || IsKeyDown(KEY_UP);
    cmd_.brake = IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN);
    cmd_.turn = 0.0f;
    if (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT))
        cmd_.turn -= 1.0f;
    if (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT))
        cmd_.turn += 1.0f;

    // Combat target — the selected object (by id). The server fires at it in StepPlayerFire (over
    // the network this is the only target source; single-player, ResolveCombat reads selected_
    // directly).
    cmd_.targetId = selected_ != nullptr ? selected_->GetId() : 0;

    // Left click — select the object under the cursor (unless over the UI). Search the
    // snapshot (M4c); the action applies to the live entity by id.
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !overUi)
    {
        Vector2 worldMouse = GetScreenToWorld2D(GetMousePosition(), camera_);
        selected_ = nullptr;
        for (const auto& e : snapshot_.entities)
            if (CheckCollisionPointCircle(worldMouse, e.pos, e.size))
            {
                selected_ = FindEntityById(e.id);
                break;
            }
        // Selecting an object opens the target window.
        if (selected_ != nullptr)
            targetWin_->SetOpen(true);
    }

    // Right click: on an object — context menu; on empty space — autopilot.
    if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && !overUi)
    {
        Vector2 worldMouse = GetScreenToWorld2D(GetMousePosition(), camera_);
        int     hitId = 0;
        for (const auto& e : snapshot_.entities)
            if (CheckCollisionPointCircle(worldMouse, e.pos, e.size))
            {
                hitId = e.id;
                break;
            }
        Entity* target = FindEntityById(hitId);
        if (target != nullptr)
            OpenContextMenu(target);
        else
            OpenContextMenuAt(worldMouse);
    }

    // Nearest station within docking range; E — dock. Single-player, detection uses
    // the live sim_ (a live Station* is needed for missions/Dock); over the network, the
    // clientWorld_ proxies (the client has no live world). Docking itself over the network is
    // server-authoritative: we send cmd_.dock, the server confirms via snapshot (see
    // BuildClientSnapshot).
    nearbyStation_ = nullptr;
    {
        const auto& src = clientWorld_;
        for (auto& e : src)
        {
            Station* st = dynamic_cast<Station*>(e.get());
            if (st == nullptr)
                continue;

            float dx = st->GetPosition().x - playerShip_->GetPosition().x;
            float dy = st->GetPosition().y - playerShip_->GetPosition().y;
            if (sqrtf(dx * dx + dy * dy) <= st->GetSize() + DOCKING_RANGE)
            {
                nearbyStation_ = st;
                break;
            }
        }
        if (playerShip_->IsWarping())
            nearbyStation_ = nullptr;  // docking is unavailable while warping
        if (nearbyStation_ != nullptr && IsKeyPressed(KEY_E))
            cmd_.dock = true;
    }
}

void Game::Undock()
{
    // Undocking is server-authoritative: send the order, the server clears the dock and
    // confirms via snapshot. We leave optimistically so the response feels instant.
    Proto::Command c;
    c.undock = true;
    clientLink_->Send(Proto::EncodeCommand(c));
    mode_ = GameMode::Flying;
    dockedStation_ = nullptr;
}

// Network: combat beams from the snapshot (server computes combat). Own shot — blue, shot at the
// player — orange, others — the shooter's faction color. Ephemeral (rebuilt each frame).
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
const WorldLoader::SystemInfo* Game::CurrentSystemInfo() const
{
    // Over the network the current system comes from the server snapshot (the local sim_ isn't
    // activated on jumps, its ActiveId() would be stuck on the start system). The system list
    // (Universe) is static.
    const std::string& active = snapshot_.systemId;
    for (const auto& s : universe_.systems)
        if (s.id == active)
            return &s;
    return nullptr;
}

// Whether a faction is hostile to the player: pirates always, being wanted by the faction, or a low
// reputation tier. Computed only from faction + account — works both for a snapshot (which
// only has the entity's faction) and for a live NPC.
bool Game::HostileToPlayerFaction(FactionId f) const
{
    if (f == FactionId::Pirates)
        return true;
    if (player_.IsWanted(f))
        return true;
    RepTier t = Factions::TierOf(player_.GetReputation(f));
    return t == RepTier::Hostile || t == RepTier::Hated;
}

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
    weaponOn_ = false;

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
            ApplyTradeAcks(s);                       // credit sales revenue (client account)
            for (const std::string& m : s.messages)  // server notifications (M4f-4)
                FlashMessage(m);
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
        p.weaponOn = weaponOn_;  // client-side weapon indicator

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
            if (Station* s = dynamic_cast<Station*>(FindEntityById(p.dockedStationId)))
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
            if (es.kind == Proto::EntityKind::Npc)
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
            if (NpcShip* n = dynamic_cast<NpcShip*>(e.get()))
                n->SetHeading(heading);
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
Station* Game::StationById(int id) const
{
    return dynamic_cast<Station*>(FindEntityById(id));
}

void Game::FlashMessage(const std::string& msg)
{
    flashMsg_ = msg;
    flashTimer_ = 2.5f;
}

// Parallax background: stars in screen coordinates, offset from the camera position
// proportionally to depth; farther layers move slower than nearer ones.
void Game::DrawStarfield()
{
    const float tileW = 2560.0f, tileH = 1440.0f;
    for (const BgStar& s : bgStars_)
    {
        float x = fmodf(s.base.x - camera_.target.x * s.depth, tileW);
        float y = fmodf(s.base.y - camera_.target.y * s.depth, tileH);
        if (x < 0)
            x += tileW;
        if (y < 0)
            y += tileH;
        if (x <= screenWidth_ && y <= screenHeight_)
            DrawPixel((int)x, (int)y, Color{ s.shade, s.shade, s.shade, 255 });
    }
}

void Game::DrawWorld()
{
    DrawStarfield();

    BeginMode2D(camera_);

    // System boundary — a faint ring at the radius limit.
    DrawCircleLines(0, 0, World::SYSTEM_RADIUS, Fade(Ui::PANEL_BORDER, 0.5f));

    // The world is drawn from the client proxies (reconciled from the snapshot), not from
    // the server's live objects (M4c). The proxies reuse the entities' native Draw().
    for (const auto& e : clientWorld_)
        e->Draw();

    // Destination-station markers for active delivery missions. We draw them only if
    // the destination station is in the CURRENT system (by id), and at its rendered
    // position — otherwise after a jump the marker would "hang" at the coordinates of a station
    // from another system.
    const auto& renderedWorld = clientWorld_;
    for (const Mission& m : missions_.Active())
    {
        if (m.type != MissionType::Delivery || m.destStationId == 0)
            continue;
        for (const auto& e : renderedWorld)
        {
            Station* st = dynamic_cast<Station*>(e.get());
            if (st == nullptr || st->GetId() != m.destStationId)
                continue;
            Vector2 p = st->GetPosition();
            float   r = st->GetSize() + 16.0f;
            DrawCircleLines(p.x, p.y, r, GOLD);
            DrawCircleLines(p.x, p.y, r + 4.0f, Fade(GOLD, 0.4f));
            break;
        }
    }

    // The selected target's ring — at the object's rendered position, not the
    // snapshot: over the network the body is rendered via interpolation "in the past", and a ring
    // from the fresh snapshot would run ahead. selected_ is a proxy (network) or a live object
    // (single-player), its position matches what's drawn.
    if (selected_ != nullptr)
        DrawCircleLines(selected_->GetPosition().x, selected_->GetPosition().y,
                        selected_->GetSize() + 10.0f, WHITE);

    if (playerShip_->IsAutopilotOn())
    {
        Vector2 t = playerShip_->GetAutopilotTarget();
        DrawCircleLines(t.x, t.y, 14.0f, GREEN);
        DrawLineEx(playerShip_->GetPosition(), t, 1.0f, Fade(GREEN, 0.4f));
    }

    // Mining beam to the field.
    if (miningBeamField_ != nullptr)
    {
        DrawLineEx(playerShip_->GetPosition(), miningBeamField_->GetPosition(), 3.0f,
                   Fade(ORANGE, 0.7f));
    }

    // Weapon range.
    if (weaponOn_)
    {
        DrawCircleLines(playerShip_->GetPosition().x, playerShip_->GetPosition().y,
                        Sim::PLAYER_WEAPON_RANGE, Fade(SKYBLUE, 0.15f));
    }

    // Weapon beams for this frame.
    for (const Beam& b : beams_)
        DrawLineEx(b.a, b.b, 2.5f, b.color);

    playerShip_->Draw();

    // Ship marker — only at far zoom, when the sprite collapses to a
    // dot. Semi-transparent "ping" rings spread out from the ship and fade;
    // the size in screen pixels is divided by zoom to stay constant.
    if (camera_.zoom < 0.5f)
    {
        float   zoomFade = Clamp((0.5f - camera_.zoom) / 0.46f, 0.0f, 1.0f);
        float   t = (float)GetTime();
        Vector2 sp = playerShip_->GetPosition();

        // Two phase-shifted rings — a continuous soft ripple.
        for (int k = 0; k < 2; k++)
        {
            float phase = fmodf(t * 0.7f + k * 0.5f, 1.0f);
            float rWorld = (4.0f + phase * 26.0f) / camera_.zoom;
            float alpha = (1.0f - phase) * 0.30f * zoomFade;
            DrawCircleLines(sp.x, sp.y, rWorld, Fade(GREEN, alpha));
        }
    }

    EndMode2D();
}

void Game::SetupWindows()
{
    // Create windows with temporary bounds; ResetWindowLayout arranges them.
    Rectangle stub{ 0.0f, 0.0f, 10.0f, 10.0f };

    windows_.push_back(std::make_unique<Window>("STATUS", stub, true));
    statusWin_ = windows_.back().get();
    statusWin_->SetContent([this](Rectangle a) { DrawStatusContent(a); });

    windows_.push_back(std::make_unique<Window>("TARGET", stub, false));
    targetWin_ = windows_.back().get();
    targetWin_->SetContent([this](Rectangle a) { DrawTargetContent(a); });

    windows_.push_back(std::make_unique<Window>("OVERVIEW", stub, false));
    overviewWin_ = windows_.back().get();
    overviewWin_->SetContent([this](Rectangle a) { DrawOverviewContent(a); });

    windows_.push_back(std::make_unique<Window>("RADAR", stub, false));
    radarWin_ = windows_.back().get();
    radarWin_->SetContent([this](Rectangle a) { DrawRadarContent(a); });

    windows_.push_back(std::make_unique<Window>("MISSIONS", stub, false));
    missionsWin_ = windows_.back().get();
    missionsWin_->SetContent([this](Rectangle a) { DrawMissionsContent(a); });

    windows_.push_back(std::make_unique<Window>("SETTINGS", stub, false));
    settingsWin_ = windows_.back().get();
    settingsWin_->SetContent([this](Rectangle a) { DrawSettingsContent(a); });

    ResetWindowLayout();
}

// Arranges windows at their default positions (right-side ones relative to the current window
// width).
void Game::ResetWindowLayout()
{
    float rx = (float)(screenWidth_ - 280);
    statusWin_->SetBounds(Rectangle{ 56.0f, 16.0f, 264.0f, 312.0f });
    targetWin_->SetBounds(Rectangle{ rx, 16.0f, 264.0f, 196.0f });
    overviewWin_->SetBounds(Rectangle{ rx, 224.0f, 264.0f, 400.0f });
    radarWin_->SetBounds(Rectangle{ 56.0f, 344.0f, 264.0f, 288.0f });
    missionsWin_->SetBounds(Rectangle{ rx, 360.0f, 264.0f, 264.0f });
    settingsWin_->SetBounds(Rectangle{ screenWidth_ / 2.0f - 150.0f, 100.0f, 300.0f, 300.0f });
}

// Changes the window size; if fullscreen mode is active — exits it first.
void Game::ApplyResolution(int w, int h)
{
    if (IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE))
        ToggleBorderlessWindowed();
    SetWindowSize(w, h);
    int mon = GetCurrentMonitor();
    SetWindowPosition((GetMonitorWidth(mon) - w) / 2, (GetMonitorHeight(mon) - h) / 2);

    // The window layout depends on the screen size — reset it for the new
    // resolution (right-side windows are anchored to the width). We update the sizes ahead of time,
    // since GetScreenWidth would only pick them up next frame.
    screenWidth_ = w;
    screenHeight_ = h;
    ResetWindowLayout();
}

// Settings window content: resolution, fullscreen mode, layout reset.
void Game::DrawSettingsContent(Rectangle area)
{
    struct Res
    {
        int w, h;
    };
    static const Res modes[] = { { 1280, 720 }, { 1600, 900 }, { 1920, 1080 } };

    int x = (int)area.x;
    int y = (int)area.y;

    Ui::Text("RESOLUTION", x, y, 14, Ui::TEXT_DIM);
    y += 22;
    for (const Res& r : modes)
    {
        bool   current = (screenWidth_ == r.w && screenHeight_ == r.h);
        Button btn(Rectangle{ area.x, (float)y, area.width, 30.0f },
                   TextFormat("%d x %d%s", r.w, r.h, current ? "   *" : ""),
                   [this, r]() { ApplyResolution(r.w, r.h); });
        btn.Process();
        y += 36;
    }

    y += 10;
    Ui::Text("DISPLAY", x, y, 14, Ui::TEXT_DIM);
    y += 22;
    bool   fs = IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE);
    Button fsBtn(Rectangle{ area.x, (float)y, area.width, 30.0f },
                 fs ? "Fullscreen: ON" : "Fullscreen: off", []() { ToggleBorderlessWindowed(); });
    fsBtn.Process();
    y += 42;

    Button resetBtn(Rectangle{ area.x, (float)y, area.width, 30.0f }, "Reset window layout",
                    [this]() { ResetWindowLayout(); });
    resetBtn.Process();
}

// Radar minimap: a free view of the system (does not follow the player). Inside the window
// you can pan (LMB drag), zoom (wheel), select an object (click), and open the context menu (RMB).
// The button in the top right centers the radar on the ship.
void Game::DrawRadarContent(Rectangle area)
{
    float     side = fminf(area.width, area.height);
    Rectangle r{ area.x + (area.width - side) / 2.0f, area.y, side, side };
    DrawRectangleRec(r, Fade(BLACK, 0.4f));
    DrawRectangleLinesEx(r, 1.0f, Fade(Ui::PANEL_BORDER, 0.6f));

    Vector2 sp = snapshot_.player.pos;  // M4c: the radar reads the snapshot
    if (!radarInit_)                    // on first display, center on the player
    {
        radarCenter_ = sp;
        radarInit_ = true;
    }

    // Base scale — from the system's extent (stable while panning).
    float maxR = 600.0f;
    for (const auto& e : snapshot_.entities)
        maxR = fmaxf(maxR, sqrtf(e.pos.x * e.pos.x + e.pos.y * e.pos.y));
    maxR *= 1.1f;

    Vector2 c{ r.x + side / 2.0f, r.y + side / 2.0f };
    float   scale = (side / 2.0f) / maxR * radarZoom_;

    auto toRadar = [&](Vector2 w) -> Vector2
    { return { c.x + (w.x - radarCenter_.x) * scale, c.y + (w.y - radarCenter_.y) * scale }; };
    auto toWorld = [&](Vector2 s) -> Vector2
    { return { radarCenter_.x + (s.x - c.x) / scale, radarCenter_.y + (s.y - c.y) / scale }; };

    Vector2 m = GetMousePosition();
    bool    overR = CheckCollisionPointRec(m, r);

    // Center-on-player button — top right of the radar.
    Rectangle recBtn{ r.x + r.width - 24.0f, r.y + 6.0f, 18.0f, 18.0f };
    bool      overRec = CheckCollisionPointRec(m, recBtn);

    // Wheel zoom.
    if (overR)
    {
        float wheel = GetMouseWheelMove();
        if (wheel != 0.0f)
            radarZoom_ = Clamp(radarZoom_ * (1.0f + wheel * 0.12f), 0.25f, 12.0f);
    }

    // LMB: the center button takes priority, otherwise pan/select.
    if (overRec && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
        radarCenter_ = sp;
    }
    else if (overR && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
        radarDragging_ = true;
        radarDragMoved_ = false;
        radarDragLast_ = m;
        radarPressPos_ = m;
    }
    if (radarDragging_)
    {
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT))
        {
            radarCenter_.x -= (m.x - radarDragLast_.x) / scale;
            radarCenter_.y -= (m.y - radarDragLast_.y) / scale;
            radarDragLast_ = m;
            if (fabsf(m.x - radarPressPos_.x) + fabsf(m.y - radarPressPos_.y) > 4.0f)
                radarDragMoved_ = true;
        }
        else
        {
            radarDragging_ = false;
            if (!radarDragMoved_)  // it was a click — select the object under the cursor
            {
                for (const auto& e : snapshot_.entities)
                    if (CheckCollisionPointCircle(m, toRadar(e.pos), 7.0f))
                    {
                        selected_ = FindEntityById(e.id);
                        if (selected_ != nullptr)
                            targetWin_->SetOpen(true);
                        break;
                    }
            }
        }
    }

    // RMB: menu on the object under the cursor, or on a map point.
    if (overR && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT))
    {
        int hitId = 0;
        for (const auto& e : snapshot_.entities)
            if (CheckCollisionPointCircle(m, toRadar(e.pos), 7.0f))
            {
                hitId = e.id;
                break;
            }
        Entity* hit = FindEntityById(hitId);
        if (hit != nullptr)
            OpenContextMenu(hit);
        else
            OpenContextMenuAt(toWorld(m));
    }

    int selId = selected_ != nullptr ? selected_->GetId() : 0;
    BeginScissorMode((int)r.x, (int)r.y, (int)r.width, (int)r.height);
    for (const auto& e : snapshot_.entities)
    {
        Vector2 p = toRadar(e.pos);
        if (!CheckCollisionPointRec(p, r))
            continue;  // outside the radar — don't draw

        Color col = GRAY;
        switch (e.kind)
        {
            case Proto::EntityKind::Star: col = GOLD; break;
            case Proto::EntityKind::Planet: col = SKYBLUE; break;
            case Proto::EntityKind::Station: col = Ui::ACCENT; break;
            case Proto::EntityKind::Field: col = ORANGE; break;
            case Proto::EntityKind::Nebula: col = Color{ 150, 90, 200, 255 }; break;
            case Proto::EntityKind::Derelict: col = Color{ 130, 130, 120, 255 }; break;
            case Proto::EntityKind::Gate: col = Color{ 90, 200, 210, 255 }; break;
            case Proto::EntityKind::Npc: col = FactionColor(e.faction); break;
            default: col = GRAY; break;
        }

        DrawCircleV(p, 3.0f, col);
        if (e.id != 0 && e.id == selId)
            DrawCircleLines((int)p.x, (int)p.y, 6.0f, WHITE);
    }

    // Player ship.
    Vector2 pp = toRadar(sp);
    if (CheckCollisionPointRec(pp, r))
    {
        DrawCircleV(pp, 3.5f, GREEN);
        DrawCircleLines((int)pp.x, (int)pp.y, 6.0f, Fade(GREEN, 0.6f));
    }
    EndScissorMode();

    // Center-on-player button (over the blips): frame + crosshair.
    DrawRectangleRec(recBtn, overRec ? Fade(Ui::ACCENT, 0.25f) : Fade(Ui::TITLE_BG, 0.8f));
    DrawRectangleLinesEx(recBtn, 1.0f, overRec ? Ui::ACCENT : Ui::PANEL_BORDER);
    Vector2 rc{ recBtn.x + recBtn.width / 2.0f, recBtn.y + recBtn.height / 2.0f };
    Color   ric = overRec ? Ui::ACCENT : Ui::TEXT_DIM;
    DrawLineEx({ rc.x - 5, rc.y }, { rc.x + 5, rc.y }, 1.0f, ric);
    DrawLineEx({ rc.x, rc.y - 5 }, { rc.x, rc.y + 5 }, 1.0f, ric);
    DrawCircleLines((int)rc.x, (int)rc.y, 3.0f, ric);
}

// List of system objects, sorted by distance; click — select.
void Game::DrawOverviewContent(Rectangle area)
{
    // Read from the snapshot (M4c), not from the live objects. Selection/menu on click map
    // back to the live entity by id (the action applies to the live object).
    Vector2 sp = snapshot_.player.pos;

    std::vector<const Proto::EntitySnapshot*> list;
    list.reserve(snapshot_.entities.size());
    for (const auto& e : snapshot_.entities)
        list.push_back(&e);
    std::sort(list.begin(), list.end(),
              [sp](const Proto::EntitySnapshot* a, const Proto::EntitySnapshot* b)
              {
                  float ax = a->pos.x - sp.x, ay = a->pos.y - sp.y;
                  float bx = b->pos.x - sp.x, by = b->pos.y - sp.y;
                  return (ax * ax + ay * ay) < (bx * bx + by * by);
              });

    Vector2 m = GetMousePosition();
    bool    clicked = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    bool    rclicked = IsMouseButtonPressed(MOUSE_BUTTON_RIGHT);
    int     rowH = 20;
    int     y = (int)area.y;
    int     selId = selected_ != nullptr ? selected_->GetId() : 0;

    for (const Proto::EntitySnapshot* e : list)
    {
        if (y + rowH > area.y + area.height)
            break;  // doesn't fit — truncate the list

        Rectangle row{ area.x, (float)y, area.width, (float)rowH };
        if (e->id != 0 && e->id == selId)
            DrawRectangleRec(row, Fade(Ui::ACCENT, 0.22f));

        float dx = e->pos.x - sp.x;
        float dy = e->pos.y - sp.y;
        Ui::Text(e->name.c_str(), (int)area.x + 4, y + 3, 14, Ui::TEXT);
        const char* d = TextFormat("%.0f", sqrtf(dx * dx + dy * dy));
        Ui::Text(d, (int)(area.x + area.width) - Ui::TextWidth(d, 14) - 4, y + 3, 14, Ui::TEXT_DIM);

        if (CheckCollisionPointRec(m, row))
        {
            if (clicked)
            {
                selected_ = FindEntityById(e->id);
                if (selected_ != nullptr)
                    targetWin_->SetOpen(true);
            }
            else if (rclicked)  // RMB — action menu for the object
            {
                selected_ = FindEntityById(e->id);
                if (selected_ != nullptr)
                    OpenContextMenu(selected_);
            }
        }
        y += rowH;
    }
}

void Game::DrawTargetContent(Rectangle area)
{
    int x = (int)area.x;
    int y = (int)area.y;

    // Read the selected target from the snapshot by id (M4c). If it's not there (vanished) —
    // there's no target.
    int                          selId = selected_ != nullptr ? selected_->GetId() : 0;
    const Proto::EntitySnapshot* e = nullptr;
    if (selId != 0)
        for (const auto& es : snapshot_.entities)
            if (es.id == selId)
            {
                e = &es;
                break;
            }

    if (e == nullptr)
    {
        Ui::Text("No target selected", x, y, 16, Ui::TEXT_DIM);
        return;
    }

    Ui::Text(e->name.c_str(), x, y, 20, Ui::ACCENT);
    y += 30;

    float dx = e->pos.x - snapshot_.player.pos.x;
    float dy = e->pos.y - snapshot_.player.pos.y;
    Ui::Text(TextFormat("Distance  %.0f", sqrtf(dx * dx + dy * dy)), x, y, 16, Ui::TEXT);
    y += 26;

    if (e->kind == Proto::EntityKind::Npc)
    {
        Ui::Text(TextFormat("Faction  %s", FactionName(e->faction).c_str()), x, y, 16,
                 FactionColor(e->faction));
        y += 26;
        bool hostile = HostileToPlayerFaction(e->faction);
        Ui::Text(hostile ? "Hostile" : "Neutral", x, y, 14,
                 hostile ? Color{ 230, 41, 55, 255 } : Ui::TEXT_DIM);
        y += 24;
        float hf = e->hullFrac;
        Ui::Text("Hull", x, y, 14, Ui::TEXT_DIM);
        DrawRectangle(x, y + 16, (int)area.width, 9, Fade(GRAY, 0.35f));
        DrawRectangle(x, y + 16, (int)(area.width * hf), 9, hf > 0.3f ? LIME : RED);
    }
    else if (e->kind == Proto::EntityKind::Station)
    {
        Ui::Text(TextFormat("Faction  %s", FactionName(e->faction).c_str()), x, y, 16,
                 FactionColor(e->faction));
    }
    else if (e->kind == Proto::EntityKind::Field && e->ore >= 0)
    {
        Ui::Text(TextFormat("Ore  %s", ResourceName((ResourceType)e->ore).c_str()), x, y, 16,
                 Ui::TEXT);
    }
}

// Window input: continuing drags and routing a press.
// Returns true if the mouse is currently captured by the UI.
bool Game::HandleWindows()
{
    for (auto& w : windows_)
        w->UpdateDrag();

    bool overUi = false;
    for (auto& w : windows_)
        if (w->ContainsMouse())
            overUi = true;

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
        Vector2 m = GetMousePosition();
        // Find the topmost window under the cursor (end of the list — on top).
        for (int i = (int)windows_.size() - 1; i >= 0; i--)
        {
            if (!windows_[i]->ContainsMouse())
                continue;

            std::unique_ptr<Window> w = std::move(windows_[i]);
            windows_.erase(windows_.begin() + i);

            if (w->CloseButtonHit(m))
                w->SetOpen(false);
            else if (w->TitleBarHit(m))
                w->StartDrag(m);

            windows_.push_back(std::move(w));  // bring to the front
            break;
        }
    }
    return overUi;
}

// Menu bar input: clicking a button toggles the corresponding window.
// Returns true if the cursor is over the bar (mouse captured by the UI).
bool Game::HandleMenuBar()
{
    Vector2 m = GetMousePosition();
    bool    over =
        CheckCollisionPointRec(m, Rectangle{ 0.0f, 0.0f, MENU_BAR_W, (float)screenHeight_ });
    if (!over || !IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        return over;

    // The MAP slot (index 5) is not a window but the full-screen map (wins[5] == nullptr).
    Window* wins[] = { statusWin_,   targetWin_, overviewWin_, radarWin_,
                       missionsWin_, nullptr,    settingsWin_ };
    for (int i = 0; i < 7; i++)
    {
        Rectangle b{ (MENU_BAR_W - MENU_BTN) / 2.0f, MENU_TOP + i * MENU_STEP, MENU_BTN, MENU_BTN };
        if (CheckCollisionPointRec(m, b))
        {
            if (wins[i] != nullptr)
                wins[i]->Toggle();
            else
                galaxyMapOpen_ = !galaxyMapOpen_;
            break;
        }
    }
    return over;
}

// Vertical menu bar on the left: window-toggle buttons,
// the active window is highlighted with the accent color.
void Game::DrawMenuBar()
{
    DrawRectangleRec(Rectangle{ 0.0f, 0.0f, MENU_BAR_W, (float)screenHeight_ }, Ui::TITLE_BG);
    DrawLineEx(Vector2{ MENU_BAR_W, 0.0f }, Vector2{ MENU_BAR_W, (float)screenHeight_ }, 1.0f,
               Ui::PANEL_BORDER);

    Window*     wins[] = { statusWin_,   targetWin_, overviewWin_, radarWin_,
                           missionsWin_, nullptr,    settingsWin_ };
    const char* labels[] = { "STA", "TGT", "OVR", "RAD", "MIS", "MAP", "SET" };
    Vector2     m = GetMousePosition();

    for (int i = 0; i < 7; i++)
    {
        Rectangle b{ (MENU_BAR_W - MENU_BTN) / 2.0f, MENU_TOP + i * MENU_STEP, MENU_BTN, MENU_BTN };
        bool      open = wins[i] ? wins[i]->IsOpen() : galaxyMapOpen_;  // MAP — not a window
        bool      hover = CheckCollisionPointRec(m, b);
        Color     accent = (open || hover) ? Ui::ACCENT : Ui::TEXT_DIM;

        DrawRectangleRec(b, open ? Fade(Ui::ACCENT, 0.25f)
                                 : (hover ? Fade(Ui::ACCENT, 0.12f) : Ui::PANEL_BG));
        DrawRectangleLinesEx(b, 1.0f, (open || hover) ? Ui::ACCENT : Ui::PANEL_BORDER);

        int tw = Ui::TextWidth(labels[i], 14);
        Ui::Text(labels[i], (int)(b.x + (b.width - tw) / 2.0f), (int)b.y + 11, 14, accent);
    }
}

// Client navigation orders: write the intent into the command; the server applies it
// to the ship in the simulation step (Simulation::StepPlayerShip), not the client directly.
void Game::OrderAutopilot(Vector2 target, float stopDist)
{
    cmd_.navMode = 1;
    cmd_.navTarget = target;
    cmd_.navStopDist = stopDist;
}
void Game::OrderWarp(Vector2 target, float dropDist)
{
    cmd_.navMode = 2;
    cmd_.navTarget = target;
    cmd_.navStopDist = dropDist;
}

// Builds the context menu for an object: common actions plus actions
// depending on the target's type (station, field, NPC).
void Game::OpenContextMenu(Entity* target)
{
    std::vector<ContextMenu::Item> items;

    // Common actions for any object.
    items.push_back({ "Target", [this, target]()
                      {
                          selected_ = target;
                          targetWin_->SetOpen(true);
                      } });
    items.push_back({ "Approach", [this, target]()
                      { OrderAutopilot(target->GetPosition(), target->GetSize() + 70.0f); } });

    // Warp — only if the target is far enough (Approach suffices up close).
    float wdx = target->GetPosition().x - playerShip_->GetPosition().x;
    float wdy = target->GetPosition().y - playerShip_->GetPosition().y;
    if (sqrtf(wdx * wdx + wdy * wdy) > 1800.0f)
    {
        items.push_back({ "Warp to", [this, target]()
                          { OrderWarp(target->GetPosition(), target->GetSize() + 70.0f); } });
    }

    // Type-specific actions.
    if (Station* st = dynamic_cast<Station*>(target))
    {
        // Docking is server-authoritative: in range we send the intent and the server
        // decides (including the reputation gate); out of range we approach first, which
        // is the whole point of the menu item — the E key only works once already close.
        items.push_back({ "Dock", [this, st]()
                          {
                              float dx = st->GetPosition().x - playerShip_->GetPosition().x;
                              float dy = st->GetPosition().y - playerShip_->GetPosition().y;
                              if (sqrtf(dx * dx + dy * dy) <= st->GetSize() + DOCKING_RANGE)
                                  cmd_.dock = true;
                              else  // far — first approach via autopilot
                                  OrderAutopilot(st->GetPosition(), st->GetSize() + 60.0f);
                          } });
    }
    else if (AsteroidField* af = dynamic_cast<AsteroidField*>(target))
    {
        items.push_back({ "Mine here", [this, af]()
                          {
                              OrderAutopilot(af->GetPosition(), af->GetSize() + 30.0f);
                              if (!snapshot_.player.mining)  // enable mining via command
                                  cmd_.toggleMining = true;
                          } });
    }
    else if (NpcShip* npc = dynamic_cast<NpcShip*>(target))
    {
        items.push_back({ "Attack", [this, npc]()
                          {
                              selected_ = npc;
                              targetWin_->SetOpen(true);
                              if (!weaponOn_)
                                  cmd_.toggleWeapon = true;
                              weaponOn_ = true;
                          } });
    }
    else if (Derelict* dr = dynamic_cast<Derelict*>(target))
    {
        if (!dr->IsLooted())
            items.push_back({ "Investigate", [this, dr]()
                              {
                                  float dx = dr->GetPosition().x - playerShip_->GetPosition().x;
                                  float dy = dr->GetPosition().y - playerShip_->GetPosition().y;
                                  if (sqrtf(dx * dx + dy * dy) <= dr->GetSize() + 120.0f)
                                      cmd_.lootId =
                                          dr->GetId();  // salvage order (server will verify)
                                  else                  // far — approach first
                                      OrderAutopilot(dr->GetPosition(), dr->GetSize() + 40.0f);
                              } });
    }
    else if (JumpGate* g = dynamic_cast<JumpGate*>(target))
    {
        std::string dest = g->GetDestination();
        std::string label = "Jump";
        for (const auto& s : universe_.systems)
            if (s.id == dest)
            {
                label = "Jump to " + s.name;
                break;
            }
        items.push_back({ label, [this, g]()
                          {
                              float dx = g->GetPosition().x - playerShip_->GetPosition().x;
                              float dy = g->GetPosition().y - playerShip_->GetPosition().y;
                              if (sqrtf(dx * dx + dy * dy) <= g->GetSize() + 200.0f)
                                  cmd_.jumpGateId = g->GetId();  // jump order (server will verify)
                              else                               // far — warp to the gate
                                  OrderWarp(g->GetPosition(), g->GetSize() + 120.0f);
                          } });
    }

    contextMenu_.Open(GetMousePosition(), std::move(items));
}

// RMB menu on empty space: fly or warp to the chosen point.
void Game::OpenContextMenuAt(Vector2 worldPoint)
{
    std::vector<ContextMenu::Item> items;

    items.push_back({ "Fly here", [this, worldPoint]() { OrderAutopilot(worldPoint, 18.0f); } });

    float dx = worldPoint.x - playerShip_->GetPosition().x;
    float dy = worldPoint.y - playerShip_->GetPosition().y;
    if (sqrtf(dx * dx + dy * dy) > 1800.0f)
    {
        items.push_back({ "Warp here", [this, worldPoint]() { OrderWarp(worldPoint, 60.0f); } });
    }

    contextMenu_.Open(GetMousePosition(), std::move(items));
}

void Game::DrawStatusContent(Rectangle area)
{
    int x = (int)area.x;
    int y = (int)area.y;
    int barW = (int)area.width;

    float shFrac = playerShip_->GetShields() / playerShip_->GetMaxShields();
    Ui::Text("SHIELDS", x, y, 14, Ui::TEXT_DIM);
    DrawRectangle(x, y + 16, barW, 9, Fade(GRAY, 0.35f));
    DrawRectangle(x, y + 16, (int)(barW * shFrac), 9, Ui::ACCENT);
    y += 32;

    float hFrac = playerShip_->GetHull() / playerShip_->GetMaxHull();
    Ui::Text("HULL", x, y, 14, Ui::TEXT_DIM);
    DrawRectangle(x, y + 16, barW, 9, Fade(GRAY, 0.35f));
    DrawRectangle(x, y + 16, (int)(barW * hFrac), 9, hFrac > 0.3f ? LIME : RED);
    y += 38;

    Ui::Text(TextFormat("Speed   %.0f", playerShip_->GetSpeed()), x, y, 16, Ui::TEXT);
    y += 22;
    Ui::Text(TextFormat("Money   %.0f", player_.GetMoney()), x, y, 16, GOLD);
    y += 22;
    // Cargo — from the snapshot (over the network playerShip_'s hold isn't synced; the server
    // collects ore into its own ship, the snapshot carries the current volume). Single-player the
    // snapshot = player.
    Ui::Text(TextFormat("Cargo   %d / %d", snapshot_.player.cargoUsed, snapshot_.player.cargoCap),
             x, y, 16, Ui::TEXT);
    y += 24;

    const Skills& sk = player_.GetSkills();
    Ui::Text(TextFormat("Skills  P%d  M%d  T%d", sk.GetLevel(SkillType::Piloting),
                        sk.GetLevel(SkillType::Mining), sk.GetLevel(SkillType::Trading)),
             x, y, 14, Ui::TEXT_DIM);
    y += 26;

    // Stabilizer/mining toggles — from the snapshot (server-authoritative; the predicted
    // playerShip_ would flicker over the network due to replaying one-shot commands).
    bool stab = snapshot_.player.stabilizer;
    bool mine = snapshot_.player.mining;
    Ui::Text(TextFormat("stabilizer  %s", stab ? "ON" : "off"), x, y, 14,
             stab ? Ui::ACCENT : Ui::TEXT_DIM);
    y += 18;
    Ui::Text(TextFormat("mining  %s", mine ? "ON" : "off"), x, y, 14,
             mine ? Ui::ACCENT : Ui::TEXT_DIM);
    y += 18;
    Ui::Text(TextFormat("weapon  %s", weaponOn_ ? "ON" : "off"), x, y, 14,
             weaponOn_ ? Ui::ACCENT : Ui::TEXT_DIM);
    y += 18;
    if (playerShip_->IsAutopilotOn())
        Ui::Text("autopilot  ON", x, y, 14, GREEN);
}

void Game::DrawHud()
{
    for (auto& w : windows_)
        w->Draw();

    if (galaxyMapOpen_)
        DrawGalaxyMap();

    DrawMenuBar();

    // Docking prompt.
    if (nearbyStation_ != nullptr)
    {
        const char* prompt = TextFormat("Press E to dock at %s", nearbyStation_->GetName().c_str());
        int         tw = Ui::TextWidth(prompt, 20);
        Ui::Text(prompt, (screenWidth_ - tw) / 2, screenHeight_ - 56, 20, Ui::ACCENT);
    }

    // Warp effect — simple and legible, in screen coordinates.
    WarpPhase wp = playerShip_->GetWarpPhase();
    if (wp == WarpPhase::Aligning)
    {
        // Label + spin-up progress bar.
        const char* w = "ALIGNING";
        Ui::Text(w, (screenWidth_ - Ui::TextWidth(w, 22)) / 2, 38, 22, Fade(SKYBLUE, 0.85f));
        int   bw = 240, bh = 8, bx = (screenWidth_ - bw) / 2, by = 66;
        float p = playerShip_->GetWarpAlignProgress();
        DrawRectangle(bx, by, bw, bh, Fade(GRAY, 0.4f));
        DrawRectangle(bx, by, (int)(bw * p), bh, SKYBLUE);
    }
    else if (wp == WarpPhase::Warping)
    {
        // Light vignette at the screen edges + a label — calm and readable.
        DrawRectangleGradientH(0, 0, 180, screenHeight_, Fade(BLACK, 0.45f), BLANK);
        DrawRectangleGradientH(screenWidth_ - 180, 0, 180, screenHeight_, BLANK,
                               Fade(BLACK, 0.45f));
        const char* w = "WARP";
        Ui::Text(w, (screenWidth_ - Ui::TextWidth(w, 22)) / 2, 38, 22, SKYBLUE);
    }

    contextMenu_.Draw();  // over the windows

    // Current system and its security level (top center).
    if (const WorldLoader::SystemInfo* si = CurrentSystemInfo())
    {
        float       sec = si->security;
        const char* tier =
            sec >= 0.7f ? "High" : (sec >= 0.4f ? "Mid" : (sec >= 0.2f ? "Low" : "Null"));
        Color col = sec >= 0.7f ? LIME : (sec >= 0.4f ? Ui::ACCENT : (sec >= 0.2f ? ORANGE : RED));
        const char* line = TextFormat("%s   Security: %s (%.1f)", si->name.c_str(), tier, sec);
        Ui::Text(line, (screenWidth_ - Ui::TextWidth(line, 16)) / 2, 14, 16, col);
    }

    // Wanted indicator: factions that have the player wanted.
    std::string wanted;
    for (int i = 0; i < Factions::Count(); i++)
        if (player_.IsWanted((FactionId)i))
            wanted += (wanted.empty() ? "" : ", ") + FactionName((FactionId)i);
    if (!wanted.empty())
    {
        const char* w = TextFormat("WANTED: %s", wanted.c_str());
        Ui::Text(w, (screenWidth_ - Ui::TextWidth(w, 16)) / 2, 36, 16, RED);
    }

    Ui::Text("[debug] F1: +money   F11: fullscreen", 56, screenHeight_ - 26, 14, Ui::TEXT_DIM);

    // Short notification (saved/loaded).
    if (flashTimer_ > 0.0f)
    {
        int tw = Ui::TextWidth(flashMsg_.c_str(), 18);
        Ui::Text(flashMsg_.c_str(), (screenWidth_ - tw) / 2, screenHeight_ - 70, 18, LIME);
    }
}

void Game::DrawStationScreen()
{
    DrawRectangle(0, 0, screenWidth_, screenHeight_, Color{ 8, 9, 14, 255 });

    int       px = 60, py = 40;
    int       pw = screenWidth_ - 120, ph = screenHeight_ - 80;
    Rectangle panel{ (float)px, (float)py, (float)pw, (float)ph };

    // Outer panel and title bar — in the window-UI style.
    DrawRectangleRec(panel, Ui::PANEL_BG);
    DrawRectangleRec(Rectangle{ (float)px, (float)py, (float)pw, 56.0f }, Ui::TITLE_BG);
    DrawRectangleLinesEx(panel, 1.0f, Ui::PANEL_BORDER);

    int contentX = px + 24;

    Ui::Text(dockedStation_->GetName().c_str(), contentX, py + 9, 28, Ui::ACCENT);

    FactionId stationFaction = dockedStation_->GetFaction();
    float     stationRep = player_.GetReputation(stationFaction);
    RepTier   stationTier = Factions::TierOf(stationRep);
    Ui::Text(TextFormat("DOCKED  ·  %s  ·  %s  ·  %s (%d)",
                        StationRoleName(dockedStation_->GetRole()).c_str(),
                        FactionName(stationFaction).c_str(),
                        Factions::TierName(stationTier).c_str(), (int)stationRep),
             contentX, py + 39, 14, Factions::TierColor(stationTier));

    // Price multipliers from reputation: high reputation makes selling and buying more favorable.
    float sellMul = 1.0f, buyMul = 1.0f;
    switch (stationTier)
    {
        case RepTier::Hostile:
            sellMul = 0.85f;
            buyMul = 1.15f;
            break;
        case RepTier::Liked:
            sellMul = 1.10f;
            buyMul = 0.92f;
            break;
        case RepTier::Allied:
            sellMul = 1.20f;
            buyMul = 0.85f;
            break;
        default: break;
    }

    const char* moneyStr = TextFormat("Money  %.0f", player_.GetMoney());
    Ui::Text(moneyStr, px + pw - Ui::TextWidth(moneyStr, 22) - 24, py + 10, 22, GOLD);
    {
        const Skills& sk = player_.GetSkills();
        const char*   skillsStr =
            TextFormat("Pilot %d    Mining %d    Trade %d", sk.GetLevel(SkillType::Piloting),
                       sk.GetLevel(SkillType::Mining), sk.GetLevel(SkillType::Trading));
        Ui::Text(skillsStr, px + pw - Ui::TextWidth(skillsStr, 14) - 24, py + 40, 14, Ui::TEXT_DIM);
    }

    // --- Wanted: pay this station's faction bounty to clear the WANTED status ---
    if (player_.IsWanted(stationFaction))
    {
        double bounty = player_.GetBounty(stationFaction);
        Ui::Text(TextFormat("WANTED by %s  ·  bounty %.0f cr", FactionName(stationFaction).c_str(),
                            bounty),
                 contentX, py + 62, 15, Color{ 230, 41, 55, 255 });
        Button payBtn(Rectangle{ (float)(contentX + 360), (float)(py + 58), 180.0f, 24.0f },
                      TextFormat("Pay bounty (%.0f)", bounty),
                      [this, stationFaction, bounty]()
                      {
                          if (!player_.CanAfford(bounty))
                          {
                              FlashMessage("Not enough credits to pay bounty");
                              return;
                          }
                          // The account is on the server — pay via command.
                          Proto::Command c;
                          c.payBountyFaction = (int)stationFaction;
                          clientLink_->Send(Proto::EncodeCommand(c));
                          FlashMessage("Bounty paid — record cleared");
                      });
        payBtn.Process();
    }

    // --- Mission board: right column if the window is wide enough ---
    int boardW = pw - 760;
    if (boardW > 460)
        boardW = 460;
    if (boardW >= 220)
        DrawMissionBoard(px + pw - boardW - 24, py + 84, boardW);

    // --- Market: selling mined ore from the hold. Prices and cargo are read from the snapshot
    // (data comes from the server), not from the live sim_/ship directly. ---
    Ui::Text("MARKET", contentX, py + 84, 20, Ui::TEXT);
    int rowY = py + 116;
    int resIdx = 0;
    for (ResourceType type : AllResourceTypes())
    {
        int   cargo = resIdx < (int)snapshot_.player.cargoByType.size()
                          ? snapshot_.player.cargoByType[resIdx]
                          : 0;
        float price =
            resIdx < (int)snapshot_.marketPrices.size() ? snapshot_.marketPrices[resIdx] : 0.0f;
        Ui::Text(
            TextFormat("%-9s   price %.1f    cargo %d", ResourceName(type).c_str(), price, cargo),
            contentX, rowY + 7, 18, cargo > 0 ? Ui::TEXT : Ui::TEXT_DIM);

        if (cargo > 0)
        {
            Button sellBtn(Rectangle{ (float)(contentX + 420), (float)rowY, 130.0f, 30.0f },
                           "Sell all",
                           [this, type, sellMul, cargo]()
                           {
                               // Selling is an order to the server; ApplyTradeAcks credits
                               // the revenue on acknowledgement (at the server's price).
                               Proto::Command c;
                               c.sellType = (int)type;
                               c.sellAmount = cargo;
                               clientLink_->Send(Proto::EncodeCommand(c));
                           });
            sellBtn.Process();
        }
        rowY += 40;
        resIdx++;
    }

    // --- Hangar: buying ships ---
    const std::vector<ShipType>& catalog = GetShipCatalog();
    int                          hangarY = rowY + 18;
    Ui::Text("HANGAR", contentX, hangarY, 20, Ui::TEXT);
    Ui::Text(TextFormat("Current ship: %s", catalog[currentShipIndex_].name.c_str()), contentX,
             hangarY + 28, 14, Ui::ACCENT);

    int shipY = hangarY + 56;
    for (size_t i = 0; i < catalog.size(); i++)
    {
        const ShipType& t = catalog[i];
        bool            current = ((int)i == currentShipIndex_);

        Ui::Text(TextFormat("%-9s   speed %.0f   cargo %d   mining %.1f", t.name.c_str(),
                            t.stats.maxSpeed, t.stats.cargoCapacity, t.stats.miningRate),
                 contentX, shipY + 7, 18, current ? Color{ 120, 210, 130, 255 } : Ui::TEXT);

        Rectangle btnRect{ (float)(contentX + 540), (float)shipY, 160.0f, 30.0f };
        if (current)
        {
            Ui::Text("CURRENT", contentX + 540, shipY + 7, 16, Color{ 120, 210, 130, 255 });
        }
        else if (ownedShips_[i])
        {
            // Ship already owned — switching is free.
            Button switchBtn(btnRect, "Switch",
                             [this, i]()
                             {
                                 Proto::Command c;
                                 c.refitShip = (int)i;
                                 clientLink_->Send(Proto::EncodeCommand(c));
                                 currentShipIndex_ = (int)i;
                             });
            switchBtn.Process();
        }
        else
        {
            Button buyBtn(btnRect, TextFormat("Buy (%.0f)", t.price * buyMul),
                          [this, i, buyMul]()
                          {
                              const ShipType& st = GetShipCatalog()[i];
                              double          price = st.price * buyMul;
                              if (!player_.CanAfford(price))  // player_ is a mirror (server money)
                                  return;
                              // The purchase is server-authoritative: the server charges and
                              // refits (BuyShip) and the mirror updates the money. Ownership and
                              // index are client-side display only — see #5.
                              Proto::Command c;
                              c.buyShip = (int)i;
                              clientLink_->Send(Proto::EncodeCommand(c));
                              ownedShips_[i] = true;
                              currentShipIndex_ = (int)i;
                          });
            buyBtn.Process();
        }
        shipY += 38;
    }

    Button undockBtn(Rectangle{ (float)contentX, (float)(py + ph - 60), 200.0f, 40.0f }, "Undock",
                     [this]() { Undock(); });
    undockBtn.Process();
}

// Station mission board: a list of offers with an Accept button on each.
void Game::DrawMissionBoard(int x, int y, int w)
{
    Ui::Text("MISSIONS", x, y, 20, Ui::TEXT);

    const std::vector<Mission>& offers = missions_.Offers();
    int                         rowY = y + 32;
    const int                   rowH = 76;

    // Defer accepting until the loop ends: Accept mutates offers_, which we're iterating.
    int toAccept = -1;
    for (size_t i = 0; i < offers.size(); i++)
    {
        const Mission& m = offers[i];

        Rectangle box{ (float)x, (float)rowY, (float)w, (float)(rowH - 8) };
        DrawRectangleRec(box, Fade(Ui::TITLE_BG, 0.5f));
        DrawRectangleLinesEx(box, 1.0f, Ui::PANEL_BORDER);

        Ui::Text(m.title.c_str(), x + 10, rowY + 7, 16, FactionColor(m.faction));
        Ui::Text(m.description.c_str(), x + 10, rowY + 29, 14, Ui::TEXT);
        Ui::Text(TextFormat("Reward  %.0f cr   rep +%.0f", m.rewardMoney, m.rewardRep), x + 10,
                 rowY + 49, 13, GOLD);

        Button accept(Rectangle{ (float)(x + w - 96), (float)(rowY + 38), 86.0f, 26.0f }, "Accept",
                      [&toAccept, i]() { toAccept = (int)i; });
        accept.Process();

        rowY += rowH;
    }
    if (toAccept >= 0)
    {
        // Accepting is a server mutation (missions are authoritative).
        Proto::Command c;
        c.acceptOffer = toAccept;
        clientLink_->Send(Proto::EncodeCommand(c));
    }

    // --- Turn-in: active missions completable at this station ---
    rowY += 8;
    Ui::Text("READY TO TURN IN", x, rowY, 16, Ui::TEXT);
    rowY += 26;

    const std::vector<Mission>& active = missions_.Active();
    int                         toComplete = -1;
    bool                        anyReady = false;
    for (size_t i = 0; i < active.size(); i++)
    {
        const Mission& m = active[i];
        // Over the network the server determines readiness (m.completable from the snapshot; the
        // client hold is a mirror, not always accurate). Single-player — a local check.
        if (!m.completable)
            continue;
        anyReady = true;

        Rectangle box{ (float)x, (float)rowY, (float)w, 36.0f };
        DrawRectangleRec(box, Fade(Ui::TITLE_BG, 0.5f));
        DrawRectangleLinesEx(box, 1.0f, Ui::PANEL_BORDER);

        Ui::Text(m.description.c_str(), x + 10, rowY + 4, 14, Ui::TEXT);
        Ui::Text(TextFormat("+%.0f cr", m.rewardMoney), x + 10, rowY + 20, 12, GOLD);

        Button complete(Rectangle{ (float)(x + w - 104), (float)(rowY + 5), 94.0f, 26.0f },
                        "Complete", [&toComplete, i]() { toComplete = (int)i; });
        complete.Process();

        rowY += 44;
    }
    if (!anyReady)
    {
        Ui::Text("Nothing to turn in here.", x, rowY, 13, Ui::TEXT_DIM);
        rowY += 20;
    }
    if (toComplete >= 0)
    {
        // Turn-in is a server mutation (the reward lands in the server-side account).
        Proto::Command c;
        c.completeMission = toComplete;
        clientLink_->Send(Proto::EncodeCommand(c));
    }

    Ui::Text(TextFormat("Active missions: %d", (int)missions_.Active().size()), x, rowY + 4, 14,
             Ui::TEXT_DIM);
}

// Active mission log: the objective and current progress for each mission.
void Game::DrawMissionsContent(Rectangle area)
{
    const std::vector<Mission>& active = missions_.Active();
    int                         x = (int)area.x;
    int                         y = (int)area.y;

    if (active.empty())
    {
        Ui::Text("No active missions", x, y, 16, Ui::TEXT_DIM);
        Ui::Text("Accept jobs at a station.", x, y + 22, 14, Ui::TEXT_DIM);
        return;
    }

    const Color done = { 120, 210, 130, 255 };  // color of a completed objective
    const int   rowH = 60;

    for (const Mission& m : active)
    {
        if (y + rowH > area.y + area.height)
            break;  // doesn't fit — truncate the list

        Ui::Text(m.title.c_str(), x, y, 16, FactionColor(m.faction));
        Ui::Text(m.description.c_str(), x, y + 19, 13, Ui::TEXT);

        // The progress line depends on the mission type.
        const char* line = "";
        bool        complete = false;
        switch (m.type)
        {
            case MissionType::Bounty:
                complete = m.progress >= m.targetCount;
                line = TextFormat("Pirates  %d / %d", m.progress, m.targetCount);
                break;
            case MissionType::Mining:
            {
                // Cargo comes from the snapshot: the predicted ship's hold is not synced.
                int cur = 0;
                {
                    int idx = 0;
                    for (ResourceType rt : AllResourceTypes())
                    {
                        if (rt == m.resource)
                        {
                            cur = idx < (int)snapshot_.player.cargoByType.size()
                                      ? snapshot_.player.cargoByType[idx]
                                      : 0;
                            break;
                        }
                        idx++;
                    }
                }
                complete = m.completable;
                line =
                    TextFormat("%s  %d / %d", ResourceName(m.resource).c_str(), cur, m.targetCount);
                break;
            }
            case MissionType::Delivery:
            {
                Station* destSt = StationById(m.destStationId);
                line = TextFormat("Deliver to %s", destSt ? destSt->GetName().c_str() : "station");
                break;
            }
        }
        Ui::Text(line, x, y + 38, 14, complete ? done : Ui::TEXT_DIM);

        y += rowH;
    }
}

// Full-screen galaxy star map (EVE-style): dimmed background,
// systems as nodes by mapPos, gate links as lines, the current system highlighted.
void Game::DrawGalaxyMap()
{
    // Full-screen dimming background.
    DrawRectangle(0, 0, screenWidth_, screenHeight_, Color{ 6, 8, 14, 235 });

    Ui::Text("GALAXY MAP", MENU_BAR_W + 24, 24, 28, Ui::ACCENT);
    Ui::Text("drag to pan, wheel to zoom   ·   [G]/[Esc] close", MENU_BAR_W + 24, 58, 14,
             Ui::TEXT_DIM);

    const std::vector<WorldLoader::SystemInfo>& systems = universe_.systems;
    if (systems.empty())
    {
        Ui::Text("No galaxy data", screenWidth_ / 2 - 60, screenHeight_ / 2, 16, Ui::TEXT_DIM);
        return;
    }

    // Graph area — almost the whole screen (with margin for the title and menu bar).
    Rectangle area{ MENU_BAR_W + 60.0f, 100.0f, screenWidth_ - MENU_BAR_W - 120.0f,
                    screenHeight_ - 160.0f };

    // Extents in mapPos.
    float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
    for (const auto& s : systems)
    {
        minX = fminf(minX, s.mapPos.x);
        minY = fminf(minY, s.mapPos.y);
        maxX = fmaxf(maxX, s.mapPos.x);
        maxY = fmaxf(maxY, s.mapPos.y);
    }
    float spanX = fmaxf(1.0f, maxX - minX), spanY = fmaxf(1.0f, maxY - minY);
    float pad = 80.0f;
    float baseScale = fminf((area.width - 2 * pad) / spanX, (area.height - 2 * pad) / spanY);

    if (!galaxyInit_)  // on first display — center on the centroid and scale 1
    {
        galaxyCenter_ = { (minX + maxX) / 2.0f, (minY + maxY) / 2.0f };
        galaxyZoom_ = 1.0f;
        galaxyInit_ = true;
    }

    // Input: wheel zoom and drag-to-pan — like the radar.
    Vector2 m = GetMousePosition();
    bool    over = CheckCollisionPointRec(m, area);
    if (over)
    {
        float wheel = GetMouseWheelMove();
        if (wheel != 0.0f)
            galaxyZoom_ = Clamp(galaxyZoom_ * (1.0f + wheel * 0.12f), 0.3f, 8.0f);
    }

    float scale = baseScale * galaxyZoom_;

    if (over && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
        galaxyDragging_ = true;
        galaxyDragLast_ = m;
    }
    if (galaxyDragging_)
    {
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT))
        {
            galaxyCenter_.x -= (m.x - galaxyDragLast_.x) / scale;
            galaxyCenter_.y -= (m.y - galaxyDragLast_.y) / scale;
            galaxyDragLast_ = m;
        }
        else
        {
            galaxyDragging_ = false;
        }
    }

    Vector2 c{ area.x + area.width / 2.0f, area.y + area.height / 2.0f };
    auto    toScreen = [&](Vector2 mp) -> Vector2
    { return { c.x + (mp.x - galaxyCenter_.x) * scale, c.y + (mp.y - galaxyCenter_.y) * scale }; };
    auto posById = [&](const std::string& id, Vector2& out) -> bool
    {
        for (const auto& s : systems)
            if (s.id == id)
            {
                out = toScreen(s.mapPos);
                return true;
            }
        return false;
    };

    BeginScissorMode((int)area.x, (int)area.y, (int)area.width, (int)area.height);

    // Gate links.
    for (const auto& l : universe_.links)
    {
        Vector2 a, b;
        if (posById(l.a, a) && posById(l.b, b))
            DrawLineEx(a, b, 1.5f, Fade(Ui::PANEL_BORDER, 0.9f));
    }

    // Current system: over the network — from the server snapshot (the client's local sim_ isn't
    // active, its ActiveId() would remain on the start system); single-player — sim_.ActiveId().
    std::string activeSys = snapshot_.systemId;

    // System nodes.
    for (const auto& s : systems)
    {
        Vector2 p = toScreen(s.mapPos);
        bool    cur = (s.id == activeSys);
        DrawCircleV(p, cur ? 10.0f : 7.0f, cur ? Ui::ACCENT : Ui::TEXT_DIM);
        if (cur)
            DrawCircleLines((int)p.x, (int)p.y, 16.0f, Fade(Ui::ACCENT, 0.6f));
        Ui::Text(s.name.c_str(), (int)p.x + 14, (int)p.y - 8, 16, cur ? Ui::ACCENT : Ui::TEXT);

        // Live summary: security/pirates/economy/controller. Source — over the network the
        // server's galaxy snapshot (galaxyState_), single-player the local aggregates.
        bool      haveStats = false;
        float     security = 0.0f, prosperity = 0.0f;
        int       pirates = 0;
        FactionId controller = FactionId::Independent;
        for (const Proto::GalaxySystemStat& g : galaxyState_.systems)
            if (g.id == s.id)
            {
                haveStats = true;
                security = g.security;
                pirates = g.pirates;
                prosperity = g.prosperity;
                controller = g.controller;
                break;
            }
        if (haveStats)
        {
            Color secCol = security >= 0.7f   ? Color{ 120, 210, 130, 255 }
                           : security >= 0.4f ? GOLD
                                              : Color{ 230, 120, 60, 255 };
            Ui::Text(
                TextFormat("sec %.2f  pir %d  econ %.0f%%", security, pirates, prosperity * 100.0f),
                (int)p.x + 14, (int)p.y + 10, 12, secCol);
            // Territory controller (L3) — in the faction's color.
            Ui::Text(FactionName(controller).c_str(), (int)p.x + 14, (int)p.y + 24, 12,
                     FactionColor(controller));
            // The node ring is tinted with the controller's color.
            DrawCircleLines((int)p.x, (int)p.y, cur ? 13.0f : 10.0f,
                            Fade(FactionColor(controller), 0.7f));
        }
        if (cur)
            Ui::Text("you are here", (int)p.x + 14, (int)p.y + 38, 12, Ui::TEXT_DIM);
    }

    EndScissorMode();

    // Galactic news feed (system captures/reconquests), from the server's galaxy snapshot.
    const std::vector<std::string>& news = galaxyState_.events;
    if (!news.empty())
    {
        int nx = screenWidth_ - 320;
        int ny = 100;
        Ui::Text("GALACTIC NEWS", nx, ny, 14, Ui::ACCENT);
        ny += 22;
        for (size_t i = news.size(); i-- > 0;)  // newest on top
        {
            Ui::Text(news[i].c_str(), nx, ny, 12, Ui::TEXT_DIM);
            ny += 18;
        }
    }
}
