// The region map (#237): every system a seed makes, on one screen, where the generator put
// it on the galaxy map. One translation unit of Editor (#17).
//
// The survey judges a rule by its distribution; this judges one region as a place. It is
// the region a server would build -- generated, then every pin on top -- so what a node
// says is what opening it shows, and the systems a pin touches are marked, which is the
// one thing about a region the generator's output cannot say. A click opens the system
// for editing through OpenGenerated, the path `worldeditor region SEED system ID` takes.

#include "Editor.h"

#include "gen/Region.h"
#include "ui/UiTheme.h"
#include "raymath.h"
#include <algorithm>
#include <cmath>
#include <string>

using nlohmann::json;

namespace
{
const float kTop = 104.0f;  // below the header
const float kPanelW = 300.0f;
const float kNode = 7.0f;  // a star's radius on the map, in pixels

// Instruments, not the objects' colours (#117): the survey's marker colours, so a belt is
// the same mark on both screens.
const Color kBelt = { 200, 172, 120, 255 };
const Color kCloud = { 150, 130, 210, 255 };
const Color kWreck = { 226, 226, 226, 255 };
const Color kRare = { 255, 214, 92, 255 };
const Color kWormhole = { 206, 116, 236, 255 };
const Color kPirates = { 236, 84, 72, 255 };

// A star's colour as the map shows it: the type, legible on a dark ground.
Color StarColor(const std::string& type)
{
    if (type == "Red")
        return { 240, 116, 84, 255 };
    if (type == "Blue")
        return { 132, 178, 255, 255 };
    if (type == "Yellow")
        return { 255, 214, 120, 255 };
    return Ui::TEXT_DIM;  // a type the map does not know yet
}

// The stars of a system document: one, two, or none.
std::vector<std::string> StarTypes(const json& doc)
{
    std::vector<std::string> out;
    if (doc.contains("stars") && doc["stars"].is_array())
        for (const json& s : doc["stars"])
            out.push_back(s.value("type", std::string()));
    else if (doc.contains("star") && doc["star"].is_object())
        out.push_back(doc["star"].value("type", std::string()));
    return out;
}

Vector2 MapPos(const json& entry)
{
    const json& m = entry.value("map", json::array());
    if (!m.is_array() || m.size() < 2 || !m[0].is_number() || !m[1].is_number())
        return { 0.0f, 0.0f };
    return { m[0].get<float>(), m[1].get<float>() };
}

// Whether a gate in `doc` is an ancient one leading to `to`: that link is an old way
// through, not part of the region's tree, and is drawn as such.
bool AncientGateTo(const json& doc, const std::string& to)
{
    if (!doc.contains("gates") || !doc["gates"].is_array())
        return false;
    for (const json& g : doc["gates"])
        if (g.value("destination", std::string()) == to &&
            g.value("archetype", std::string()) == "gate.ancient")
            return true;
    return false;
}

// A summary counts an object under its archetype when it names one, so a station a player
// or a pin put there is "station.trade_hub", not "stations" -- and it is still a station.
bool IsStation(const std::string& kind)
{
    return kind == "stations" || kind.rfind("station.", 0) == 0;
}

int StationCount(const Gen::SystemSummary& s)
{
    int n = 0;
    for (const auto& kv : s.things)
        n += IsStation(kv.first) ? kv.second : 0;
    return n;
}

// Everything that is not a belt, a cloud, a wreck or a station: the rare finds, which the
// generator marks by naming an archetype.
int RareCount(const Gen::SystemSummary& s)
{
    int n = 0;
    for (const auto& kv : s.things)
        if (kv.first != "asteroidFields" && kv.first != "nebulae" && kv.first != "derelicts" &&
            !IsStation(kv.first))
            n += kv.second;
    return n;
}

void DashedLine(Vector2 a, Vector2 b, float thick, Color c)
{
    const float len = Vector2Distance(a, b);
    if (len <= 0.0f)
        return;
    const Vector2 d = Vector2Scale(Vector2Subtract(b, a), 1.0f / len);
    for (float t = 0.0f; t < len; t += 10.0f)
        DrawLineEx(Vector2Add(a, Vector2Scale(d, t)),
                   Vector2Add(a, Vector2Scale(d, fminf(t + 5.0f, len))), thick, c);
}

// What is under a system's name, one mark per kind. The same marks in the legend.
enum class Mark
{
    Planet,
    Station,
    Belt,
    Cloud,
    Wreck,
    Rare
};

void DrawMark(float x, float y, Color c, Mark m)
{
    switch (m)
    {
        case Mark::Planet: DrawCircleV({ x + 3.0f, y }, 2.5f, c); break;
        case Mark::Station: DrawRectangleLinesEx({ x, y - 3.0f, 6.0f, 6.0f }, 1.0f, c); break;
        case Mark::Belt: DrawCircleLinesV({ x + 3.0f, y }, 3.0f, c); break;
        case Mark::Cloud:  // soft, with an edge
            DrawCircleV({ x + 3.0f, y }, 3.5f, Fade(c, 0.45f));
            DrawCircleLinesV({ x + 3.0f, y }, 3.5f, c);
            break;
        case Mark::Wreck:
            DrawLineV({ x, y - 3.0f }, { x + 6.0f, y + 3.0f }, c);
            DrawLineV({ x, y + 3.0f }, { x + 6.0f, y - 3.0f }, c);
            break;
        case Mark::Rare: DrawPoly({ x + 3.0f, y }, 4, 3.5f, 45.0f, c); break;
    }
}

// One mark and its count, left to right from x. Returns the x after it.
float Icon(float x, float y, int count, Color c, Mark m)
{
    DrawMark(x, y, c, m);
    const char* n = TextFormat("%d", count);
    Ui::Text(n, (int)x + 8, (int)y - 5, 10, Ui::TEXT);
    return x + 8.0f + (float)Ui::TextWidth(n, 10) + 6.0f;
}

}  // namespace

