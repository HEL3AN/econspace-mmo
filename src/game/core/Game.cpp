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
#include "core/Actions.h"
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

    // Presentation data: factions, archetypes, materials. In a dev build we read the source data/
    // (the same path the editor writes to), otherwise a copy next to the exe.
#ifdef GAME_DATA_DIR
    dataDir_ = GAME_DATA_DIR;
#else
    dataDir_ = std::string(GetApplicationDirectory()) + "data/";
#endif
    // The interface: its theme, the fonts it names (#297), and this machine's UI scale.
    Ui::LoadAssets(dataDir_);
    {
        std::string error;
        uiSettingsPath_ = std::string(GetApplicationDirectory()) + "ui_settings.json";
        uiSettingsWritable_ = Ui::LoadUiSettings(uiSettingsPath_, error);
        if (!uiSettingsWritable_)
            TraceLog(LOG_WARNING, "UI settings: %s", error.c_str());
    }
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

        // Which windows are there at all: the station's while docked, space's while flying
        // (#297). Then who the mouse belongs to this frame, decided once for everything: a
        // click reaches the window in front of it and nothing behind.
        desk_.SetContext(CTX_DOCKED, mode_ == GameMode::Docked);
        desk_.SetContext(CTX_SPACE, mode_ == GameMode::Flying);
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
        else
            HandleDockedInput();

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
                cmd_.nameSystem.clear();
                cmd_.jumpGateId = cmd_.lootId = 0;
                cmd_.deploy.clear();
                cmd_.dismantleId = 0;
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
        // Docked, the station's hall and its windows are interface, not world, so they go
        // through the chain only if the player asked for the interface to be treated.
        // Otherwise the chain would be running over an empty scene and laying grain behind
        // a menu.
        const bool useChain = (flying || treatHud) && (!shooting || shotTreated_);

        if (useChain)
            treatment_.Begin(screenWidth_, screenHeight_, shooting ? &shotTarget_ : nullptr);
        if (flying)
            DrawWorld();
        else
            DrawStationHall();
        if (treatHud)
            DrawHud();
        if (useChain)
            treatment_.End();
        Render::Perf::Mark(Render::Perf::Phase::Treatment);

        if (!treatHud)
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
    if (startSelectFrames_ > 0 && --startSelectFrames_ == 0)
    {
        // The nearest thing that is not this ship: the overview's first row.
        const std::vector<Overview::Row> rows = Overview::Build(
            snapshot_.entities, snapshot_.player.pos, Overview::Filter::All,
            Overview::Sort::Distance, [](const Proto::EntitySnapshot&) { return false; });
        for (const Overview::Row& r : rows)
            if ((selected_ = FindEntityById(r.entity->id)) != nullptr)
                break;
        if (selected_ != nullptr)
        {
            desk_.SetOpen(WIN_TARGET, true);
            if (startMenu_)
                OpenContextMenu(selected_, { screenWidth_ * 0.42f, screenHeight_ * 0.3f });
        }
        startSelectFrames_ = -1;
    }

    if (startDockFrames_ > 0 && --startDockFrames_ == 0)
        startDockFrames_ = 1;  // from here on, every frame until docked
    if (startDockFrames_ == 1)
        DriveStartDock();

    // A text field that has the keyboard -- the map's name field, the range being typed --
    // takes every key: its letters are not hotkeys and its W is not thrust (#145, #297).
    // The mouse still works; only the keys are its.
    const bool typing = desk_.KeyboardTaken();
    auto       key = [typing](int k) { return !typing && IsKeyPressed(k); };
    auto       held = [typing](int k) { return !typing && IsKeyDown(k); };

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
    if (key(KEY_X))
        cmd_.toggleStabilizer = true;

    if (key(KEY_M))
        cmd_.toggleMining = true;

    if (key(KEY_F))
    {
        cmd_.toggleWeapon = true;
        weaponOn_ = !weaponOn_;  // optimistic: the snapshot confirms it
    }

    if (key(KEY_T))
        desk_.Toggle(WIN_TARGET);

    if (key(KEY_O))
        desk_.Toggle(WIN_OVERVIEW);

    if (key(KEY_R))
        desk_.Toggle(WIN_RADAR);

    if (key(KEY_J))
        desk_.Toggle(WIN_MISSIONS);

    if (key(KEY_G))
        desk_.Toggle(WIN_MAP);
    // The sensor screen (#123); Esc closes it too (HandleEscape).
    if (key(KEY_V))
        desk_.Toggle(WIN_SENSOR);
    // While it is open, the mouse points at cells rather than at the world behind them.
    const bool onSensor = desk_.Owns(WIN_SENSOR);
    if (desk_.IsOpen(WIN_MAP) && key(KEY_N) && CanNameHere())
    {
        // The field is drawn on the map, which takes it from here (DrawGalaxyMap).
        nameBuf_.clear();
        desk_.KeyboardFocus().Take(WIN_MAP, "name");
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
    if (key(KEY_C))
        rig_.Recenter();

    // Held control axes: W — thrust, S — brake, A/D — turn. Written into the
    // command; the server applies it to the ship in the tick (Simulation::StepPlayerShip).
    cmd_.thrust = held(KEY_W) || held(KEY_UP);
    cmd_.brake = held(KEY_S) || held(KEY_DOWN);
    cmd_.turn = 0.0f;
    if (held(KEY_A) || held(KEY_LEFT))
        cmd_.turn -= 1.0f;
    if (held(KEY_D) || held(KEY_RIGHT))
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
        if (nearbyStation_ != nullptr && key(KEY_E))
            cmd_.dock = true;
    }
}

