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
#include "economy/Resource.h"
#include "ui/Button.h"
#include "ui/UiTheme.h"
#include "ui/Desk.h"
#include "ui/Input.h"
#include "ui/Units.h"
#include "render/Textures.h"
#include "raymath.h"
#include <nlohmann/json.hpp>
#include <cmath>
#include <cstdio>
#include <string>
#include <algorithm>
#include <fstream>

// Menu bar (Neocom): strip width and button geometry.
static const float MENU_BAR_W = 46.0f;
static const float MENU_BTN = 36.0f;
static const float MENU_STEP = 46.0f;
static const float MENU_TOP = 12.0f;

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
    Render::Perf::Mark(Render::Perf::Phase::Overlay);
}

void Game::SetupWindows()
{
    using Ui::Anchor;
    using Ui::Layer;
    using Ui::WindowSpec;

    // A window the player arranges: placed from a corner, kept per account.
    auto panel = [](const char* id, const char* label, Anchor anchor, Rectangle place)
    {
        WindowSpec s;
        s.id = id;
        s.menuLabel = label;
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
    auto fullScreen = [this]()
    { return Rectangle{ 0.0f, 0.0f, (float)screenWidth_, (float)screenHeight_ }; };

    // Registered in the menu bar's order. The places are the ones the windows have always
    // had at 1280 x 720; anchored, they now keep to their side at any other size.
    // The status window is laid out by Ui::Layout (#297), so it can be any size: resizable,
    // and no smaller than two gauges and a field.
    WindowSpec status = panel(WIN_STATUS, "STA", Anchor::TopLeft, { 56.0f, 16.0f, 264.0f, 312.0f });
    status.resizable = true;
    status.minSize = { 200.0f, 150.0f };
    desk_.AddWindow(status, "STATUS", true, [this](Ui::Frame& f) { DrawStatusContent(f); });
    // The selected item and the overview, one above the other on the right, as EVE has them:
    // pick from the list, act from the window above it (#297). Both are laid out by
    // Ui::Layout, so both can be any size.
    WindowSpec target =
        panel(WIN_TARGET, "TGT", Anchor::TopRight, { 16.0f, 16.0f, 300.0f, 300.0f });
    target.resizable = true;
    target.minSize = { 240.0f, 160.0f };
    // The windows that look at space are space's: docked, they are put away and come back
    // as they were on undocking (#297). The station's windows take their place.
    target.context = CTX_SPACE;
    desk_.AddWindow(target, "SELECTED ITEM", false, [this](Ui::Frame& f) { DrawTargetContent(f); });
    // Open from the start: it is the instrument a player flies by (#157), and a player who
    // has to know a key exists before they can navigate has not been told how to play.
    WindowSpec overview =
        panel(WIN_OVERVIEW, "OVR", Anchor::TopRight, { 16.0f, 328.0f, 300.0f, 376.0f });
    overview.resizable = true;
    overview.minSize = { 260.0f, 160.0f };
    overview.context = CTX_SPACE;
    desk_.AddWindow(overview, "OVERVIEW", true, [this](Ui::Frame& f) { DrawOverviewContent(f); });
    // The radar is a picture drawn by hand inside a layout (#297): its toolbar is laid out,
    // the scope takes whatever room is left, so it can be any size.
    WindowSpec radar = panel(WIN_RADAR, "RAD", Anchor::TopLeft, { 56.0f, 344.0f, 264.0f, 288.0f });
    radar.resizable = true;
    radar.minSize = { 180.0f, 180.0f };
    radar.context = CTX_SPACE;
    desk_.AddWindow(radar, "RADAR", false, [this](Ui::Frame& f) { DrawRadarContent(f); });
    // A list of missions and the chosen one in full: one above the other when narrow, side
    // by side when wide. Beside the selected item rather than on top of the overview, which
    // is open from the start.
    WindowSpec missions =
        panel(WIN_MISSIONS, "MIS", Anchor::TopRight, { 324.0f, 16.0f, 300.0f, 320.0f });
    missions.resizable = true;
    missions.minSize = { 240.0f, 220.0f };
    desk_.AddWindow(missions, "MISSIONS", false, [this](Ui::Frame& f) { DrawMissionsContent(f); });

    // The two screens cover the world view and the windows; whichever opened last is on top.
    // Covering means the windows are not drawn at all while one is open: the map is drawn
    // translucent over the world, and the panels used to show through it (#297).
    auto screen = [&surface](const char* id, const char* label, Ui::EscRule esc)
    {
        WindowSpec s = surface(id, label, Layer::Screen, esc);
        s.covers = true;
        return s;
    };
    Ui::Desk::Surface map;
    map.bounds = fullScreen;
    map.draw = [this]() { DrawGalaxyMap(); };
    desk_.AddSurface(screen(WIN_MAP, "MAP", Ui::EscRule::Close), false, map);
    Ui::Desk::Surface sensor;
    sensor.bounds = [this]()
    { return Rectangle{ MENU_BAR_W, 0.0f, screenWidth_ - MENU_BAR_W, (float)screenHeight_ }; };
    sensor.draw = [this]() { DrawSensorScreen(); };
    WindowSpec sensorSpec = screen(WIN_SENSOR, "SNS", Ui::EscRule::Close);
    sensorSpec.context = CTX_SPACE;  // a readout of the space around the ship
    desk_.AddSurface(sensorSpec, false, sensor);

    WindowSpec settings = panel(WIN_SETTINGS, "SET", Anchor::Top, { 0.0f, 100.0f, 340.0f, 400.0f });
    settings.resizable = true;
    settings.minSize = { 260.0f, 240.0f };
    desk_.AddWindow(settings, "SETTINGS", false, [this](Ui::Frame& f) { DrawSettingsContent(f); });

    // Docked (#297): the station is windows like any other, in the "docked" context -- there
    // while the ship is berthed, put away as they were when it leaves. They used to be one
    // screen over everything; now they can be moved, resized, grouped and stacked as tabs,
    // and the missions window stands beside them. Where the target and overview windows
    // are in space, which are put away while docked.
    WindowSpec station =
        panel(WIN_STATION, "STN", Anchor::TopLeft, { 56.0f, 344.0f, 264.0f, 300.0f });
    station.resizable = true;
    station.minSize = { 220.0f, 200.0f };
    station.context = CTX_DOCKED;
    desk_.AddWindow(station, "STATION", true, [this](Ui::Frame& f) { DrawStationContent(f); });
    WindowSpec market =
        panel(WIN_MARKET, "MKT", Anchor::TopRight, { 16.0f, 16.0f, 300.0f, 320.0f });
    market.resizable = true;
    market.minSize = { 240.0f, 220.0f };
    market.context = CTX_DOCKED;
    desk_.AddWindow(market, "MARKET", true, [this](Ui::Frame& f) { DrawMarketContent(f); });
    WindowSpec hangar =
        panel(WIN_HANGAR, "HGR", Anchor::TopRight, { 16.0f, 344.0f, 608.0f, 300.0f });
    hangar.resizable = true;
    hangar.minSize = { 240.0f, 220.0f };
    hangar.context = CTX_DOCKED;
    desk_.AddWindow(hangar, "HANGAR", true, [this](Ui::Frame& f) { DrawHangarContent(f); });

    // Above the screens it opens; Esc passes over it. Docked too: it opens the station's
    // windows again once they have been closed.
    Ui::Desk::Surface bar;
    bar.bounds = [this]() { return Rectangle{ 0.0f, 0.0f, MENU_BAR_W, (float)screenHeight_ }; };
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

    // F10's panel: over everything, and closing it writes what was tuned.
    Ui::Desk::Surface look;
    look.bounds = [this]() { return TreatmentPanelRect(); };
    look.onClose = [this]() { SaveTreatment(); };
    look.draw = [this]() { DrawTreatmentSettings(); };
    desk_.AddSurface(surface(WIN_LOOK, "", Layer::Overlay, Ui::EscRule::Close), false, look);
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

    Ui::TableSpec table;
    table.id = "overview";
    table.columns = { { "name", Size::Grow(), Ui::Align::Start, true },
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
                return Ui::Cell{ row.name, row.hostile ? t.standing.hostile
                                           : held      ? t.colors.accent
                                                       : t.colors.text };
            case 1: return Ui::Cell{ row.kind, t.colors.dim };
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
            if (e->kind == Proto::EntityKind::Npc)
            {
                L.Field("Hull", TextFormat("%.0f%%", e->hullFrac * 100.0f), t.colors.text);
                L.Bar(e->hullFrac, e->hullFrac > 0.3f ? t.colors.good : t.colors.bad);
            }
            else if (e->kind == Proto::EntityKind::Structure)
            {
                const auto l = layout_.byId.find(e->id);
                if (l != layout_.byId.end())
                {
                    const Proto::EntityLayout& el = l->second;
                    const bool                 mine = el.owner == pilotName_;
                    L.Field("Built by", mine ? "you" : el.owner,
                            mine ? t.standing.own : t.colors.text);
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
                }
            }
            else if (e->kind == Proto::EntityKind::Field && e->ore >= 0)
                L.Field("Ore", ResourceName((ResourceType)e->ore), t.colors.text);

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
                              const char*   label;
                              Actions::Verb verb;
                              const char*   tip;
                          };
                          static const Hold holds[] = {
                              { "orbit", "Orbit", Actions::Verb::Orbit, "Circle it at this range" },
                              { "keep", "Keep", Actions::Verb::Keep,
                                "Hold this distance from it, moving only as much as that takes" },
                              { "follow", "Follow", Actions::Verb::Follow,
                                "Hold this distance and match its velocity" },
                          };
                          for (const Hold& h : holds)
                          {
                              // Lit while it is the order the ship is flying.
                              const bool on = heldHere && Actions::HoldMode(h.verb) ==
                                                              (int)playerShip_->GetHoldMode() + 2;
                              if (L.Button(h.id, h.label, on))
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

// Menu bar input: clicking a button toggles the window or screen it stands for. The buttons
// are the desk's registry in order (#297), so a window registered with a label has one.
void Game::HandleMenuBar()
{
    if (!desk_.Owns(WIN_MENUBAR) || !IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        return;
    const Vector2                         m = GetMousePosition();
    const std::vector<Ui::Desk::MenuSlot> slots = desk_.MenuSlots();
    for (size_t i = 0; i < slots.size(); i++)
    {
        Rectangle b{ (MENU_BAR_W - MENU_BTN) / 2.0f, MENU_TOP + i * MENU_STEP, MENU_BTN, MENU_BTN };
        if (CheckCollisionPointRec(m, b))
        {
            desk_.Toggle(slots[i].id);
            break;
        }
    }
}

// Vertical menu bar on the left: window-toggle buttons,
// the active window is highlighted with the accent color.
void Game::DrawMenuBar()
{
    DrawRectangleRec(Rectangle{ 0.0f, 0.0f, MENU_BAR_W, (float)screenHeight_ }, Ui::TITLE_BG);
    DrawLineEx(Vector2{ MENU_BAR_W, 0.0f }, Vector2{ MENU_BAR_W, (float)screenHeight_ }, 1.0f,
               Ui::PANEL_BORDER);

    const std::vector<Ui::Desk::MenuSlot> slots = desk_.MenuSlots();
    for (size_t i = 0; i < slots.size(); i++)
    {
        Rectangle b{ (MENU_BAR_W - MENU_BTN) / 2.0f, MENU_TOP + i * MENU_STEP, MENU_BTN, MENU_BTN };
        const char* label = slots[i].label.c_str();
        bool        open = slots[i].open;
        bool        hover = Ui::MouseOver(b);
        Color       accent = (open || hover) ? Ui::ACCENT : Ui::TEXT_DIM;

        DrawRectangleRec(b, open ? Fade(Ui::ACCENT, 0.25f)
                                 : (hover ? Fade(Ui::ACCENT, 0.12f) : Ui::PANEL_BG));
        DrawRectangleLinesEx(b, 1.0f, (open || hover) ? Ui::ACCENT : Ui::PANEL_BORDER);

        int tw = Ui::TextWidth(label, 14);
        Ui::Text(label, (int)(b.x + (b.width - tw) / 2.0f), (int)b.y + 11, 14, accent);
    }
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

void Game::DrawHud()
{
    if (hudHidden_)
        return;
    // Docked, the windows stand in the station's hall: nothing here is about flying.
    const bool flying = mode_ == GameMode::Flying;
    if (flying && !desk_.IsOpen(WIN_SENSOR))
        DrawScaleBar();  // a scale for the world view; the sensor screen states its own
    desk_.Draw(Ui::Layer::Panels);

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

    Ui::Text("[debug] F1: +money   F11: fullscreen", 56, screenHeight_ - 26, 14, Ui::TEXT_DIM);

    // Short notification (saved/loaded).
    if (flashTimer_ > 0.0f)
    {
        int tw = Ui::TextWidth(flashMsg_.c_str(), 18);
        Ui::Text(flashMsg_.c_str(), (screenWidth_ - tw) / 2, screenHeight_ - 70, 18, LIME);
    }
}

// Full-screen galaxy star map (EVE-style): dimmed background, systems as nodes by mapPos,
// gate links as lines, the current system highlighted. The map itself is a picture drawn by
// hand; around it, the heading, the legend and the news are laid out (#297), and the
// picture takes whatever room they leave.
void Game::DrawGalaxyMap()
{
    using Ui::Box;
    using Ui::Size;
    using Ui::TextStyle;
    const Ui::Theme& t = Ui::CurrentTheme();
    DrawRectangle(0, 0, screenWidth_, screenHeight_, t.colors.shade);

    // Galactic news (system captures, reconquests) from the server's galaxy snapshot, newest
    // first. A column of its own that wraps and scrolls: a long line used to run off the
    // right of the screen.
    const std::vector<std::string>& news = galaxyState_.events;
    Ui::Layout&                     L = mapLayout_;
    L.Begin(desk_.SurfaceFrame(
        WIN_MAP, { MENU_BAR_W, 0.0f, screenWidth_ - MENU_BAR_W, (float)screenHeight_ }));
    L.Column(Box().Grow().Pad(t.metrics.padding * 2.0f).Gap(t.metrics.gap),
             [&]
             {
                 L.Row(Box().GrowX().Gap(t.metrics.padding).Align(Ui::Align::Start, Ui::Align::End),
                       [&]
                       {
                           L.Text("GALAXY MAP", TextStyle::Heading().Tint(t.colors.accent));
                           L.Text("drag to pan  ·  wheel to zoom  ·  [G] / [Esc] close",
                                  TextStyle::Small().Tint(t.colors.dim));
                       });
                 L.Row(Box().Grow().Gap(t.metrics.padding * 2.0f),
                       [&]
                       {
                           L.Row(Box().Id("graph").Grow(), [] {});
                           L.Column(
                               Box()
                                   .Width(Size::Fixed(t.metrics.sidePanel))
                                   .Height(Size::Grow())
                                   .Pad(t.metrics.padding)
                                   .Gap(t.metrics.gap)
                                   .Fill(t.colors.panel)
                                   .Border(t.colors.border, t.metrics.border)
                                   .Radius(t.metrics.radius),
                               [&]
                               {
                                   L.Text("LEGEND", TextStyle::Label());
                                   const TextStyle note = TextStyle::Small().Wrap();
                                   L.Text("Filled: charted. A ring alone: uncharted -- a gate "
                                          "leads there, and nothing more is known.",
                                          note);
                                   L.Text("The ring's colour is who holds the system.", note);
                                   L.Text("sec, pir, econ: security, pirates, prosperity.", note);
                                   L.Divider();
                                   L.Text("GALACTIC NEWS", TextStyle::Label());
                                   L.Scroll(
                                       Box().Grow().Id("news").Gap(t.metrics.gap),
                                       [&]
                                       {
                                           if (news.empty())
                                               L.Text("Nothing has changed hands yet.",
                                                      TextStyle::Small().Tint(t.colors.dim).Wrap());
                                           for (size_t i = news.size(); i-- > 0;)
                                               L.Text(news[i], TextStyle::Small().Wrap());
                                       });
                               });
                       });
             });
    L.End();
    // The picture's room; the furniture around it is drawn over the shade.
    const Rectangle area = L.BoxOf("graph");
    L.Draw();

    const std::vector<WorldLoader::SystemInfo>& systems = universe_.systems;
    if (systems.empty() || area.width <= 0.0f || area.height <= 0.0f)
    {
        const char* none = "No galaxy data";
        Ui::Text(none, (int)(area.x + (area.width - Ui::TextWidth(none, 16)) * 0.5f),
                 (int)(area.y + area.height * 0.5f), 16, t.colors.dim);
        return;
    }
    Vector2 nameAt{ -1.0f, -1.0f };  // where the name field goes, while it is being typed in

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

    // Input: wheel zoom and drag-to-pan — like the radar. Drawn by the desk, which says
    // whether the map owns the mouse (#297).
    Vector2 m = GetMousePosition();
    bool    over = Ui::MouseOver(area);
    if (over)
    {
        float wheel = Ui::MouseWheel();
        if (wheel != 0.0f)
            galaxyZoom_ = Clamp(galaxyZoom_ * (1.0f + wheel * 0.12f), 0.3f, 8.0f);
    }

    float scale = baseScale * galaxyZoom_;

    if (over && Ui::MousePressed(MOUSE_BUTTON_LEFT))
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

    // Which system the player is in, according to the server. The client has no
    // simulation of its own to ask (#3).
    std::string activeSys = snapshot_.systemId;

    // How close each system's nearest neighbour is on screen. With a region beyond the
    // wormhole (#140) the map holds twenty systems, and three lines of text under each one
    // is a smear: a crowded system shows its details only while it is the one you are in
    // or the one under the cursor, and the wheel brings the rest back.
    auto nearestOnScreen = [&](const WorldLoader::SystemInfo& s)
    {
        float best = 1e9f;
        for (const auto& o : systems)
            if (&o != &s)
            {
                const float dx = (o.mapPos.x - s.mapPos.x) * scale;
                const float dy = (o.mapPos.y - s.mapPos.y) * scale;
                best = fminf(best, sqrtf(dx * dx + dy * dy));
            }
        return best;
    };

    // Labels are placed, not just drawn (#259, #313): the system you are in and the one under
    // the cursor claim their text first, and any other label that would land on a claimed
    // one gives up its details, then its name. Spacing alone did not keep a neighbour's name
    // off the five lines the current system says about itself.
    std::vector<const WorldLoader::SystemInfo*> order;
    for (const auto& s : systems)
        order.push_back(&s);
    auto priority = [&](const WorldLoader::SystemInfo* s)
    {
        if (s->id == activeSys)
            return 0;
        return CheckCollisionPointCircle(m, toScreen(s->mapPos), 12.0f) ? 1 : 2;
    };
    std::stable_sort(order.begin(), order.end(),
                     [&](const WorldLoader::SystemInfo* a, const WorldLoader::SystemInfo* b)
                     { return priority(a) < priority(b); });
    std::vector<Rectangle> claimed;
    auto                   unclaimed = [&](Rectangle r)
    {
        for (const Rectangle& c : claimed)
            if (CheckCollisionRecs(r, c))
                return false;
        return true;
    };

    // System nodes.
    for (const WorldLoader::SystemInfo* sp : order)
    {
        const WorldLoader::SystemInfo& s = *sp;
        Vector2                        p = toScreen(s.mapPos);
        bool                           cur = (s.id == activeSys);
        const bool                     hovered = CheckCollisionPointCircle(m, p, 12.0f);
        const float                    room = nearestOnScreen(s);
        bool                           detail = cur || hovered || room >= 110.0f;
        bool                           named = cur || hovered || room >= 45.0f;
        float                          lx = p.x + 14.0f;  // where its labels start
        {
            // What its labels would cover: the name line, and the lines under it.
            float nameW = (float)Ui::TextWidth(s.name.c_str(), 16);
            if (!s.designation.empty())
                nameW += 8.0f + (float)Ui::TextWidth(s.designation.c_str(), 12);
            const float     detailW = fmaxf(nameW, s.charted ? 190.0f : 80.0f);
            const float     bottom = !s.charted ? 24.0f
                                     : cur      ? (CanNameHere() ? 82.0f : 66.0f)
                                                : (s.discoverer.empty() ? 38.0f : 52.0f);
            const Rectangle nameBox{ p.x + 14.0f, p.y - 8.0f, nameW, 18.0f };
            const Rectangle leftBox{ p.x - 14.0f - nameW, p.y - 8.0f, nameW, 18.0f };
            const Rectangle detailBox{ p.x + 14.0f, p.y - 8.0f, detailW, bottom + 8.0f };
            // In order: everything to the right, the name to the right, the name to the left
            // of the node. A label that fits nowhere is left to the cursor.
            if (!cur && !hovered)
            {
                if (detail && !unclaimed(detailBox))
                    detail = false;
                if (named && !detail && !unclaimed(nameBox))
                {
                    if (unclaimed(leftBox))
                        lx = leftBox.x;
                    else
                        named = false;
                }
            }
            if (detail)
                claimed.push_back(detailBox);
            else if (named)
                claimed.push_back(lx < p.x ? leftBox : nameBox);
        }
        // Uncharted (#144): a gate from somewhere known leads there, and that is all anyone
        // knows -- an empty ring and a designation, no numbers.
        if (!s.charted)
        {
            DrawCircleLines((int)p.x, (int)p.y, 7.0f, Ui::TEXT_DIM);
            if (named)
            {
                Ui::Text(s.name.c_str(), (int)lx, (int)p.y - 8, 16, Ui::TEXT_DIM);
                if (detail)
                    Ui::Text("uncharted", (int)lx, (int)p.y + 10, 12, Ui::TEXT_DIM);
            }
            continue;
        }
        DrawCircleV(p, cur ? 10.0f : 7.0f, cur ? Ui::ACCENT : Ui::TEXT_DIM);
        if (cur)
            DrawCircleLines((int)p.x, (int)p.y, 16.0f, Fade(Ui::ACCENT, 0.6f));
        if (named)
        {
            Ui::Text(s.name.c_str(), (int)lx, (int)p.y - 8, 16, cur ? Ui::ACCENT : Ui::TEXT);
            // A named system keeps its designation beside the name (#145).
            if (!s.designation.empty())
                Ui::Text(s.designation.c_str(), (int)lx + 8 + Ui::TextWidth(s.name.c_str(), 16),
                         (int)p.y - 6, 12, Ui::TEXT_DIM);
        }

        // Live summary: security/pirates/economy/controller, from the server's galaxy
        // snapshot (galaxyState_).
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
        if (haveStats && !detail)
            // Crowded: the controller only as the ring's colour.
            DrawCircleLines((int)p.x, (int)p.y, 10.0f, Fade(FactionColor(controller), 0.7f));
        else if (haveStats)
        {
            Color secCol = security >= 0.7f   ? Color{ 120, 210, 130, 255 }
                           : security >= 0.4f ? GOLD
                                              : Color{ 230, 120, 60, 255 };
            Ui::Text(
                TextFormat("sec %.2f  pir %d  econ %.0f%%", security, pirates, prosperity * 100.0f),
                (int)lx, (int)p.y + 10, 12, secCol);
            // Territory controller (L3) — in the faction's color.
            Ui::Text(FactionName(controller).c_str(), (int)lx, (int)p.y + 24, 12,
                     FactionColor(controller));
            // The node ring is tinted with the controller's color.
            DrawCircleLines((int)p.x, (int)p.y, cur ? 13.0f : 10.0f,
                            Fade(FactionColor(controller), 0.7f));
        }
        if (cur)
            Ui::Text("you are here", (int)lx, (int)p.y + 38, 12, Ui::TEXT_DIM);
        if (detail && !s.discoverer.empty())
            Ui::Text(TextFormat("found by %s", s.discoverer.c_str()), (int)lx,
                     (int)p.y + (cur ? 52 : 38), 12, Ui::TEXT_DIM);
        if (cur && CanNameHere())
        {
            if (desk_.KeyboardFocus().Holds(WIN_MAP, "name"))
                nameAt = { lx, p.y + 60.0f };  // the field is drawn after the clip
            else
                Ui::Text("[N] name this system", (int)lx, (int)p.y + 66, 14, Ui::ACCENT);
        }
    }

    EndScissorMode();

    // The name field (#145): a Ui::Layout text field with the keyboard routed to it by the
    // desk (#297). Enter sends the name on the next numbered input; Esc, or a click
    // elsewhere, puts it away.
    if (nameAt.x >= 0.0f)
    {
        const Rectangle field{ nameAt.x, nameAt.y, Ui::Px(260.0f),
                               Ui::Px(t.metrics.buttonHeight + t.fontSize.small * 1.6f +
                                      t.metrics.rowGap) };
        Ui::Layout&     N = mapNameLayout_;
        N.Begin(desk_.SurfaceFrame(WIN_MAP, field));
        N.Column(Ui::Box().Grow().Gap(t.metrics.rowGap),
                 [&]
                 {
                     Ui::TextFieldOptions o;
                     o.maxLength = 24;
                     o.placeholder = "a name for this system";
                     const Ui::EditResult r = N.TextField("name", nameBuf_, o);
                     // The server checks it and says why not in the journal, which flashes
                     // here; a name it accepts comes back to everyone in the galaxy index. It
                     // rides on the next numbered input like any one-shot intent: a command of
                     // its own would be one more tick of movement the server steps and this
                     // client never predicted.
                     if (r.submitted && !nameBuf_.empty())
                         cmd_.nameSystem = nameBuf_;
                     N.Text("[Enter] name it   [Esc] cancel", Ui::TextStyle::Small());
                 });
        N.End();
        N.Draw();
    }
}

// The screen treatment's settings, over everything and never treated themselves (#120).
// A panel seen through the effect it is adjusting is a panel you cannot read while
// adjusting it.
Rectangle Game::TreatmentPanelRect() const
{
    const float w = 320.0f;
    const float h = Render::TreatmentPanelHeight(treatment_, &materials_) + 24.0f;
    return { (float)screenWidth_ - w - 16.0f, 60.0f, w, fminf(h, (float)screenHeight_ - 80.0f) };
}

void Game::DrawTreatmentSettings()
{
    const Rectangle panel = TreatmentPanelRect();

    DrawRectangleRec(panel, Ui::PANEL_BG);
    DrawRectangleLinesEx(panel, 1.0f, Ui::PANEL_BORDER);

    BeginScissorMode((int)panel.x, (int)panel.y, (int)panel.width, (int)panel.height);
    Render::DrawTreatmentPanel(
        { panel.x + 12.0f, panel.y + 12.0f, panel.width - 24.0f, panel.height - 24.0f }, treatment_,
        &materials_);
    EndScissorMode();

    Ui::Text("F10 closes and saves", (int)panel.x + 12, (int)(panel.y + panel.height - 16.0f), 10,
             Ui::TEXT_DIM);
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
    const Rectangle  screen{ MENU_BAR_W, 0.0f, screenWidth_ - MENU_BAR_W, (float)screenHeight_ };
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

    std::vector<Render::Item> scene;
    scene.reserve(clientWorld_.size());
    for (const auto& e : clientWorld_)
        scene.push_back(e->Describe());
    Render::Item own = playerShip_->Describe();
    own.pos = shipDrawPos_;

    sensorCentre_ = own.pos;
    sensorPicture_ =
        Sensor::Scan(std::move(scene), own, cols, rows, sensorRange_,
                     [&](int id)
                     {
                         auto it = reads.find(id);
                         return it != reads.end() ? it->second : Sensor::Allegiance::Unowned;
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
            L.Row(Box().GrowX().Gap(t.metrics.padding).Align(Ui::Align::Start, Ui::Align::End),
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
                              .Radius(t.metrics.radius),
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
                                                      .Width(Size::Fixed(t.metrics.barHeight))
                                                      .Height(Size::Fixed(t.metrics.barHeight))
                                                      .Fill(Sensor::ColorOf(a)),
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
