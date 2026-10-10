// Game — lifecycle, the frame loop, and turning input into commands.
//
// Game is split across three translation units rather than one 2000-line file. They
// implement the same class and share its state; the split is by what the code is doing,
// so that finding "how does a click become an order" does not mean scrolling past the
// station screen. See GameNet.cpp and GameHud.cpp.
#include "core/Game.h"
#include "render/Perf.h"
#include "sim/PlayerStep.h"
#include "sim/WarpPath.h"

#include "core/Archetype.h"
#include "core/Blueprint.h"
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
#include "render/Textures.h"
#include "raymath.h"
#include <nlohmann/json.hpp>
#include <cmath>
#include <string>
#include <algorithm>
#include <fstream>

// How close counts as docking range is the station's own reach, from its archetype -- the
// rule the server applies. A constant here was a second copy that would have stopped
// agreeing the moment stations grew (#159).
static float DockReach(const Entity& st)
{
    const Archetype* a = st.GetArchetype();
    return st.GetSize() + (a != nullptr ? a->dockRange : 0.0f);
}

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
    // raylib quits on Esc unless told otherwise. Here Esc closes what is on top
    // (HandleEscape); leaving the game is the window's close button.
    SetExitKey(KEY_NULL);
    Ui::LoadAssets();

    // Presentation data: factions, archetypes, materials. In a dev build we read the source data/
    // (the same path the editor writes to), otherwise a copy next to the exe.
#ifdef GAME_DATA_DIR
    dataDir_ = GAME_DATA_DIR;
#else
    dataDir_ = std::string(GetApplicationDirectory()) + "data/";
