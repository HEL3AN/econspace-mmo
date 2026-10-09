// Game — windows, HUD and the station screen.
//
// Presentation only: this file reads state and draws it, and holds no rules of its own.
// It is the largest of the three because a game is mostly interface, and keeping it apart
// is what keeps the other two readable.
//
// Part of the Game class; see Game.cpp.
#include "core/Game.h"
#include "render/TreatmentPanel.h"
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
#include "ui/Window.h"
#include "render/Textures.h"
#include "raymath.h"
#include <nlohmann/json.hpp>
#include <cmath>
#include <string>
#include <algorithm>
#include <fstream>

// Menu bar (Neocom): strip width and button geometry.
static const float MENU_BAR_W = 46.0f;
static const float MENU_BTN = 36.0f;
static const float MENU_STEP = 46.0f;
// Hostile, as the instruments say it: the overview, the radar (#117).
static const Color HOSTILE = { 230, 90, 80, 255 };
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
        Render::Present(std::move(scene), lights, camera_, *backend_);
    }

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

    // Drawn last and on its own, so it stays on top of the beams and range rings above.
    backend_->Draw(playerShip_->Describe());

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

    // Open from the start: it is the instrument a player flies by (#157), and a player who
    // has to know a key exists before they can navigate has not been told how to play.
    windows_.push_back(std::make_unique<Window>("OVERVIEW", stub, true));
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
            case Proto::EntityKind::Station:
                col = HostileToPlayerFaction(e.faction) ? HOSTILE : Ui::ACCENT;
                break;
            case Proto::EntityKind::Field: col = ORANGE; break;
            case Proto::EntityKind::Nebula: col = Color{ 150, 90, 200, 255 }; break;
            case Proto::EntityKind::Derelict: col = Color{ 130, 130, 120, 255 }; break;
            case Proto::EntityKind::Gate: col = Color{ 90, 200, 210, 255 }; break;
            // The radar is an instrument, so allegiance is allowed here -- as this player
            // sees it: hostile is red, the rest wear their faction (#117).
            case Proto::EntityKind::Npc:
                col = HostileToPlayerFaction(e.faction) ? HOSTILE : FactionColor(e.faction);
                break;
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
    // The overview is the instrument a player flies by (#157): pick a thing from the list,
    // then choose what to do about it with a right click. What goes in it and in what order
    // is decided by Overview::Build, which a test can hold; this only draws it. Read from the
    // snapshot (M4c), not from the live objects; a click maps back to the proxy by id.
    const Vector2 sp = snapshot_.player.pos;
    const Vector2 m = GetMousePosition();
    const bool    clicked = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    const bool    rclicked = IsMouseButtonPressed(MOUSE_BUTTON_RIGHT);
    const int     rowH = 20;
    float         y = area.y;

    // --- Tabs -------------------------------------------------------------------------
    {
        float x = area.x;
        for (Overview::Filter f : Overview::AllFilters())
        {
            const char* label = Overview::Label(f);
            const float w = (float)Ui::TextWidth(label, 13) + 14.0f;
            Rectangle   tab{ x, y, w, 20.0f };
            const bool  on = (overviewFilter_ == f);
            const bool  over = CheckCollisionPointRec(m, tab);
            DrawRectangleRec(tab, on ? Fade(Ui::ACCENT, 0.25f)
                                     : (over ? Fade(Ui::ACCENT, 0.10f) : Fade(Ui::TITLE_BG, 0.6f)));
            Ui::Text(label, (int)x + 7, (int)y + 3, 13, on ? Ui::ACCENT : Ui::TEXT_DIM);
            if (over && clicked)
                overviewFilter_ = f;
            x += w + 3.0f;
        }
        y += 24.0f;
    }

    // --- Column headers: click to sort --------------------------------------------------
    {
        struct Col
        {
            const char*    label;
            Overview::Sort sort;
            float          x;
        };
        const Col cols[] = { { "name", Overview::Sort::Name, area.x + 4.0f },
                             { "type", Overview::Sort::Kind, area.x + area.width * 0.58f },
                             { "dist", Overview::Sort::Distance, area.x + area.width - 40.0f } };
        for (const Col& c : cols)
        {
            const bool on = (overviewSort_ == c.sort);
            Rectangle  hit{ c.x - 2.0f, y, 52.0f, 16.0f };
            Ui::Text(TextFormat("%s%s", c.label, on ? " v" : ""), (int)c.x, (int)y, 11,
                     on ? Ui::ACCENT : Ui::TEXT_DIM);
            if (clicked && CheckCollisionPointRec(m, hit))
                overviewSort_ = c.sort;
        }
        y += 16.0f;
        DrawLineEx({ area.x, y }, { area.x + area.width, y }, 1.0f, Fade(Ui::PANEL_BORDER, 0.6f));
        y += 3.0f;
    }

    // --- Rows -------------------------------------------------------------------------
    const std::vector<Overview::Row> rows = Overview::Build(
        snapshot_.entities, sp, overviewFilter_, overviewSort_,
        [this](const Proto::EntitySnapshot& e)
        { return e.kind == Proto::EntityKind::Npc && HostileToPlayerFaction(e.faction); });

    const int selId = selected_ != nullptr ? selected_->GetId() : 0;
    for (const Overview::Row& r : rows)
    {
        if (y + rowH > area.y + area.height)
            break;  // doesn't fit — truncate the list
        const Proto::EntitySnapshot& e = *r.entity;
        Rectangle                    row{ area.x, y, area.width, (float)rowH };

        // Marked when the ship is holding station on it: a standing order with no visible
        // sign of running is an order a player cannot trust.
        const bool held = (playerShip_ && e.id != 0 && playerShip_->GetHoldTargetId() == e.id &&
                           playerShip_->GetHoldMode() != HoldMode::None);
        if (e.id != 0 && e.id == selId)
            DrawRectangleRec(row, Fade(Ui::ACCENT, 0.22f));
        else if (held)
            DrawRectangleRec(row, Fade(Ui::ACCENT, 0.12f));
        else if (CheckCollisionPointRec(m, row))
            DrawRectangleRec(row, Fade(Ui::ACCENT, 0.06f));

        // Hostiles in red, because this list is where allegiance belongs: the instrument, not
        // the world view (#117).
        const Color nameCol = r.hostile ? HOSTILE : (held ? Ui::ACCENT : Ui::TEXT);
        // Clipped short of the type column, ending in "..": a name that runs into the next
        // column reads as one word with the type.
        std::string name = r.name;
        const int   nameRoom = (int)(area.width * 0.58f) - 10;
        if (Ui::TextWidth(name.c_str(), 14) > nameRoom)
        {
            while (!name.empty() && Ui::TextWidth((name + "..").c_str(), 14) > nameRoom)
                name.pop_back();
            name += "..";
        }
        Ui::Text(name.c_str(), (int)area.x + 4, (int)y + 3, 14, nameCol);
        Ui::Text(r.kind.c_str(), (int)(area.x + area.width * 0.58f), (int)y + 4, 12, Ui::TEXT_DIM);
        // Thousands past ten thousand: a column of seven-digit numbers is one nobody reads.
        const char* d = r.distance >= 10000.0f ? TextFormat("%.0fk", r.distance / 1000.0f)
                                               : TextFormat("%.0f", r.distance);
        Ui::Text(d, (int)(area.x + area.width) - Ui::TextWidth(d, 14) - 4, (int)y + 3, 14,
                 Ui::TEXT_DIM);

        if (CheckCollisionPointRec(m, row))
        {
            if (clicked)
            {
                selected_ = FindEntityById(e.id);
                if (selected_ != nullptr)
                    targetWin_->SetOpen(true);
            }
            else if (rclicked)  // the actions on this thing: approach, orbit, warp, dock...
            {
                selected_ = FindEntityById(e.id);
                if (selected_ != nullptr)
                    OpenContextMenu(selected_);
            }
        }
        y += rowH;
    }

    if (rows.empty())
        Ui::Text(overviewFilter_ == Overview::Filter::Hostile ? "nothing hostile in sight"
                                                              : "nothing here",
                 (int)area.x + 4, (int)y + 4, 12, Ui::TEXT_DIM);
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
    DrawScaleBar();
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

    // Reputation makes buying a ship cheaper. Selling needs no multiplier here: the server
    // prices a sale, and the revenue arrives with its acknowledgement.
    float buyMul = 1.0f;
    switch (stationTier)
    {
        case RepTier::Hostile: buyMul = 1.15f; break;
        case RepTier::Liked: buyMul = 0.92f; break;
        case RepTier::Allied: buyMul = 0.85f; break;
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

    // --- Market: selling mined ore from the hold. Prices and cargo come from the
    // snapshot, which is the only place the client has them. ---
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
                           [this, type, cargo]()
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
    Ui::Text(TextFormat("Current ship: %s", catalog[CurrentShipIndex()].name.c_str()), contentX,
             hangarY + 28, 14, Ui::ACCENT);

    int shipY = hangarY + 56;
    for (size_t i = 0; i < catalog.size(); i++)
    {
        const ShipType& t = catalog[i];
        bool            current = ((int)i == CurrentShipIndex());

        Ui::Text(TextFormat("%-9s   speed %.0f   cargo %d   mining %.1f", t.name.c_str(),
                            t.stats.maxSpeed, t.stats.cargoCapacity, t.stats.miningRate),
                 contentX, shipY + 7, 18, current ? Color{ 120, 210, 130, 255 } : Ui::TEXT);

        Rectangle btnRect{ (float)(contentX + 540), (float)shipY, 160.0f, 30.0f };
        if (current)
        {
            Ui::Text("CURRENT", contentX + 540, shipY + 7, 16, Color{ 120, 210, 130, 255 });
        }
        else if (OwnsShip((int)i))
        {
            // Ship already owned — switching is free.
            Button switchBtn(btnRect, "Switch",
                             [this, i]()
                             {
                                 Proto::Command c;
                                 c.refitShip = (int)i;
                                 clientLink_->Send(Proto::EncodeCommand(c));
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
                              // The purchase is server-authoritative: the server charges,
                              // records the ship as owned and refits (BuyShip); the
                              // snapshot brings all three back (#5).
                              Proto::Command c;
                              c.buyShip = (int)i;
                              clientLink_->Send(Proto::EncodeCommand(c));
                              // No local bookkeeping: the next snapshot says whether the
                              // server agreed, and that is the only answer worth showing.
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

    // Which system the player is in, according to the server. The client has no
    // simulation of its own to ask (#3).
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

// The screen treatment's settings, over everything and never treated themselves (#120).
// A panel seen through the effect it is adjusting is a panel you cannot read while
// adjusting it.
void Game::DrawTreatmentSettings()
{
    const float w = 320.0f;
    const float h = Render::TreatmentPanelHeight(treatment_, &materials_) + 24.0f;
    Rectangle   panel{ (float)screenWidth_ - w - 16.0f, 60.0f, w,
                       fminf(h, (float)screenHeight_ - 80.0f) };

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