// ---- Entering, leaving, generating ----------------------------------------------------

Rectangle Editor::RegionButtonRect() const
{
    // To the right of the save button: the left of the header belongs to the title.
    const Rectangle s = SaveButtonRect();
    return { s.x + s.width + 6.0f, s.y, 110.0f, s.height };
}

void Editor::OpenRegionMap(uint64_t seed, const std::string& focus)
{
    if (mode_ != Mode::Region)
        regionMap_.back = mode_;
    BuildRegionMap(seed > 0 ? seed : 1);
    regionMap_.focus = focus;
    mode_ = Mode::Region;
    activeField_.clear();
    openDropdown_.clear();
    placeArchetype_.clear();
}

void Editor::ToggleRegionMap()
{
    if (mode_ == Mode::Region)
    {
        mode_ = regionMap_.back == Mode::Region ? Mode::System : regionMap_.back;
        return;
    }
    // The region of whatever is in front of you: the generated system being edited, the
    // enlarged survey card, the survey's first seed -- or the one the map last showed.
    if (mode_ == Mode::System && generated_.open)
        OpenRegionMap(generated_.seed, generated_.id);
    else if (mode_ == Mode::Survey && surveySelected_ >= 0 &&
             surveySelected_ < (int)surveyCards_.size())
    {
        const Gen::SurveySystem& s = surveyCards_[(size_t)surveySelected_].system;
        OpenRegionMap(s.seed, s.id);
    }
    else if (mode_ == Mode::Survey)
        OpenRegionMap(surveySeed_);
    else
        OpenRegionMap(regionMap_.seed > 0 ? regionMap_.seed : 1);
}

