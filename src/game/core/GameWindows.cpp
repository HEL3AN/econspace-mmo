// Game -- the missions, radar and settings windows, on Ui::Layout (#297).
//
// Presentation only, like GameHud.cpp: each window declares its content every frame with
// the shared widgets, sizes and colours from the theme, and reads its rows from a model
// with no drawing in it. Part of the Game class; see Game.cpp.
#include "core/Game.h"

#include "core/Faction.h"
#include "entities/Station.h"
#include "sim/MissionList.h"
#include "sim/Overview.h"
#include "sim/Sensor.h"
#include "ui/Fonts.h"
#include "ui/Paint.h"
#include "ui/Theme.h"
#include "ui/Units.h"

#include "raymath.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
// A sum of credits short enough for a column: 950, 12.5k, 1.2M.
std::string Credits(double v)
{
    char buf[32];
    if (v >= 1000000.0)
        std::snprintf(buf, sizeof(buf), "%.1fM", v / 1000000.0);
    else if (v >= 10000.0)
        std::snprintf(buf, sizeof(buf), "%.1fk", v / 1000.0);
    else
        std::snprintf(buf, sizeof(buf), "%.0f", v);
    return buf;
}

// A world distance as an instrument says it: 800, 25k, 1.2M.
std::string Span(float u)
{
    char buf[32];
    if (u >= 1000000.0f)
        std::snprintf(buf, sizeof(buf), "%.1fM", u / 1000000.0f);
    else if (u >= 1000.0f)
        std::snprintf(buf, sizeof(buf), "%.0fk", u / 1000.0f);
    else
        std::snprintf(buf, sizeof(buf), "%.0f", u);
    return buf;
}
}  // namespace

Color Game::StandingColor(FactionId f) const
{
    const Ui::Theme::Standing& s = Ui::CurrentTheme().standing;
    if (HostileToPlayerFaction(f))
        return s.hostile;
    const RepTier tier = Factions::TierOf(player_.GetReputation(f));
    return tier == RepTier::Liked || tier == RepTier::Allied ? s.friendly : s.neutral;
}

Ui::Mark Game::StandingMark(Sensor::Allegiance a)
{
    // The sharpest shape for the one that matters most, the softest for a friend.
    switch (a)
    {
        case Sensor::Allegiance::Own: return Ui::Mark::Ring;
        case Sensor::Allegiance::Friendly: return Ui::Mark::Circle;
        case Sensor::Allegiance::Neutral: return Ui::Mark::Diamond;
        case Sensor::Allegiance::Hostile: return Ui::Mark::Triangle;
        case Sensor::Allegiance::Unowned: break;
    }
    return Ui::Mark::None;
}

// --- Missions ---------------------------------------------------------------------------

// The goal prompt's state (#364), read from what the missions window reads: the snapshot's
// lists through MissionList, and the nearest station from the client's own proxies.
Goal::Prompt Game::CurrentGoal() const
{
    if (!Goal::Shown(Ui::GoalSetting(), Ui::GoalLearned()) || choosingStart_ || !playerShip_)
        return Goal::Prompt();
    Goal::State s;
    s.docked = mode_ == GameMode::Docked;
    s.dockedStationId = snapshot_.player.dockedStationId;
    s.offers = (int)snapshot_.missionOffers.size();
    s.roomForMore = (int)snapshot_.missionActive.size() < MissionSystem::MAX_ACTIVE;
    Obs::View view;
    view.snapshot = &snapshot_;
    view.layout = &layout_.byId;
    for (const MissionList::Row& r : MissionList::Build(view, MissionList::Board::Active))
    {
        Goal::Mission m;
        m.title = r.title;
        m.ready = r.ready;
        m.needs = r.needs;
        m.handInId = r.handInId;
        m.handInName = r.handInName;
        m.anywhere = r.job;
        s.active.push_back(std::move(m));
    }
    float best = 0.0f;
    for (const auto& e : clientWorld_)
    {
        if (e->GetKind() != EntityKind::Station)
            continue;
        const float d = Vector2Distance(e->GetPosition(), playerShip_->GetPosition());
        if (s.nearestStationId == 0 || d < best)
        {
            best = d;
            s.nearestStationId = e->GetId();
            s.nearestStationName = e->GetName();
        }
    }
    return Goal::Next(s);
}