#endif
    Factions::Load(dataDir_ + "factions.json");  // faction properties/relations
    // Before any entity exists -- and the player's own ship is one. It used to be built
    // seventeen lines above this call, under this same comment, which made it the one
    // object in the client with no archetype: no glyph, no sprite, no components (#127).
    if (!Archetypes::Load(dataDir_ + "archetypes.json"))
        TraceLog(LOG_ERROR, "Archetypes: %s", Archetypes::Error().c_str());
    // What can be built, for the menu (#39). The server decides; without the file the menu
    // simply offers nothing.
    if (!Blueprints::Load(dataDir_ + "blueprints.json"))
        TraceLog(LOG_WARNING, "Blueprints: %s", Blueprints::Error().c_str());
    // No galaxy index here: the server sends it at login (#206), before the first layout.
    if (!Render::Materials::Load(dataDir_ + "materials.json"))
        TraceLog(LOG_WARNING, "Materials: %s", Render::Materials::Error().c_str());
    materials_.Load(dataDir_);  // one shader per material (#121)
    shapeBackend_.SetMaterials(&materials_);
    treatment_.Load(dataDir_);  // shaders and the pass chain (#120); safe if neither exists

    // The authoritative ship lives on the server. This one is the client's prediction of
    // it: the same Ship type, stepped with the same Sim::StepPlayerShip, corrected by
    // every snapshot. Its starting position is a placeholder until the first snapshot.
    playerShip_ = std::make_unique<Ship>(Vector2{ 0.0f, 3000.0f }, GetShipCatalog()[0].stats);

    camera_ = {};
    camera_.target = playerShip_->GetPosition();
    camera_.offset = { screenWidth_ / 2.0f, screenHeight_ / 2.0f };
    camera_.rotation = 0.0f;
    camera_.zoom = 1.0f;
    rig_.SetViewport((float)screenWidth_, (float)screenHeight_);
    rig_.Snap(playerShip_->GetPosition());

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
        Render::Perf::BeginFrame();
        float dt = GetFrameTime();

        // Pick up the current window size (it may have been resized with the mouse).
        screenWidth_ = GetScreenWidth();
        screenHeight_ = GetScreenHeight();
        rig_.SetViewport((float)screenWidth_, (float)screenHeight_);

        // Who the mouse belongs to this frame, decided once for everything (#297): a click
        // reaches the window in front of it and nothing behind.
        desk_.BeginFrame();

        // Debug commands (work in any mode).
        if (IsKeyPressed(KEY_F11))
            ToggleBorderlessWindowed();
        if (IsKeyPressed(KEY_F9))  // what a frame costs (#296)
        {
            perfOverlay_ = !perfOverlay_;
            if (perfFrames_ < 0)
                Render::Perf::Enable(perfOverlay_);
            perfRolled_ = GetTime();
        }
        if (IsKeyPressed(KEY_F10))  // the screen treatment's settings (#120)
            desk_.Toggle(WIN_LOOK);
        HandleEscape();
        desk_.HandleMouse();       // raising, dragging and closing windows
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
        // The server accepts a burst this large and no larger; see Sim::MAX_CATCHUP_SECONDS.
        if (simAccumulator_ > Sim::MAX_CATCHUP_SECONDS)
            simAccumulator_ = Sim::MAX_CATCHUP_SECONDS;
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
                shipPrevPos_ = playerShip_->GetPosition();
                shipPrevHeading_ = playerShip_->GetHeading();
                Sim::StepPlayerShip(*playerShip_, cmd_, 1.0f, SIM_DT, HoldTarget());
                // One-shot intents applied/sent this tick — clear them (axes are held).
                cmd_.toggleStabilizer = cmd_.toggleMining = cmd_.toggleWeapon = false;
                cmd_.dock = cmd_.undock = false;
                cmd_.navMode = 0;
                cmd_.jumpGateId = cmd_.lootId = 0;
                cmd_.deploy.clear();
            }
            simAccumulator_ -= SIM_DT;
        }

        // Client↔server boundary: the snapshot (world + market + player) comes from
        // econserver and the client picks it up here. We build the snapshot both in
        // flight and while docked; proxy-world reconciliation only in flight.
        BuildClientSnapshot();
        if (mode_ == GameMode::Flying)
            ReconcileClientWorld();

        // The camera is the player's (#158): it follows the ship until they look away, and
        // pulls back while the ship is in warp. A change of system snaps it, because there
        // is nothing between the old position and the new one to glide across.
        if (mode_ == GameMode::Flying)
        {
            UpdateShipDrawPose();
            if (cameraSnap_)
            {
                rig_.Snap(shipDrawPos_);
                cameraSnap_ = false;
            }
            // The camera follows the ship as drawn, or the ship shudders against it.
            rig_.Update(dt, shipDrawPos_, playerShip_->IsWarping());
            camera_ = rig_.Camera();
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

        Render::Perf::Mark(Render::Perf::Phase::Update);

        const bool shooting = !shotPath_.empty();
        if (shooting && shotTarget_.id == 0)
            shotTarget_ = LoadRenderTexture(screenWidth_, screenHeight_);
        BeginDrawing();
        if (shooting)
            BeginTextureMode(shotTarget_);
        ClearBackground(BLACK);

        // Everything between Begin and End goes through the chain. The HUD is drawn
        // inside or outside it depending on the setting, because it carries numbers people
        // fly by and a pixelated fuel gauge is a worse game (#120). When the treatment is
        // off or unavailable, Begin and End do nothing and this is the old draw order.
        const bool treatHud = treatment_.Config().treatHud;
        const bool flying = (mode_ == GameMode::Flying);
        // The station screen is interface, not world, so it goes through the chain only if
        // the player asked for the interface to be treated. Otherwise the chain would be
        // running over an empty scene and laying grain behind a menu.
        const bool useChain = (flying || treatHud) && (!shooting || shotTreated_);

        if (useChain)
            treatment_.Begin(screenWidth_, screenHeight_, shooting ? &shotTarget_ : nullptr);
        if (flying)
        {
            DrawWorld();
            if (treatHud)
                DrawHud();
        }
        else
        {
            Ui::MouseScope scope(desk_.Owns(WIN_STATION));  // F10's panel can sit over it
            DrawStationScreen();
        }
        if (useChain)
            treatment_.End();
        Render::Perf::Mark(Render::Perf::Phase::Treatment);

        if (flying && !treatHud)
            DrawHud();

        // Above everything, and never treated: a settings screen seen through the effect
        // it is adjusting is a settings screen you cannot read while adjusting it.
        desk_.Draw(Ui::Layer::Overlay);
        // Not a window: it is read, never clicked, so it takes no part in who owns the mouse.
        if (perfOverlay_)
            DrawPerfOverlay();
        Render::Perf::Mark(Render::Perf::Phase::Hud);

        if (shooting)
        {
            EndTextureMode();
            if (--shotFrames_ <= 0)
            {
                Image shot = LoadImageFromTexture(shotTarget_.texture);
                ImageFlipVertical(&shot);  // a render texture is stored upside down
                ImageFormat(&shot, PIXELFORMAT_UNCOMPRESSED_R8G8B8);  // no stray alpha
                if (!ExportImage(shot, shotPath_.c_str()))
                    TraceLog(LOG_WARNING, "Could not write %s", shotPath_.c_str());
                UnloadImage(shot);
                UnloadRenderTexture(shotTarget_);
                EndDrawing();
                if (perfFrames_ >= 0)
                {
                    Render::Perf::Mark(Render::Perf::Phase::Present);
                    Render::Perf::EndFrame();
                    Render::Perf::Roll();
                    std::fprintf(stderr, "perf (shot, treatment %s) %dx%d\n%s",
                                 shotTreated_ && treatment_.Config().enabled ? "on" : "off",
                                 screenWidth_, screenHeight_,
                                 Render::Perf::Report(Render::Perf::Last()).c_str());
                }
                break;
            }
        }
        EndDrawing();
        desk_.Persist();  // a window moved, opened or closed: the layout is written now
        Render::Perf::Mark(Render::Perf::Phase::Present);
        Render::Perf::EndFrame();

        // The overlay averages over a second; --perf over the second half of its run.
        if (perfOverlay_ && perfFrames_ < 0 && GetTime() - perfRolled_ >= 1.0)
        {
            Render::Perf::Roll();
            perfRolled_ = GetTime();
        }
        if (perfFrames_ >= 0)
        {
            perfFrames_--;
            if (perfFrames_ == perfTotal_ / 2)
                Render::Perf::Roll();  // the first half was the connection settling
            if (perfFrames_ == 0 && shotPath_.empty())
            {
                Render::Perf::Roll();
                std::fprintf(stderr, "perf (treatment %s) %dx%d\n%s",
                             treatment_.Config().enabled && treatment_.Available() ? "on" : "off",
                             screenWidth_, screenHeight_,
                             Render::Perf::Report(Render::Perf::Last()).c_str());
                break;
            }
        }
    }
}

