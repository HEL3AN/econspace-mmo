#include "sim/Observation.h"

#include "core/Faction.h"
#include "economy/Resource.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <vector>

namespace
{

// How many surrounding objects to list. Brief is a turn's worth of awareness; Full is for
// deciding. Navigation anchors (stations and gates) are listed regardless of the cap,
// because "there is a station somewhere behind you" is exactly what an agent needs and
// exactly what a nearest-first cut would drop in a busy system.
constexpr size_t BRIEF_NEARBY = 10;
constexpr size_t FULL_NEARBY = 30;

std::string Fmt(const char* fmt, ...)
{
    char    buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    return std::string(buf);
}

const char* KindName(Proto::EntityKind k)
{
    switch (k)
    {
        case Proto::EntityKind::Star: return "star";
        case Proto::EntityKind::Planet: return "planet";
        case Proto::EntityKind::Station: return "station";
        case Proto::EntityKind::Field: return "field";
        case Proto::EntityKind::Gate: return "gate";
        case Proto::EntityKind::Nebula: return "nebula";
        case Proto::EntityKind::Derelict: return "derelict";
        case Proto::EntityKind::Npc: return "ship";
        case Proto::EntityKind::PlayerShip: return "pilot";
        case Proto::EntityKind::Unknown: break;
    }
    return "object";
}

// NpcRole as it travels in the snapshot (an int, to keep the protocol independent of the
// game's enums). Kept in the same order as NpcRole.
const char* RoleName(int role)
{
    switch (role)
    {
        case 0: return "trader";
        case 1: return "miner";
        case 2: return "police";
        case 3: return "pirate";
        case 4: return "warship";
        default: return "ship";
    }
}

// Whether an NPC would shoot at us, from what the client can see: pirates always, plus any
// faction we are wanted by or standing badly with. This mirrors the server's own predicate
// rather than inventing a second one -- an agent that thinks a patrol is friendly while the
// server has it hunting is worse off than one told nothing.
bool HostileToPlayer(const Proto::EntitySnapshot& e, const Proto::PlayerView& p)
{
    if (e.kind != Proto::EntityKind::Npc)
        return false;
    if (RoleName(e.role) == std::string("pirate"))
        return true;

    size_t idx = (size_t)e.faction;
    if (idx < p.bounty.size() && p.bounty[idx] > 0.0)
        return true;
    if (idx < p.reputation.size())
    {
        RepTier tier = Factions::TierOf(p.reputation[idx]);
        return tier == RepTier::Hostile || tier == RepTier::Hated;
    }
    return false;
}

struct Seen
{
    const Proto::EntitySnapshot* e = nullptr;
    float                        dist = 0.0f;
    bool                         hostile = false;
};

// One line per object: id, what it is, its name, how far, which way, and whatever detail
// that kind carries (ore for a field, destination for a gate, hull for a ship).
std::string Line(const Seen& s, const Proto::PlayerView& p,
                 const std::map<int, Proto::EntityLayout>* layout)
{
    const Proto::EntitySnapshot& e = *s.e;
    const char* kind = e.kind == Proto::EntityKind::Npc ? RoleName(e.role) : KindName(e.kind);

    std::string name = e.name;
    std::string extra;
    if (layout != nullptr)
    {
        auto it = layout->find(e.id);
        if (it != layout->end())
        {
            if (name.empty())
                name = it->second.name;
            if (e.kind == Proto::EntityKind::Gate && !it->second.dest.empty())
                extra = "  to " + it->second.dest;
            else if (e.kind == Proto::EntityKind::Derelict && it->second.reward > 0.0)
                extra = "  lootable";
        }
    }
    if (e.kind == Proto::EntityKind::Field && e.ore >= 0)
        extra = "  " + ResourceName((ResourceType)e.ore);
    if (e.kind == Proto::EntityKind::Station)
        extra = e.id == p.dockedStationId ? "  DOCKED HERE" : "  dockable";
    if (e.kind == Proto::EntityKind::Npc)
    {
        // No "HOSTILE" suffix: the section heading above already says it, and every
        // repeated word is a token an agent pays for.
        extra = Fmt("  %s  hull %d%%", FactionName(e.faction).c_str(), (int)(e.hullFrac * 100.0f));
    }
    if (e.kind == Proto::EntityKind::PlayerShip)
    {
        // Another player (#4). Worth saying plainly: an agent that reads "pilot" as
        // scenery is one that does not know it is sharing the system with someone who
        // decides things for themselves.
        if (name.empty())
            name = "another pilot";
        extra = Fmt("  player  hull %d%%", (int)(e.hullFrac * 100.0f));
    }

    std::string line =
        Fmt("  #%-4d %-8s %-22s %6.0fu %-2s%s", e.id, kind, name.c_str(), s.dist,
            Obs::Compass(e.pos.x - p.pos.x, e.pos.y - p.pos.y).c_str(), extra.c_str());
    while (!line.empty() && line.back() == ' ')  // column padding, not content
        line.pop_back();
    return line;
}

}  // namespace