void Editor::BuildRegionMap(uint64_t seed)
{
    RegionMap&        m = regionMap_;
    json              homeDoc;
    Gen::RegionParams params = RegionParamsFromData(homeDoc);
    params.seed = seed;
    m.seed = seed;
    m.region = Gen::GenerateRegion(params);
    m.pins.clear();
    m.everySeed.clear();
    m.notice.clear();
    m.problems = 0;
    m.homeMap = params.homeMap;
    m.homeName = params.homeId;
    for (const WorldLoader::SystemInfo& info : universe_.systems)
        if (info.id == params.homeId)
            m.homeName = info.name;

    // The pins, as the server applies them; and which systems they touch, which is what the
    // map marks. A pin that cannot apply is counted and said in the log, as on opening.
    const json pins = ReadPins(dataDir_ + "pins.json");
    if (pins.is_null())
        TraceLog(LOG_WARNING, "Region map: %spins.json is not valid JSON -- shown unpinned",
                 dataDir_.c_str());
    else
    {
        std::vector<std::string> problems;
        Gen::ApplyPins(m.region, pins, seed, problems);
        for (const std::string& p : problems)
            TraceLog(LOG_WARNING, "Pins: %s -- not applied", p.c_str());
        m.problems = (int)problems.size();
        if (pins.contains("pins") && pins["pins"].is_array())
            for (const json& pin : pins["pins"])
            {
                if (!pin.is_object() || !pin.contains("system") || !pin["system"].is_string())
                    continue;
                const std::string id = pin["system"].get<std::string>();
                if (!pin.contains("seed"))
                    m.everySeed.insert(id);
                else if (pin["seed"].is_number_unsigned() && pin["seed"].get<uint64_t>() == seed)
                    m.pins[id]++;
            }
    }

    m.summaries.clear();
    float x1 = m.homeMap.x, y1 = m.homeMap.y, x2 = x1, y2 = y1;
    for (const json& s : m.region.systems)
    {
        const std::string id = s.value("id", std::string());
        const auto        doc = m.region.documents.find(id);
        m.summaries.push_back(doc != m.region.documents.end() ? Gen::Summarize(doc->second)
                                                              : Gen::SystemSummary());
        const Vector2 p = MapPos(s);
        x1 = fminf(x1, p.x);
        y1 = fminf(y1, p.y);
        x2 = fmaxf(x2, p.x);
        y2 = fmaxf(y2, p.y);
    }
    m.bounds = { x1, y1, fmaxf(1.0f, x2 - x1), fmaxf(1.0f, y2 - y1) };
    TraceLog(LOG_INFO, "Region map: seed %llu, %d systems, %d pinned, entry %s",
             (unsigned long long)seed, (int)m.region.systems.size(),
             (int)(m.pins.size() + m.everySeed.size()), m.region.entryId.c_str());
}

// ---- Layout ---------------------------------------------------------------------------

Rectangle Editor::RegionMapRect() const
{
    return { 16.0f, kTop, (float)screenWidth_ - kPanelW - 32.0f,
             (float)screenHeight_ - kTop - 12.0f };
}

Vector2 Editor::RegionToScreen(Vector2 p) const
{
    // Fitted to the view. A region grows away from home, so it is tall and narrow, and at
    // one scale its rings crowd into a column whose labels run into each other: each axis
    // gets its own scale, but neither more than twice the other, so the layout keeps its
    // shape -- rings stay rings, near stays near. A margin for the labels, which hang to
    // the right of a node.
    const Rectangle r = RegionMapRect();
    const Rectangle b = regionMap_.bounds;
    const float     left = 30.0f, right = 150.0f, top = 26.0f, bottom = 44.0f;
    const float     w = r.width - left - right, h = r.height - top - bottom;
    float           sx = w / b.width, sy = h / b.height;
    sx = fminf(sx, 2.0f * sy);
    sy = fminf(sy, 2.0f * sx);
    const float cx = r.x + left + w / 2.0f, cy = r.y + top + h / 2.0f;
    return { cx + (p.x - (b.x + b.width / 2.0f)) * sx, cy + (p.y - (b.y + b.height / 2.0f)) * sy };
}

int Editor::RegionHit(Vector2 p) const
{
    if (!CheckCollisionPointRec(p, RegionMapRect()))
        return -1;
    const float reach = kNode + 7.0f;
    int         best = -1;
    float       bestD = reach * reach;
    for (int i = 0; i < (int)regionMap_.region.systems.size(); i++)
    {
        const float d =
            Vector2DistanceSqr(p, RegionToScreen(MapPos(regionMap_.region.systems[(size_t)i])));
        if (d < bestD)
        {
            bestD = d;
            best = i;
        }
    }
    if (best < 0 && Vector2DistanceSqr(p, RegionToScreen(regionMap_.homeMap)) < reach * reach)
        return -2;
    return best;
}

// ---- Input ----------------------------------------------------------------------------