void Game::MeasurePerf(int frames)
{
    perfTotal_ = perfFrames_ = frames > 1 ? frames : 2;
    Render::Perf::Enable(true);
    // Uncapped: a frame that waits for the next sixtieth of a second measures the wait.
    SetTargetFPS(0);
}

// The last second's averages, in the corner, over everything and never treated.
void Game::DrawPerfOverlay()
{
    const std::string text = Render::Perf::Report(Render::Perf::Last());
    const float       x = 60.0f, y = (float)screenHeight_ - 250.0f;
    DrawRectangle((int)x - 6, (int)y - 6, 470, 236, Fade(BLACK, 0.8f));
    Ui::Text("F9  frame cost  (cpu+gpu per phase; sections are cpu)", (int)x, (int)y, 11,
             Ui::ACCENT);
    int    line = 0;
    size_t from = 0;
    while (from < text.size())
    {
        const size_t      to = text.find('\n', from);
        const std::string row =
            text.substr(from, to == std::string::npos ? std::string::npos : to - from);
        Ui::Text(row.c_str(), (int)x, (int)y + 16 + line * 14, 11, Ui::TEXT_DIM);
        line++;
        if (to == std::string::npos)
            break;
        from = to + 1;
    }
}

void Game::SetPilotName(const std::string& n)
{
    pilotName_ = n;
    // Next to the executable, not in data/: it is this machine's preference, not the game's.
    desk_.UseLayoutFile(std::string(GetApplicationDirectory()) + "ui_layout.json", n);
}