// --dock: the nearest station, warped to if it is far and flown to if it is near, then the
// dock order every frame until the server says docked.
void Game::DriveStartDock()
{
    Station* nearest = nullptr;
    float    best = 0.0f;
    for (const auto& e : clientWorld_)
        if (e->GetKind() == EntityKind::Station)
        {
            const float d = Vector2Distance(e->GetPosition(), playerShip_->GetPosition());
            if (nearest == nullptr || d < best)
            {
                nearest = static_cast<Station*>(e.get());
                best = d;
            }
        }
    if (nearest == nullptr)
        return;
    if (best <= DockReach(*nearest))
        cmd_.dock = true;
    else if (!playerShip_->IsWarping() && !playerShip_->IsAutopilotOn())
    {
        if (best > 20000.0f)
            OrderWarp(nearest->GetPosition(), nearest->GetSize() + 60.0f);
        else
            OrderAutopilot(nearest->GetPosition(), nearest->GetSize() + 60.0f);
    }
}

// Docked, the ship takes no orders but leaving; the windows still open and close by key,
// and the menu bar opens what has been closed (#297).
void Game::HandleDockedInput()
{
    startDockFrames_ = -1;  // --dock has done its job
    HandleMenuBar();
    if (desk_.KeyboardTaken())  // a number being typed into the market is not a hotkey
        return;
    if (IsKeyPressed(KEY_J))
        desk_.Toggle(WIN_MISSIONS);
    if (IsKeyPressed(KEY_G))
        desk_.Toggle(WIN_MAP);
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
    missionsTab_ = 0;  // the board was the station's
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

Actions::Target Game::ActionTarget(const Entity& e) const
{
    Actions::Target t;
    t.id = e.GetId();
    t.kind = e.GetKind();
    t.pos = e.GetPosition();
    t.size = e.GetSize();
    t.name = e.GetName();
    if (t.kind == EntityKind::Derelict)
        t.looted = static_cast<const Derelict&>(e).IsLooted();
    if (t.kind == EntityKind::Structure)
    {
        const Structure& s = static_cast<const Structure&>(e);
        t.mine = !pilotName_.empty() && s.GetOwner() == pilotName_;
        if (const Blueprint* bp = s.GetBlueprint())
        {
            const double now = snapshot_.time;  // the last word from the server is near enough
            for (const auto& c : Blueprints::Refund(*bp, s.Progress(now), s.HullFraction(now)))
                t.refund += (t.refund.empty() ? "" : ", ") + std::to_string(c.second) + " " +
                            ResourceName(c.first);
        }
    }
    if (t.kind == EntityKind::Gate)
    {
        const std::string& dest = static_cast<const JumpGate&>(e).GetDestination();
        for (const auto& s : universe_.systems)
            if (s.id == dest)
                t.destinationName = s.name;
    }
    return t;
}

// Does one of the actions Actions::For offered (#297). Everything here becomes a command
// for the server, apart from Select and SetRange, which are the interface's own. Where an
// action depends on how close the ship is -- dock, jump, salvage -- the client asks the
// question it can answer (is it in reach?) and the server decides the rest.
void Game::Perform(const Actions::Action& a, int targetId, Vector2 point)
{
    using Actions::Verb;
    Entity* target = targetId != 0 ? FindEntityById(targetId) : nullptr;
    auto    distanceTo = [this](const Entity* e)
    {
        const float dx = e->GetPosition().x - playerShip_->GetPosition().x;
        const float dy = e->GetPosition().y - playerShip_->GetPosition().y;
        return sqrtf(dx * dx + dy * dy);
    };
    switch (a.verb)
    {
        case Verb::FlyHere: OrderAutopilot(point, a.distance); return;
        case Verb::WarpHere: OrderWarp(point, a.distance); return;
        case Verb::Build:
            cmd_.deploy = a.blueprint;
            cmd_.deployPos = point;
            return;
        // The missions are the server's (M4f-2): a one-off command, and the next snapshot
        // says whether it was taken.
        case Verb::Accept:
        case Verb::HandIn:
        {
            Proto::Command c;
            if (a.verb == Verb::Accept)
                c.acceptOffer = a.mission;
            else
                c.completeMission = a.mission;
            clientLink_->Send(Proto::EncodeCommand(c));
            return;
        }
        default: break;
    }
    if (target == nullptr)
        return;  // gone since the menu opened
    switch (a.verb)
    {
        case Verb::Select:
            selected_ = target;
            desk_.SetOpen(WIN_TARGET, true);
            break;
        case Verb::Approach: OrderAutopilot(target->GetPosition(), a.distance); break;
        case Verb::Orbit:
        case Verb::Keep:
        case Verb::Follow: OrderHold(Actions::HoldMode(a.verb), target->GetId(), a.distance); break;
        case Verb::SetRange:
            // Any distance, not just the presets: the selected-item window's range field,
            // with the keyboard already in it.
            selected_ = target;
            holdRange_ = a.distance;
            holdRangeFor_ = target->GetId();
            desk_.SetOpen(WIN_TARGET, true);
            desk_.KeyboardFocus().Take(WIN_TARGET, "range");
            break;
        case Verb::Warp: OrderWarp(target->GetPosition(), a.distance); break;
        case Verb::Dock:
            // Docking is server-authoritative: in range we send the intent and the server
            // decides (including the reputation gate); out of range we approach first, which
            // is the whole point of the menu item -- the E key only works once already close.
            if (distanceTo(target) <= DockReach(*target))
                cmd_.dock = true;
            else
                OrderAutopilot(target->GetPosition(), target->GetSize() + 60.0f);
            break;
        case Verb::Mine:
            OrderAutopilot(target->GetPosition(), target->GetSize() + 30.0f);
            if (!snapshot_.player.mining)  // enable mining via command
                cmd_.toggleMining = true;
            break;
        case Verb::Dismantle:
            cmd_.dismantleId = target->GetId();  // the server checks reach and ownership
            break;
        case Verb::Attack:
            selected_ = target;
            desk_.SetOpen(WIN_TARGET, true);
            if (!weaponOn_)
                cmd_.toggleWeapon = true;
            weaponOn_ = true;
            break;
        case Verb::Investigate:
            if (distanceTo(target) <= target->GetSize() + 120.0f)
                cmd_.lootId = target->GetId();  // salvage order (server will verify)
            else                                // far — approach first
                OrderAutopilot(target->GetPosition(), target->GetSize() + 40.0f);
            break;
        case Verb::Jump:
            if (distanceTo(target) <= target->GetSize() + 200.0f)
                cmd_.jumpGateId = target->GetId();  // jump order (server will verify)
            else                                    // far — warp to the gate
                OrderWarp(target->GetPosition(), target->GetSize() + 120.0f);
            break;
        case Verb::FlyHere:
        case Verb::WarpHere:
        case Verb::Build:
        case Verb::Accept:
        case Verb::HandIn: break;  // handled above: they need no target
    }
}

// The right-click menu on an object: every action Actions::For offers for it, the same list
// the selected-item window shows as buttons.
void Game::OpenContextMenu(Entity* target)
{
    OpenContextMenu(target, GetMousePosition());
}

void Game::OpenContextMenu(Entity* target, Vector2 at)
{
    std::vector<ContextMenu::Item> items;
    const int                      id = target->GetId();
    for (const Actions::Action& a : Actions::For(ActionTarget(*target), playerShip_->GetPosition()))
        items.push_back({ a.label, [this, a, id]() { Perform(a, id, { 0.0f, 0.0f }); } });
    contextMenu_.Open(at, std::move(items));
}

// The right-click menu on empty space: fly or warp there, or build something.
void Game::OpenContextMenuAt(Vector2 worldPoint)
{
    std::vector<ContextMenu::Item> items;
    const std::vector<Blueprint>   none;
    for (const Actions::Action& a :
         Actions::ForPoint(worldPoint, playerShip_->GetPosition(),
                           mode_ == GameMode::Flying ? Blueprints::All() : none))
        items.push_back({ a.label, [this, a, worldPoint]() { Perform(a, 0, worldPoint); } });
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
    if (!IsKeyPressed(KEY_ESCAPE))  // a text field with the keyboard takes it (Desk::Escape)
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
