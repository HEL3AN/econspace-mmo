// The survey: many generated systems on one screen, with the seed on each (#141). One
// translation unit of Editor (#17).
//
// A single good system proves nothing about a generator, because a single good system can
// be arranged. This is the gallery's argument (#118) one level up: a rule is judged by its
// distribution, so the screen shows a run of consecutive seeds, every system of each
// region, and says out loud what a rule hides best -- systems that came out empty, systems
// that came out the same as another, and the range of everything being tuned.
//
// The counting is Gen::Analyse, which has no window and is tested. This half draws it, and
// draws each system through WorldLoader::BuildSystem and Render::Present -- the path the
// game takes -- so a card is what a player arriving there would find.

#include "Editor.h"

#include "core/World.h"
#include "entities/Planet.h"
#include "gen/Region.h"
#include "ui/Controls.h"
#include "ui/UiTheme.h"
#include "raymath.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace
{
const float kTop = 100.0f;  // below the header
const float kLeft = 16.0f;
const float kGap = 8.0f;
const float kPanelW = 300.0f;
const float kMinCard = 140.0f;  // below this a whole system stops reading; scroll instead

const Color kEmpty = { 236, 84, 72, 255 };
const Color kThin = { 232, 178, 64, 255 };
const Color kTwin = { 206, 116, 236, 255 };
// The markers' colours. They are an instrument, not the object's colour (#117).
const Color kBelt = { 200, 172, 120, 255 };
const Color kCloud = { 150, 130, 210, 255 };
const Color kWreck = { 226, 226, 226, 255 };
const Color kRare = { 255, 214, 92, 255 };

// The smallest a thing is drawn on a card, in pixels. A planet is a hundredth of a
// system's width and a gate a thousandth: at true size a card is a star and some rings.
// Enlarged, never shrunk, and only on the screen -- the documents are untouched.
float MinPixels(EntityKind k)
{
    switch (k)
    {
        case EntityKind::Star: return 7.0f;
        case EntityKind::Planet: return 3.5f;
        case EntityKind::Field: return 4.5f;
        case EntityKind::Nebula: return 6.0f;
        case EntityKind::Derelict: return 2.5f;
        case EntityKind::Gate: return 3.0f;
        default: return 3.0f;
    }
}

// raylib's scissor does not nest (see Editor_Gallery): every clipped draw states its own
// rectangle, already intersected with the one it lives in.
Rectangle Clip(Rectangle a, Rectangle b)
{
    const float x1 = fmaxf(a.x, b.x), y1 = fmaxf(a.y, b.y);
    const float x2 = fminf(a.x + a.width, b.x + b.width);
    const float y2 = fminf(a.y + a.height, b.y + b.height);
    return { x1, y1, fmaxf(0.0f, x2 - x1), fmaxf(0.0f, y2 - y1) };
}

void BeginClip(Rectangle r)
{
    BeginScissorMode((int)r.x, (int)r.y, (int)r.width, (int)r.height);
}

// A kind's name as a person reads it. The survey counts whatever arrays a document has,
// so an unknown one is shown as it is spelled.
std::string KindName(const std::string& kind)
{
    if (kind == "asteroidFields")
        return "belts";
    if (kind == "derelicts")
        return "wrecks";
    if (kind == "nebulae")
        return "clouds";
    return kind;
}

std::string KindShort(const std::string& kind)
{
    if (kind == "asteroidFields")
        return "b";
    if (kind == "derelicts")
        return "w";
    if (kind == "nebulae")
        return "n";
    if (kind == "stations")
        return "st";
    return kind.substr(0, std::min<size_t>(kind.size(), 3)) + ":";
}

// One distribution as a row of bars. Returns the height it took.
float Histogram(float x, float y, float w, const char* title,
                const std::vector<std::pair<std::string, int>>& bins, int total, Color bar)
{
    Ui::Text(title, (int)x, (int)y, 12, Ui::TEXT_DIM);
    if (bins.empty() || total <= 0)
    {
        Ui::Text("--", (int)x + 110, (int)y, 12, Ui::TEXT_DIM);
        return 20.0f;
    }
    int most = 1;
    for (const auto& b : bins)
        most = std::max(most, b.second);

    const float barsTop = y + 28.0f;
    const float barsH = 30.0f;
    const float slot = fminf(36.0f, w / (float)bins.size());
    for (size_t i = 0; i < bins.size(); i++)
    {
        const float bx = x + slot * (float)i;
        const float h = barsH * (float)bins[i].second / (float)most;
        DrawRectangleRec({ bx + 2.0f, barsTop + barsH - h, slot - 4.0f, h }, Fade(bar, 0.8f));
        if (bins[i].second > 0)
            Ui::Text(TextFormat("%d", bins[i].second), (int)(bx + 3.0f),
                     (int)(barsTop + barsH - h - 11.0f), 10, Ui::TEXT);
        // Clipped to the slot: a long category name belongs to its own bar.
        BeginScissorMode((int)bx, (int)(barsTop + barsH + 1.0f), (int)slot, 14);
        Ui::Text(bins[i].first.c_str(), (int)(bx + 2.0f), (int)(barsTop + barsH + 2.0f), 10,
                 Ui::TEXT_DIM);
        EndScissorMode();
    }
    return 28.0f + barsH + 18.0f;
}

std::vector<std::pair<std::string, int>> Bins(const std::map<int, int>& m)
{
    std::vector<std::pair<std::string, int>> out;
    if (m.empty())
        return out;
    // Every value between the smallest and the largest, so a gap is a visible gap.
    for (int v = m.begin()->first; v <= m.rbegin()->first; v++)
    {
        const auto it = m.find(v);
        out.push_back({ std::to_string(v), it == m.end() ? 0 : it->second });
    }
    return out;
}

std::vector<std::pair<std::string, int>> Bins(const std::map<std::string, int>& m)
{
    return std::vector<std::pair<std::string, int>>(m.begin(), m.end());
}

}  // namespace