void Game::HandleInput(float dt)
{
    (void)dt;

    if (startWarpFrames_ > 0 && --startWarpFrames_ == 0)
    {
        OrderWarp(startWarpTarget_, 500.0f);
        startWarpFrames_ = -1;
    }

    // Naming a system takes the keyboard: its letters are not hotkeys (#145).
    if (naming_)
    {
        HandleNaming();
        return;
    }

    // The desk has chosen who the mouse belongs to (#297): a popup, a window, a screen, or
    // -- when none of them is under the cursor -- the world. The map and the sensor screen
    // cover the windows, so they take the clicks the windows would have.
    {
        Ui::MouseScope scope(desk_.Owns(WIN_CONTEXT));
        contextMenu_.Update();  // a click elsewhere closes it, and still lands there
    }
    HandleMenuBar();
    const bool toWorld = desk_.WorldOwnsMouse();

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
        desk_.Toggle(WIN_TARGET);

    if (IsKeyPressed(KEY_O))
        desk_.Toggle(WIN_OVERVIEW);

    if (IsKeyPressed(KEY_R))
        desk_.Toggle(WIN_RADAR);

    if (IsKeyPressed(KEY_J))
        desk_.Toggle(WIN_MISSIONS);

    if (IsKeyPressed(KEY_G))
        desk_.Toggle(WIN_MAP);
    // The sensor screen (#123); Esc closes it too (HandleEscape).
    if (IsKeyPressed(KEY_V))
        desk_.Toggle(WIN_SENSOR);
    // While it is open, the mouse points at cells rather than at the world behind them.
    const bool onSensor = desk_.Owns(WIN_SENSOR);
    if (desk_.IsOpen(WIN_MAP) && IsKeyPressed(KEY_N) && CanNameHere())
    {
        naming_ = true;
        nameBuf_.clear();
        while (GetCharPressed() != 0)
        {
            // the N that opened the field is not the name's first letter
        }
    }

    // Over a window, the wheel and the drag go to the window (the radar has its own).
    float wheel = GetMouseWheelMove();
    if (wheel != 0.0f && onSensor)
        sensorRange_ = Sensor::StepRange(sensorRange_, wheel > 0.0f ? -1 : 1);  // in is closer
    else if (wheel != 0.0f && toWorld)
        rig_.Zoom(wheel, GetMousePosition());

    // Middle button looks away; C comes back. Left and right are already select and the
    // context menu, and looking around is not worth taking either of them. A drag begun in
    // the world stays the world's when it passes over a window.
    if (IsMouseButtonPressed(MOUSE_BUTTON_MIDDLE) && toWorld)
        panLast_ = GetMousePosition();
    if (IsMouseButtonDown(MOUSE_BUTTON_MIDDLE) && toWorld)
    {
        const Vector2 m = GetMousePosition();
        rig_.Pan({ m.x - panLast_.x, m.y - panLast_.y });
        panLast_ = m;
    }
    if (IsKeyPressed(KEY_C))
        rig_.Recenter();

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
    // the only target source: the server decides what a shot hits
    // directly).
    cmd_.targetId = selected_ != nullptr ? selected_->GetId() : 0;

    // On the sensor screen a click picks a cell: what is in it is selected, and a right
    // click opens the same menu the world view would -- on the thing, or on the place.
    if (onSensor && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
        int     id = 0;
        Vector2 at = { 0.0f, 0.0f };
        if (SensorPick(GetMousePosition(), id, at))
        {
            selected_ = FindEntityById(id);
            if (selected_ != nullptr)
                desk_.SetOpen(WIN_TARGET, true);
        }
    }
    if (onSensor && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT))
    {
        int     id = 0;
        Vector2 at = { 0.0f, 0.0f };
        if (SensorPick(GetMousePosition(), id, at))
        {
            Entity* target = FindEntityById(id);
            if (target != nullptr)
                OpenContextMenu(target);
            else
                OpenContextMenuAt(at);
        }
    }

    // Left click — select the object under the cursor (unless over the UI). Search the
    // snapshot (M4c); the action applies to the live entity by id.
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && toWorld)
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
            desk_.SetOpen(WIN_TARGET, true);
    }

    // Right click: on an object — context menu; on empty space — autopilot.
    if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && toWorld)
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
            if (sqrtf(dx * dx + dy * dy) <= DockReach(*st))
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
    // Which system the player is in comes from the snapshot; the client has no simulation
    // to ask. The system list itself (Universe) is the one the server sent at login (#206);
    // until it arrives this finds nothing, and callers already handle that.
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