void Editor::HandleRegionInput()
{
    if (IsKeyPressed(KEY_ESCAPE))
    {
        ToggleRegionMap();
        return;
    }
    if (IsKeyPressed(KEY_PAGE_DOWN))
        OpenRegionMap(regionMap_.seed + 1);
    if (IsKeyPressed(KEY_PAGE_UP) && regionMap_.seed > 1)
        OpenRegionMap(regionMap_.seed - 1);

    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        return;
    const int hit = RegionHit(GetMousePosition());
    if (hit == -2)
    {
        regionMap_.notice = "the home system is a file of its own: Galaxy map, double-click";
        return;
    }
    if (hit < 0)
        return;
    // An unsaved edit is not thrown away by a click on a map: it takes a second hand.
    const bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    if (generated_.open && dirty_ && !shift)
    {
        regionMap_.notice = "unsaved edit to " + generated_.name +
                            ": Esc back and Ctrl+S, or Shift+click to discard it";
        return;
    }
    const std::string id = regionMap_.region.systems[(size_t)hit].value("id", std::string());
    OpenGenerated(regionMap_.seed, id);
}

// ---- Drawing --------------------------------------------------------------------------

void Editor::DrawRegionMap()
{
    const RegionMap& m = regionMap_;
    const Rectangle  r = RegionMapRect();
    DrawRectangleRec(r, Fade(Ui::PANEL_BG, 0.5f));
    DrawRectangleLinesEx(r, 1.0f, Ui::PANEL_BORDER);
    BeginScissorMode((int)r.x, (int)r.y, (int)r.width, (int)r.height);

    // Positions by id: the home system is not one of the region's, but its links are.
    std::map<std::string, Vector2> at;
    for (const json& s : m.region.systems)
        at[s.value("id", std::string())] = RegionToScreen(MapPos(s));
    std::string homeId;
    for (const json& l : m.region.links)
        if (l.is_array() && l.size() == 2 && l[0].is_string() && l[1].is_string())
            for (int k = 0; k < 2; k++)
                if (!at.count(l[k].get<std::string>()))
                    homeId = l[k].get<std::string>();
    const Vector2 home = RegionToScreen(m.homeMap);
    if (!homeId.empty())
        at[homeId] = home;

    // Links first, under the nodes: the wormhole loud, an ancient gate dashed.
    for (const json& l : m.region.links)
    {
        if (!l.is_array() || l.size() != 2 || !l[0].is_string() || !l[1].is_string())
            continue;
        const std::string a = l[0].get<std::string>(), b = l[1].get<std::string>();
        if (!at.count(a) || !at.count(b))
            continue;
        const Vector2 pa = at[a], pb = at[b];
        if (a == homeId || b == homeId)
        {
            DashedLine(pa, pb, 2.5f, kWormhole);
            const Vector2 mid = Vector2Scale(Vector2Add(pa, pb), 0.5f);
            Ui::Text("wormhole", (int)mid.x + 6, (int)mid.y - 14, 11, kWormhole);
            continue;
        }
        const auto da = m.region.documents.find(a);
        if (da != m.region.documents.end() && AncientGateTo(da->second, b))
        {
            DashedLine(pa, pb, 1.5f, Fade(kRare, 0.7f));
            continue;
        }
        DrawLineEx(pa, pb, 1.5f, Fade(Ui::ACCENT, 0.45f));
    }

    // Home: a hollow ring, because it is not part of the region and not edited here.
    DrawCircleLinesV(home, kNode + 2.0f, Ui::TEXT);
    DrawCircleLinesV(home, kNode - 2.0f, Fade(Ui::TEXT, 0.6f));
    Ui::Text(m.homeName.c_str(), (int)(home.x + kNode + 6.0f), (int)(home.y - 7.0f), 13, Ui::TEXT);
    Ui::Text("home", (int)(home.x + kNode + 6.0f), (int)(home.y + 7.0f), 10, Ui::TEXT_DIM);

    const int hover = RegionHit(GetMousePosition());
    for (size_t i = 0; i < m.region.systems.size(); i++)
    {
        const json&                    s = m.region.systems[i];
        const std::string              id = s.value("id", std::string());
        const Gen::SystemSummary&      sum = m.summaries[i];
        const Vector2                  p = at[id];
        const auto                     doc = m.region.documents.find(id);
        const std::vector<std::string> stars =
            doc != m.region.documents.end() ? StarTypes(doc->second) : std::vector<std::string>{};

        // Pinned: an amber ring outside everything, the pin colour of the system view.
        const bool pinned = m.pins.count(id) || m.everySeed.count(id);
        if (pinned)
        {
            DrawRing(p, kNode + 5.0f, kNode + 8.0f, 0.0f, 360.0f, 32, PIN_CHANGED);
            if (m.everySeed.count(id) && !m.pins.count(id))
                DrawRing(p, kNode + 5.0f, kNode + 8.0f, 0.0f, 360.0f, 32, Fade(Ui::PANEL_BG, 0.5f));
        }
        if (id == m.focus)
            DrawCircleLinesV(p, kNode + 12.0f, Ui::ACCENT);
        if ((int)i == hover)
            DrawCircleLinesV(p, kNode + 3.0f, Ui::ACCENT);

        // The star, or two of them side by side.
        if (stars.size() >= 2)
        {
            DrawCircleV({ p.x - 3.5f, p.y }, kNode - 2.0f, StarColor(stars[0]));
            DrawCircleV({ p.x + 3.5f, p.y }, kNode - 2.0f, StarColor(stars[1]));
        }
        else if (stars.size() == 1)
            DrawCircleV(p, kNode, StarColor(stars[0]));
        else
            DrawCircleLinesV(p, kNode, Ui::TEXT_DIM);
        if (s.value("owner", std::string()) == "Pirates")
            DrawTriangle({ p.x, p.y - kNode - 9.0f }, { p.x - 4.0f, p.y - kNode - 3.0f },
                         { p.x + 4.0f, p.y - kNode - 3.0f }, kPirates);

        // The designation, the character unless ordinary, and what is there worth a stop.
        const float lx = p.x + kNode + 9.0f;
        std::string name = s.value("name", id);
        Ui::Text(name.c_str(), (int)lx, (int)(p.y - 13.0f), 13, Ui::TEXT);
        // Empty is the survey's verdict (#141), and as loud here: nothing worth a stop.
        std::string tag = sum.character == "ordinary" ? std::string() : sum.character;
        if (sum.Empty())
            tag += tag.empty() ? "EMPTY" : "  EMPTY";
        if (!tag.empty())
            Ui::Text(tag.c_str(), (int)lx + Ui::TextWidth(name.c_str(), 13) + 6, (int)(p.y - 11.0f),
                     10, sum.Empty() ? kPirates : Ui::TEXT_DIM);
        float       ix = lx;
        const float iy = p.y + 7.0f;
        ix = Icon(ix, iy, sum.planets, Ui::TEXT_DIM, Mark::Planet);
        if (StationCount(sum) > 0)
            ix = Icon(ix, iy, StationCount(sum), Ui::ACCENT, Mark::Station);
        if (sum.Count("asteroidFields") > 0)
            ix = Icon(ix, iy, sum.Count("asteroidFields"), kBelt, Mark::Belt);
        if (sum.Count("nebulae") > 0)
            ix = Icon(ix, iy, sum.Count("nebulae"), kCloud, Mark::Cloud);
        if (sum.Count("derelicts") > 0)
            ix = Icon(ix, iy, sum.Count("derelicts"), kWreck, Mark::Wreck);
        if (RareCount(sum) > 0)
            Icon(ix, iy, RareCount(sum), kRare, Mark::Rare);
    }
    EndScissorMode();

    // Hover: the system in words, beside the cursor.
    if (hover >= 0)
    {
        const json&                                s = m.region.systems[(size_t)hover];
        const std::string                          id = s.value("id", std::string());
        const Gen::SystemSummary&                  sum = m.summaries[(size_t)hover];
        std::vector<std::pair<std::string, Color>> lines;
        lines.push_back({ TextFormat("%s   %s   depth %d", s.value("name", id).c_str(), id.c_str(),
                                     Gen::DepthOf(id)),
                          Ui::ACCENT });
        const auto  doc = m.region.documents.find(id);
        std::string stars;
        for (const std::string& t :
             doc != m.region.documents.end() ? StarTypes(doc->second) : std::vector<std::string>{})
            stars += (stars.empty() ? "" : " + ") + t;
        std::string line = (stars.empty() ? std::string("no star") : stars + " star") +
                           "   character: " + (sum.character.empty() ? "-" : sum.character);
        lines.push_back({ line, Ui::TEXT });
        line = TextFormat("security %.2f", s.value("security", 0.0));
        if (!s.value("owner", std::string()).empty())
            line += "   held by " + s.value("owner", std::string());
        lines.push_back({ line, Ui::TEXT });
        line = std::to_string(sum.planets) + " planets   " + std::to_string(sum.gates) + " gates";
        for (const auto& kv : sum.things)
        {
            std::string kind = kv.first;
            if (kind == "asteroidFields")
                kind = "belts";
            else if (kind == "derelicts")
                kind = "wrecks";
            else if (kind == "nebulae")
                kind = "clouds";
            line += "   " + std::to_string(kv.second) + " " + kind;
        }
        lines.push_back({ line, Ui::TEXT });
        if (m.pins.count(id))
            lines.push_back({ TextFormat("pinned: %d pin%s for seed %llu", m.pins.at(id),
                                         m.pins.at(id) == 1 ? "" : "s", (unsigned long long)m.seed),
                              PIN_CHANGED });
        if (m.everySeed.count(id))
            lines.push_back({ "pinned for every seed", PIN_CHANGED });
        if (sum.Empty())
            lines.push_back({ "empty: nothing but a star, planets and gates", kPirates });
        lines.push_back({ "click: open for editing", Ui::TEXT_DIM });

        int w = 0;
        for (const auto& l : lines)
            w = std::max(w, Ui::TextWidth(l.first.c_str(), 13));
        const Vector2 mp = GetMousePosition();
        Rectangle     box{ mp.x + 16.0f, mp.y + 12.0f, (float)w + 20.0f,
                           (float)lines.size() * 17.0f + 12.0f };
        if (box.x + box.width > r.x + r.width)
            box.x = mp.x - 16.0f - box.width;
        if (box.y + box.height > r.y + r.height)
            box.y = mp.y - 12.0f - box.height;
        DrawRectangleRec(box, Ui::PANEL_BG);
        DrawRectangleLinesEx(box, 1.0f, Ui::PANEL_BORDER);
        for (size_t i = 0; i < lines.size(); i++)
            Ui::Text(lines[i].first.c_str(), (int)box.x + 10, (int)(box.y + 7.0f + 17.0f * i), 13,
                     lines[i].second);
    }
    else if (hover == -2)
    {
        const Vector2 mp = GetMousePosition();
        Ui::Text(
            TextFormat("%s: the home system, where the wormhole opens from", m.homeName.c_str()),
            (int)mp.x + 16, (int)mp.y + 12, 13, Ui::TEXT);
    }

    if (!m.notice.empty())
        Ui::Text(m.notice.c_str(), (int)r.x + 10, (int)(r.y + r.height - 22.0f), 13, PIN_CHANGED);
}