// ---- Entering, leaving, generating ----------------------------------------------------

Rectangle Editor::SurveyButtonRect() const
{
    const Rectangle g = GalleryButtonRect();
    return { g.x - 6.0f - 110.0f, g.y, 110.0f, g.height };
}

void Editor::OpenSurvey(uint64_t seed, int card)
{
    surveySeed_ = seed > 0 ? seed : 1;
    // Fifty systems want more than the editor's default window. Only from the command
    // line: a key press that resizes someone's window would be a surprise.
    const int mon = GetCurrentMonitor();
    const int w = std::min(1900, GetMonitorWidth(mon) - 40);
    const int h = std::min(1060, GetMonitorHeight(mon) - 80);
    if (w > screenWidth_ && h > screenHeight_)
    {
        SetWindowSize(w, h);
        SetWindowPosition((GetMonitorWidth(mon) - w) / 2, 32);
        screenWidth_ = w;
        screenHeight_ = h;
    }
    EnterSurveyMode(true);
    if (card >= 0 && card < (int)surveyCards_.size())
        surveySelected_ = card;
}

void Editor::EnterSurveyMode(bool on)
{
    mode_ = on ? Mode::Survey : Mode::System;
    activeField_.clear();
    openDropdown_.clear();
    placeArchetype_.clear();
    if (on && surveyCards_.empty())
        RegenerateSurvey();
}