Sensor::Standing Game::ViewerStanding() const
{
    Sensor::Standing s;
    s.hostile = [this](FactionId f) { return HostileToPlayerFaction(f); };
    s.tier = [this](FactionId f) { return Factions::TierOf(player_.GetReputation(f)); };
    return s;
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

    // Around a star or a planet rather than through it (#160). The bend point travels in
    // the command, so the server flies the same two legs this client predicts.
    std::vector<WarpPath::Body> bodies;
    for (const auto& e : clientWorld_)
        if (e->GetKind() == EntityKind::Star || e->GetKind() == EntityKind::Planet)
            bodies.push_back({ e->GetPosition(), e->GetSize() });
    cmd_.navViaSet = WarpPath::Via(playerShip_->GetPosition(), target, bodies, cmd_.navVia);
}

// Standing hold: orbit (mode 3), keep at range (mode 4) or follow (mode 5, #298). Unlike
// Approach and Warp these
// do not finish -- they run until something releases them, which is what flying a fight or
// waiting beside a gate actually is (#157).
void Game::OrderHold(int mode, int targetId, float range)
{
    cmd_.navMode = mode;
    cmd_.navHoldId = targetId;
    cmd_.navRange = range;
}

void Game::ReleaseHold()
{
    // There is no "stop" command: a hold ends when the ship is told to do something else,
    // and an autopilot order to where it already is says exactly that.
    OrderAutopilot(playerShip_->GetPosition(), 1.0f);
}

// The distances a hold is offered at, as multiples of the target's own radius. Written
// against the object rather than in absolute units so the same menu is sensible beside a
// sixteen-unit ship and a station forty times larger -- and so it survives the system
// growing (M9) without every number in it becoming wrong.
static const float HOLD_RANGES[] = { 2.0f, 5.0f, 12.0f };