// The missions window: the missions taken, or the board of the station the ship is docked
// at, as a table; and the chosen one in full, with what can be done about it. The rows are
// MissionList::Build's and the buttons are Actions' -- the mission's own (take it, hand it
// in) and those of the station it ends at (target it, fly there, dock), the same list the
// right-click menu offers for that station.
void Game::DrawMissionsContent(const Ui::Frame& f)
{
    using Ui::Box;
    using Ui::Size;
    using Ui::TextStyle;
    const Ui::Theme& t = Ui::CurrentTheme();
    Ui::Layout&      L = missionsLayout_;

    Obs::View view;
    view.snapshot = &snapshot_;
    view.layout = &layout_.byId;
    missionsTab_ = std::clamp(missionsTab_, 0, 1);
    const bool                          offers = missionsTab_ == 1;
    const std::vector<MissionList::Row> rows =
        MissionList::Build(view, offers ? MissionList::Board::Offers : MissionList::Board::Active);
    const int taken = (int)snapshot_.missionActive.size();

    // Something is always chosen while there is anything to choose: the details pane is the
    // point of the window, and an empty one asks for a click nobody knows to make.
    int& sel = missionsSel_[missionsTab_];
    sel = rows.empty() ? -1 : std::clamp(sel, 0, (int)rows.size() - 1);

    const std::vector<std::string> tabs = {
        std::string(TextFormat("Active  %d / %d", taken, MissionSystem::MAX_ACTIVE)),
        std::string(TextFormat("Offers  %d", (int)snapshot_.missionOffers.size())),
    };

    Ui::TableSpec table;
    table.id = "missions";
    table.columns = { { "mission", Size::Grow(), Ui::Align::Start, false },
                      { offers ? "type" : "progress", Size::Fixed(64.0f), Ui::Align::Start, false },
                      { "reward", Size::Fixed(56.0f), Ui::Align::End, false } };
    table.rows = (int)rows.size();
    table.empty = !offers                   ? "None taken: a station's board has work"
                  : snapshot_.player.docked ? "This station has no work on its board"
                                            : "The board is a station's: dock to see its work";
    table.cell = [&](int r, int c)
    {
        const MissionList::Row& row = rows[r];
        switch (c)
        {
            case 0: return Ui::Cell{ row.title, row.ready ? t.colors.good : t.colors.text };
            case 1: return Ui::Cell{ row.status, row.ready ? t.colors.good : t.colors.dim };
            default: return Ui::Cell{ Credits(row.reward), t.colors.money };
        }
    };
    table.rowFill = [&](int r) { return r == sel ? t.colors.selected : Color{ 0, 0, 0, 0 }; };
    table.tooltip = [&](int r) { return rows[r].title; };

    // The chosen mission in full, and its buttons.
    auto details = [&]
    {
        if (sel < 0)
            return;
        const MissionList::Row& m = rows[sel];
        L.Text(m.title, TextStyle::Title());
        L.Text(m.description, TextStyle::Body().Wrap());
        // Who gave it, in the instruments' colours: allegiance is the viewer's (#117).
        L.Field("Issuer", FactionName(m.faction), StandingColor(m.faction));
        L.Field("Reward", TextFormat("%.0f cr", m.reward), t.colors.money);
        L.Field("Standing", TextFormat("%+.1f", m.rep), t.colors.text);
        if (!offers)
        {
            L.Field("Progress", m.status, m.ready ? t.colors.good : t.colors.text);
            if (m.progress >= 0.0f)
                L.Bar(m.progress, m.ready ? t.colors.good : t.colors.accent);
        }
        if (m.job)
        {
            // A faction plan's job (#345): where the work is, how long it has, and no station
            // to go back to -- the faction pays by wire.
            L.Field("Where", m.where, t.colors.text);
            // A siege's job: the side it is for is the issuer's; this is the other (#407).
            if (m.siege)
                L.Field("Against", FactionName(m.against), StandingColor(m.against));
            if (!m.due.empty())
                L.Field("Time", m.due, m.due == "overdue" ? t.colors.warn : t.colors.text);
            L.Field("Hand in", "anywhere", t.colors.text);
        }
        else
        {
            // A market's contract (#343) is on the clock too, and may be for another system.
            if (!m.where.empty())
                L.Field("Where", m.where, t.colors.text);
            if (!m.due.empty())
                L.Field("Time", m.due, m.due == "overdue" ? t.colors.warn : t.colors.text);
            L.Field("Hand in at", m.handInHere ? m.handInName : "another system",
                    m.handInHere ? t.colors.text : t.colors.dim);
        }
        if (!m.needs.empty())
            L.Text("Needs: " + m.needs, TextStyle::Small().Tint(t.colors.dim).Wrap());

        // What can be done: the mission's own actions, then the station's that take the ship
        // there. The rest of the station's menu -- orbit, keep -- is the selected-item
        // window's, one click away through Target.
        Actions::MissionTarget target;
        target.index = m.index;
        target.offer = offers;
        target.ready = m.ready;
        target.docked = snapshot_.player.docked;
        target.active = taken;
        std::vector<Actions::Action> acts = Actions::ForMission(target);
        int                          stationId = 0;
        // Where it ends, when that is here: the station it is handed in at, or a supply job's
        // site -- flown to the same way, and docked at only if it is a station.
        Entity* there = nullptr;
        if (m.handInHere && !snapshot_.player.docked)
        {
            if (m.job)
            {
                for (const auto& e : clientWorld_)
                    if (e->GetId() == m.handInId)
                        there = e.get();
            }
            else
                there = StationById(m.handInId);
        }
        if (there != nullptr)
        {
            stationId = there->GetId();
            for (const Actions::Action& a :
                 Actions::For(ActionTarget(*there), playerShip_->GetPosition()))
                if (a.verb == Actions::Verb::Select || a.verb == Actions::Verb::Approach ||
                    a.verb == Actions::Verb::Warp || a.verb == Actions::Verb::Dock)
                    acts.push_back(a);
        }
        if (offers && snapshot_.player.docked && taken >= MissionSystem::MAX_ACTIVE)
            L.Text(TextFormat("%d missions taken: hand one in before taking another", taken),
                   TextStyle::Small().Tint(t.colors.warn).Wrap());
        if (acts.empty())
            return;
        L.Divider();
        // Two to a row, as in the selected-item window.
        for (size_t i = 0; i < acts.size(); i += 2)
            L.Row(Box().GrowX().Gap(t.metrics.rowGap),
                  [&]
                  {
                      for (size_t k = i; k < std::min(acts.size(), i + 2); k++)
                      {
                          const std::string id = "act" + std::to_string(k);
                          const bool        mission = acts[k].verb == Actions::Verb::Accept ||
                                                      acts[k].verb == Actions::Verb::HandIn;
                          if (L.Button(id, acts[k].label, mission))
                              Perform(acts[k], mission ? 0 : stationId, { 0.0f, 0.0f });
                      }
                  });
    };

    // Side by side once there is room for a list and a description that wraps sensibly.
    const bool wide = f.Area().width >= Ui::Px(520.0f);
    L.Begin(f);
    L.Column(Box().Grow().Gap(t.metrics.gap),
             [&]
             {
                 L.Tabs("tabs", tabs, missionsTab_);
                 auto list = [&]
                 {
                     const Ui::TableEvents ev = L.Table(table);
                     if (ev.clicked >= 0)
                         sel = ev.clicked;
                 };
                 auto pane = [&](Box box)
                 { L.Scroll(box.Id("details").Gap(t.metrics.rowGap), details); };
                 if (wide)
                     L.Row(Box().Grow().Gap(t.metrics.padding),
                           [&]
                           {
                               L.Column(Box().Width(Size::Percent(0.5f)).Height(Size::Grow()),
                                        list);
                               pane(Box().Grow());
                           });
                 else
                 {
                     // The list keeps a share of the height, so a long description scrolls
                     // in its own pane rather than pushing the list away.
                     L.Column(Box().GrowX().Height(Size::Percent(0.42f)), list);
                     L.Divider();
                     pane(Box().Grow());
                 }
             });
    L.End();
    L.Draw();
}

