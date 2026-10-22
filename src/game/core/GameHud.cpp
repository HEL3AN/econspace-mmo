// Game — windows, HUD and the station screen.
//
// Presentation only: this file reads state and draws it, and holds no rules of its own.
// It is the largest of the three because a game is mostly interface, and keeping it apart
// is what keeps the other two readable.
//
// Part of the Game class; see Game.cpp.
#include "core/Game.h"
#include "render/TreatmentPanel.h"
#include "render/Perf.h"
#include "sim/Observation.h"
#include "sim/Overview.h"
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
#include "ui/Desk.h"
#include "ui/Input.h"
#include "ui/Paint.h"
#include "ui/Units.h"
#include "render/Textures.h"
#include "raymath.h"
#include <nlohmann/json.hpp>
#include <cmath>
#include <cstdio>
#include <string>
#include <algorithm>
#include <fstream>

// The menu bar (Neocom): a strip of window buttons down the left edge, as wide as the theme
// says, at the interface's scale.
static float MenuBarW()
{
    return Ui::Px(Ui::CurrentTheme().metrics.menuBar);
}

void Game::DrawStarfield()
{
    const float tileW = 2560.0f, tileH = 1440.0f;

    // The camera's motion this frame, in screen pixels. A jump of more than a screen is a
    // cut -- a new system, a recentre -- and the sky neither scrolls nor streaks across it.
    Vector2 move = { (camera_.target.x - skyLastTarget_.x) * camera_.zoom,
                     (camera_.target.y - skyLastTarget_.y) * camera_.zoom };
    if (!skyPrimed_ || fabsf(move.x) > tileW || fabsf(move.y) > tileH)
        move = { 0.0f, 0.0f };
    skyPrimed_ = true;
    skyLastTarget_ = camera_.target;
    skyScroll_.x = fmodf(skyScroll_.x + move.x, tileW * 100.0f);  // bounded, and a whole
    skyScroll_.y = fmodf(skyScroll_.y + move.y, tileH * 100.0f);  // number of tiles

    for (const BgStar& s : bgStars_)
    {
        float x = fmodf(s.base.x - skyScroll_.x * s.depth, tileW);
        float y = fmodf(s.base.y - skyScroll_.y * s.depth, tileH);
        if (x < 0)
            x += tileW;
        if (y < 0)
            y += tileH;
        if (x > screenWidth_ || y > screenHeight_)
            continue;
        const Color c{ s.shade, s.shade, s.shade, 255 };

        // Moving faster than a few pixels a frame, a point becomes a streak along the way it
        // came: in warp the sky shows the speed instead of strobing. Capped, so a star never
        // becomes a line across the screen.
        const float sx = move.x * s.depth, sy = move.y * s.depth;
        const float len = sqrtf(sx * sx + sy * sy);
        if (len < 3.0f)
        {
            DrawPixel((int)x, (int)y, c);
            continue;
        }
        const float k = std::min(len, 140.0f) / len;
        DrawLineEx({ x, y }, { x + sx * k, y + sy * k }, 1.0f, Fade(c, 0.75f));
    }
}

void Game::DrawWorld()
{
    DrawStarfield();
    Render::Perf::Mark(Render::Perf::Phase::Sky);

    BeginMode2D(camera_);

    // System boundary — a faint ring at the radius limit.
    DrawCircleLines(0, 0, World::SYSTEM_RADIUS, Fade(Ui::PANEL_BORDER, 0.5f));

    // The world is drawn from the client proxies (reconciled from the snapshot), not from
    // the server's live objects (M4c). Each proxy describes itself and the active backend
    // decides what that becomes (#35) — the client no longer knows how a station looks.
    {
        std::vector<Render::Item> scene;
        scene.reserve(clientWorld_.size());
        for (const auto& e : clientWorld_)
            scene.push_back(e->Describe());
        // The system lights itself (#119): the stars are already in the scene, because
        // they are in the layout the server sent on entry, so nothing new crosses the
        // wire. A system with two of them is lit by two of them.
        const Render::Lighting lights = Render::LightsFrom(scene);
        // The camera goes with them: a material shades a fragment by where it fell
        // relative to the object, so it has to know where the object landed (#121).
        // On the world's clock, so this station turns as it does for everyone else (#192).
        Render::Present(std::move(scene), lights, camera_, shapeBackend_,
                        worldClock_.Now(GetTime()));
    }
    Render::Perf::Mark(Render::Perf::Phase::World);

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
            if (e->GetKind() != EntityKind::Station || e->GetId() != m.destStationId)
                continue;
            Vector2 p = e->GetPosition();
            float   r = e->GetSize() + 16.0f;
            DrawCircleLines(p.x, p.y, r, GOLD);
            DrawCircleLines(p.x, p.y, r + 4.0f, Fade(GOLD, 0.4f));
            break;
        }
    }

    // The selected target's ring — at the object's rendered position, not the snapshot:
    // the body is drawn by interpolation "in the past", and a ring from the fresh snapshot
    // would run ahead of it. selected_ is a proxy, so its position is what was drawn.
    if (selected_ != nullptr)
        DrawCircleLines(selected_->GetPosition().x, selected_->GetPosition().y,
                        selected_->GetSize() + 10.0f, WHITE);

    // Where a plain move-to is heading is worth a marker. An orbit or keep-at-range steers
    // the autopilot at a point that runs ahead of the ship all the way round (#157); drawn,
    // that is a line the ship chases forever, and in EVE a ship in orbit simply orbits.
    if (playerShip_->IsAutopilotOn() && playerShip_->GetHoldMode() == HoldMode::None)
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

    // Drawn last and on its own, so it stays on top of the beams and range rings above.
    Render::Item ship = playerShip_->Describe();
    ship.pos = shipDrawPos_;  // between simulation steps, as the camera sees it
    ship.heading = shipDrawHeading_;
    shapeBackend_.Draw(ship);

    // The ship's marker is an instrument and is drawn with them, over the world in screen
    // space (DrawSpaceBrackets), so it reads the same at every zoom.

    EndMode2D();
    Render::Perf::Mark(Render::Perf::Phase::Overlay);
}

