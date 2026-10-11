#include "sim/Sensor.h"

#include "render/TextBackend.h"
#include "ui/Theme.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace Sensor
{

Allegiance Classify(const Proto::EntitySnapshot& e, const Standing& viewer)
{
    switch (e.kind)
    {
        case Proto::EntityKind::Station:
        case Proto::EntityKind::Npc:
        {
            if (viewer.hostile && viewer.hostile(e.faction))
                return Allegiance::Hostile;
            const RepTier t = viewer.tier ? viewer.tier(e.faction) : RepTier::Neutral;
            return (t == RepTier::Liked || t == RepTier::Allied) ? Allegiance::Friendly
                                                                 : Allegiance::Neutral;
        }
        // Another pilot has a side, but there are no standings between players yet, so
        // the honest reading is that nobody has said.
        case Proto::EntityKind::PlayerShip: return Allegiance::Neutral;
        default: return Allegiance::Unowned;
    }
}

Color ColorOf(Allegiance a)
{
    // The theme's standing colours (#297): one place for them, the theme in use.
    const Ui::Theme::Standing& s = Ui::CurrentTheme().standing;
    switch (a)
    {
        case Allegiance::Own: return s.own;
        case Allegiance::Friendly: return s.friendly;
        case Allegiance::Neutral: return s.neutral;
        case Allegiance::Hostile: return s.hostile;
        case Allegiance::Unowned: break;
    }
    return s.unowned;
}

const char* Word(Allegiance a)
{
    switch (a)
    {
        case Allegiance::Own: return "you";
        case Allegiance::Friendly: return "friendly";
        case Allegiance::Neutral: return "neutral";
        case Allegiance::Hostile: return "hostile";
        case Allegiance::Unowned: break;
    }
    return "unowned";
}

const char* KindWord(EntityKind k)
{
    switch (k)
    {
        case EntityKind::Star: return "star";
        case EntityKind::Planet: return "planet";
        case EntityKind::Station: return "station";
        case EntityKind::Field: return "belt";
        case EntityKind::Gate: return "gate";
        case EntityKind::Nebula: return "nebula";
        case EntityKind::Derelict: return "wreck";
        case EntityKind::Npc: return "ship";
        case EntityKind::PlayerShip: return "pilot";
        case EntityKind::Structure: return "structure";
        case EntityKind::Unknown: break;
    }
    return "object";
}

const std::vector<float>& Ranges()
{
    static const std::vector<float> ranges = { 2000.0f,    5000.0f,   10000.0f,  25000.0f,
                                               50000.0f,   100000.0f, 250000.0f, 500000.0f,
                                               1000000.0f, 2000000.0f };
    return ranges;
}

float DefaultRange()
{
    // The neighbourhood rather than the system: what is near enough to matter. The whole
    // system is a few steps of the wheel away, and the radar already shows it.
    return 250000.0f;
}

float StepRange(float current, int steps)
{
    const std::vector<float>& r = Ranges();
    // The nearest step to `current`, so a range that is not one of them still moves sensibly.
    int at = 0;
    for (int i = 1; i < (int)r.size(); i++)
        if (std::fabs(r[(size_t)i] - current) < std::fabs(r[(size_t)at] - current))
            at = i;
    at = std::clamp(at + steps, 0, (int)r.size() - 1);
    return r[(size_t)at];
}

const Cell& Picture::At(int x, int y) const
{
    static const Cell empty;
    if (x < 0 || x >= width || y < 0 || y >= height)
        return empty;
    return cells[(size_t)y * (size_t)width + (size_t)x];
}

namespace
{
// The eight cells around one, sides before corners: a glyph moved off its cell stays in
// line with the one it gave way to wherever it can.
constexpr int NEIGHBOURS[8][2] = { { 1, 0 },  { -1, 0 },  { 0, -1 }, { 0, 1 },
                                   { 1, -1 }, { -1, -1 }, { 1, 1 },  { -1, 1 } };

std::string Lower(std::string s)
{
    for (char& c : s)
        c = (char)std::tolower((unsigned char)c);
    return s;
}

// What the legend gathers about one glyph on the screen.
struct Key
{
    char                     glyph = ' ';
    std::vector<std::string> kinds;  // the class words, first seen first
    std::vector<std::string> types;  // the type names, where known
    bool                     untyped = false;
};

void AddOnce(std::vector<std::string>& v, const std::string& s)
{
    if (std::find(v.begin(), v.end(), s) == v.end())
        v.push_back(s);
}
}  // namespace

Picture Scan(std::vector<Render::Item> items, const Render::Item& own, int width, int height,
             float span, const AllegianceOf& allegianceOf, const TypeNameOf& typeNameOf)
{
    // The grid is the projection -- which cell a world point falls in -- and nothing is
    // drawn into it: who keeps a cell, and where the other one goes, is decided here.
    const Render::GridBackend grid(width, height, span, own.pos);

    Picture p;
    p.width = grid.Width();
    p.height = grid.Height();
    p.span = grid.Span();
    const size_t n = (size_t)p.width * (size_t)p.height;
    p.cells.resize(n);
    // Per cell: where its occupant really is, what kind of thing it is and its type name.
    std::vector<Vector2>     at(n, own.pos);
    std::vector<EntityKind>  kinds(n, EntityKind::Unknown);
    std::vector<std::string> types(n);
    auto index = [&](int x, int y) { return (size_t)y * (size_t)p.width + (size_t)x; };

    // The viewer is the centre of the screen by definition, and holds it against anything
    // that shares the cell: losing track of yourself on your own instrument is the one thing
    // it must not do. Whatever is parked on top of the ship moves aside instead.
    const int cx = p.width / 2, cy = p.height / 2;
    Cell&     centre = p.cells[index(cx, cy)];
    centre.glyph = own.glyph.empty() ? '@' : own.glyph[0];
    centre.id = 0;
    centre.allegiance = Allegiance::Own;

    // Whatever Present would draw on top keeps its cell: the higher layer first, and of two
    // on one layer the later one.
    std::stable_sort(items.begin(), items.end(), [](const Render::Item& a, const Render::Item& b)
                     { return a.layer < b.layer; });
    for (auto it = items.rbegin(); it != items.rend(); ++it)
    {
        const Render::Item& item = *it;
        int                 x = 0, y = 0;
        if (!grid.CellOf(item.pos, x, y))
            continue;  // off the screen, rather than piled on its edge
        bool moved = false;
        if (p.cells[index(x, y)].glyph != ' ')
        {
            // Taken: the free neighbour on the side this thing actually lies from the one
            // holding the cell, so a moon east of its planet is drawn east of it.
            const Vector2 d = { item.pos.x - at[index(x, y)].x, item.pos.y - at[index(x, y)].y };
            int           best = -1;
            float         bestScore = 0.0f;
            for (int k = 0; k < 8; k++)
            {
                const int nx = x + NEIGHBOURS[k][0], ny = y + NEIGHBOURS[k][1];
                if (nx < 0 || nx >= p.width || ny < 0 || ny >= p.height ||
                    p.cells[index(nx, ny)].glyph != ' ')
                    continue;
                const float score =
                    (d.x * (float)NEIGHBOURS[k][0] + d.y * (float)NEIGHBOURS[k][1]) /
                    std::sqrt((float)(NEIGHBOURS[k][0] * NEIGHBOURS[k][0] +
                                      NEIGHBOURS[k][1] * NEIGHBOURS[k][1]));
                if (best < 0 || score > bestScore)
                {
                    best = k;
                    bestScore = score;
                }
            }
            if (best < 0)
                continue;  // hemmed in on every side: the one on top speaks for the crowd
            x += NEIGHBOURS[best][0];
            y += NEIGHBOURS[best][1];
            moved = true;
        }
        const size_t i = index(x, y);
        Cell&        c = p.cells[i];
        c.glyph = item.glyph.empty() ? '?' : item.glyph[0];
        c.id = item.id;
        c.allegiance = allegianceOf ? allegianceOf(item.id) : Allegiance::Unowned;
        c.offset = moved;
        at[i] = item.pos;
        kinds[i] = item.kind;
        types[i] = typeNameOf ? typeNameOf(item.id) : std::string();
    }

    // The legend: one line per glyph, read off the screen in reading order. A glyph that
    // stands for several classes of thing names them all; two glyphs that would come out
    // as the same word -- a gas giant and a rocky planet are both "planet" -- are told
    // apart by their type, where each stands for one.
    std::vector<Key> keys;
    for (int y = 0; y < p.height; y++)
        for (int x = 0; x < p.width; x++)
        {
            const size_t i = index(x, y);
            const Cell&  c = p.cells[i];
            if (c.glyph == ' ' || (x == cx && y == cy))
                continue;
            auto k = std::find_if(keys.begin(), keys.end(),
                                  [&](const Key& key) { return key.glyph == c.glyph; });
            if (k == keys.end())
            {
                keys.push_back(Key{});
                k = keys.end() - 1;
                k->glyph = c.glyph;
            }
            AddOnce(k->kinds, KindWord(kinds[i]));
            if (types[i].empty())
                k->untyped = true;
            else
                AddOnce(k->types, Lower(types[i]));
        }
    std::vector<std::string> words;
    for (const Key& k : keys)
    {
        std::string w;
        for (const std::string& kind : k.kinds)
            w += (w.empty() ? "" : ", ") + kind;
        words.push_back(w);
    }
    p.legend.push_back({ centre.glyph, "you" });
    for (size_t i = 0; i < keys.size(); i++)
    {
        // Shared: another glyph on the screen also stands for this class of thing.
        std::string w = words[i];
        bool        shared = false;
        for (size_t j = 0; j < keys.size(); j++)
            for (const std::string& kind : keys[i].kinds)
                shared |= j != i && std::find(keys[j].kinds.begin(), keys[j].kinds.end(), kind) !=
                                        keys[j].kinds.end();
        if (shared && keys[i].types.size() == 1 && !keys[i].untyped)
            w = keys[i].types[0];
        p.legend.push_back({ keys[i].glyph, w });
    }
    // Still the same word for two glyphs -- types nobody could tell apart either: the glyph
    // itself says which line is which.
    for (size_t i = 1; i < p.legend.size(); i++)
        for (size_t j = 1; j < p.legend.size(); j++)
            if (i != j && p.legend[i].kind == p.legend[j].kind)
            {
                p.legend[i].kind += std::string(" (") + p.legend[i].glyph + ")";
                break;
            }
    return p;
}

}  // namespace Sensor