// --- Radar ------------------------------------------------------------------------------

// The radar (#297): a free view of the system that does not follow the ship. A toolbar laid
// out like any window, and below it the scope, which takes whatever room the window has and
// is drawn by hand -- a picture, not a layout. In the scope: drag pans, the wheel zooms, a
// click selects, a right click opens the menu on a thing or on a point.
//
// An instrument, so allegiance is shown here, as this pilot sees it, in the theme's standing
// colours (#117); things nobody owns are told apart by shape rather than by colour.
void Game::DrawRadarContent(const Ui::Frame& f)
{
    using Ui::Box;
    using Ui::Size;
    using Ui::TextStyle;
    const Ui::Theme& t = Ui::CurrentTheme();
    Ui::Layout&      L = radarLayout_;

    const Vector2 sp = snapshot_.player.pos;  // M4c: the radar reads the snapshot
    if (!radarInit_)                          // on first display, center on the player
    {
        radarCenter_ = sp;
        radarInit_ = true;
    }

    // Base scale -- from the system's extent (stable while panning), over the smaller side
    // of where the scope was last drawn.
    float maxR = 600.0f;
    for (const auto& e : snapshot_.entities)
        maxR = fmaxf(maxR, sqrtf(e.pos.x * e.pos.x + e.pos.y * e.pos.y));
    maxR *= 1.1f;
    auto scaleOf = [&](Rectangle r)
    { return r.width > 0.0f ? (fminf(r.width, r.height) / 2.0f) / maxR * radarZoom_ : 0.0f; };

    bool        centre = false, zoomIn = false, zoomOut = false;
    const float lastScale = scaleOf(radarScope_);
    L.Begin(f);
    L.Column(Box().Grow().Gap(t.metrics.rowGap),
             [&]
             {
                 L.Row(
                     Box().GrowX().Gap(t.metrics.rowGap).Align(Ui::Align::Start, Ui::Align::Center),
                     [&]
                     {
                         L.Column(Box().Width(Size::Fixed(56.0f)),
                                  [&] { centre = L.Button("centre", "Ship"); });
                         L.Tooltip("centre", "Centre the radar on your ship");
                         L.Column(Box().Width(Size::Fixed(30.0f)),
                                  [&] { zoomOut = L.Button("out", "-"); });
                         L.Column(Box().Width(Size::Fixed(30.0f)),
                                  [&] { zoomIn = L.Button("in", "+"); });
                         L.Spacer();
                         // How far across the scope reaches, side to side.
                         if (lastScale > 0.0f)
                             L.Text(Span(radarScope_.width / lastScale) + " across",
                                    TextStyle::Small().Tint(t.colors.dim));
                     });
                 L.Row(Box().Id("scope").Grow().Border(t.colors.border, t.metrics.border), [] {});
             });
    L.End();
    const Rectangle r = L.BoxOf("scope");
    radarScope_ = r;
    L.Draw();
    if (r.width <= 0.0f || r.height <= 0.0f)
        return;

    if (centre)
        radarCenter_ = sp;
    if (zoomIn || zoomOut)
        radarZoom_ = Clamp(radarZoom_ * (zoomIn ? 1.25f : 0.8f), 0.25f, 12.0f);

    const Vector2 c{ r.x + r.width / 2.0f, r.y + r.height / 2.0f };
    const float   scale = scaleOf(r);
    auto          toRadar = [&](Vector2 w) -> Vector2
    { return { c.x + (w.x - radarCenter_.x) * scale, c.y + (w.y - radarCenter_.y) * scale }; };
    auto toWorld = [&](Vector2 s) -> Vector2
    { return { radarCenter_.x + (s.x - c.x) / scale, radarCenter_.y + (s.y - c.y) / scale }; };

    // Over it only when the radar's window owns the mouse (#297).
    const Vector2 m = f.Mouse();
    const bool    over = f.Hovered(r);
    const float   pick = Ui::Px(7.0f);
    auto          hitAt = [&](Vector2 at)
    {
        for (const auto& e : snapshot_.entities)
            if (CheckCollisionPointCircle(at, toRadar(e.pos), pick))
                return e.id;
        return 0;
    };

    if (over)
    {
        const float wheel = f.Wheel();
        if (wheel != 0.0f)
            radarZoom_ = Clamp(radarZoom_ * (1.0f + wheel * 0.12f), 0.25f, 12.0f);
    }
    if (over && f.Pressed(MOUSE_BUTTON_LEFT))
    {
        radarDragging_ = true;
        radarDragMoved_ = false;
        radarDragLast_ = m;
        radarPressPos_ = m;
    }
    if (radarDragging_)
    {
        // A press keeps the window the mouse's owner until it is let go (#297).
        if (f.Down(MOUSE_BUTTON_LEFT))
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
            if (!radarDragMoved_)  // it was a click -- select the object under the cursor
            {
                selected_ = FindEntityById(hitAt(m));
                if (selected_ != nullptr)
                    desk_.SetOpen(WIN_TARGET, true);
            }
        }
    }
    if (over && f.Pressed(MOUSE_BUTTON_RIGHT))
    {
        Entity* hit = FindEntityById(hitAt(m));
        if (hit != nullptr)
            OpenContextMenu(hit);
        else
            OpenContextMenuAt(toWorld(m));
    }

    const Sensor::Standing standing = ViewerStanding();
    const float            u = Ui::Scale();  // the blips follow the interface's scale
    const int              selId = selected_ != nullptr ? selected_->GetId() : 0;
    // What the cursor rests on, for its label (#362); not while the scope is being dragged.
    const int hoverId = over && !(radarDragging_ && radarDragMoved_) ? hitAt(m) : 0;
    BeginScissorMode((int)r.x, (int)r.y, (int)r.width, (int)r.height);
    for (const auto& e : snapshot_.entities)
    {
        const Vector2 p = toRadar(e.pos);
        if (!CheckCollisionPointRec(p, r))
            continue;
        const Sensor::Allegiance a = Sensor::Classify(e, standing);
        Color                    col = Sensor::ColorOf(a);
        switch (e.kind)
        {
            // Unowned: one colour, told apart by shape -- none of them a standing's shape.
            case Proto::EntityKind::Star: DrawCircleV(p, 4.5f * u, col); break;
            case Proto::EntityKind::Planet:
                DrawCircleLinesV(p, 4.0f * u, col);
                DrawCircleV(p, 1.5f * u, col);
                break;
            case Proto::EntityKind::Field:  // a scatter of rocks, not a hostile's triangle
                DrawCircleV({ p.x - 2.5f * u, p.y + 1.5f * u }, 1.3f * u, col);
                DrawCircleV({ p.x + 2.5f * u, p.y + 1.5f * u }, 1.3f * u, col);
                DrawCircleV({ p.x, p.y - 2.5f * u }, 1.3f * u, col);
                break;
            case Proto::EntityKind::Nebula: DrawCircleLinesV(p, 6.0f * u, Fade(col, 0.5f)); break;
            case Proto::EntityKind::Derelict:
                DrawLineEx({ p.x - 3 * u, p.y - 3 * u }, { p.x + 3 * u, p.y + 3 * u }, u, col);
                DrawLineEx({ p.x - 3 * u, p.y + 3 * u }, { p.x + 3 * u, p.y - 3 * u }, u, col);
                break;
            case Proto::EntityKind::Gate: DrawPolyLinesEx(p, 4, 4.5f * u, 0.0f, u, col); break;
            // Someone's: the colour is their side, as this pilot sees it, and a hostile one
            // carries the hostile triangle above it, so the colour is never the only sign.
            case Proto::EntityKind::Station:
                DrawRectangleV({ p.x - 3.0f * u, p.y - 3.0f * u }, { 6.0f * u, 6.0f * u }, col);
                if (a == Sensor::Allegiance::Hostile)
                    Ui::DrawMark({ p.x, p.y - 7.5f * u }, 2.6f * u, Ui::Mark::Triangle, col);
                break;
            // Yours or someone else's -- the one allegiance a structure has yet (#39). Yours
            // has a dot in it.
            case Proto::EntityKind::Structure:
            {
                const auto l = layout_.byId.find(e.id);
                const bool mine = l != layout_.byId.end() && l->second.owner == pilotName_;
                col = mine ? t.standing.own : t.standing.neutral;
                DrawRectangleLinesEx({ p.x - 3.0f * u, p.y - 3.0f * u, 6.0f * u, 6.0f * u }, u,
                                     col);
                if (mine)
                    DrawCircleV(p, 1.2f * u, col);
                break;
            }
            // Ships: a shape per standing (#362), the same as the overview's chips.
            default: Ui::DrawMark(p, 3.6f * u, StandingMark(a), col); break;
        }
        if (e.id != 0 && e.id == selId)
            DrawCircleLinesV(p, 7.0f * u, t.colors.text);
    }

    // The player's own ship.
    const Vector2 pp = toRadar(sp);
    if (CheckCollisionPointRec(pp, r))
        Ui::DrawMark(pp, 5.5f * u, StandingMark(Sensor::Allegiance::Own), t.standing.own);

    // The blip under the cursor, by name, distance and standing (#362): a reading a pilot
    // can get without selecting it, and the standing in words as well as colour and shape.
    if (hoverId != 0)
        for (const auto& e : snapshot_.entities)
            if (e.id == hoverId)
            {
                const Sensor::Allegiance a = Sensor::Classify(e, standing);
                std::string label = (e.name.empty() ? std::string(Overview::KindWord(e)) : e.name) +
                                    "  " + Ui::Distance(Vector2Distance(e.pos, sp));
                if (a != Sensor::Allegiance::Unowned)
                    label += std::string("  ") + Sensor::Word(a);
                const float   px = (float)Ui::FontPx(Ui::Px(t.fontSize.body));
                const Vector2 ext = Ui::MeasureString(Ui::Face::Regular, px, label);
                const Vector2 p = toRadar(e.pos);
                const float   pad = Ui::Px(4.0f);
                // Right of the blip, or left of it where the scope ends.
                float x = p.x + Ui::Px(10.0f);
                if (x + ext.x + 2.0f * pad > r.x + r.width)
                    x = p.x - Ui::Px(10.0f) - ext.x - 2.0f * pad;
                const float y =
                    std::clamp(p.y - ext.y * 0.5f - pad, r.y, r.y + r.height - ext.y - 2.0f * pad);
                const Rectangle plate{ x, y, ext.x + 2.0f * pad, ext.y + 2.0f * pad };
                DrawRectangleRec(plate, Ui::Faded(t.colors.panel, 0.92f));
                DrawRectangleLinesEx(plate, 1.0f, t.colors.border);
                Ui::DrawString(Ui::Face::Regular, label, { x + pad, y + pad }, px, t.colors.text);
                break;
            }
    EndScissorMode();
}