void Game::SetupWindows()
{
    using Ui::Anchor;
    using Ui::Layer;
    using Ui::WindowSpec;

    // A window the player arranges: placed from a corner, kept per account.
    auto panel =
        [](const char* id, const char* label, const char* tip, Anchor anchor, Rectangle place)
    {
        WindowSpec s;
        s.id = id;
        s.menuLabel = label;
        s.menuTip = tip;
        s.layer = Layer::Panels;
        s.anchor = anchor;
        s.place = place;
        s.persist = true;
        return s;
    };
    // Something drawn and kept elsewhere that still takes part in the order, the mouse and
    // Esc: a screen, the menu bar, a popup.
    auto surface = [](const char* id, const char* label, Layer layer, Ui::EscRule esc)
    {
        WindowSpec s;
        s.id = id;
        s.menuLabel = label;
        s.layer = layer;
        s.esc = esc;
        return s;
    };
    // Where the station's stack of tabs stands (#364): beside the station window, under the
    // location plate and the goal line.
    constexpr Rectangle DOCKED_STACK{ 344.0f, 96.0f, 520.0f, 500.0f };
    auto                fullScreen = [this]()
    { return Rectangle{ 0.0f, 0.0f, (float)screenWidth_, (float)screenHeight_ }; };

    // Registered in the menu bar's order. The places are the ones the windows have always
    // had at 1280 x 720; anchored, they now keep to their side at any other size.
    // The status window is laid out by Ui::Layout (#297), so it can be any size: resizable,
    // and no smaller than two gauges and a field.
    WindowSpec status = panel(WIN_STATUS, "Status", "Status: the ship and its pilot",
                              Anchor::TopLeft, { 72.0f, 8.0f, 264.0f, 312.0f });
    status.resizable = true;
    status.minSize = { 200.0f, 150.0f };
    desk_.AddWindow(status, "STATUS", true, [this](Ui::Frame& f) { DrawStatusContent(f); });
    // The selected item and the overview, one above the other on the right, as EVE has them:
    // pick from the list, act from the window above it (#297). Both are laid out by
    // Ui::Layout, so both can be any size.
    WindowSpec target = panel(WIN_TARGET, "Selected", "Selected item: what to do about it  (T)",
                              Anchor::TopRight, { 8.0f, 8.0f, 300.0f, 300.0f });
    target.resizable = true;
    target.minSize = { 240.0f, 160.0f };
    // The windows that look at space are space's: docked, they are put away and come back
    // as they were on undocking (#297). The station's windows take their place.
    target.context = CTX_SPACE;
    desk_.AddWindow(target, "SELECTED ITEM", false, [this](Ui::Frame& f) { DrawTargetContent(f); });
    // Open from the start: it is the instrument a player flies by (#157), and a player who
    // has to know a key exists before they can navigate has not been told how to play.
    WindowSpec overview = panel(WIN_OVERVIEW, "Overview", "Overview: what is in the system  (O)",
                                Anchor::TopRight, { 8.0f, 316.0f, 300.0f, 396.0f });
    overview.resizable = true;
    overview.minSize = { 260.0f, 160.0f };
    overview.context = CTX_SPACE;
    desk_.AddWindow(overview, "OVERVIEW", true, [this](Ui::Frame& f) { DrawOverviewContent(f); });
    // The radar is a picture drawn by hand inside a layout (#297): its toolbar is laid out,
    // the scope takes whatever room is left, so it can be any size.
    WindowSpec radar = panel(WIN_RADAR, "Radar", "Radar: the space around you  (R)",
                             Anchor::TopLeft, { 72.0f, 328.0f, 264.0f, 288.0f });
    radar.resizable = true;
    radar.minSize = { 180.0f, 180.0f };
    radar.context = CTX_SPACE;
    desk_.AddWindow(radar, "RADAR", false, [this](Ui::Frame& f) { DrawRadarContent(f); });
    // A list of missions and the chosen one in full: one above the other when narrow, side
    // by side when wide. A tab of the station's market (below), so docked it is beside the
    // station rather than on top of it; in space the same place is left of the selected item
    // and under the goal line, clear of the overview, which is open from the start.
    WindowSpec missions = panel(WIN_MISSIONS, "Missions", "Missions: work taken and offered  (J)",
                                Anchor::TopLeft, DOCKED_STACK);
    missions.resizable = true;
    missions.minSize = { 240.0f, 220.0f };
    missions.tabOf = WIN_MARKET;
    desk_.AddWindow(missions, "MISSIONS", false, [this](Ui::Frame& f) { DrawMissionsContent(f); });
    // Chat (#347): the local channel and private lines. Open from the start in space: other
    // people are what makes this an MMO, and a channel nobody knows is there is a channel
    // nobody talks in. Two windows showing the one log, one per context, because no place is
    // free in both: in space it is bottom left, under the status window; docked, that is the
    // station's and the stack beside it fills the rest, so there it waits for Enter or the
    // menu bar, and opens on the right, where the overview was.
    // The ship's scanner (#346): a pass, and what it found. Space's: docked, nothing scans.
    WindowSpec scanner = panel(WIN_SCANNER, "Scanner", "Scanner: find what is hidden  (P)",
                               Anchor::TopRight, { 316.0f, 316.0f, 300.0f, 300.0f });
    scanner.resizable = true;
    scanner.minSize = { 240.0f, 180.0f };
    scanner.context = CTX_SPACE;
    desk_.AddWindow(scanner, "SCANNER", false, [this](Ui::Frame& f) { DrawScannerContent(f); });
    WindowSpec chat = panel(WIN_CHAT, "Chat", "Chat: local and private  (Enter)",
                            Anchor::BottomLeft, { 72.0f, 8.0f, 440.0f, 210.0f });
    chat.resizable = true;
    chat.minSize = { 280.0f, 130.0f };
    chat.context = CTX_SPACE;
    desk_.AddWindow(chat, "CHAT", true, [this](Ui::Frame& f) { DrawChatContent(f); });
    WindowSpec chatDocked = chat;
    chatDocked.id = WIN_CHAT_DOCKED;
    chatDocked.anchor = Anchor::TopRight;
    chatDocked.place = { 8.0f, 96.0f, 400.0f, 320.0f };
    chatDocked.context = CTX_DOCKED;
    desk_.AddWindow(chatDocked, "CHAT", false, [this](Ui::Frame& f) { DrawChatContent(f); });
    // The corporation (#347): in space and docked alike, where the station's stack stands.
    WindowSpec corp = panel(WIN_CORP, "Corp", "Corporation: its members, invites, founding one",
                            Anchor::TopLeft, { 344.0f, 96.0f, 380.0f, 396.0f });
    corp.resizable = true;
    corp.minSize = { 280.0f, 220.0f };
    desk_.AddWindow(corp, "CORPORATION", false, [this](Ui::Frame& f) { DrawCorpContent(f); });

    // The two screens cover the world view and the windows; whichever opened last is on top.
    // Covering means the windows are not drawn at all while one is open: the map is drawn
    // translucent over the world, and the panels used to show through it (#297).
    auto screen = [&surface](const char* id, const char* label, const char* tip, Ui::EscRule esc)
    {
        WindowSpec s = surface(id, label, Layer::Screen, esc);
        s.menuTip = tip;
        s.covers = true;
        return s;
    };
    Ui::Desk::Surface map;
    map.bounds = fullScreen;
    map.draw = [this]() { DrawGalaxyMap(); };
    desk_.AddSurface(screen(WIN_MAP, "Map", "Galaxy map  (G)", Ui::EscRule::Close), false, map);
    Ui::Desk::Surface sensor;
    sensor.bounds = [this]()
    { return Rectangle{ MenuBarW(), 0.0f, screenWidth_ - MenuBarW(), (float)screenHeight_ }; };
    sensor.draw = [this]() { DrawSensorScreen(); };
    WindowSpec sensorSpec = screen(WIN_SENSOR, "Sensor", "Sensor screen  (V)", Ui::EscRule::Close);
    sensorSpec.context = CTX_SPACE;  // a readout of the space around the ship
    desk_.AddSurface(sensorSpec, false, sensor);

    WindowSpec settings = panel(WIN_SETTINGS, "Settings", "Settings: display, interface, keys",
                                Anchor::Top, { 0.0f, 100.0f, 360.0f, 330.0f });
    settings.resizable = true;
    settings.minSize = { 260.0f, 240.0f };
    settings.reopen = false;  // opened for a moment: never found open at the next login (#364)
    desk_.AddWindow(settings, "SETTINGS", false, [this](Ui::Frame& f) { DrawSettingsContent(f); });

    // Docked (#297): the station is windows like any other, in the "docked" context -- there
    // while the ship is berthed, put away as they were when it leaves. They used to be one
    // screen over everything; now they can be moved, resized, grouped and stacked as tabs,
    // and the missions window stands beside them. Where the target and overview windows
    // are in space, which are put away while docked.
    WindowSpec station =
        panel(WIN_STATION, "Station", "The station: its owner, your standing, undocking",
              Anchor::TopLeft, { 72.0f, 328.0f, 264.0f, 300.0f });
    station.resizable = true;
    station.minSize = { 220.0f, 200.0f };
    station.context = CTX_DOCKED;
    desk_.AddWindow(station, "STATION", true, [this](Ui::Frame& f) { DrawStationContent(f); });
    // Docking used to put these on top of each other (#364): now they tile -- the station
    // on the left under the status window, and beside it one stack, the market with the
    // hangar and the missions as its tabs, wide enough for each to lay out side by side.
    WindowSpec market = panel(WIN_MARKET, "Market", "Market: sell what is in the hold",
                              Anchor::TopLeft, DOCKED_STACK);
    market.resizable = true;
    market.minSize = { 240.0f, 220.0f };
    market.context = CTX_DOCKED;
    desk_.AddWindow(market, "MARKET", true, [this](Ui::Frame& f) { DrawMarketContent(f); });
    WindowSpec hangar = panel(WIN_HANGAR, "Hangar", "Hangar: switch, buy or refit a ship",
                              Anchor::TopLeft, DOCKED_STACK);
    hangar.resizable = true;
    hangar.minSize = { 240.0f, 220.0f };
    hangar.context = CTX_DOCKED;
    hangar.tabOf = WIN_MARKET;
    desk_.AddWindow(hangar, "HANGAR", true, [this](Ui::Frame& f) { DrawHangarContent(f); });

    // A ship was lost (#344): what went, what insurance paid and where the pilot woke. Opened
    // by the snapshot that first says so, in space or docked alike; never found open at login.
    WindowSpec loss = panel(WIN_LOSS, "", "", Anchor::Top, { 0.0f, 120.0f, 380.0f, 300.0f });
    loss.resizable = true;
    loss.minSize = { 280.0f, 220.0f };
    loss.reopen = false;
    // Above the station's windows, which come forward on the dock that comes with it.
    loss.layer = Layer::Overlay;
    desk_.AddWindow(loss, "SHIP LOST", false, [this](Ui::Frame& f) { DrawLossContent(f); });

    // Above the screens it opens; Esc passes over it. Docked too: it opens the station's
    // windows again once they have been closed.
    Ui::Desk::Surface bar;
    bar.bounds = [this]() { return Rectangle{ 0.0f, 0.0f, MenuBarW(), (float)screenHeight_ }; };
    bar.isOpen = [this]() { return !hudHidden_; };
    bar.draw = [this]() { DrawMenuBar(); };
    // A window dragged to the left snaps against it rather than sliding under it.
    WindowSpec barSpec = surface(WIN_MENUBAR, "", Layer::Modal, Ui::EscRule::Ignore);
    barSpec.snapTarget = true;
    desk_.AddSurface(barSpec, false, bar);

    Ui::Desk::Surface menu;
    menu.bounds = [this]() { return contextMenu_.Bounds(); };
    menu.isOpen = [this]() { return contextMenu_.IsOpen(); };
    menu.onClose = [this]() { contextMenu_.Close(); };
    menu.draw = [this]() { contextMenu_.Draw(); };
    desk_.AddSurface(surface(WIN_CONTEXT, "", Layer::Popup, Ui::EscRule::Close), false, menu);

    // F10's panel (#120): a window like the others, on the one layer the treatment never
    // reaches -- a settings window seen through the effect it is adjusting cannot be read
    // while adjusting it. It groups and stacks with nothing below it for the same reason.
    // No context: the look is tuned in space and docked alike.
    WindowSpec look = panel(WIN_LOOK, "", "", Anchor::TopRight, { 316.0f, 8.0f, 320.0f, 520.0f });
    look.layer = Layer::Overlay;
    look.resizable = true;
    look.minSize = { 260.0f, 200.0f };
    desk_.AddWindow(look, "SCREEN TREATMENT", false,
                    [this](Ui::Frame& f) { DrawTreatmentContent(f); });
}

