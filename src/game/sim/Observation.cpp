#include "sim/Observation.h"

#include "core/Archetype.h"
#include "core/Faction.h"
#include "economy/Resource.h"
#include "entities/ShipType.h"
#include "missions/Mission.h"

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
        case Proto::EntityKind::Structure: return "structure";
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
                 const std::map<int, Proto::EntityLayout>* layout, double now)
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
            else if (e.kind == Proto::EntityKind::Derelict && it->second.reward > 0.0 &&
                     !it->second.looted)  // a searched wreck says so once it is (#38)
                extra = "  lootable";
            else if (e.kind == Proto::EntityKind::Structure)
            {
                // Whose, and how far along -- from the clock, as the client draws it (#39).
                const Proto::EntityLayout& l = it->second;
                const double               span = l.completesAt - l.startedAt;
                if (l.completesAt > now && span > 0.0)
                    extra = Fmt("  site %d%%, %.0fs left",
                                (int)((now - l.startedAt) / span * 100.0), l.completesAt - now);
                if (!l.owner.empty())
                    extra += "  by " + l.owner;
            }
        }
    }
    if (e.kind == Proto::EntityKind::Field && e.ore >= 0)
        extra = "  " + ResourceName((ResourceType)e.ore);
    // Dockable is a component, asked of the archetype the layout names (#195) -- the same
    // question the server's docking pass asks (#34), so a dock a player builds (#44) is one
    // here too. Without a layout to ask, a station is assumed to be one, as it always was.
    bool dockable = e.kind == Proto::EntityKind::Station;
    if (layout != nullptr)
    {
        auto it = layout->find(e.id);
        if (it != layout->end() && !it->second.archetype.empty())
        {
            const Archetype* a = Archetypes::Find(it->second.archetype);
            dockable = a != nullptr && a->components.Has(Component::Dockable);
        }
    }
    if (dockable)
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
        Fmt("  #%-4d %-8s %-22s %7.0fu %-2s%s", e.id, kind, name.c_str(), s.dist,
            Obs::Compass(e.pos.x - p.pos.x, e.pos.y - p.pos.y).c_str(), extra.c_str());
    while (!line.empty() && line.back() == ' ')  // column padding, not content
        line.pop_back();
    return line;
}

// A station by id, named if this system's layout knows it. Missions address stations by id
// and survive a jump, so the station may well be somewhere else -- say so rather than
// print a bare number that looks like an id observe would list.
std::string StationName(const Obs::View& view, int id)
{
    if (view.layout != nullptr)
    {
        auto it = view.layout->find(id);
        if (it != view.layout->end() && !it->second.name.empty())
            return Fmt("%s (#%d)", it->second.name.c_str(), id);
    }
    if (view.snapshot != nullptr)
        for (const Proto::EntitySnapshot& e : view.snapshot->entities)
            if (e.id == id && !e.name.empty())
                return Fmt("%s (#%d)", e.name.c_str(), id);
    return Fmt("station #%d, not in this system", id);
}

const char* MissionKind(int type)
{
    switch ((MissionType)type)
    {
        case MissionType::Bounty: return "bounty";
        case MissionType::Mining: return "mining";
        case MissionType::Delivery: return "delivery";
    }
    return "job";
}

// Where a mission is handed in. A delivery ends at its destination; everything else goes
// back to the station that gave it -- the same rule as Simulation::MissionCompletableNow.
int HandInStation(const Proto::MissionView& m)
{
    return (MissionType)m.type == MissionType::Delivery ? m.destStationId : m.giverStationId;
}

std::string MissionLine(const Obs::View& view, size_t index, const Proto::MissionView& m)
{
    std::string out = Fmt("  [%d] %s: %s -- %s\n", (int)index, MissionKind(m.type), m.title.c_str(),
                          m.description.c_str());
    out += Fmt("       reward %.0f cr, %s standing %+.1f; hand in at %s\n", m.rewardMoney,
               FactionName((FactionId)m.faction).c_str(), m.rewardRep,
               StationName(view, HandInStation(m)).c_str());
    return out;
}

}  // namespace