void Editor::RegenerateSurvey()
{
    // The regions a server would generate from these seeds: the same home system, the
    // same map around it, so the survey is of the real thing and not of a lab copy.
    Gen::RegionParams base;
    nlohmann::json    homeDoc;
    for (const WorldLoader::SystemInfo& info : universe_.systems)
    {
        base.knownMap.push_back(info.mapPos);
        if (info.id != universe_.startId)
            continue;
        base.homeId = info.id;
        base.homeMap = info.mapPos;
        std::ifstream in(dataDir_ + "systems/" + info.file);
        if (in.is_open())
            homeDoc = nlohmann::json::parse(in, nullptr, false);
    }
    if (base.homeId.empty())
        base.homeId = "home";
    if (homeDoc.is_object())
        base.homeSystem = &homeDoc;

    surveyCards_.clear();
    std::vector<nlohmann::json> docs;
    for (Gen::SurveySystem& s : Gen::GenerateSurvey(base, surveySeed_, surveyRegions_))
    {
        SurveyCard card;
        // Built and described once: a scene of fifty systems is a few thousand items, and
        // nothing on a card moves.
        for (const auto& e : WorldLoader::BuildSystem(s.doc))
        {
            card.items.push_back(e->Describe());
            const Planet* p =
                e->GetKind() == EntityKind::Planet ? dynamic_cast<const Planet*>(e.get()) : nullptr;
            card.labels.push_back(p != nullptr ? PlanetTypeName(p->GetPlanetType()) : e->GetName());
        }
        docs.push_back(s.doc);
        card.system = std::move(s);
        surveyCards_.push_back(std::move(card));
    }
    survey_ = Gen::Analyse(docs);
    if (surveySelected_ >= (int)surveyCards_.size())
        surveySelected_ = -1;
    surveyScroll_ = 0.0f;
    TraceLog(LOG_INFO, "Survey: seeds %llu..%llu, %d systems, %d empty, %d thin, %d twins",
             (unsigned long long)surveySeed_,
             (unsigned long long)(surveySeed_ + (uint64_t)surveyRegions_ - 1),
             survey_.stats.systems, survey_.stats.empty, survey_.stats.thin, survey_.stats.twins);
}

// ---- Layout -------------------------------------------------------------------------

Rectangle Editor::SurveyGridRect() const
{
    return { kLeft, kTop, (float)screenWidth_ - kPanelW - kLeft * 2.0f,
             (float)screenHeight_ - kTop - 10.0f };
}

Rectangle Editor::SurveyCardRect(int index) const
{
    const Rectangle grid = SurveyGridRect();
    const int       n = std::max(1, (int)surveyCards_.size());

    // The largest square card at which every system fits on the screen at once; below a
    // size a system stops reading, and then the grid scrolls instead.
    float cell = 0.0f;
    int   cols = 1;
    for (int c = 1; c <= n; c++)
    {
        const int   rows = (n + c - 1) / c;
        const float byW = (grid.width - kGap * (float)(c - 1)) / (float)c;
        const float byH = (grid.height - kGap * (float)(rows - 1)) / (float)rows;
        const float s = fminf(byW, byH);
        if (s > cell)
        {
            cell = s;
            cols = c;
        }
    }
    if (cell < kMinCard)
    {
        cols = std::max(1, (int)((grid.width + kGap) / (kMinCard + kGap)));
        cell = (grid.width - kGap * (float)(cols - 1)) / (float)cols;  // the full width
    }
    const int   row = index / cols, col = index % cols;
    const float used = (float)cols * cell + (float)(cols - 1) * kGap;
    const float left = grid.x + fmaxf(0.0f, (grid.width - used) / 2.0f);
    return { left + (float)col * (cell + kGap), grid.y + (float)row * (cell + kGap) - surveyScroll_,
             cell, cell };
}

int Editor::SurveyHit(Vector2 p) const
{
    if (!CheckCollisionPointRec(p, SurveyGridRect()))
        return -1;
    for (int i = 0; i < (int)surveyCards_.size(); i++)
        if (CheckCollisionPointRec(p, SurveyCardRect(i)))
            return i;
    return -1;
}

// ---- Input ----------------------------------------------------------------------------