// Changes the window size; if fullscreen mode is active — exits it first.
void Game::ApplyResolution(int w, int h)
{
    if (IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE))
        ToggleBorderlessWindowed();
    SetWindowSize(w, h);
    int mon = GetCurrentMonitor();
    SetWindowPosition((GetMonitorWidth(mon) - w) / 2, (GetMonitorHeight(mon) - h) / 2);

    // The windows keep to their anchors on their own (#297); the sizes are updated ahead of
    // time only because GetScreenWidth picks them up next frame.
    screenWidth_ = w;
    screenHeight_ = h;
}

// The overview (#157), on Ui::Layout (#297): the filters as tabs, and a table that sorts by
// its headings and scrolls -- the playtest found the old list cut off where the window
// ended. What goes in it and in what order is Overview::Build's, which a test can hold;
// this only draws it. Read from the snapshot (M4c); a click maps back to the proxy by id.
void Game::DrawOverviewContent(const Ui::Frame& f)
{
    using Ui::Box;
    using Ui::Size;
    const Ui::Theme& t = Ui::CurrentTheme();
    Ui::Layout&      L = overviewLayout_;

    const std::vector<Overview::Filter>& filters = Overview::AllFilters();
    std::vector<std::string>             tabs;
    for (Overview::Filter filter : filters)
        tabs.push_back(Overview::Label(filter));
    overviewTab_ = std::clamp(overviewTab_, 0, (int)filters.size() - 1);

    // The table's columns are the model's sorts.
    static const Overview::Sort      SORTS[] = { Overview::Sort::Name, Overview::Sort::Kind,
                                                 Overview::Sort::Distance };
    const std::vector<Overview::Row> rows = Overview::Build(
        snapshot_.entities, snapshot_.player.pos, filters[overviewTab_],
        SORTS[std::clamp(overviewSort_.column, 0, 2)], [this](const Proto::EntitySnapshot& e)
        { return e.kind == Proto::EntityKind::Npc && HostileToPlayerFaction(e.faction); });

    const int  selId = selected_ != nullptr ? selected_->GetId() : 0;
    const bool holding = playerShip_ && playerShip_->GetHoldMode() != HoldMode::None;
    const int  heldId = holding ? playerShip_->GetHoldTargetId() : 0;

    // Whose each row is, as this pilot sees it: the chip before the name (#361), in the
    // same standing colours as the radar and the brackets in the world view, and in the
    // radar's shapes so it never rests on colour alone (#362).
    const Sensor::Standing standing = ViewerStanding();
    auto                   chipOf = [&](const Proto::EntitySnapshot& e, Ui::Cell& cell)
    {
        Sensor::Allegiance a = Sensor::Classify(e, standing);
        if (e.kind == Proto::EntityKind::Structure)
        {
            const auto l = layout_.byId.find(e.id);
            const bool mine = l != layout_.byId.end() && l->second.owner == pilotName_;
            a = mine ? Sensor::Allegiance::Own : Sensor::Allegiance::Neutral;
            // A corp mate's is a friend's (#347): their batteries hold fire for this pilot.
            if (!mine && l != layout_.byId.end() && InMyCorp(l->second.owner))
                a = Sensor::Allegiance::Friendly;
        }
        else if (e.kind == Proto::EntityKind::PlayerShip && InMyCorp(e.name))
            a = Sensor::Allegiance::Friendly;
        if (a == Sensor::Allegiance::Unowned)
            return;
        cell.chip = Sensor::ColorOf(a);
        cell.chipMark = StandingMark(a);
    };

    Ui::TableSpec table;
    table.id = "overview";
    table.columns = { { "name", Size::Grow(), Ui::Align::Start, true, true },
                      { "type", Size::Fit(0.0f, 96.0f), Ui::Align::Start, true },
                      { "dist", Size::Fit(), Ui::Align::End, true } };
    table.rows = (int)rows.size();
    table.sort = &overviewSort_;
    table.empty = filters[overviewTab_] == Overview::Filter::Hostile ? "nothing hostile in sight"
                                                                     : "nothing here";
    table.cell = [&](int r, int c)
    {
        const Overview::Row& row = rows[r];
        const bool           held = row.entity->id != 0 && row.entity->id == heldId;
        switch (c)
        {
            // Hostiles in the instruments' hostile colour, because this list is where
            // allegiance belongs: the instrument, not the world view (#117). What the ship is
            // holding station on is marked: a standing order with no visible sign of running
            // is an order a player cannot trust.
            case 0:
            {
                // Another pilot under their corporation's ticker (#347).
                const std::string name =
                    row.entity->kind == Proto::EntityKind::PlayerShip ? Tagged(row.name) : row.name;
                // The convoy an escort job has this pilot fly with (#430), in the accent: it
                // is what the pilot is here for, and the list is where that is said (#117).
                const bool convoy =
                    Obs::ConvoyPartOf(snapshot_, row.entity->id) != Obs::ConvoyPart::None;
                Ui::Cell cell{ name, row.hostile      ? t.standing.hostile
                                     : held || convoy ? t.colors.accent
                                                      : t.colors.text };
                chipOf(*row.entity, cell);
                return cell;
            }
            case 1:
                switch (Obs::ConvoyPartOf(snapshot_, row.entity->id))
                {
                    case Obs::ConvoyPart::Hauler: return Ui::Cell{ "your convoy", t.colors.accent };
                    case Obs::ConvoyPart::Escort:
                        return Ui::Cell{ "convoy escort", t.colors.accent };
                    case Obs::ConvoyPart::None: break;
                }
                return Ui::Cell{ row.kind, t.colors.dim };
            default: return Ui::Cell{ Ui::Distance(row.distance), t.colors.dim };
        }
    };
    table.rowFill = [&](int r)
    {
        const int id = rows[r].entity->id;
        if (id != 0 && id == selId)
            return t.colors.selected;
        if (id != 0 && id == heldId)
            return t.colors.hover;
        return Color{ 0, 0, 0, 0 };
    };
    // The whole name and the exact distance, for a row cut short.
    table.tooltip = [&](int r)
    { return std::string(TextFormat("%s  -  %.0f", rows[r].name.c_str(), rows[r].distance)); };

    L.Begin(f);
    L.Column(Box().Grow().Gap(t.metrics.gap),
             [&]
             {
                 L.Tabs("tabs", tabs, overviewTab_);
                 const Ui::TableEvents ev = L.Table(table);
                 // Left: select it. Right: what to do about it -- approach, orbit, warp, dock.
                 const int hit = ev.clicked >= 0 ? ev.clicked : ev.rightClicked;
                 if (hit >= 0)
                 {
                     selected_ = FindEntityById(rows[hit].entity->id);
                     if (selected_ != nullptr && ev.clicked >= 0)
                         desk_.SetOpen(WIN_TARGET, true);
                     else if (selected_ != nullptr)
                         OpenContextMenu(selected_);
                 }
             });
    L.End();
    L.Draw();
}

