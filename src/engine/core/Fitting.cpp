#include "core/Fitting.h"

#include <algorithm>
#include <map>
#include <tuple>

namespace Ships
{
namespace
{
const ModulePart* Module(const Catalogue& c, const std::string& id)
{
    return c.FindModule(id);
}

bool LooksOn(const ModulePart& m, const std::string& on)
{
    for (const auto& [kind, look] : m.look)
        if (kind == on)
            return true;
    return false;
}

// How many of each module a fit carries.
std::map<std::string, int> Counts(const std::vector<FitLine>& fit)
{
    std::map<std::string, int> n;
    for (const FitLine& l : fit)
        n[l.module] += l.count;
    return n;
}

double OreValue(const Cost& cost, const std::vector<float>& orePrices)
{
    double v = 0.0;
    for (const auto& [r, n] : cost)
        if ((size_t)r.Index() < orePrices.size())
            v += (double)n * (double)orePrices[(size_t)r.Index()];
    return v;
}
}  // namespace

std::vector<SocketGroup> Sockets(const Catalogue& c, const Design& hull,
                                 const std::vector<FitLine>& fit)
{
    std::vector<SocketGroup> out;
    auto                     add = [&](const std::string& position, const std::string& sectionId)
    {
        const Section* s = c.FindSection(sectionId);
        if (s == nullptr)
            return;
        for (const auto& [kind, n] : s->sockets)
        {
            SocketGroup* g = nullptr;
            for (SocketGroup& x : out)
                if (x.in == position && x.on == kind)
                    g = &x;
            if (g == nullptr)
            {
                out.push_back(SocketGroup{ position, kind, 0, 0, {} });
                g = &out.back();
                for (const ModulePart& m : c.modules)
                    // A drive goes astern (Validate), so it is not offered anywhere else.
                    if (LooksOn(m, kind) && (m.provides.thrust <= 0.0f || position == "stern"))
                        g->modules.push_back(m.id);
            }
            g->capacity += n;
        }
    };
    add("bow", hull.bow);
    for (const std::string& m : hull.mids)
        add("mid", m);
    add("stern", hull.stern);
    for (const FitLine& l : fit)
        for (SocketGroup& g : out)
            if (g.in == l.in && g.on == l.on)
                g.used += l.count;
    return out;
}

std::vector<FitLine> Normalized(std::vector<FitLine> fit)
{
    static const char* const ORDER[] = { "bow", "mid", "stern" };
    auto                     rank = [](const std::string& in)
    {
        for (int i = 0; i < 3; i++)
            if (in == ORDER[i])
                return i;
        return 3;
    };
    std::sort(fit.begin(), fit.end(),
              [&](const FitLine& a, const FitLine& b)
              {
                  return std::make_tuple(rank(a.in), a.in, a.on, a.module) <
                         std::make_tuple(rank(b.in), b.in, b.on, b.module);
              });
    std::vector<FitLine> out;
    for (const FitLine& l : fit)
    {
        if (l.count <= 0)
            continue;
        if (!out.empty() && out.back().in == l.in && out.back().on == l.on &&
            out.back().module == l.module)
            out.back().count += l.count;
        else
            out.push_back(l);
    }
    return out;
}

bool PlayerFit(const Catalogue& c, const Design& hull, const std::vector<FitLine>& fit, Design& out,
               std::string& error)
{
    for (const FitLine& l : fit)
    {
        if (l.scale != 0.0f)
        {
            error = "'" + l.module + "' is the size its look says; a fit sets no scale";
            return false;
        }
        if (l.count < 0)
        {
            error = "'" + l.module + "': a count is not negative";
            return false;
        }
        const ModulePart* m = Module(c, l.module);
        if (m == nullptr)
        {
            error = "there is no module '" + l.module + "'";
            return false;
        }
        if (!LooksOn(*m, l.on))
        {
            error = "'" + l.module + "' does not go on a '" + l.on + "' socket";
            return false;
        }
    }

    const std::vector<FitLine> mine = Normalized(fit);
    std::vector<FitLine>       stock = Normalized(hull.fit);
    for (FitLine& l : stock)
        l.scale = 0.0f;
    bool same = mine.size() == stock.size();
    for (size_t i = 0; same && i < mine.size(); i++)
        same = mine[i].module == stock[i].module && mine[i].in == stock[i].in &&
               mine[i].on == stock[i].on && mine[i].count == stock[i].count;
    if (same)
    {
        out = hull;
        return true;
    }

    Design d = hull;
    d.fit = mine;
    // The hull's own lines keep how the hull draws them: a refit that leaves the drive alone
    // leaves it looking as it did.
    for (FitLine& l : d.fit)
        for (const FitLine& h : hull.fit)
            if (h.module == l.module && h.in == l.in && h.on == l.on)
                l.scale = h.scale;
    d.id = ContentId(d);
    d.name = hull.name;

    Stats s;
    if (!Derive(c, d, s, error))
    {
        // Validate's words name the design by its hash, which means nothing to a pilot.
        const std::string prefix = "design '" + d.id + "': ";
        if (error.compare(0, prefix.size(), prefix) == 0)
            error = error.substr(prefix.size());
        return false;
    }
    if (s.acceleration <= 0.0f)
    {
        error = "with no drive the ship could not leave the station";
        return false;
    }
    out = std::move(d);
    return true;
}

double RefitPrice(const Catalogue& c, const Design& from, const Design& to,
                  const std::vector<float>& orePrices, float markup)
{
    const std::map<std::string, int> was = Counts(from.fit), now = Counts(to.fit);
    double                           price = 0.0;
    std::map<std::string, int>       delta = now;
    for (const auto& [id, n] : was)
        delta[id] -= n;
    for (const auto& [id, n] : delta)
    {
        const ModulePart* m = Module(c, id);
        if (m == nullptr || n == 0)
            continue;
        const double each = OreValue(m->cost, orePrices);
        price += n > 0 ? n * each * markup : n * each * REFIT_REFUND;
    }
    return price;
}

std::vector<FitLine> PlayerLines(const Design& d)
{
    std::vector<FitLine> fit = Normalized(d.fit);
    for (FitLine& l : fit)
        l.scale = 0.0f;  // PlayerFit gives the hull's own lines back the hull's drawing
    return fit;
}

float WarshipDps(const Catalogue& c, const std::string& faction)
{
    const std::vector<std::string>* list = c.DesignsFor(faction, "warship");
    if (list == nullptr || list->empty())
        return 0.0f;
    float sum = 0.0f;
    for (const std::string& id : *list)
    {
        const Design* d = c.FindDesign(id);
        Stats         st;
        std::string   why;
        if (d != nullptr && Derive(c, *d, st, why))
            sum += Dps(st);
    }
    return sum / (float)list->size();
}

bool ValidDesignName(const std::string& name, std::string& error)
{
    if (name.empty())
    {
        error = "a design needs a name";
        return false;
    }
    if (name.size() > DESIGN_NAME_MAX)
    {
        error = "a design's name is at most " + std::to_string(DESIGN_NAME_MAX) + " characters";
        return false;
    }
    for (char ch : name)
        if ((unsigned char)ch < 32 || (unsigned char)ch > 126)
        {
            error = "a design's name is letters, digits, spaces and punctuation";
            return false;
        }
    if (name.front() == ' ' || name.back() == ' ')
    {
        error = "a design's name does not start or end with a space";
        return false;
    }
    return true;
}
}  // namespace Ships