// Builds the context menu for an object: common actions plus actions
// depending on the target's type (station, field, NPC).
void Game::OpenContextMenu(Entity* target)
{
    std::vector<ContextMenu::Item> items;

    // Common actions for any object.
    items.push_back({ "Target", [this, target]()
                      {
                          selected_ = target;
                          desk_.SetOpen(WIN_TARGET, true);
                      } });
    items.push_back({ "Approach", [this, target]()
                      { OrderAutopilot(target->GetPosition(), target->GetSize() + 70.0f); } });

    // The two standing behaviours. Every fight is flown as one of these and there was no
    // way to ask for either of them.
    const int   tid = target->GetId();
    const float base = target->GetSize();
    if (tid != 0)
    {
        for (float mult : HOLD_RANGES)
        {
            const float range = base * mult;
            items.push_back({ TextFormat("Orbit at %.0f", range),
                              [this, tid, range]() { OrderHold(3, tid, range); } });
        }
        // Named for what it does (#309): hold this distance from it, wherever it goes.
        items.push_back({ TextFormat("Hold %.0f from it", base * HOLD_RANGES[1]),
                          [this, tid, base]() { OrderHold(4, tid, base * HOLD_RANGES[1]); } });
        // Matching its velocity as well as its distance: the one that stays with a station
        // going round a planet rather than arriving behind it (#298).
        items.push_back({ TextFormat("Follow at %.0f", base * HOLD_RANGES[1]),
                          [this, tid, base]() { OrderHold(5, tid, base * HOLD_RANGES[1]); } });
        // Any distance, not just the presets.
        const std::string name = target->GetName();
        items.push_back({ "Hold at a range...", [this, tid, base, name]()
                          {
                              rangePicker_.Open(GetMousePosition(), name, base,
                                                base * HOLD_RANGES[1],
                                                [this, tid](int mode, float range)
                                                { OrderHold(mode, tid, range); });
                          } });
    }

    // Warp — only if the target is far enough (Approach suffices up close). The drop
    // distance is offered rather than assumed: arriving on top of a station and arriving
    // far enough out to look at it first are different intentions.
    float wdx = target->GetPosition().x - playerShip_->GetPosition().x;
    float wdy = target->GetPosition().y - playerShip_->GetPosition().y;
    if (sqrtf(wdx * wdx + wdy * wdy) > 1800.0f)
    {
        items.push_back({ "Warp to", [this, target]()
                          { OrderWarp(target->GetPosition(), target->GetSize() + 70.0f); } });
        items.push_back({ TextFormat("Warp to, %.0f out", base * HOLD_RANGES[2]),
                          [this, target, base]()
                          { OrderWarp(target->GetPosition(), base * HOLD_RANGES[2]); } });
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
                                  if (sqrtf(dx * dx + dy * dy) <= DockReach(*st))
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
                                  desk_.SetOpen(WIN_TARGET, true);
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
        // Nothing to do with one yet but go there; taking one down is the next slice (#39).
        case EntityKind::Structure: break;
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

    // Build here (#39): every blueprint whose reach the point is within, with what it costs.
    // Only the reach is checked here, so the menu does not offer what could never work; the
    // rest -- the hold, the room, the caps -- the server decides and says in the journal.
    if (mode_ == GameMode::Flying)
        for (const Blueprint& bp : Blueprints::All())
        {
            if (sqrtf(dx * dx + dy * dy) > bp.reach)
                continue;
            std::string cost;
            for (const auto& c : bp.cost)
                cost += (cost.empty() ? "" : ", ") + std::to_string(c.second) + " " +
                        ResourceName(c.first);
            const std::string id = bp.id;
            items.push_back({ "Build " + bp.name + " (" + cost + ")", [this, id, worldPoint]()
                              {
                                  cmd_.deploy = id;
                                  cmd_.deployPos = worldPoint;
                              } });
        }

    contextMenu_.Open(GetMousePosition(), std::move(items));
}

void Game::SaveTreatment()
{
    // Written on close rather than on every slider frame: this is a file, and a slider
    // being dragged is sixty writes a second.
    std::string error;
    if (!treatment_.Save(error))
        TraceLog(LOG_WARNING, "Treatment: %s", error.c_str());
}

// Esc closes the topmost thing that is open and does nothing when nothing is; the desk
// keeps the order (#297): F10's panel, then a popup, then the map or the sensor screen,
// then the window in front. It never quits: an MMO client that drops you on a stray key is
// a ship left drifting. The station screen is not closed by it either -- leaving it is
// undocking, an order to the server, and that wants its own button -- so it stops Esc
// there rather than letting it reach the windows behind it.
void Game::HandleEscape()
{
    if (!IsKeyPressed(KEY_ESCAPE) || naming_)  // the name field takes its own Esc
        return;
    desk_.Escape();
}

bool Game::CanNameHere() const
{
    for (const WorldLoader::SystemInfo& si : universe_.systems)
        if (si.id == snapshot_.systemId)
            return !pilotName_.empty() && si.discoverer == pilotName_ && si.designation.empty();
    return false;
}

void Game::HandleNaming()
{
    for (int c = GetCharPressed(); c != 0; c = GetCharPressed())
        if (c >= 32 && c < 127 && nameBuf_.size() < 24)
            nameBuf_ += (char)c;
    if (IsKeyPressed(KEY_BACKSPACE) && !nameBuf_.empty())
        nameBuf_.pop_back();
    if (IsKeyPressed(KEY_ESCAPE))
        naming_ = false;
    if (IsKeyPressed(KEY_ENTER) && !nameBuf_.empty())
    {
        // The server checks it and says why not in the journal, which flashes here; a name
        // it accepts comes back to everyone in the galaxy index.
        Proto::Command c;
        c.nameSystem = nameBuf_;
        clientLink_->Send(Proto::EncodeCommand(c));
        naming_ = false;
    }
}

void Game::UpdateShipDrawPose()
{
    const Vector2 now = playerShip_->GetPosition();
    const float   alpha = std::fmin(1.0f, std::fmax(0.0f, simAccumulator_ / SIM_DT));
    const float   dx = now.x - shipPrevPos_.x, dy = now.y - shipPrevPos_.y;
    // A jump, an arrival or a respawn is not a movement to smooth: a step that covered more
    // than a warp could in one tick is a teleport, drawn where it ended.
    if (dx * dx + dy * dy > 10000.0f * 10000.0f)
    {
        shipDrawPos_ = now;
        shipDrawHeading_ = playerShip_->GetHeading();
        return;
    }
    shipDrawPos_ = { shipPrevPos_.x + dx * alpha, shipPrevPos_.y + dy * alpha };
    float turn = playerShip_->GetHeading() - shipPrevHeading_;
    while (turn > PI)
        turn -= 2.0f * PI;
    while (turn < -PI)
        turn += 2.0f * PI;
    shipDrawHeading_ = shipPrevHeading_ + turn * alpha;
}