// The selected-item window (#297), as EVE has it: what the target is, and what to do about
// it. The buttons are Actions::For -- the list the right-click menu shows and the agent's
// tools mirror -- and the range control is the one the orbit, keep and follow buttons use,
// the player's own distance rather than one of the menu's three presets (#298).
void Game::DrawTargetContent(const Ui::Frame& f)
{
    using Ui::Box;
    using Ui::Size;
    using Ui::TextStyle;
    const Ui::Theme& t = Ui::CurrentTheme();
    Ui::Layout&      L = targetLayout_;

    // Read the selected target from the snapshot by id (M4c). If it's not there (vanished),
    // there's no target.
    const int                    selId = selected_ != nullptr ? selected_->GetId() : 0;
    const Proto::EntitySnapshot* e = nullptr;
    for (const auto& es : snapshot_.entities)
        if (selId != 0 && es.id == selId)
        {
            e = &es;
            break;
        }

    L.Begin(f);
    if (e == nullptr)
    {
        L.Text("No target selected", TextStyle::Body().Tint(t.colors.dim));
        L.End();
        L.Draw();
        return;
    }

    // A new target starts at its own default range; the player's choice stays with the
    // target it was made for.
    if (holdRangeFor_ != e->id)
    {
        holdRange_ = Actions::DefaultRange(e->size);
        holdRangeFor_ = e->id;
    }
    // From just outside the object to far enough to stand off a planet; nearer than anything
    // a sublight hold would take minutes to cross. The overview and a warp cover the rest.
    const float lo = std::max(e->size * 1.1f, 30.0f);
    const float hi = std::max(60000.0f, lo * 4.0f);
    holdRange_ = std::clamp(holdRange_, lo, hi);

    const std::vector<Actions::Action> actions =
        selected_ != nullptr ? Actions::For(ActionTarget(*selected_), playerShip_->GetPosition())
                             : std::vector<Actions::Action>();
    const float              dist = Vector2Distance(e->pos, snapshot_.player.pos);
    const Sensor::Allegiance allegiance = Sensor::Classify(*e, ViewerStanding());
    const bool               heldHere =
        playerShip_->GetHoldMode() != HoldMode::None && playerShip_->GetHoldTargetId() == e->id;

    L.Column(
        Box().Grow().ScrollY().Id("target").Gap(t.metrics.gap),
        [&]
        {
            L.Text(e->name.empty() ? Overview::KindWord(*e) : e->name, TextStyle::Title());
            L.Field("Distance", TextFormat("%.0f", dist), t.colors.text);

            if (e->kind == Proto::EntityKind::Npc || e->kind == Proto::EntityKind::Station)
                // Allegiance, in the instruments' colours (#117).
                L.Field("Faction", FactionName(e->faction), Sensor::ColorOf(allegiance));
            // Another pilot's corporation (#347), a corp mate's in the friendly colour.
            if (e->kind == Proto::EntityKind::PlayerShip)
            {
                const std::string tick = TickerOf(e->name);
                const bool        mate = InMyCorp(e->name);
                L.Field("Corporation",
                        tick.empty() ? std::string("none")
                        : mate       ? corpView_.name + " [" + tick + "]"
                                     : "[" + tick + "]",
                        mate ? t.standing.friendly : t.colors.text);
            }
            if (e->kind == Proto::EntityKind::Npc)
            {
                L.Field("Hull", TextFormat("%.0f%%", e->hullFrac * 100.0f), t.colors.text);
                L.Bar(e->hullFrac, e->hullFrac > 0.3f ? t.colors.good : t.colors.bad);
            }
            // A hauler's hold, or what its wreck still holds (#343).
            if ((e->kind == Proto::EntityKind::Npc || e->kind == Proto::EntityKind::Derelict) &&
                e->cargo >= 0 && e->cargo < Goods::Count() && e->cargoAmount > 0)
                L.Field(e->kind == Proto::EntityKind::Npc ? "Cargo" : "Holds",
                        TextFormat("%d %s", e->cargoAmount,
                                   ResourceName(ResourceType::FromIndex(e->cargo)).c_str()),
                        t.colors.text);
            else if (e->kind == Proto::EntityKind::Structure)
            {
                const auto l = layout_.byId.find(e->id);
                if (l != layout_.byId.end())
                {
                    const Proto::EntityLayout& el = l->second;
                    const bool                 mine = el.owner == pilotName_;
                    L.Field("Built by", mine ? "you" : Tagged(el.owner),
                            mine                 ? t.standing.own
                            : InMyCorp(el.owner) ? t.standing.friendly
                                                 : t.colors.text);
                    const double now = worldClock_.Now(GetTime());
                    if (el.completesAt > now)
                    {
                        // The site's progress is the clock's, not a figure the server sends
                        // (#136).
                        const double span = el.completesAt - el.startedAt;
                        const float  p = span > 0.0 ? (float)((now - el.startedAt) / span) : 1.0f;
                        L.Field("Building", TextFormat("%.0f s left", el.completesAt - now),
                                t.colors.dim);
                        L.Bar(Clamp(p, 0.0f, 1.0f), t.colors.warn);
                    }
                    else if (el.expiresAt > 0.0)
                        L.Field("Stands", TextFormat("%.0f min more", (el.expiresAt - now) / 60.0),
                                t.colors.dim);
                    // Its hull, from the proxy, which knows its blueprint and its time line: a
                    // site's grows as it goes up (#39).
                    if (const Entity* p = FindEntityById(e->id))
                        if (p->GetKind() == EntityKind::Structure)
                        {
                            const Structure& s = *static_cast<const Structure*>(p);
                            const float      h = s.HullFraction(now);
                            L.Field("Hull", TextFormat("%.0f%%", h * 100.0f), t.colors.text);
                            L.Bar(h, h > 0.3f ? t.colors.good : t.colors.bad);
                            // What it mends (#431 D1): fire below this rate does nothing.
                            if (s.MendRate() > 0.0f)
                                L.Field("Mends", TextFormat("%.0f hull/s", s.MendRate()),
                                        t.colors.dim);
                            else if (!s.Mends())
                                L.Field("Mends", "nothing: unsupplied", t.colors.warn);
                        }
                    // Its owner sets its rules, and so does their corporation's CEO or a
                    // director (#347).
                    if (ManagesFor(el.owner) && selected_ != nullptr &&
                        ActionTarget(*selected_).armed)
                        DrawDefenceControls(L, el);
                }
            }
            else if (e->kind == Proto::EntityKind::Field && e->ore >= 0)
            {
                L.Field("Ore", ResourceName(ResourceType::FromIndex(e->ore)), t.colors.text);
                // What is left (#348): mined by everyone, growing back slowly.
                if (e->oreMax > 0)
                {
                    const float left = (float)e->oreLeft / (float)e->oreMax;
                    L.Field("Left", TextFormat("%d of %d", e->oreLeft, e->oreMax),
                            left > 0.3f ? t.colors.text : t.colors.warn);
                    L.Bar(Clamp(left, 0.0f, 1.0f), left > 0.3f ? t.colors.good : t.colors.warn);
                }
            }
            else if (e->kind == Proto::EntityKind::Gate)
            {
                // An unstable mouth (#342 S4): counted down by the clock, as a site is, and
                // only for small ships until somebody anchors it.
                const auto l = layout_.byId.find(e->id);
                if (l != layout_.byId.end() && l->second.expiresAt > 0.0)
                {
                    const double left =
                        std::max(0.0, l->second.expiresAt - worldClock_.Now(GetTime()));
                    L.Field(
                        "Collapses in",
                        TextFormat("%d h %02d min", (int)(left / 3600.0), (int)(left / 60.0) % 60),
                        t.colors.warn);
                    L.Field("Passage", "small ships only", t.colors.dim);
                }
            }

            // --- Holding station at the player's own range ---------------------------
            if (e->id != 0)
            {
                L.Divider();
                L.Text("RANGE", TextStyle::Label());
                L.Row(Box().GrowX().Gap(t.metrics.gap).Align(Ui::Align::Start, Ui::Align::Center),
                      [&]
                      {
                          // By ratio: 200 to 400 is as large a step as 20 000 to 40 000.
                          L.Column(Box().GrowX(),
                                   [&] { L.Slider("rangeSlider", holdRange_, lo, hi, true); });
                          L.Column(Box().Width(Size::Fixed(72.0f)),
                                   [&] { L.NumberField("range", holdRange_, lo, hi); });
                      });
                L.Row(Box().GrowX().Gap(t.metrics.rowGap),
                      [&]
                      {
                          struct Hold
                          {
                              const char*   id;
                              Actions::Verb verb;
                              const char*   tip;
                          };
                          // Named as the menu names them (#365): the range is beside them.
                          static const Hold holds[] = {
                              { "orbit", Actions::Verb::Orbit, "Circle it at this range" },
                              { "keep", Actions::Verb::Keep,
                                "Hold this distance from it, moving only as much as that takes" },
                              { "follow", Actions::Verb::Follow,
                                "Hold this distance and match its velocity" },
                          };
                          for (const Hold& h : holds)
                          {
                              // Lit while it is the order the ship is flying.
                              const bool on = heldHere && Actions::HoldMode(h.verb) ==
                                                              (int)playerShip_->GetHoldMode() + 2;
                              if (L.Button(h.id, Actions::Name(h.verb), on))
                              {
                                  Actions::Action a;
                                  a.verb = h.verb;
                                  a.distance = holdRange_;
                                  Perform(a, e->id, { 0.0f, 0.0f });
                              }
                              L.Tooltip(h.id, h.tip);
                          }
                      });
            }

            // --- Everything else that can be done about it ----------------------------
            // Two to a row: the list is the menu's, less what this window already shows.
            std::vector<const Actions::Action*> rest;
            for (const Actions::Action& a : actions)
                if (a.verb != Actions::Verb::Select && a.verb != Actions::Verb::SetRange &&
                    Actions::HoldMode(a.verb) == 0)
                    rest.push_back(&a);
            if (!rest.empty())
                L.Divider();
            for (size_t i = 0; i < rest.size(); i += 2)
                L.Row(Box().GrowX().Gap(t.metrics.rowGap),
                      [&]
                      {
                          for (size_t k = i; k < std::min(rest.size(), i + 2); k++)
                          {
                              const std::string id = "act" + std::to_string(k);
                              if (L.Button(id, rest[k]->label))
                                  Perform(*rest[k], e->id, { 0.0f, 0.0f });
                          }
                      });
        });
    L.End();
    L.Draw();
}

