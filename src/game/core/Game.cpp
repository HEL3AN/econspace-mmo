// Game — lifecycle, the frame loop, and turning input into commands.
//
// Game is split across three translation units rather than one 2000-line file. They
// implement the same class and share its state; the split is by what the code is doing,
// so that finding "how does a click become an order" does not mean scrolling past the
// station screen. See GameNet.cpp and GameHud.cpp.
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

// Player combat and mining are now server-side (Simulation::StepPlayerFire/StepPlayerMining);
// the weapon range for rendering the targeting circle is Sim::PLAYER_WEAPON_RANGE.

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
                    if (e->GetKind() != EntityKind::Field)
                        continue;
                    AsteroidField* f = static_cast<AsteroidField*>(e.get());
                    float          dx = f->GetPosition().x - pp.x;
                    float          dy = f->GetPosition().y - pp.y;
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

    // Nearest station within docking range; E — dock. Detection runs over the clientWorld_
    // proxies, the only world the client has. Docking itself is server-authoritative: we
    // send cmd_.dock and the server confirms via snapshot (see BuildClientSnapshot), so a
    // proxy being in range is a prompt, not a decision.
    nearbyStation_ = nullptr;
    {
        const auto& src = clientWorld_;
        for (auto& e : src)
        {
            if (e->GetKind() != EntityKind::Station)
                continue;
            Station* st = static_cast<Station*>(e.get());

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

Station* Game::StationById(int id) const
{
    Entity* e = FindEntityById(id);
    if (e == nullptr || e->GetKind() != EntityKind::Station)
        return nullptr;
    return static_cast<Station*>(e);
}

void Game::FlashMessage(const std::string& msg)
{
    flashMsg_ = msg;
    flashTimer_ = 2.5f;
}

// Parallax background: stars in screen coordinates, offset from the camera position
// proportionally to depth; farther layers move slower than nearer ones.
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

    // Kind-specific actions. Listed exhaustively rather than with a `default:` so that a
    // new kind of object — the point of #44 — cannot quietly ship with an empty menu.
    switch (target->GetKind())
    {
        case EntityKind::Station:
        {
            Station* st = static_cast<Station*>(target);
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
            break;
        }
        case EntityKind::Field:
        {
            AsteroidField* af = static_cast<AsteroidField*>(target);
            items.push_back({ "Mine here", [this, af]()
                              {
                                  OrderAutopilot(af->GetPosition(), af->GetSize() + 30.0f);
                                  if (!snapshot_.player.mining)  // enable mining via command
                                      cmd_.toggleMining = true;
                              } });
            break;
        }
        case EntityKind::Npc:
        {
            NpcShip* npc = static_cast<NpcShip*>(target);
            items.push_back({ "Attack", [this, npc]()
                              {
                                  selected_ = npc;
                                  targetWin_->SetOpen(true);
                                  if (!weaponOn_)
                                      cmd_.toggleWeapon = true;
                                  weaponOn_ = true;
                              } });
            break;
        }
        case EntityKind::Derelict:
        {
            Derelict* dr = static_cast<Derelict*>(target);
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
            break;
        }
        case EntityKind::Gate:
        {
            JumpGate*   g = static_cast<JumpGate*>(target);
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
                                      cmd_.jumpGateId =
                                          g->GetId();  // jump order (server will verify)
                                  else                 // far — warp to the gate
                                      OrderWarp(g->GetPosition(), g->GetSize() + 120.0f);
                              } });
            break;
        }
        // Scenery and the player's own ship: fly-to and warp-to, already added above, are all
        // there is to do with them.
        case EntityKind::Star:
        case EntityKind::Planet:
        case EntityKind::Nebula:
        case EntityKind::PlayerShip:
        case EntityKind::Unknown: break;
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