// --- Settings ---------------------------------------------------------------------------

float Game::AutoUiScale() const
{
    return Ui::AutoUserScale(GetMonitorHeight(GetCurrentMonitor()), Ui::DisplayScale());
}

// `s` <= 0: back to what this screen suggests, and nothing kept in the file.
void Game::ApplyUiScale(float s)
{
    Ui::OverrideDisplayScale(0.0f);  // a --uiscale run is over
    uiScaleLocked_ = false;
    if (s > 0.0f)
        Ui::ChooseUserScale(s);
    else
        Ui::UseAutoUserScale(AutoUiScale());
    if (uiSettingsWritable_ && !Ui::SaveUiSettings(uiSettingsPath_))
        TraceLog(LOG_WARNING, "UI settings: cannot write %s", uiSettingsPath_.c_str());
}

// The settings window: what the display shows, how the interface is sized, and the keys.
// Every setting here is applied as it is chosen and kept where it was kept before: the UI
// scale in ui_settings.json, the screen treatment in look.json, the window layout in
// ui_layout.json.
void Game::DrawSettingsContent(const Ui::Frame& f)
{
    using Ui::Box;
    using Ui::Size;
    using Ui::TextStyle;
    const Ui::Theme& t = Ui::CurrentTheme();
    Ui::Layout&      L = settingsLayout_;

    struct Res
    {
        int w, h;
    };
    static const Res   modes[] = { { 1280, 720 }, { 1600, 900 }, { 1920, 1080 } };
    static const float scales[] = { 1.0f, 1.25f, 1.5f, 2.0f };
    // The keys, as HandleInput reads them. Not rebindable yet; this is the list to learn.
    static const char* const keys[][2] = {
        { "W  /  Up", "Thrust" },
        { "S  /  Down", "Brake" },
        { "A D  /  Left Right", "Turn" },
        { "E", "Dock, near a station" },
        { "X", "Stabiliser" },
        { "M", "Mining laser" },
        { "F", "Weapon" },
        { "C", "Camera back to the ship" },
        { "Middle drag", "Look around" },
        { "Wheel", "Zoom" },
        { "T", "Selected item" },
        { "O", "Overview" },
        { "R", "Radar" },
        { "J", "Missions" },
        { "G", "Galaxy map" },
        { "N", "Name this system, on the map" },
        { "V", "Sensor screen" },
        { "Esc", "Close what is on top" },
        { "F9", "What a frame costs" },
        { "F10", "Screen treatment" },
        { "F11", "Fullscreen" },
    };

    // The scale's slider moves a draft; letting go applies it. Applied while held, the
    // window would grow under the cursor and the slider run away from it.
    if (!scaleDragging_)
        scaleDraft_ = Ui::UserScale();

    const bool wide = f.Area().width >= Ui::Px(440.0f);
    L.Begin(f);
    L.Column(
        Box().Grow().Gap(t.metrics.gap),
        [&]
        {
            L.Tabs("tabs", { "Display", "Interface", "Keys" }, settingsTab_);
            if (settingsTab_ == 2)
            {
                Ui::TableSpec table;
                table.id = "keys";
                table.columns = { { "key", Size::Fixed(wide ? 140.0f : 112.0f) },
                                  { "does", Size::Grow() } };
                table.rows = (int)(sizeof(keys) / sizeof(keys[0]));
                table.cell = [&](int r, int c)
                { return Ui::Cell{ keys[r][c], c == 0 ? t.colors.accent : t.colors.text }; };
                table.tooltip = [&](int r)
                { return std::string(keys[r][0]) + "  -  " + keys[r][1]; };
                L.Table(table);
                return;
            }

            L.Scroll(
                Box().Grow().Id("page").Gap(t.metrics.gap),
                [&]
                {
                    // A form: each setting's name on the left, its controls beside it, and
                    // what needs saying about it under the controls.
                    auto hint = [&](const std::string& text, Color c)
                    { L.FormRow("", [&] { L.Text(text, TextStyle::Small().Tint(c).Wrap()); }); };
                    if (settingsTab_ == 0)
                    {
                        L.FormRow("Resolution",
                                  [&]
                                  {
                                      // Side by side when there is room for three, one above
                                      // the other otherwise.
                                      Box group = Box().GrowX().Gap(t.metrics.rowGap);
                                      if (!wide)
                                          group.Column();
                                      L.Open(group);
                                      for (int i = 0; i < 3; i++)
                                      {
                                          const Res& r = modes[i];
                                          const bool current =
                                              screenWidth_ == r.w && screenHeight_ == r.h;
                                          if (L.Button("res" + std::to_string(i),
                                                       TextFormat("%d x %d", r.w, r.h), current))
                                              ApplyResolution(r.w, r.h);
                                      }
                                      L.Close();
                                  });

                        const bool fs = IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE);
                        L.FormRow("Fullscreen",
                                  [&]
                                  {
                                      if (L.Button("fullscreen", fs ? "On" : "Off", fs))
                                          ToggleBorderlessWindowed();
                                  });
                        L.Tooltip("fullscreen", "Borderless, the size of the monitor (F11)");

                        const bool on = treatment_.Config().enabled;
                        L.FormRow("Screen treatment",
                                  [&]
                                  {
                                      if (L.Button("treat", on ? "On" : "Off", on))
                                      {
                                          treatment_.Config().enabled = !on;
                                          SaveTreatment();
                                      }
                                      if (L.Button("tune", "Tune...", desk_.IsOpen(WIN_LOOK)))
                                          desk_.Toggle(WIN_LOOK);
                                  });
                        L.Tooltip("tune", "Every pass of the treatment, one by one (F10)");
                        if (!treatment_.Available())
                            hint("This machine compiled none of its shaders: the picture is drawn "
                                 "without it.",
                                 t.colors.warn);
                    }
                    else
                    {
                        // Every theme in data/ui_themes/; the one in use lit.
                        // One under another: five names in a line are wider than the window.
                        L.FormRow("Theme",
                                  [&]
                                  {
                                      L.Column(Box().GrowX().Gap(t.metrics.rowGap),
                                               [&]
                                               {
                                                   for (size_t i = 0; i < themes_.size(); i++)
                                                       if (L.Button("theme" + std::to_string(i),
                                                                    themes_[i].name,
                                                                    themes_[i].id == t.id))
                                                           themeToUse_ = themes_[i].id;
                                               });
                                  });
                        // Off until asked for: the world is drawn into a texture and blurred
                        // every frame, which a laptop's GPU feels (#297).
                        const bool blurOn = Ui::BlurSetting();
                        L.FormRow("Blur behind windows",
                                  [&]
                                  {
                                      if (L.Button("blur", blurOn ? "On" : "Off", blurOn))
                                      {
                                          Ui::SetBlurSetting(!blurOn);
                                          if (uiSettingsWritable_ &&
                                              !Ui::SaveUiSettings(uiSettingsPath_))
                                              TraceLog(LOG_WARNING, "UI settings: cannot write %s",
                                                       uiSettingsPath_.c_str());
                                      }
                                  });
                        hint(t.style.blurs ? "The world under a window, frosted. It costs a copy "
                                             "of the world every frame; off on a slow GPU."
                                           : "This theme's windows are solid; the blur is for "
                                             "glass ones.",
                             t.colors.dim);
                        // The instruments over the world view (#361): the marker on the
                        // player's own ship and the standing brackets on the others.
                        const bool bracketsOn = Ui::BracketSetting();
                        L.FormRow(
                            "Brackets in space",
                            [&]
                            {
                                if (L.Button("brackets", bracketsOn ? "On" : "Off", bracketsOn))
                                {
                                    Ui::SetBracketSetting(!bracketsOn);
                                    if (uiSettingsWritable_ && !Ui::SaveUiSettings(uiSettingsPath_))
                                        TraceLog(LOG_WARNING, "UI settings: cannot write %s",
                                                 uiSettingsPath_.c_str());
                                }
                            });
                        hint("A mark on your ship, and on every ship in view one in the colour "
                             "of its standing toward you.",
                             t.colors.dim);
                        // Orange and blue for the standings, over any theme (#362).
                        const bool cbOn = Ui::ColourBlindSetting();
                        L.FormRow("Colour-blind palette",
                                  [&]
                                  {
                                      if (L.Button("cblind", cbOn ? "On" : "Off", cbOn))
                                      {
                                          Ui::SetColourBlindSetting(!cbOn);
                                          if (uiSettingsWritable_ &&
                                              !Ui::SaveUiSettings(uiSettingsPath_))
                                              TraceLog(LOG_WARNING, "UI settings: cannot write %s",
                                                       uiSettingsPath_.c_str());
                                      }
                                  });
                        hint("Standings in orange and blue rather than red and green, on the "
                             "radar, the overview and the brackets. Each is a shape as well.",
                             t.colors.dim);
                        // The goal prompt (#364). Turning it on again starts the count again:
                        // a player who asks for it back wants to see it.
                        const bool goalOn =
                            Ui::GoalSetting() && Ui::GoalLearned() < Goal::LEARNED_AFTER;
                        L.FormRow("What to do next",
                                  [&]
                                  {
                                      if (L.Button("goal", goalOn ? "On" : "Off", goalOn))
                                      {
                                          Ui::SetGoalSetting(!goalOn);
                                          if (!goalOn)
                                              Ui::SetGoalLearned(0);
                                          if (uiSettingsWritable_ &&
                                              !Ui::SaveUiSettings(uiSettingsPath_))
                                              TraceLog(LOG_WARNING, "UI settings: cannot write %s",
                                                       uiSettingsPath_.c_str());
                                      }
                                  });
                        hint("A line under the system's name with the next step, until you have "
                             "handed in a few missions.",
                             t.colors.dim);
                        L.FormRow("Interface scale",
                                  [&]
                                  {
                                      if (L.Slider("scale", scaleDraft_, Ui::MIN_USER_SCALE,
                                                   Ui::MAX_USER_SCALE))
                                          scaleDragging_ = true;
                                      L.Text(TextFormat("%.0f%%", scaleDraft_ * 100.0f),
                                             TextStyle::Body());
                                  });
                        if (scaleDragging_ && !f.Down(MOUSE_BUTTON_LEFT))
                        {
                            scaleDragging_ = false;
                            ApplyUiScale(std::round(scaleDraft_ * 20.0f) / 20.0f);  // 5% steps
                        }
                        L.FormRow(
                            "",
                            [&]
                            {
                                // Four in a line when there is room, two by two
                                // otherwise: in a wider face four ran off the window.
                                Box group = Box().GrowX().Gap(t.metrics.rowGap);
                                if (!wide)
                                    group.Column();
                                L.Open(group);
                                for (int row = 0; row < (wide ? 1 : 2); row++)
                                    L.Row(Box().GrowX().Gap(t.metrics.rowGap),
                                          [&]
                                          {
                                              const int n = wide ? 4 : 2;
                                              for (int k = 0; k < n; k++)
                                              {
                                                  const int   i = row * n + k;
                                                  const float s = scales[i];
                                                  const bool  current =
                                                      !uiScaleLocked_ && Ui::UserScaleChosen() &&
                                                      fabsf(Ui::UserScale() - s) < 0.01f;
                                                  if (L.Button("scale" + std::to_string(i),
                                                               TextFormat("%.0f%%", s * 100.0f),
                                                               current))
                                                      ApplyUiScale(s);
                                              }
                                          });
                                L.Close();
                            });
                        if (uiScaleLocked_)
                            hint("Set for this run by --uiscale; choosing one here keeps it.",
                                 t.colors.dim);
                        hint("Times what the display asks for, which is " +
                                 std::string(TextFormat("%.0f%%", Ui::DisplayScale() * 100.0f)) +
                                 " here.",
                             t.colors.dim);
                        // Until one is chosen, the scale is what this screen suggests (#362).
                        const bool auto_ = !Ui::UserScaleChosen() && !uiScaleLocked_;
                        L.FormRow("",
                                  [&]
                                  {
                                      if (L.Button("scaleauto",
                                                   TextFormat("Automatic  %.0f%%",
                                                              AutoUiScale() * 100.0f),
                                                   auto_))
                                          ApplyUiScale(0.0f);
                                  });
                        L.Tooltip("scaleauto", "What this screen's size and DPI suggest; "
                                               "choosing a scale above replaces it");

                        L.FormRow("Windows",
                                  [&]
                                  {
                                      if (L.Button("reset", "Reset layout"))
                                          desk_.ResetLayout();
                                  });
                        hint("Every window back where it started: unpinned, ungrouped and out of "
                             "its tabs.",
                             t.colors.dim);
                    }
                });
        });
    L.End();
    L.Draw();
}