// One's own armed structure's rules of engagement (#340): the four modes, the threshold or
// the allow-list the chosen one reads, and what it costs in this system -- the law is the
// system's holder's, as the server will judge it. A change is sent at once; the layout's
// delta says when the server has taken it.
void Game::DrawDefenceControls(Ui::Layout& L, const Proto::EntityLayout& el)
{
    using Ui::Box;
    using Ui::TextStyle;
    const Ui::Theme& t = Ui::CurrentTheme();
    const Defence&   now = el.defence;
    if (defenceFor_ != el.id || defenceSeen_ != now)
    {
        defenceFor_ = el.id;
        defenceSeen_ = now;
        defenceThreshold_ = now.threshold;
        defenceList_ = AllowListText(now);
    }
    Defence want = now;
    bool    send = false;

    L.Divider();
    L.Text("RULES OF ENGAGEMENT", TextStyle::Label());
    static const Engage modes[] = { Engage::Hold, Engage::Return, Engage::Enemies,
                                    Engage::Strangers };
    static const char*  tips[] = {
        "Never fires, not even back",
        "Fires back at whoever shot at it in the last minute",
        ("Also fires first at factions you stand below the threshold with, and at players "
         "wanted by a lawful faction"),
        "Fires at everyone but you and the names on the allow-list",
    };
    for (int row = 0; row < 2; row++)
        L.Row(Box().GrowX().Gap(t.metrics.rowGap),
              [&]
              {
                  for (int k = row * 2; k < row * 2 + 2; k++)
                  {
                      const std::string id = std::string("roe_") + EngageName(modes[k]);
                      if (L.Button(id, EngageLabel(modes[k]), now.engage == modes[k]) &&
                          now.engage != modes[k])
                      {
                          want.engage = modes[k];
                          send = true;
                      }
                      L.Tooltip(id, tips[k]);
                  }
              });
    if (now.engage == Engage::Enemies)
        L.FormRow("Below standing",
                  [&]
                  {
                      if (L.NumberField("roe_threshold", defenceThreshold_, Defence::THRESHOLD_MIN,
                                        Defence::THRESHOLD_MAX))
                      {
                          want.threshold = defenceThreshold_;
                          send = true;
                      }
                  });
    if (now.engage == Engage::Strangers)
    {
        L.Text("Holds fire for (players and factions, by commas):",
               TextStyle::Body().Tint(t.colors.dim));
        Ui::TextFieldOptions opt;
        opt.placeholder = "nobody";
        opt.maxLength = 512;
        if (L.TextField("roe_list", defenceList_, opt).submitted)
        {
            ReadAllowList(defenceList_, want);
            send = true;
        }
    }

    // What the chosen mode costs here: the law the server will judge it by.
    bool      lawful = false;
    FactionId law = Factions::Unaffiliated();
    for (const Proto::GalaxySystemStat& g : galaxyState_.systems)
        if (g.id == layout_.systemId)
        {
            law = g.controller;
            lawful = g.claimed && Factions::IsLawful(law);
        }
    const bool costly =
        lawful && (now.engage == Engage::Enemies || now.engage == Engage::Strangers);
    L.Text(DefenceCost(now.engage, lawful, law),
           TextStyle::Body().Wrap().Tint(costly ? t.colors.warn : t.colors.dim));

    if (send && want != now)
    {
        cmd_.defenceId = el.id;
        cmd_.defence = want;
    }
}

// Menu bar input: clicking a button toggles the window or screen it stands for. The buttons
// are the desk's registry in order (#297), so a window registered with a label has one.
void Game::HandleMenuBar()
{
    // The bar is laid out while it is drawn, which is where a click on it is seen; the window
    // it stands for is opened or closed here, before anything else is drawn.
    if (!menuToggle_.empty())
        desk_.Toggle(menuToggle_);
    menuToggle_.clear();
    // A theme picked in Settings, likewise switched to between frames: it rasterises every
    // font again, which a layout half declared in the old one must not see.
    if (!themeToUse_.empty() && Ui::UseTheme(themeToUse_))
    {
        Ui::SetThemeSetting(themeToUse_);
        if (uiSettingsWritable_ && !Ui::SaveUiSettings(uiSettingsPath_))
            TraceLog(LOG_WARNING, "UI settings: cannot write %s", uiSettingsPath_.c_str());
    }
    themeToUse_.clear();
}

// The menu bar down the left edge, laid out like a window (#297): one button per window,
// named in a word rather than a three-letter code, the open ones lit, and the key that opens
// each one said when the cursor rests on it.
void Game::DrawMenuBar()
{
    using Ui::Box;
    using Ui::Size;
    const Ui::Theme& t = Ui::CurrentTheme();
    Ui::Layout&      L = menuLayout_;
    const Rectangle  area{ 0.0f, 0.0f, MenuBarW(), (float)screenHeight_ };

    const std::vector<Ui::Desk::MenuSlot> slots = desk_.MenuSlots();
    L.Begin(desk_.SurfaceFrame(WIN_MENUBAR, area));
    L.Column(Box()
                 .Grow()
                 .Pad(t.metrics.rowGap, t.metrics.gap)
                 .Gap(t.metrics.rowGap)
                 .Fill(t.colors.title),
             [&]
             {
                 for (size_t i = 0; i < slots.size(); i++)
                 {
                     const bool open = slots[i].open;
                     const bool hover = L.Hovered("menu", (uint32_t)i);
                     if (L.Clicked("menu", (uint32_t)i))
                         menuToggle_ = slots[i].id;
                     L.Row(Box()
                               .Id("menu", (uint32_t)i)
                               .GrowX()
                               .Height(Size::Fixed(t.metrics.buttonHeight + t.metrics.gap))
                               .Align(Ui::Align::Center, Ui::Align::Center)
                               .Fill(open    ? t.colors.selected
                                     : hover ? t.colors.hover
                                             : t.colors.panel)
                               .Border(open || hover ? t.colors.accent : t.colors.border,
                                       t.metrics.border)
                               .ThemeCorners(),
                           [&]
                           {
                               Ui::TextStyle st = Ui::TextStyle::Caption().Tint(
                                   open || hover ? t.colors.accent : t.colors.text);
                               st.size = t.fontSize.small;
                               L.Text(slots[i].label, st);
                           });
                     L.Tooltip("menu", slots[i].tip, (uint32_t)i);
                 }
             });
    L.End();
    L.Draw();
    DrawLineEx({ area.width, 0.0f }, { area.width, area.height }, 1.0f, t.colors.border);
}

// The status window, declared with Ui::Layout (#297) -- the first window that is, and the
// pattern for the rest: no coordinates, every size and colour from the theme, and a layout
// that follows the window. Narrow, it is one column; wide enough, the ship's condition and
// its systems sit side by side. Too short for its content, it scrolls rather than clipping.
//
// Everything is read from the snapshot or the account mirror; nothing here gives an order.
void Game::DrawStatusContent(const Ui::Frame& f)
{
    using Ui::Box;
    using Ui::Size;
    using Ui::TextStyle;
    const Ui::Theme& t = Ui::CurrentTheme();
    Ui::Layout&      L = statusLayout_;

    const float   shFrac = playerShip_->GetShields() / playerShip_->GetMaxShields();
    const float   hFrac = playerShip_->GetHull() / playerShip_->GetMaxHull();
    const Skills& sk = player_.GetSkills();
    // Toggles from the snapshot (server-authoritative; the predicted playerShip_ would
    // flicker over the network due to replaying one-shot commands).
    const bool stab = snapshot_.player.stabilizer;
    const bool mine = snapshot_.player.mining;

    // A labelled bar: what it measures, how much, and the bar itself.
    auto gauge = [&](const char* label, float frac, Color c)
    {
        L.Column(Box().GrowX().Gap(t.metrics.rowGap),
                 [&]
                 {
                     L.Field(label, TextFormat("%.0f%%", frac * 100.0f), t.colors.text);
                     L.Bar(frac, c);
                 });
    };
    auto condition = [&]
    {
        L.Column(Box().GrowX().Gap(t.metrics.gap),
                 [&]
                 {
                     gauge("Shields", shFrac, t.colors.accent);
                     gauge("Hull", hFrac, hFrac > 0.3f ? t.colors.good : t.colors.bad);
                     L.Divider();
                     L.Field("Speed", TextFormat("%.0f", playerShip_->GetSpeed()), t.colors.text);
                     L.Field("Money", TextFormat("%.0f", player_.GetMoney()), t.colors.money);
                     // From the snapshot: the server collects ore into its own ship.
                     L.Field("Cargo",
                             TextFormat("%d / %d", snapshot_.player.cargoUsed,
                                        snapshot_.player.cargoCap),
                             t.colors.text);
                 });
    };
    auto systems = [&]
    {
        L.Column(Box().GrowX().Gap(t.metrics.gap),
                 [&]
                 {
                     L.Field("Skills",
                             TextFormat("P%d  M%d  T%d", sk.GetLevel(SkillType::Piloting),
                                        sk.GetLevel(SkillType::Mining),
                                        sk.GetLevel(SkillType::Trading)),
                             t.colors.dim);
                     L.Row(Box().GrowX().Gap(t.metrics.rowGap),
                           [&]
                           {
                               L.Chip("STAB", stab, t.colors.accent);
                               L.Chip("MINE", mine, t.colors.accent);
                               L.Chip("WPN", weaponOn_, t.colors.accent);
                               L.Chip("AUTO", playerShip_->IsAutopilotOn(), t.colors.good);
                           });
                 });
    };

    // Wide enough for two columns of fields that each still read as label ... value.
    const bool wide = f.Area().width >= Ui::Px(420.0f);
    L.Begin(f);
    L.Column(Box().Grow().ScrollY().Id("status").Gap(t.metrics.gap),
             [&]
             {
                 if (wide)
                     L.Row(Box().GrowX().Gap(t.metrics.padding * 2.0f),
                           [&]
                           {
                               condition();
                               systems();
                           });
                 else
                 {
                     condition();
                     L.Divider();
                     systems();
                 }
             });
    L.End();
    L.Draw();
}

// A scale bar, bottom right, and a reminder of how to get back when the camera has been
// taken off the ship. In a system a million units across (#159) a player cannot tell what
// they are looking at without the first, and cannot find their ship without the second.
void Game::DrawScaleBar()
{
    const Render::ScaleBar bar = Render::ScaleBarFor(camera_.zoom, 160.0f);
    if (bar.pixels <= 0.0f)
        return;
    const float x1 = (float)screenWidth_ - 28.0f, x0 = x1 - bar.pixels;
    const float y = (float)screenHeight_ - 30.0f;
    DrawLineEx({ x0, y }, { x1, y }, 2.0f, Ui::TEXT_DIM);
    DrawLineEx({ x0, y - 5.0f }, { x0, y + 5.0f }, 2.0f, Ui::TEXT_DIM);
    DrawLineEx({ x1, y - 5.0f }, { x1, y + 5.0f }, 2.0f, Ui::TEXT_DIM);

    const float w = bar.worldLength;
    const char* label = w >= 1000.0f ? TextFormat("%gk", w / 1000.0f) : TextFormat("%g", w);
    Ui::Text(label, (int)(x0 + (bar.pixels - Ui::TextWidth(label, 13)) * 0.5f), (int)y - 20, 13,
             Ui::TEXT_DIM);

    if (!rig_.Following())
        Ui::Text("camera free  ·  [C] back to ship", (int)x0 - 120, (int)y + 8, 11, Ui::ACCENT);
}