void Editor::HandleSurveyInput()
{
    const int count = (int)surveyCards_.size();

    if (IsKeyPressed(KEY_ESCAPE))
    {
        if (surveySelected_ >= 0)
            surveySelected_ = -1;
        else
            EnterSurveyMode(false);
        return;
    }

    // Paging: the next run of seeds, the previous one, the same one again.
    if (IsKeyPressed(KEY_PAGE_DOWN))
    {
        surveySeed_ += (uint64_t)surveyRegions_;
        RegenerateSurvey();
    }
    if (IsKeyPressed(KEY_PAGE_UP) && surveySeed_ > 1)
    {
        surveySeed_ = surveySeed_ > (uint64_t)surveyRegions_ ? surveySeed_ - surveyRegions_ : 1;
        RegenerateSurvey();
    }
    if (IsKeyPressed(KEY_R))
        RegenerateSurvey();
    if (IsKeyPressed(KEY_M))
        surveyMarkers_ = !surveyMarkers_;
    if ((IsKeyPressed(KEY_EQUAL) || IsKeyPressed(KEY_KP_ADD)) && surveyRegions_ < 12)
    {
        surveyRegions_++;
        RegenerateSurvey();
    }
    if ((IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_KP_SUBTRACT)) && surveyRegions_ > 1)
    {
        surveyRegions_--;
        RegenerateSurvey();
    }

    const Vector2 m = GetMousePosition();
    if (surveySelected_ >= 0)
    {
        // The enlarged card: arrows step through the run, a click puts it back.
        if (IsKeyPressed(KEY_RIGHT) && surveySelected_ + 1 < count)
            surveySelected_++;
        if (IsKeyPressed(KEY_LEFT) && surveySelected_ > 0)
            surveySelected_--;
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(m, SurveyGridRect()))
            surveySelected_ = -1;
        return;
    }

    if (CheckCollisionPointRec(m, SurveyGridRect()))
    {
        const float wheel = GetMouseWheelMove();
        if (wheel != 0.0f)
            surveyScroll_ -= wheel * 60.0f;
        const float last = count > 0 ? SurveyCardRect(count - 1).y + surveyScroll_ +
                                           SurveyCardRect(count - 1).height
                                     : kTop;
        surveyScroll_ =
            Clamp(surveyScroll_, 0.0f, fmaxf(0.0f, last + 10.0f - (float)screenHeight_));

        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
            surveySelected_ = SurveyHit(m);
    }
}

// ---- Drawing --------------------------------------------------------------------------

void Editor::DrawSurveySystem(const SurveyCard& card, Rectangle box, bool detail, bool labels)
{
    // One scale for every card: the whole system, a million units, edge to edge. A rule
    // about how far out things go is then something the eye can compare across the grid.
    Camera2D cam{};
    cam.offset = { box.x + box.width / 2.0f, box.y + box.height / 2.0f };
    cam.target = { 0.0f, 0.0f };
    cam.zoom = (fminf(box.width, box.height) * 0.5f - 4.0f) / World::SYSTEM_RADIUS;

    // Enlarged, a card can afford to show things bigger than a pixel's worth of a system.
    const float minScale = detail ? 2.0f : 1.0f;

    if (labels)
    {
        BeginClip(Clip(box, SurveyGridRect()));
        // Markers: an instrument over the picture, not part of it (#117) -- a belt or a gate
        // a few pixels across is the thing most worth counting and the hardest to see.
        if (surveyMarkers_)
            for (const Render::Item& it : card.items)
            {
                const Vector2 p = GetWorldToScreen2D(it.pos, cam);
                const float   r = fmaxf(it.size * cam.zoom, MinPixels(it.kind) * minScale);
                switch (it.kind)
                {
                    case EntityKind::Star:
                    case EntityKind::Planet: break;
                    case EntityKind::Field: DrawCircleLinesV(p, r + 2.0f, kBelt); break;
                    case EntityKind::Nebula: DrawCircleLinesV(p, r, Fade(kCloud, 0.8f)); break;
                    case EntityKind::Derelict:
                        DrawLineV({ p.x - 3.0f, p.y - 3.0f }, { p.x + 3.0f, p.y + 3.0f }, kWreck);
                        DrawLineV({ p.x - 3.0f, p.y + 3.0f }, { p.x + 3.0f, p.y - 3.0f }, kWreck);
                        break;
                    case EntityKind::Gate:
                        DrawRectangleLinesEx({ p.x - 3.0f, p.y - 3.0f, 6.0f, 6.0f }, 1.0f,
                                             Ui::ACCENT);
                        break;
                    default:
                        // Anything the survey has no marker for -- a new kind a rule just
                        // added -- is the thing to notice, so it gets the loudest one.
                        DrawPoly(p, 4, 4.5f, 45.0f, kRare);
                        break;
                }
            }
        if (detail)
        {
            // Names beside things, never on them: the picture is what is being judged.
            for (size_t i = 0; i < card.items.size(); i++)
            {
                const Render::Item& it = card.items[i];
                if (it.kind == EntityKind::Star)
                    continue;
                const Vector2 p = GetWorldToScreen2D(it.pos, cam);
                const float   r = fmaxf(it.size * cam.zoom, MinPixels(it.kind) * minScale);
                Ui::Text(card.labels[i].c_str(), (int)(p.x + r + 5.0f), (int)(p.y - 6.0f), 12,
                         it.kind == EntityKind::Gate ? Ui::ACCENT : Ui::TEXT);
            }
        }
        EndScissorMode();
        return;
    }

    std::vector<Render::Item> scene = card.items;
    // Lit before anything is enlarged: the light is where the star is and reaches as far
    // as it does in the game.
    const Render::Lighting lights = Render::LightsFrom(scene);
    for (Render::Item& it : scene)
        it.size = fmaxf(it.size, MinPixels(it.kind) * minScale / cam.zoom);

    BeginClip(Clip(box, SurveyGridRect()));
    BeginMode2D(cam);
    // The edge of the system, and every orbit as a thin ring: the backends draw a planet's
    // ring at a fraction of its colour, which at this scale is nothing.
    DrawCircleLines(0, 0, World::SYSTEM_RADIUS, Fade(Ui::PANEL_BORDER, 0.6f));
    for (const Render::Item& it : scene)
        if (it.ring > 0.0f)
            DrawCircleLines(0, 0, it.ring, Fade(Ui::TEXT_DIM, detail ? 0.35f : 0.45f));
    Render::Present(std::move(scene), lights, cam, *backend_);
    EndMode2D();
    EndScissorMode();
}

