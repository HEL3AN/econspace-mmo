#include "sim/Sensor.h"

#include "render/TextBackend.h"
#include "ui/Theme.h"

#include <algorithm>
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
    // The theme's standing colours (#297): one place for them, data/ui_theme.json.
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

Picture Scan(std::vector<Render::Item> items, const Render::Item& own, int width, int height,
             float span, const AllegianceOf& allegianceOf)
{
    // The kinds behind the glyphs, kept before the items are handed over: the grid keeps a
    // character and an id per cell, and the legend wants the class of thing.
    std::vector<std::pair<int, EntityKind>> kinds;
    kinds.reserve(items.size());
    for (const Render::Item& it : items)
        kinds.push_back({ it.id, it.kind });

    Render::GridBackend grid(width, height, span, own.pos);
    Render::Present(std::move(items), grid);

    Picture p;
    p.width = grid.Width();
    p.height = grid.Height();
    p.span = grid.Span();
    p.cells.resize((size_t)p.width * (size_t)p.height);

    for (int y = 0; y < p.height; y++)
        for (int x = 0; x < p.width; x++)
        {
            Cell& c = p.cells[(size_t)y * (size_t)p.width + (size_t)x];
            c.glyph = grid.At(x, y);
            c.id = grid.IdAt(x, y);
            if (c.glyph == ' ')
                continue;
            c.allegiance = allegianceOf ? allegianceOf(c.id) : Allegiance::Unowned;

            EntityKind kind = EntityKind::Unknown;
            for (const auto& k : kinds)
                if (k.first == c.id)
                {
                    kind = k.second;
                    break;
                }
            const char* word = KindWord(kind);
            const bool  known =
                std::any_of(p.legend.begin(), p.legend.end(), [&](const LegendEntry& l)
                            { return l.glyph == c.glyph && std::strcmp(l.kind, word) == 0; });
            if (!known)
                p.legend.push_back({ c.glyph, word });
        }

    // The viewer is the centre of the screen by definition, and drawn over anything that
    // shares the cell: losing track of yourself on your own instrument is the one thing it
    // must not do.
    Cell& centre = p.cells[(size_t)(p.height / 2) * (size_t)p.width + (size_t)(p.width / 2)];
    centre.glyph = own.glyph.empty() ? '@' : own.glyph[0];
    centre.id = 0;
    centre.allegiance = Allegiance::Own;
    p.legend.insert(p.legend.begin(), { centre.glyph, "you" });
    return p;
}

}  // namespace Sensor