float Game::ShownSecurity(const std::string& id, float base) const
{
    for (const Proto::GalaxySystemStat& g : galaxyState_.systems)
        if (g.id == id)
            return g.security;
    return base;
}

double Game::MarketFeeHere() const
{
    for (const Proto::GalaxySystemStat& g : galaxyState_.systems)
        if (g.id == layout_.systemId)
            return g.FeeOr((float)Goods::Rules().fee);
    return Goods::Rules().fee;
}

// Where the ship is, at the top of the screen: the system, how safe it is, and who wants the
// pilot. A plate laid out like a window's content (#297) rather than coloured text loose on
// the world, and drawn under the windows, so a window over it hides it instead of the two
// running through each other.
void Game::DrawLocation(const Goal::Prompt& goal)
{
    using Ui::Box;
    using Ui::TextStyle;
    const WorldLoader::SystemInfo* si = CurrentSystemInfo();
    if (si == nullptr && goal.text.empty())
        return;
    const Ui::Theme& t = Ui::CurrentTheme();
    std::string      wanted;
    for (FactionId f : Factions::All())
        if (player_.IsWanted(f))
            wanted += (wanted.empty() ? "" : ", ") + FactionName(f);

    Ui::Layout& L = locationLayout_;
    L.Begin({ MenuBarW(), 0.0f, screenWidth_ - MenuBarW(), (float)screenHeight_ }, Ui::Scale());
    L.Column(
        Box()
            .GrowX()
            .Pad(0.0f, t.metrics.gap)
            .Gap(t.metrics.rowGap)
            .Align(Ui::Align::Center, Ui::Align::Start),
        [&]
        {
            if (si != nullptr)
            {
                const float sec = ShownSecurity(si->id, si->security);
                const char* tier = sec >= 0.7f   ? "High"
                                   : sec >= 0.4f ? "Mid"
                                   : sec >= 0.2f ? "Low"
                                                 : "Null";
                const Color col = sec >= 0.7f   ? t.colors.good
                                  : sec >= 0.4f ? t.colors.accent
                                  : sec >= 0.2f ? t.colors.warn
                                                : t.colors.bad;
                L.Column(
                    Box()
                        .Pad(t.metrics.padding, t.metrics.rowGap)
                        .Align(Ui::Align::Center, Ui::Align::Start)
                        .Fill(t.colors.panel)
                        .Border(t.colors.border, t.metrics.border)
                        .ThemeCorners(),
                    [&]
                    {
                        L.Row(Box().Gap(t.metrics.gap).Align(Ui::Align::Start, Ui::Align::Center),
                              [&]
                              {
                                  L.Text(si->name, TextStyle::Strong());
                                  L.Text(TextFormat("%s security  %.1f", tier, sec),
                                         TextStyle::Label().Tint(col));
                              });
                        if (!wanted.empty())
                            L.Text("Wanted by " + wanted, TextStyle::Label().Tint(t.colors.bad));
                    });
            }
            // What to do next (#364), under where the player is: a line, not a window, so
            // it asks for nothing and is gone once the loop is learned.
            if (!goal.text.empty())
                L.Row(Box()
                          .Pad(t.metrics.padding, t.metrics.rowGap)
                          .Gap(t.metrics.gap)
                          .Align(Ui::Align::Start, Ui::Align::Center)
                          .Fill(t.colors.panel)
                          .Border(t.colors.accent, t.metrics.border)
                          .ThemeCorners(),
                      [&]
                      {
                          L.Text("NEXT", TextStyle::Label().Tint(t.colors.accent));
                          L.Text(goal.text, TextStyle::Body());
                      });
        });
    L.End();
    L.Draw();
}

// The goal's pointer (#364): docked, a frame round the missions window it sends the player
// to; in space, a chevron beside the ship turned toward what it names, and a bracket on that
// thing when it is on the screen. In the accent colour: it is the interface speaking, not a
// standing (#117).
void Game::DrawGoalPointer(const Goal::Prompt& goal)
{
    const Ui::Theme& t = Ui::CurrentTheme();
    const float      thin = std::max(1.0f, Ui::Px(1.0f));
    const float      pulse = 0.6f + 0.4f * std::sin((float)GetTime() * 4.0f);
    const Color      col = Fade(t.colors.accent, pulse);
    if (goal.missionsWindow)
    {
        const int h = desk_.Layout().Find(WIN_MISSIONS);
        if (h != Ui::DeskLayout::NONE && desk_.Layout().Shown(h))
        {
            const Rectangle r = desk_.Layout().Rect(h);
            const float     m = Ui::Px(3.0f);
            DrawRectangleLinesEx({ r.x - m, r.y - m, r.width + 2.0f * m, r.height + 2.0f * m },
                                 2.0f * thin, col);
        }
        return;
    }
    const Entity* target = goal.pointAt != 0 ? FindEntityById(goal.pointAt) : nullptr;
    if (target == nullptr || mode_ != GameMode::Flying)
        return;
    const Vector2 ship = GetWorldToScreen2D(shipDrawPos_, camera_);
    const Vector2 at = GetWorldToScreen2D(target->GetPosition(), camera_);
    const Vector2 d{ at.x - ship.x, at.y - ship.y };
    const float   len = Vector2Length(d);
    const float   r = std::max(target->GetSize() * camera_.zoom + Ui::Px(8.0f), Ui::Px(14.0f));
    const bool    onScreen =
        at.x > -r && at.y > -r && at.x < screenWidth_ + r && at.y < screenHeight_ + r;
    if (onScreen)
        DrawCircleLinesV(at, r, col);
    // Beside the ship unless the target is right there with it.
    if (len < Ui::Px(60.0f))
        return;
    const Vector2 dir{ d.x / len, d.y / len };
    const Vector2 side{ -dir.y, dir.x };
    const float   at0 = Ui::Px(34.0f), size = Ui::Px(7.0f);
    const Vector2 tip{ ship.x + dir.x * (at0 + size), ship.y + dir.y * (at0 + size) };
    const Vector2 back{ ship.x + dir.x * at0, ship.y + dir.y * at0 };
    DrawLineEx(tip, { back.x + side.x * size, back.y + side.y * size }, 2.0f * thin, col);
    DrawLineEx(tip, { back.x - side.x * size, back.y - side.y * size }, 2.0f * thin, col);
}

namespace
{
// Four corner marks around a square `r` from its centre to each side: a bracket, which
// says "this one" without covering what it marks.
void CornerBracket(Vector2 c, float r, float arm, float thick, Color col)
{
    for (int sx = -1; sx <= 1; sx += 2)
        for (int sy = -1; sy <= 1; sy += 2)
        {
            const Vector2 k{ c.x + sx * r, c.y + sy * r };
            DrawLineEx(k, { k.x - sx * arm, k.y }, thick, col);
            DrawLineEx(k, { k.x, k.y - sy * arm }, thick, col);
        }
}

// 0 at or below `lo`, 1 at or above `hi`, smooth between, on a log scale: the zoom and the
// distances here run over three orders of magnitude.
float LogRamp(float v, float lo, float hi)
{
    if (v <= lo)
        return 0.0f;
    if (v >= hi)
        return 1.0f;
    const float t = std::log(v / lo) / std::log(hi / lo);
    return t * t * (3.0f - 2.0f * t);
}
}  // namespace