void Editor::DrawSurvey(bool labels)
{
    const Rectangle grid = SurveyGridRect();
    const int       count = (int)surveyCards_.size();

    // The enlarged card stands in for the grid rather than over it: two pictures at once
    // under a screen treatment is one too many to judge.
    if (surveySelected_ >= 0 && surveySelected_ < count)
    {
        const SurveyCard& c = surveyCards_[(size_t)surveySelected_];
        const float       side = fminf(grid.width, grid.height);
        const Rectangle   box{ grid.x + (grid.width - side) / 2.0f, grid.y, side, side };
        if (!labels)
        {
            DrawRectangleRec(box, Fade(Ui::PANEL_BG, 0.6f));
            DrawSurveySystem(c, box, true, false);
            return;
        }
        DrawRectangleLinesEx(box, 1.0f, Ui::PANEL_BORDER);
        DrawSurveySystem(c, box, true, true);

        const Gen::SystemSummary& s = survey_.summaries[(size_t)surveySelected_];
        const int                 twin = survey_.twinOf[(size_t)surveySelected_];
        float                     y = box.y + 10.0f;
        const int                 x = (int)box.x + 12;
        Ui::Text(TextFormat("%s   seed %llu   %s   depth %d", c.system.designation.c_str(),
                            (unsigned long long)c.system.seed, c.system.id.c_str(), c.system.depth),
                 x, (int)y, 18, Ui::ACCENT);
        y += 24.0f;
        std::string line = (s.star.empty() ? std::string("no") : s.star) + " star   " +
                           std::to_string(s.planets) + " planets   " + std::to_string(s.gates) +
                           " gates";
        if (!s.character.empty())
            line += "   character: " + s.character;
        Ui::Text(line.c_str(), x, (int)y, 13, Ui::TEXT);
        y += 18.0f;
        std::string things;
        for (const auto& kv : s.things)
            things += std::to_string(kv.second) + " " + KindName(kv.first) + "   ";
        Ui::Text(things.empty() ? "nothing else" : things.c_str(), x, (int)y, 13, Ui::TEXT);
        y += 20.0f;
        if (s.Empty())
            Ui::Text("EMPTY: nothing here but a star, planets and gates", x, (int)y, 14, kEmpty);
        else if (s.Thin())
            Ui::Text("THIN: one thing worth stopping for", x, (int)y, 14, kThin);
        if (twin >= 0)
            Ui::Text(TextFormat("TWIN of %s (seed %llu): same star, same counts",
                                surveyCards_[(size_t)twin].system.designation.c_str(),
                                (unsigned long long)surveyCards_[(size_t)twin].system.seed),
                     x, (int)y + 18, 14, kTwin);
        Ui::Text(TextFormat("card %d of %d   <- / -> step   click or Esc: back to the grid",
                            surveySelected_ + 1, count),
                 x, (int)(box.y + box.height - 22.0f), 12, Ui::TEXT_DIM);
        return;
    }

    // Twins are shown together: hovering one card outlines every card with its signature.
    const int hover = SurveyHit(GetMousePosition());
    int       hoverGroup = -1;
    if (hover >= 0)
        hoverGroup = survey_.twinOf[(size_t)hover] >= 0 ? survey_.twinOf[(size_t)hover] : hover;

    for (int i = 0; i < count; i++)
    {
        const Rectangle card = SurveyCardRect(i);
        if (card.y + card.height < grid.y || card.y > grid.y + grid.height)
            continue;
        const SurveyCard&         c = surveyCards_[(size_t)i];
        const Gen::SystemSummary& s = survey_.summaries[(size_t)i];
        const int                 twin = survey_.twinOf[(size_t)i];
        const int                 group = twin >= 0 ? twin : i;

        if (!labels)
        {
            BeginClip(Clip(grid, card));
            DrawRectangleRec(card, i == hover ? Fade(Ui::ACCENT, 0.08f) : Fade(Ui::PANEL_BG, 0.7f));
            EndScissorMode();
            DrawSurveySystem(c, card, false, false);
            continue;
        }
        // The same box the picture was fitted to, so the markers land on it.
        DrawSurveySystem(c, card, false, true);

        BeginClip(Clip(grid, card));
        Color border = Ui::PANEL_BORDER;
        if (s.Empty())
            border = kEmpty;
        else if (twin >= 0)
            border = kTwin;
        if (hoverGroup >= 0 && group == hoverGroup)
            border = i == hover ? Ui::ACCENT : kTwin;
        DrawRectangleLinesEx(card, (hoverGroup >= 0 && group == hoverGroup) ? 2.0f : 1.0f, border);

        // Seed and designation on top, what it has at the bottom, the verdict at the right.
        DrawRectangleRec({ card.x + 1.0f, card.y + 1.0f, card.width - 2.0f, 30.0f },
                         Fade(Ui::PANEL_BG, 0.55f));
        Ui::Text(TextFormat("%s", c.system.designation.c_str()), (int)card.x + 6, (int)card.y + 3,
                 13, Ui::TEXT);
        Ui::Text(TextFormat("seed %llu", (unsigned long long)c.system.seed),
                 (int)(card.x + card.width) - 6 -
                     Ui::TextWidth(TextFormat("seed %llu", (unsigned long long)c.system.seed), 11),
                 (int)card.y + 5, 11, Ui::TEXT_DIM);
        std::string sub = "d" + std::to_string(c.system.depth) + "  " + s.star;
        if (!s.character.empty())
            sub += "  " + s.character;
        Ui::Text(sub.c_str(), (int)card.x + 6, (int)card.y + 18, 10, Ui::TEXT_DIM);

        std::string counts = "p" + std::to_string(s.planets);
        for (const auto& kv : s.things)
            counts += " " + KindShort(kv.first) + std::to_string(kv.second);
        counts += " g" + std::to_string(s.gates);
        const float by = card.y + card.height - 18.0f;
        DrawRectangleRec({ card.x + 1.0f, by - 2.0f, card.width - 2.0f, 19.0f },
                         Fade(Ui::PANEL_BG, 0.55f));
        Ui::Text(counts.c_str(), (int)card.x + 6, (int)by, 11, Ui::TEXT);

        const char* flag = nullptr;
        Color       fc = Ui::TEXT;
        std::string twinText;
        if (s.Empty())
        {
            flag = "EMPTY";
            fc = kEmpty;
        }
        else if (twin >= 0)
        {
            twinText = "= " + surveyCards_[(size_t)twin].system.designation + " s" +
                       std::to_string(surveyCards_[(size_t)twin].system.seed);
            flag = twinText.c_str();
            fc = kTwin;
        }
        else if (s.Thin())
        {
            flag = "thin";
            fc = kThin;
        }
        if (flag != nullptr)
            Ui::Text(flag, (int)(card.x + card.width) - 6 - Ui::TextWidth(flag, 12), (int)by - 1,
                     12, fc);
        EndScissorMode();
    }
}

