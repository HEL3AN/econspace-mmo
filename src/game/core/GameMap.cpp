// Game -- the galaxy map (#432): the full-screen star map, moved here unchanged from
// GameHud.cpp so the map's route planning has a file of its own.
//
// Part of the Game class; see Game.cpp.

#include "core/Game.h"
#include "ui/Desk.h"
#include "ui/Input.h"
#include "ui/Paint.h"
#include "ui/UiTheme.h"
#include "ui/Units.h"
#include "raymath.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

// The menu bar's width, as GameHud.cpp has it: the map starts to its right.
static float MenuBarW()
{
    return Ui::Px(Ui::CurrentTheme().metrics.menuBar);
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
    // Opaque, as the sensor screen is (#366): the shade alone let a bright ship sprite show
    // through the middle of the map, a second picture of a different place.
    Color ground = t.colors.shade;
    ground.a = 255;
    DrawRectangle(0, 0, screenWidth_, screenHeight_, ground);

    // Galactic news (system captures, reconquests) from the server's galaxy snapshot, newest
    // first. A column of its own that wraps and scrolls: a long line used to run off the
    // right of the screen.
    const std::vector<std::string>& news = galaxyState_.events;
    Ui::Layout&                     L = mapLayout_;
    L.Begin(desk_.SurfaceFrame(
        WIN_MAP, { MenuBarW(), 0.0f, screenWidth_ - MenuBarW(), (float)screenHeight_ }));
    L.Column(
        Box().Grow().Pad(t.metrics.padding * 2.0f).Gap(t.metrics.gap),
        [&]
        {
            L.Row(Box().GrowX().Gap(t.metrics.padding).Align(Ui::Align::Start, Ui::Align::Center),
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
                      L.Column(Box()
                                   .Width(Size::Fixed(t.metrics.sidePanel))
                                   .Height(Size::Grow())
                                   .Pad(t.metrics.padding)
                                   .Gap(t.metrics.gap)
                                   .Fill(t.colors.panel)
                                   .Border(t.colors.border, t.metrics.border)
                                   .ThemeCorners(),
                               [&]
                               {
                                   L.Text("LEGEND", TextStyle::Label());
                                   const TextStyle note = TextStyle::Small().Wrap();
                                   L.Text("Filled: charted. A ring alone: uncharted -- a gate "
                                          "leads there, and nothing more is known.",
                                          note);
                                   L.Text("The ring's colour is who holds the system; a "
                                          "triangle over it, that they would shoot you.",
                                          note);
                                   L.Text("sec, pir, econ: security, pirates, prosperity.", note);
                                   L.Text("A dashed line: a wormhole your scans found. Only "
                                          "you see it, and it closes.",
                                          note);
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

    // The picture's own text, at the theme's sizes and the player's scale like every window
    // (#366): a system's name a step above body text, what is known about it at the label
    // size, the naming hint at body size. Hand-placed pixel sizes stayed at 16 and 12 on a
    // screen where everything else had grown, and read as specks. Every offset around a
    // node is in the same units, so the lines under a name move apart as the text grows.
    const float k = Ui::Scale();
    auto        u = [k](float units) { return units * k; };
    const int   namePx = (int)Ui::Px(t.fontSize.body + 3.0f);
    const int   notePx = (int)Ui::Px(t.fontSize.label + 1.0f);
    const int   hintPx = (int)Ui::Px(t.fontSize.body + 1.0f);
    const float line = (float)notePx + u(3.0f);  // one line of notes under the name

    const std::vector<WorldLoader::SystemInfo>& systems = universe_.systems;
    if (systems.empty() || area.width <= 0.0f || area.height <= 0.0f)
    {
        const char* none = "No galaxy data";
        Ui::Text(none, (int)(area.x + (area.width - Ui::TextWidth(none, namePx)) * 0.5f),
                 (int)(area.y + area.height * 0.5f), namePx, t.colors.dim);
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
    float pad = u(80.0f);
    float baseScale = fminf((area.width - 2 * pad) / spanX, (area.height - 2 * pad) / spanY);

    // On first display: where you are, close enough to read your own civilisation and the
    // frontier beside it (#342 S2). The whole galaxy -- several centres thousands of map
    // units apart -- is a wheel away.
    constexpr float NEAR_SPAN = 1800.0f, MAX_ZOOM = 40.0f;
    if (!galaxyInit_)
    {
        galaxyCenter_ = { (minX + maxX) / 2.0f, (minY + maxY) / 2.0f };
        galaxyZoom_ = 1.0f;
        for (const auto& s : systems)
            if (s.id == snapshot_.systemId)
            {
                galaxyCenter_ = s.mapPos;
                galaxyZoom_ = Clamp(fmaxf(spanX, spanY) / NEAR_SPAN, 1.0f, MAX_ZOOM);
                // Better: everything within two jumps fills the picture, with room beside the
                // outermost nodes for their names (#366). A fixed span left the neighbourhood
                // a knot in the middle of an empty screen.
                std::vector<std::string> near = { s.id };
                for (int hop = 0; hop < 2; hop++)
                {
                    const size_t known = near.size();
                    for (const auto& l : universe_.links)
                        for (size_t i = 0; i < known; i++)
                        {
                            if (l.a == near[i] &&
                                std::find(near.begin(), near.end(), l.b) == near.end())
                                near.push_back(l.b);
                            else if (l.b == near[i] &&
                                     std::find(near.begin(), near.end(), l.a) == near.end())
                                near.push_back(l.a);
                        }
                }
                float reachX = 0.0f, reachY = 0.0f;
                for (const auto& o : systems)
                    if (std::find(near.begin(), near.end(), o.id) != near.end())
                    {
                        reachX = fmaxf(reachX, fabsf(o.mapPos.x - s.mapPos.x));
                        reachY = fmaxf(reachY, fabsf(o.mapPos.y - s.mapPos.y));
                    }
                const float roomX = area.width * 0.5f - u(150.0f);
                const float roomY = area.height * 0.5f - u(40.0f);
                if (reachX + reachY > 0.0f && roomX > 0.0f && roomY > 0.0f && baseScale > 0.0f)
                {
                    const float fit = fminf(reachX > 0.0f ? roomX / reachX : 1e9f,
                                            reachY > 0.0f ? roomY / reachY : 1e9f);
                    galaxyZoom_ = Clamp(fit / baseScale, 1.0f, MAX_ZOOM);
                }
            }
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
            galaxyZoom_ = Clamp(galaxyZoom_ * (1.0f + wheel * 0.12f), 0.3f, MAX_ZOOM);
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
        if (!posById(l.a, a) || !posById(l.b, b))
            continue;
        if (l.kind != "hole")
        {
            DrawLineEx(a, b, 1.5f, Fade(Ui::PANEL_BORDER, 0.9f));
            continue;
        }
        // A roaming hole this pilot found (#342 S9): dashed, in the hole's own colour, and
        // only theirs -- nobody else's map has it.
        const float dx = b.x - a.x, dy = b.y - a.y;
        const float len = sqrtf(dx * dx + dy * dy);
        const int   dashes = std::max(1, (int)(len / u(10.0f)));
        for (int k = 0; k < dashes; k += 2)
        {
            const float t0 = (float)k / dashes, t1 = (float)(k + 1) / dashes;
            DrawLineEx({ a.x + dx * t0, a.y + dy * t0 }, { a.x + dx * t1, a.y + dy * t1 }, 2.0f,
                       Color{ 120, 220, 190, 230 });
        }
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
        return CheckCollisionPointCircle(m, toScreen(s->mapPos), u(12.0f)) ? 1 : 2;
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
    // Every node first: a label never runs across another system's disc (#366). A label
    // starts clear of its own node, so this costs it nothing.
    for (const auto& s : systems)
    {
        const Vector2 q = toScreen(s.mapPos);
        const float   r = u(11.0f);
        claimed.push_back({ q.x - r, q.y - r, 2.0f * r, 2.0f * r });
    }

    // System nodes.
    for (const WorldLoader::SystemInfo* sp : order)
    {
        const WorldLoader::SystemInfo& s = *sp;
        Vector2                        p = toScreen(s.mapPos);
        bool                           cur = (s.id == activeSys);
        const bool                     hovered = CheckCollisionPointCircle(m, p, u(12.0f));
        const float                    room = nearestOnScreen(s);
        bool                           detail = cur || hovered || room >= u(110.0f);
        bool                           named = cur || hovered || room >= u(45.0f);
        const float                    gapX = u(14.0f);  // from the node to its labels
        float                          lx = p.x + gapX;  // where its labels start
        // The name is centred on the node; line `i` of what is known sits under it.
        const float nameY = p.y - (float)namePx * 0.5f;
        auto        lineY = [&](int i)
        { return (int)(p.y + (float)namePx * 0.5f + u(2.0f) + (float)(i - 1) * line); };
        {
            // What its labels would cover: the name line, and the lines under it.
            float nameW = (float)Ui::TextWidth(s.name.c_str(), namePx);
            if (!s.designation.empty())
                nameW += u(8.0f) + (float)Ui::TextWidth(s.designation.c_str(), notePx);
            const float     detailW = fmaxf(nameW, u(s.charted ? 190.0f : 80.0f));
            const int       lines = !s.charted ? 1
                                    : cur      ? (CanNameHere() ? 5 : 4)
                                               : (s.discoverer.empty() ? 2 : 3);
            const float     bottom = (float)lineY(lines) + line - nameY;
            const Rectangle nameBox{ p.x + gapX, nameY, nameW, (float)namePx + u(2.0f) };
            const Rectangle leftBox{ p.x - gapX - nameW, nameY, nameW, (float)namePx + u(2.0f) };
            const Rectangle detailBox{ p.x + gapX, nameY, detailW, bottom };
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
            DrawCircleLines((int)p.x, (int)p.y, u(7.0f), Ui::TEXT_DIM);
            if (named)
            {
                Ui::Text(s.name.c_str(), (int)lx, (int)nameY, namePx, Ui::TEXT_DIM);
                if (detail)
                    Ui::Text("uncharted", (int)lx, lineY(1), notePx, Ui::TEXT_DIM);
            }
            continue;
        }
        DrawCircleV(p, u(cur ? 10.0f : 7.0f), cur ? Ui::ACCENT : Ui::TEXT_DIM);
        if (cur)
            DrawCircleLines((int)p.x, (int)p.y, u(16.0f), Fade(Ui::ACCENT, 0.6f));
        if (named)
        {
            Ui::Text(s.name.c_str(), (int)lx, (int)nameY, namePx, cur ? Ui::ACCENT : Ui::TEXT);
            // A named system keeps its designation beside the name (#145), on its baseline.
            if (!s.designation.empty())
                Ui::Text(s.designation.c_str(),
                         (int)(lx + u(8.0f)) + Ui::TextWidth(s.name.c_str(), namePx),
                         (int)(nameY + (float)(namePx - notePx) * 0.75f), notePx, Ui::TEXT_DIM);
        }

        // Live summary: security/pirates/economy/controller, from the server's galaxy
        // snapshot (galaxyState_).
        bool      haveStats = false;
        float     security = 0.0f, prosperity = 0.0f;
        int       pirates = 0;
        FactionId controller = Factions::Unaffiliated();
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
        const float ring = u(cur ? 13.0f : 10.0f);
        // A system held by a faction that would shoot this pilot says so by shape as well as
        // by the ring's colour (#362): the hostile triangle over the ring, as on the radar.
        const bool hostileHeld = haveStats && HostileToPlayerFaction(controller);
        if (hostileHeld)
            Ui::DrawMark({ p.x, p.y - ring - u(6.0f) }, u(4.5f), Ui::Mark::Triangle,
                         t.standing.hostile);
        if (haveStats && !detail)
            // Crowded: the controller only as the ring's colour.
            DrawCircleLines((int)p.x, (int)p.y, ring, Fade(FactionColor(controller), 0.7f));
        else if (haveStats)
        {
            Color secCol = security >= 0.7f   ? Color{ 120, 210, 130, 255 }
                           : security >= 0.4f ? GOLD
                                              : Color{ 230, 120, 60, 255 };
            Ui::Text(
                TextFormat("sec %.1f  pir %d  econ %.0f%%", security, pirates, prosperity * 100.0f),
                (int)lx, lineY(1), notePx, secCol);
            // Territory controller (L3) — in the faction's color.
            Ui::Text(FactionName(controller).c_str(), (int)lx, lineY(2), notePx,
                     FactionColor(controller));
            // The node ring is tinted with the controller's color.
            DrawCircleLines((int)p.x, (int)p.y, ring, Fade(FactionColor(controller), 0.7f));
        }
        if (cur)
            Ui::Text("you are here", (int)lx, lineY(3), notePx, Ui::TEXT_DIM);
        if (detail && !s.discoverer.empty())
            Ui::Text(TextFormat("found by %s", s.discoverer.c_str()), (int)lx, lineY(cur ? 4 : 3),
                     notePx, Ui::TEXT_DIM);
        if (cur && CanNameHere())
        {
            if (desk_.KeyboardFocus().Holds(WIN_MAP, "name"))
                nameAt = { lx, (float)lineY(4) + line * 0.5f };  // drawn after the clip
            else
                Ui::Text("[N] name this system", (int)lx, lineY(5), hintPx, Ui::ACCENT);
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