// The marker on the player's own ship and the standing brackets on the others (#361).
//
// The world view keeps objects in their art colours (#117), and that left a ship thirty
// pixels long in the colour of the station behind it, and no way to see from the picture
// which of the ships around it would shoot. Both are instrument readings, as in EVE: the
// bracket says whose a ship is *to this pilot*, in the theme's standing colours that the
// radar and the overview use, and nothing about the ship itself changes.
//
// The rules. Sizes are pixels through the interface's scale, so a bracket reads the same at
// every zoom: never smaller than a few pixels round the ship, never inside a ship drawn
// large. Your own ship is always marked. Another is bracketed while it is on screen;
// zoomed out to a whole system, and far from you, a bracket fades -- a hundred full-strength
// brackets over a system map would be the only thing on it -- except a hostile one, which
// keeps most of its strength and is drawn heavier, because it is the reason to look.
void Game::DrawSpaceBrackets()
{
    if (!Ui::BracketSetting() || !playerShip_)
        return;
    const Ui::Theme& t = Ui::CurrentTheme();
    const float      zoom = camera_.zoom;
    const float      thin = std::max(1.0f, Ui::Px(1.0f));

    const Sensor::Standing standing = ViewerStanding();
    const Vector2          own = playerShip_->GetPosition();
    const float            zoomFade = LogRamp(zoom, 0.002f, 0.03f);
    for (const auto& e : clientWorld_)
    {
        const EntityKind kind = e->GetKind();
        if (kind != EntityKind::Npc && kind != EntityKind::PlayerShip)
            continue;
        const Vector2 sp = GetWorldToScreen2D(e->GetPosition(), camera_);
        const float   r = std::max(e->GetSize() * zoom + Ui::Px(3.0f), Ui::Px(7.0f));
        if (sp.x < -r || sp.y < -r || sp.x > screenWidth_ + r || sp.y > screenHeight_ + r)
            continue;

        Proto::EntitySnapshot probe;  // all Classify reads is the kind and the side
        probe.kind =
            kind == EntityKind::Npc ? Proto::EntityKind::Npc : Proto::EntityKind::PlayerShip;
        if (kind == EntityKind::Npc)
            probe.faction = static_cast<const NpcShip&>(*e).GetFaction();
        const Sensor::Allegiance a = Sensor::Classify(probe, standing);
        const bool               hostile = a == Sensor::Allegiance::Hostile;

        // Subtle further than a fight is from you: full within a few weapon ranges, about
        // half by the time it is across a neighbourhood.
        const float dist = Vector2Distance(e->GetPosition(), own);
        const float nearFade = 1.0f - 0.55f * LogRamp(dist, 2000.0f, 30000.0f);
        float       alpha =
            hostile ? 0.95f * std::max(0.7f, std::min(nearFade, 1.0f)) * std::max(0.7f, zoomFade)
                    : 0.6f * nearFade * zoomFade;
        if (alpha < 0.03f)
            continue;
        CornerBracket(sp, r, Ui::Px(hostile ? 4.0f : 3.0f), hostile ? 2.0f * thin : thin,
                      Fade(Sensor::ColorOf(a), alpha));
        // A hostile bracket is notched (#362): its colour is not the only thing that says so.
        if (hostile)
            Ui::DrawNotch(sp, r, Ui::Px(4.0f), Fade(Sensor::ColorOf(a), alpha));
    }

    // Your own ship, last, so it is on top: a bracket a little larger than the others, the
    // way the nose points, and the way the ship is actually going -- not the same thing in
    // a ship that turns faster than it changes course.
    const Vector2 sp = GetWorldToScreen2D(shipDrawPos_, camera_);
    const float   r = std::max(playerShip_->GetSize() * zoom + Ui::Px(5.0f), Ui::Px(11.0f));
    const Color   col = t.standing.own;
    CornerBracket(sp, r, Ui::Px(4.0f), thin, Fade(col, 0.85f));
    const Vector2 nose{ std::cos(shipDrawHeading_), std::sin(shipDrawHeading_) };
    const float   tick0 = r * 1.15f, tick1 = tick0 + Ui::Px(7.0f);
    DrawLineEx({ sp.x + nose.x * tick0, sp.y + nose.y * tick0 },
               { sp.x + nose.x * tick1, sp.y + nose.y * tick1 }, 2.0f * thin, col);
    const Vector2 v = playerShip_->GetVelocity();
    const float   speed = Vector2Length(v);
    const float   top = std::max(playerShip_->GetStats().maxSpeed, 1.0f);
    if (speed > top * 0.02f)
    {
        // As long as the ship is fast, to a limit: in warp the line is simply at its longest.
        const Vector2 dir{ v.x / speed, v.y / speed };
        const float   len = Ui::Px(6.0f) + Ui::Px(22.0f) * std::min(1.0f, speed / top);
        const Vector2 a{ sp.x + dir.x * r, sp.y + dir.y * r };
        const Vector2 b{ sp.x + dir.x * (r + len), sp.y + dir.y * (r + len) };
        DrawLineEx(a, b, thin, Fade(col, 0.55f));
        DrawCircleV(b, 1.5f * thin, Fade(col, 0.8f));
    }
}

void Game::DrawHud()
{
    if (hudHidden_)
        return;
    // Docked, the windows stand in the station's hall: nothing here is about flying.
    const bool flying = mode_ == GameMode::Flying;
    if (flying && !desk_.IsOpen(WIN_SENSOR) && !desk_.IsOpen(WIN_MAP))
        DrawSpaceBrackets();
    if (flying && !desk_.IsOpen(WIN_SENSOR))
        DrawScaleBar();  // a scale for the world view; the sensor screen states its own
    // Under the windows: one dragged over it covers it, never mixes with it.
    const Goal::Prompt goal = CurrentGoal();
    DrawLocation(goal);
    if (flying && !goal.missionsWindow && !desk_.IsOpen(WIN_SENSOR) && !desk_.IsOpen(WIN_MAP))
        DrawGoalPointer(goal);
#ifndef NDEBUG
    // A development build's hint only (#341): the money is honoured by a server started with
    // --dev, and a player of a published build has no use for the line.
    Ui::Text("[debug] F1: +money   F11: fullscreen", (int)(MenuBarW() + Ui::Px(8.0f)),
             screenHeight_ - (int)Ui::Px(24.0f), 12, Ui::TEXT_DIM);
#endif
    desk_.Draw(Ui::Layer::Panels);
    if (goal.missionsWindow)
        DrawGoalPointer(goal);  // round a window, so after the windows

    // Over the windows: the map, and the sensor screen -- a screen of its own with its own
    // legend, which a window parked on top of it would hide. The flight keys still work
    // under it.
    desk_.Draw(Ui::Layer::Screen);

    desk_.Draw(Ui::Layer::Modal);  // the menu bar

    // Docking prompt.
    if (flying && nearbyStation_ != nullptr && !desk_.IsOpen(WIN_MAP))
    {
        const char* prompt = TextFormat("Press E to dock at %s", nearbyStation_->GetName().c_str());
        int         tw = Ui::TextWidth(prompt, 20);
        Ui::Text(prompt, (screenWidth_ - tw) / 2, screenHeight_ - 56, 20, Ui::ACCENT);
    }

    // Warp effect — simple and legible, in screen coordinates.
    WarpPhase wp = flying ? playerShip_->GetWarpPhase() : WarpPhase::None;
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

    desk_.Draw(Ui::Layer::Popup);  // the context menu, over the windows
    Ui::DrawTooltip();             // over everything the HUD draws

    // What the ship is holding station on, and how well (#157, #298). A standing behaviour
    // runs until something releases it, so it has to be visible the whole time it does.
    if (flying && playerShip_->GetHoldMode() != HoldMode::None)
    {
        // Said as what it does (#309): a keep holds a distance from the target, and "keeping
        // at range" left the player to guess which distance and why the ship moved.
        const HoldMode    hm = playerShip_->GetHoldMode();
        const Entity*     t = FindEntityById(playerShip_->GetHoldTargetId());
        const std::string name = t ? t->GetName() : std::string("?");
        const float       range = playerShip_->GetHoldRange();
        std::string       line =
            hm == HoldMode::Orbit  ? TextFormat("ORBITING %s  at %.0f", name.c_str(), range)
            : hm == HoldMode::Keep ? TextFormat("HOLDING %.0f FROM %s", range, name.c_str())
                                   : TextFormat("FOLLOWING %s  at %.0f", name.c_str(), range);
        if (t != nullptr)
            line += TextFormat("  (now %.0f)",
                               Vector2Distance(t->GetPosition(), playerShip_->GetPosition()));
        Ui::Text(line.c_str(), (screenWidth_ - Ui::TextWidth(line.c_str(), 16)) / 2,
                 screenHeight_ - 98, 16, Ui::ACCENT);
    }

    // Short notification (saved/loaded).
    if (flashTimer_ > 0.0f)
    {
        int tw = Ui::TextWidth(flashMsg_.c_str(), 18);
        Ui::Text(flashMsg_.c_str(), (screenWidth_ - tw) / 2, screenHeight_ - 70, 18, LIME);
    }
}

// The screen treatment's settings (#120): the shared panel, and look.json written when a
// change is let go of -- a slider being dragged would otherwise be sixty writes a second.
void Game::DrawTreatmentContent(const Ui::Frame& f)
{
    if (lookPanel_.Draw(f, treatment_, &materials_))
        lookDirty_ = true;
    if (lookDirty_ && !f.Down(MOUSE_BUTTON_LEFT))
    {
        SaveTreatment();
        lookDirty_ = false;
    }
}

// A world distance as the sensor screen says it: 2k, 250k, 1M.
static std::string SensorUnits(float u)
{
    char buf[32];
    if (u >= 1000000.0f)
        std::snprintf(buf, sizeof(buf), "%gM", u / 1000000.0f);
    else if (u >= 1000.0f)
        std::snprintf(buf, sizeof(buf), "%gk", std::round(u / 100.0f) / 10.0f);
    else
        std::snprintf(buf, sizeof(buf), "%.0f", u);
    return buf;
}