void Editor::DrawSurveyPanel()
{
    const Rectangle panel{ (float)screenWidth_ - kPanelW, 0.0f, kPanelW, (float)screenHeight_ };
    DrawRectangleRec(panel, Ui::PANEL_BG);
    DrawRectangleLinesEx(panel, 1.0f, Ui::PANEL_BORDER);

    const float             x = panel.x + 14.0f;
    const float             w = panel.width - 28.0f;
    float                   y = 14.0f;
    const Gen::SurveyStats& st = survey_.stats;

    Ui::Text("SURVEY", (int)x, (int)y, 14, Ui::ACCENT);
    Ui::Text(
        TextFormat("generator v%d   backend %s [F2]", Gen::GENERATOR_VERSION, backend_->Name()),
        (int)x + 70, (int)y + 2, 11, Ui::TEXT_DIM);
    y += 22.0f;
    Ui::Text(TextFormat("seeds %llu-%llu   %d systems", (unsigned long long)surveySeed_,
                        (unsigned long long)(surveySeed_ + (uint64_t)surveyRegions_ - 1),
                        st.systems),
             (int)x, (int)y, 13, Ui::TEXT);
    y += 22.0f;

    // The three verdicts first, as numbers: they are what the screen is for.
    auto verdict = [&](const char* name, int n, Color c, float at)
    {
        Ui::Text(TextFormat("%d", n), (int)at, (int)y, 20, n > 0 ? c : Ui::TEXT_DIM);
        Ui::Text(name, (int)at, (int)y + 22, 11, Ui::TEXT_DIM);
    };
    verdict("empty", st.empty, kEmpty, x);
    verdict("thin", st.thin, kThin, x + w / 3.0f);
    verdict("twins", st.twins, kTwin, x + 2.0f * w / 3.0f);
    y += 44.0f;
    DrawLine((int)x, (int)y, (int)(x + w), (int)y, Ui::PANEL_BORDER);
    y += 8.0f;

    const int n = st.systems;
    y += Histogram(x, y, w, "star", Bins(st.stars), n, Color{ 240, 200, 110, 255 });
    if (!st.characters.empty())
        y += Histogram(x, y, w, "character", Bins(st.characters), n, kTwin);
    y += Histogram(x, y, w, "planets", Bins(st.planets), n, Ui::ACCENT);
    y += Histogram(x, y, w, "outermost orbit (x100k)", Bins(st.reach), n, Ui::ACCENT);
    y += Histogram(x, y, w, "worth stopping for (all kinds)", Bins(st.worth), n, kThin);
    for (const auto& kv : st.things)
    {
        if (y > (float)screenHeight_ - 120.0f)
            break;  // a run with many kinds outgrows the panel; the commonest come first
        y += Histogram(x, y, w,
                       TextFormat("%s   (%d in all)", KindName(kv.first).c_str(),
                                  st.totals.count(kv.first) ? st.totals.at(kv.first) : 0),
                       Bins(kv.second), n, Ui::ACCENT);
    }

    const float ky = (float)screenHeight_ - 74.0f;
    DrawLine((int)x, (int)ky - 6, (int)(x + w), (int)ky - 6, Ui::PANEL_BORDER);
    Ui::Text("PgDn / PgUp  next / previous seeds", (int)x, (int)ky, 11, Ui::TEXT_DIM);
    Ui::Text("+ / -  regions per page   R  again   M  markers", (int)x, (int)ky + 15, 11,
             Ui::TEXT_DIM);
    Ui::Text("click  enlarge     F10  treatment     Esc  back", (int)x, (int)ky + 30, 11,
             Ui::TEXT_DIM);
    Ui::Text("p planets  b belts  w wrecks  n clouds  g gates", (int)x, (int)ky + 45, 11,
             Ui::TEXT_DIM);
}