namespace Obs
{

FactionId DockedFaction(const View& view)
{
    if (view.snapshot == nullptr || view.layout == nullptr || !view.snapshot->player.docked)
        return FactionId::Independent;
    auto it = view.layout->find(view.snapshot->player.dockedStationId);
    return it == view.layout->end() ? FactionId::Independent : it->second.faction;
}

std::string MissionNeeds(const View& view, const Proto::MissionView& m)
{
    if (m.completable)
        return std::string();
    const Proto::PlayerView* p = view.snapshot != nullptr ? &view.snapshot->player : nullptr;
    switch ((MissionType)m.type)
    {
        case MissionType::Bounty:
            if (m.progress < m.targetCount)
                return Fmt("destroy %d more pirate%s", m.targetCount - m.progress,
                           m.targetCount - m.progress == 1 ? "" : "s");
            break;
        case MissionType::Mining:
        {
            int held = 0;
            if (p != nullptr && m.resource >= 0 && m.resource < (int)p->cargoByType.size())
                held = p->cargoByType[m.resource];
            if (held < m.targetCount)
                return Fmt("carry %d %s (the hold has %d)", m.targetCount,
                           ResourceName((ResourceType)m.resource).c_str(), held);
            break;
        }
        case MissionType::Delivery: break;
    }
    return "dock at " + StationName(view, HandInStation(m));
}

std::string DescribeMissions(const View& view)
{
    if (view.snapshot == nullptr)
        return "No world state yet.\n";
    const Proto::Snapshot& snap = *view.snapshot;
    std::string            out;

    if (!snap.player.docked)
        out += "OFFERS  none: the job board is at a station; dock to see its work\n";
    else if (snap.missionOffers.empty())
        out += "OFFERS  this station has no work on its board\n";
    else
    {
        out += Fmt("OFFERS at %s -- accept_mission takes the number\n",
                   StationName(view, snap.player.dockedStationId).c_str());
        for (size_t i = 0; i < snap.missionOffers.size(); i++)
            out += MissionLine(view, i, snap.missionOffers[i]);
    }

    if (snap.missionActive.empty())
    {
        out += "ACTIVE  none\n";
        return out;
    }
    out += "ACTIVE -- complete_mission takes the number\n";
    for (size_t i = 0; i < snap.missionActive.size(); i++)
    {
        const Proto::MissionView& m = snap.missionActive[i];
        out += MissionLine(view, i, m);
        if ((MissionType)m.type == MissionType::Bounty)
            out += Fmt("       progress %d/%d\n", m.progress, m.targetCount);
        out += m.completable ? "       READY TO HAND IN here\n"
                             : "       needs: " + MissionNeeds(view, m) + "\n";
    }
    return out;
}

std::string DescribeHangar(const View& view)
{
    if (view.snapshot == nullptr)
        return "No world state yet.\n";
    const Proto::PlayerView&     p = view.snapshot->player;
    const std::vector<ShipType>& catalog = GetShipCatalog();

    auto owns = [&p](int i)
    {
        for (int o : p.ownedShips)
            if (o == i)
                return true;
        return false;
    };

    // Outside a station the prices are the catalog's: what a station charges depends on who
    // owns it, and there is no station to ask.
    const FactionId sf = DockedFaction(view);
    const float     standing = (size_t)sf < p.reputation.size() ? p.reputation[(size_t)sf] : 0.0f;
    const float     mul = p.docked ? ShipPriceMultiplier(Factions::TierOf(standing)) : 1.0f;

    std::string out =
        p.docked
            ? Fmt("HANGAR at %s, owned by %s -- money %.0f cr\n",
                  StationName(view, p.dockedStationId).c_str(), FactionName(sf).c_str(), p.money)
            : Fmt("HANGAR -- money %.0f cr; dock at a station to buy or switch ships\n", p.money);
    for (size_t i = 0; i < catalog.size(); i++)
    {
        const ShipType& t = catalog[i];
        std::string     status;
        if ((int)i == p.shipIndex)
            status = "FLYING";
        else if (owns((int)i))
            status = "owned, switch_ship for free";
        else if (p.docked)
            status = Fmt("for sale %.0f cr%s", t.price * mul,
                         p.money >= t.price * mul ? "" : " (cannot afford)");
        else
            status = Fmt("list price %.0f cr", t.price);
        out += Fmt("  [%d] %-8s speed %.0f  cargo %d  mining %.1f/s  %s\n", (int)i, t.name.c_str(),
                   t.stats.maxSpeed, t.stats.cargoCapacity, t.stats.miningRate, status.c_str());
    }
    return out;
}

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
    // Docked is attached: the ship goes round with a station on an orbit (#298).
    const char* mode = p.docked ? "docked (attached, moves with the station)"
                                : (p.warpPhase != 0 ? "in warp" : "flying");
    out += Fmt("SHIP   hull %.0f/%.0f  shields %.0f/%.0f  cargo %d/%d  money %.0f cr\n", p.hull,
               p.maxHull, p.shields, p.maxShields, p.cargoUsed, p.cargoCap, p.money);
    out += Fmt("       %s  stabilizer %s  weapons %s  mining %s%s\n", mode,
               p.stabilizer ? "on" : "off", p.weaponOn ? "ARMED" : "off", p.mining ? "on" : "off",
               p.autopilot ? "  autopilot engaged" : "");
    // A standing hold runs until something releases it (#157, #298), so say it is running.
    if (p.holdMode != 0)
        out += Fmt("       holding: %s object %d at %.0f\n",
                   p.holdMode == 1   ? "orbiting"
                   : p.holdMode == 2 ? "keeping at range of"
                                     : "following",
                   p.holdTargetId, p.holdRange);

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
            hostiles += Line(s, p, view.layout, snap.time) + "\n";
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
        nearby += Line(s, p, view.layout, snap.time) + "\n";
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