// The sensor screen (#123): the surroundings projected onto a fixed grid, centred on the
// ship. Sensor::Scan decides what lands in which cell and how it reads to this pilot; this
// lays the grid out on the screen and draws it.
void Game::DrawSensorScreen()
{
    using Ui::Box;
    using Ui::Size;
    using Ui::TextStyle;
    const Ui::Theme& t = Ui::CurrentTheme();
    const Rectangle  screen{ MenuBarW(), 0.0f, screenWidth_ - MenuBarW(), (float)screenHeight_ };
    // Opaque: a readout over a picture of the same place would be two answers at once.
    DrawRectangleRec(screen, t.colors.sensor);

    // The grid fills the room the heading and the legend leave, as it was laid out last
    // frame: the picture is scanned before the legend that lists what is in it is declared.
    // Before the first layout, a guess at the same room.
    Rectangle area = sensorArea_;
    if (area.width <= 0.0f || area.height <= 0.0f)
        area = { screen.x + Ui::Px(24.0f), Ui::Px(80.0f),
                 screen.width - Ui::Px(48.0f + t.metrics.sidePanel),
                 screen.height - Ui::Px(104.0f) };
    const float cellPx = Ui::Px(16.0f);

    // Odd counts, so the ship has a cell of its own in the middle rather than a corner.
    int cols = std::max(9, (int)(area.width / cellPx));
    int rows = std::max(9, (int)(area.height / cellPx));
    if (cols % 2 == 0)
        cols--;
    if (rows % 2 == 0)
        rows--;

    // What this pilot makes of each thing in the snapshot. Allegiance is decided here, by
    // the one looking, and never read off the object (#117).
    const Sensor::Standing            me = ViewerStanding();
    std::map<int, Sensor::Allegiance> reads;
    for (const auto& e : snapshot_.entities)
        reads[e.id] = Sensor::Classify(e, me);

    std::vector<Render::Item>  scene;
    std::map<int, std::string> typeNames;  // for the legend to tell two planets' glyphs apart
    scene.reserve(clientWorld_.size());
    for (const auto& e : clientWorld_)
    {
        scene.push_back(e->Describe());
        if (e->GetArchetype() != nullptr)
            typeNames[e->GetId()] = e->GetArchetype()->name;
    }
    Render::Item own = playerShip_->Describe();
    own.pos = shipDrawPos_;

    sensorCentre_ = own.pos;
    sensorPicture_ = Sensor::Scan(
        std::move(scene), own, cols, rows, sensorRange_,
        [&](int id)
        {
            auto it = reads.find(id);
            return it != reads.end() ? it->second : Sensor::Allegiance::Unowned;
        },
        [&](int id)
        {
            auto it = typeNames.find(id);
            return it != typeNames.end() ? it->second : std::string();
        });
    sensorCellPx_ = cellPx;
    sensorOrigin_ = { area.x + (area.width - cols * cellPx) * 0.5f,
                      area.y + (area.height - rows * cellPx) * 0.5f };

    // The heading, and beside the grid the legend: what the colours mean, then what the
    // characters on the screen stand for (#297).
    const std::string across = SensorUnits(sensorRange_);
    const std::string cell = SensorUnits(sensorPicture_.UnitsPerCell());
    Ui::Layout&       L = sensorLayout_;
    L.Begin(desk_.SurfaceFrame(WIN_SENSOR, screen));
    L.Column(
        Box().Grow().Pad(t.metrics.padding * 2.0f).Gap(t.metrics.gap),
        [&]
        {
            L.Row(Box().GrowX().Gap(t.metrics.padding).Align(Ui::Align::Start, Ui::Align::Center),
                  [&]
                  {
                      L.Text("SENSORS", TextStyle::Heading().Tint(t.colors.accent));
                      L.Text(across + " across  ·  " + cell +
                                 " a cell  ·  wheel: range  ·  click: select  ·  [V] close",
                             TextStyle::Small().Tint(t.colors.dim));
                  });
            L.Row(Box().Grow().Gap(t.metrics.padding * 2.0f),
                  [&]
                  {
                      L.Row(Box().Id("grid").Grow(), [] {});
                      L.Column(
                          Box()
                              .Width(Size::Fixed(t.metrics.sidePanel))
                              .Height(Size::Grow())
                              .Pad(t.metrics.padding)
                              .Gap(t.metrics.gap)
                              .Fill(t.colors.panel)
                              .Border(t.colors.border, t.metrics.border)
                              .ThemeCorners(),
                          [&]
                          {
                              L.Text("ALLEGIANCE", TextStyle::Label());
                              for (Sensor::Allegiance a :
                                   { Sensor::Allegiance::Own, Sensor::Allegiance::Friendly,
                                     Sensor::Allegiance::Neutral, Sensor::Allegiance::Hostile,
                                     Sensor::Allegiance::Unowned })
                                  L.Row(Box()
                                            .GrowX()
                                            .Gap(t.metrics.gap)
                                            .Align(Ui::Align::Start, Ui::Align::Center),
                                        [&]
                                        {
                                            L.Row(Box()
                                                      // The radar's shape, at a size a
                                                      // shape can be told by (#366).
                                                      .Width(Size::Fixed(t.fontSize.label))
                                                      .Height(Size::Fixed(t.fontSize.label))
                                                      .Fill(Sensor::ColorOf(a))
                                                      .Mark(StandingMark(a)),
                                                  [] {});
                                            L.Text(Sensor::Word(a), TextStyle::Body());
                                        });
                              L.Divider();
                              L.Text("IN VIEW", TextStyle::Label());
                              L.Scroll(
                                  Box().Grow().Id("inview").Gap(t.metrics.rowGap),
                                  [&]
                                  {
                                      if (sensorPicture_.legend.empty())
                                          L.Text("Nothing within range.",
                                                 TextStyle::Small().Tint(t.colors.dim));
                                      for (const Sensor::LegendEntry& l : sensorPicture_.legend)
                                          L.Row(Box().GrowX().Gap(t.metrics.gap),
                                                [&]
                                                {
                                                    L.Row(Box().Width(Size::Fixed(t.fontSize.body)),
                                                          [&]
                                                          {
                                                              L.Text(std::string(1, l.glyph),
                                                                     TextStyle::Strong());
                                                          });
                                                    L.Text(l.kind, TextStyle::Body());
                                                });
                                  });
                          });
                  });
        });
    L.End();
    sensorArea_ = L.BoxOf("grid");  // where the grid goes from the next frame on
    L.Draw();

    // The frame, and a faint lattice every fourth cell from the ship, so a distance can be
    // counted off the screen.
    const Rectangle grid{ sensorOrigin_.x, sensorOrigin_.y, cols * cellPx, rows * cellPx };
    DrawRectangleLinesEx(grid, 1.0f, Fade(t.colors.border, 0.7f));
    // Each character set at its own size from the strong face, so it is as crisp as text.
    const float   glyphPx = (float)Ui::FontPx(cellPx * 1.15f);
    const int     cx = cols / 2, cy = rows / 2;
    const Vector2 mouse = GetMousePosition();
    const int     selId = selected_ != nullptr ? selected_->GetId() : 0;
    int           hoverId = 0;
    for (int y = 0; y < rows; y++)
        for (int x = 0; x < cols; x++)
        {
            const Sensor::Cell& c = sensorPicture_.At(x, y);
            const float         px = sensorOrigin_.x + x * cellPx;
            const float         py = sensorOrigin_.y + y * cellPx;
            const Vector2       mid = { px + cellPx * 0.5f, py + cellPx * 0.5f };
            if (c.glyph == ' ')
            {
                if ((x - cx) % 4 == 0 && (y - cy) % 4 == 0)
                    DrawRectangleV({ mid.x - 0.5f, mid.y - 0.5f }, { 1.0f, 1.0f },
                                   Fade(t.colors.dim, 0.45f));
                continue;
            }
            const std::string_view glyph(&c.glyph, 1);
            const Vector2          ext = Ui::MeasureString(Ui::Face::Strong, glyphPx, glyph);
            Ui::DrawString(Ui::Face::Strong, glyph, { mid.x - ext.x * 0.5f, mid.y - ext.y * 0.5f },
                           glyphPx, Sensor::ColorOf(c.allegiance));
            // A thing with a side carries its standing's shape as well as its colour, the
            // radar's and the overview's (#362), small in the cell's corner (#366).
            const Ui::Mark mark = StandingMark(c.allegiance);
            if (mark != Ui::Mark::None && c.allegiance != Sensor::Allegiance::Own)
                Ui::DrawMark({ px + cellPx - Ui::Px(2.5f), py + Ui::Px(2.5f) }, Ui::Px(2.5f), mark,
                             Sensor::ColorOf(c.allegiance));
            const Rectangle box{ px, py, cellPx, cellPx };
            if (c.id != 0 && c.id == selId)
                DrawRectangleLinesEx(box, 1.0f, t.colors.text);
            if (c.id != 0 && Ui::MouseOver(box))
                hoverId = c.id;
        }

    // What the cursor is on, by name and distance: a character says what class of thing it
    // is, and a pilot choosing between two of them needs to know which.
    if (hoverId != 0)
        for (const auto& e : snapshot_.entities)
            if (e.id == hoverId)
            {
                const float       d = std::hypot(e.pos.x - own.pos.x, e.pos.y - own.pos.y);
                const std::string dist = SensorUnits(d);
                const std::string label =
                    (e.name.empty() ? std::string(Overview::KindWord(e)) : e.name) + "  " + dist;
                const float px = (float)Ui::FontPx(Ui::Px(t.fontSize.body));
                Ui::DrawString(Ui::Face::Regular, label,
                               { mouse.x + Ui::Px(14.0f), mouse.y - px * 0.5f }, px, t.colors.text);
                break;
            }
}

bool Game::SensorPick(Vector2 screen, int& id, Vector2& world) const
{
    const Sensor::Picture& p = sensorPicture_;
    if (p.width == 0 || sensorCellPx_ <= 0.0f)
        return false;
    const int x = (int)std::floor((screen.x - sensorOrigin_.x) / sensorCellPx_);
    const int y = (int)std::floor((screen.y - sensorOrigin_.y) / sensorCellPx_);
    if (x < 0 || x >= p.width || y < 0 || y >= p.height)
        return false;
    id = p.At(x, y).id;
    const float u = p.UnitsPerCell();
    world = { sensorCentre_.x + (x - p.width / 2) * u, sensorCentre_.y + (y - p.height / 2) * u };
    return true;
}