void Editor::DrawRegionPanel()
{
    const RegionMap& m = regionMap_;
    const Rectangle  panel{ (float)screenWidth_ - kPanelW, 0.0f, kPanelW, (float)screenHeight_ };
    DrawRectangleRec(panel, Ui::PANEL_BG);
    DrawRectangleLinesEx(panel, 1.0f, Ui::PANEL_BORDER);
    const int x = (int)panel.x + 14;
    int       y = 14;

    Ui::Text("REGION", x, y, 14, Ui::ACCENT);
    Ui::Text(
        TextFormat("seed %llu   generator v%d", (unsigned long long)m.seed, Gen::GENERATOR_VERSION),
        x + 70, y + 2, 11, Ui::TEXT_DIM);
    y += 24;
    int pinned = 0, empty = 0;
    for (size_t i = 0; i < m.region.systems.size(); i++)
    {
        const std::string id = m.region.systems[i].value("id", std::string());
        pinned += (m.pins.count(id) || m.everySeed.count(id)) ? 1 : 0;
        empty += m.summaries[i].Empty() ? 1 : 0;
    }
    Ui::Text(TextFormat("%d systems   %d pinned   %d empty", (int)m.region.systems.size(), pinned,
                        empty),
             x, y, 13, Ui::TEXT);
    y += 18;
    std::string entry = m.region.entryId;
    for (const json& s : m.region.systems)
        if (s.value("id", std::string()) == m.region.entryId)
            entry = s.value("name", entry);
    Ui::Text(TextFormat("wormhole: %s -> %s", m.homeName.c_str(), entry.c_str()), x, y, 13,
             kWormhole);
    y += 18;
    if (m.problems > 0)
    {
        Ui::Text(TextFormat("%d pin%s did not apply: see the log", m.problems,
                            m.problems == 1 ? "" : "s"),
                 x, y, 13, kPirates);
        y += 18;
    }
    y += 8;
    DrawLine(x, y, x + (int)kPanelW - 28, y, Ui::PANEL_BORDER);
    y += 12;

    // The legend: every mark on the map, said once.
    Ui::Text("STARS", x, y, 11, Ui::TEXT_DIM);
    y += 18;
    int sx = x;
    for (const char* t : { "Yellow", "Red", "Blue" })
    {
        DrawCircleV({ (float)sx + 6.0f, (float)y + 6.0f }, 6.0f, StarColor(t));
        Ui::Text(t, sx + 16, y, 12, Ui::TEXT);
        sx += 22 + Ui::TextWidth(t, 12) + 12;
    }
    DrawCircleV({ (float)sx + 3.0f, (float)y + 6.0f }, 4.5f, StarColor("Yellow"));
    DrawCircleV({ (float)sx + 10.0f, (float)y + 6.0f }, 4.5f, StarColor("Red"));
    Ui::Text("binary", sx + 19, y, 12, Ui::TEXT);
    y += 24;

    Ui::Text("UNDER A NAME", x, y, 11, Ui::TEXT_DIM);
    y += 18;
    struct Row
    {
        Mark        shape;
        Color       c;
        const char* what;
    };
    const Row rows[] = {
        { Mark::Planet, Ui::TEXT_DIM, "planets" }, { Mark::Station, Ui::ACCENT, "stations" },
        { Mark::Belt, kBelt, "asteroid belts" },   { Mark::Cloud, kCloud, "clouds" },
        { Mark::Wreck, kWreck, "wrecks" },         { Mark::Rare, kRare, "rare finds" }
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++)
    {
        const int   col = (int)i % 2, row = (int)i / 2;
        const float ix = (float)(x + col * 136), iy = (float)(y + row * 18 + 6);
        DrawMark(ix, iy, rows[i].c, rows[i].shape);
        Ui::Text(rows[i].what, (int)ix + 12, (int)iy - 6, 12, Ui::TEXT);
    }
    y += 3 * 18 + 10;

    Ui::Text("MARKS", x, y, 11, Ui::TEXT_DIM);
    y += 18;
    DrawRing({ (float)x + 7.0f, (float)y + 6.0f }, 5.0f, 7.5f, 0.0f, 360.0f, 24, PIN_CHANGED);
    Ui::Text("pinned (faint: a pin for every seed)", x + 20, y, 12, Ui::TEXT);
    y += 20;
    DrawTriangle({ (float)x + 7.0f, (float)y }, { (float)x + 3.0f, (float)y + 8.0f },
                 { (float)x + 11.0f, (float)y + 8.0f }, kPirates);
    Ui::Text("held by pirates", x + 20, y, 12, Ui::TEXT);
    y += 20;
    DrawCircleLinesV({ (float)x + 7.0f, (float)y + 6.0f }, 7.0f, Ui::ACCENT);
    Ui::Text("where the map was opened from", x + 20, y, 12, Ui::TEXT);
    y += 20;
    DashedLine({ (float)x, (float)y + 6.0f }, { (float)x + 14.0f, (float)y + 6.0f }, 2.5f,
               kWormhole);
    Ui::Text("the wormhole from home", x + 20, y, 12, Ui::TEXT);
    y += 20;
    DashedLine({ (float)x, (float)y + 6.0f }, { (float)x + 14.0f, (float)y + 6.0f }, 1.5f,
               Fade(kRare, 0.7f));
    Ui::Text("an ancient gate, deep to ring 1", x + 20, y, 12, Ui::TEXT);
    y += 20;
    DrawLineEx({ (float)x, (float)y + 6.0f }, { (float)x + 14.0f, (float)y + 6.0f }, 1.5f,
               Fade(Ui::ACCENT, 0.45f));
    Ui::Text("a gate link", x + 20, y, 12, Ui::TEXT);

    const int ky = screenHeight_ - 74;
    DrawLine(x, ky - 6, x + (int)kPanelW - 28, ky - 6, Ui::PANEL_BORDER);
    Ui::Text("click  open the system for editing", x, ky, 11, Ui::TEXT_DIM);
    Ui::Text("hover  what is there", x, ky + 15, 11, Ui::TEXT_DIM);
    Ui::Text("PgDn / PgUp  next / previous seed", x, ky + 30, 11, Ui::TEXT_DIM);
    Ui::Text("Esc / F5  back", x, ky + 45, 11, Ui::TEXT_DIM);
}