namespace Obs
{

std::string Compass(float dx, float dy)
{
    if (std::fabs(dx) < 0.001f && std::fabs(dy) < 0.001f)
        return "--";
    // World y grows downward, so north (up on screen) is -y. Doing this here once keeps
    // every caller from having to remember the flip.
    static const char* points[] = { "E", "SE", "S", "SW", "W", "NW", "N", "NE" };
    float              deg = std::atan2(dy, dx) * 180.0f / 3.14159265f;
    if (deg < 0.0f)
        deg += 360.0f;
    int idx = (int)((deg + 22.5f) / 45.0f) % 8;
    return points[idx];
}

std::string Describe(const View& view, Detail detail)
{
    if (view.snapshot == nullptr)
        return "No world state yet.\n";

    const Proto::Snapshot&   snap = *view.snapshot;
    const Proto::PlayerView& p = snap.player;
    std::string              out;

    // --- Where we are -------------------------------------------------------
    std::string sysName = snap.systemId;
    std::string sysExtra;
    if (view.universe != nullptr)
    {
        for (const WorldLoader::SystemInfo& si : view.universe->systems)
            if (si.id == snap.systemId)
            {
                sysName = si.name;
                sysExtra = Fmt("  security %.2f", si.security);
                break;
            }
    }
    if (view.galaxy != nullptr)
    {
        for (const Proto::GalaxySystemStat& g : view.galaxy->systems)
            if (g.id == snap.systemId)
            {
                sysExtra += "  controlled by " + FactionName(g.controller);
                break;
            }
    }
    // Only print the id alongside the name when they differ — without the galaxy index
    // loaded they are the same string, and "core (core)" is noise.
    out += sysName == snap.systemId ? Fmt("SYSTEM %s%s\n", sysName.c_str(), sysExtra.c_str())
                                    : Fmt("SYSTEM %s (%s)%s\n", sysName.c_str(),
                                          snap.systemId.c_str(), sysExtra.c_str());

    // --- What we are flying -------------------------------------------------
    const char* mode = p.docked ? "docked" : (p.warpPhase != 0 ? "in warp" : "flying");
    out += Fmt("SHIP   hull %.0f/%.0f  shields %.0f/%.0f  cargo %d/%d  money %.0f cr\n", p.hull,
               p.maxHull, p.shields, p.maxShields, p.cargoUsed, p.cargoCap, p.money);
    out += Fmt("       %s  stabilizer %s  weapons %s  mining %s%s\n", mode,
               p.stabilizer ? "on" : "off", p.weaponOn ? "ARMED" : "off", p.mining ? "on" : "off",
               p.autopilot ? "  autopilot engaged" : "");

    // Cargo by name, so an agent can decide what to sell without a second call.
    if (!p.cargoByType.empty())
    {
        std::string cargo;
        const auto& types = AllResourceTypes();
        for (size_t i = 0; i < types.size() && i < p.cargoByType.size(); i++)
            if (p.cargoByType[i] > 0)
                cargo += Fmt("%s%s %d", cargo.empty() ? "" : ", ", ResourceName(types[i]).c_str(),
                             p.cargoByType[i]);
        if (!cargo.empty())
            out += "CARGO  " + cargo + "\n";
    }

    // --- What is around us --------------------------------------------------
    std::vector<Seen> seen;
    seen.reserve(snap.entities.size());
    for (const Proto::EntitySnapshot& e : snap.entities)
    {
        float dx = e.pos.x - p.pos.x;
        float dy = e.pos.y - p.pos.y;
        seen.push_back({ &e, std::sqrt(dx * dx + dy * dy), HostileToPlayer(e, p) });
    }
    std::sort(seen.begin(), seen.end(),
              [](const Seen& a, const Seen& b) { return a.dist < b.dist; });

    // Hostiles first and in full: they are the reason to change plan.
    std::string hostiles;
    for (const Seen& s : seen)
        if (s.hostile)
            hostiles += Line(s, p, view.layout) + "\n";
    if (!hostiles.empty())
        out += "HOSTILE\n" + hostiles;

    const size_t cap = detail == Detail::Full ? FULL_NEARBY : BRIEF_NEARBY;
    size_t       shown = 0, skipped = 0;
    std::string  nearby;
    for (const Seen& s : seen)
    {
        if (s.hostile)
            continue;  // already listed
        bool anchor =
            s.e->kind == Proto::EntityKind::Station || s.e->kind == Proto::EntityKind::Gate;
        if (shown >= cap && !anchor)
        {
            skipped++;
            continue;
        }
        nearby += Line(s, p, view.layout) + "\n";
        shown++;
    }
    if (!nearby.empty())
    {
        out += skipped > 0 ? Fmt("NEARBY (%d shown, %d further away not listed)\n", (int)shown,
                                 (int)skipped)
                           : "NEARBY\n";
        out += nearby;
    }

    // --- Station business ---------------------------------------------------
    if (p.docked && !snap.marketPrices.empty())
    {
        std::string prices;
        const auto& types = AllResourceTypes();
        for (size_t i = 0; i < types.size() && i < snap.marketPrices.size(); i++)
            prices += Fmt("%s%s %.1f", prices.empty() ? "" : ", ", ResourceName(types[i]).c_str(),
                          snap.marketPrices[i]);
        out += "MARKET " + prices + "\n";
    }
    if (!snap.missionOffers.empty())
    {
        out += Fmt("OFFERS (%d at this station)\n", (int)snap.missionOffers.size());
        for (size_t i = 0; i < snap.missionOffers.size(); i++)
            out += Fmt("  [%d] %s  %.0f cr\n", (int)i, snap.missionOffers[i].title.c_str(),
                       snap.missionOffers[i].rewardMoney);
    }

    // --- Standing business --------------------------------------------------
    if (!snap.missionActive.empty())
    {
        out += "MISSIONS\n";
        for (size_t i = 0; i < snap.missionActive.size(); i++)
        {
            const Proto::MissionView& m = snap.missionActive[i];
            out += Fmt("  [%d] %s  %d/%d%s\n", (int)i, m.title.c_str(), m.progress, m.targetCount,
                       m.completable ? "  READY TO HAND IN" : "");
        }
    }

    // --- What just happened -------------------------------------------------
    if (!snap.events.empty())
    {
        out += "EVENTS\n";
        // The kind is printed next to the prose deliberately: an agent should branch on the
        // kind, never parse the sentence. Showing both makes that obvious instead of implied.
        for (const Ev::Event& e : snap.events)
            out += Fmt("  [%d] %-14s %s\n", e.seq, Ev::KindName(e.kind), e.text.c_str());
    }

    if (detail == Detail::Full)
    {
        out += Fmt("STANDING\n");
        for (size_t i = 0; i < p.reputation.size(); i++)
        {
            double owed = i < p.bounty.size() ? p.bounty[i] : 0.0;
            out += Fmt("  %-16s %+.1f%s\n", FactionName((FactionId)i).c_str(), p.reputation[i],
                       owed > 0.0 ? Fmt("  WANTED, bounty %.0f cr", owed).c_str() : "");
        }
    }

    return out;
}

}  // namespace Obs
