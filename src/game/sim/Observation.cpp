#include "sim/Observation.h"

#include "core/Archetype.h"
#include "core/Fitting.h"
#include "core/Faction.h"
#include "economy/Resource.h"
#include "entities/ShipType.h"
#include "entities/Structure.h"
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

    const size_t idx = (size_t)e.faction.Index();
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
            else if (e.kind == Proto::EntityKind::Derelict && !it->second.looted &&
                     (it->second.reward > 0.0 || e.cargoAmount > 0))
                extra = "  lootable";  // a searched wreck says so once it is (#38)
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
                // Whom it fires at, when that is not only back (#340).
                if (!l.defence.IsDefault())
                {
                    extra += std::string("  fires: ") + EngageName(l.defence.engage);
                    if (l.defence.engage == Engage::Enemies)
                        extra += Fmt(" below %.0f", l.defence.threshold);
                    else if (l.defence.engage == Engage::Strangers &&
                             !AllowListText(l.defence).empty())
                        extra += " but " + AllowListText(l.defence);
                }
                // Its hull once it has been hit, by the rule the server destroys it by.
                if (l.damage > 0.0f)
                {
                    Structure probe(l.pos, l.size, l.name, l.archetype);
                    probe.StartBuilding(l.startedAt, l.completesAt);
                    probe.SetBlueprint(l.blueprint);
                    probe.SetDamage(l.damage);
                    extra += Fmt("  hull %.0f%%", probe.HullFraction(now) * 100.0f);
                }
            }
        }
    }
    if (e.kind == Proto::EntityKind::Field && e.ore >= 0)
    {
        extra = "  " + ResourceName(ResourceType::FromIndex(e.ore));
        if (e.oreMax > 0)  // what is left of it (#348)
            extra += Fmt(" %d/%d left", e.oreLeft, e.oreMax);
    }
    // What a wreck holds (#343).
    if (e.kind == Proto::EntityKind::Derelict && e.cargo >= 0 && e.cargo < Goods::Count() &&
        e.cargoAmount > 0)
        extra += Fmt("  holds %d ", e.cargoAmount) + ResourceName(ResourceType::FromIndex(e.cargo));
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
        // A hauler's hold (#343): what a wreck of it would hold.
        if (e.cargo >= 0 && e.cargo < Goods::Count() && e.cargoAmount > 0)
            extra += Fmt("  carrying %d ", e.cargoAmount) +
                     ResourceName(ResourceType::FromIndex(e.cargo));
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
        case MissionType::Supply: return "supply";
        case MissionType::Escort: return "escort";
        case MissionType::Siege: return "siege";
    }
    return "job";
}

// Where a mission is handed in. A delivery ends at its destination; a plan's job anywhere
// (#345), so at no station; everything else goes back to the station that gave it -- the same
// rule as Simulation::MissionCompletableNow.
int HandInStation(const Proto::MissionView& m)
{
    if (IsPlanJob((MissionType)m.type))
        return 0;
    return (MissionType)m.type == MissionType::Delivery ? m.destStationId : m.giverStationId;
}

std::string MissionLine(const Obs::View& view, size_t index, const Proto::MissionView& m)
{
    std::string out = Fmt("  [%d] %s: %s -- %s\n", (int)index, MissionKind(m.type), m.title.c_str(),
                          m.description.c_str());
    if (IsPlanJob((MissionType)m.type))
    {
        // A plan's job: where it is, how long it has, and that the faction pays by wire.
        const std::string due = Obs::JobDue(view, m);
        out += Fmt("       reward %.0f cr, %s standing %+.1f; in %s (system %s)%s%s; hand in "
                   "anywhere\n",
                   m.rewardMoney, FactionName(m.faction).c_str(), m.rewardRep, m.systemName.c_str(),
                   m.systemId.c_str(), due.empty() ? "" : ", ", due.c_str());
        return out;
    }
    out += Fmt("       reward %.0f cr, %s standing %+.1f; hand in at %s\n", m.rewardMoney,
               FactionName(m.faction).c_str(), m.rewardRep,
               StationName(view, HandInStation(m)).c_str());
    return out;
}

}  // namespace

namespace Obs
{

FactionId DockedFaction(const View& view)
{
    if (view.snapshot == nullptr || view.layout == nullptr || !view.snapshot->player.docked)
        return Factions::Unaffiliated();
    auto it = view.layout->find(view.snapshot->player.dockedStationId);
    return it == view.layout->end() ? Factions::Unaffiliated() : it->second.faction;
}

const Ships::Design* CurrentHull(const View& view)
{
    if (view.snapshot == nullptr)
        return nullptr;
    const int                    i = view.snapshot->player.shipIndex;
    const std::vector<ShipType>& catalog = GetShipCatalog();
    if (i < 0 || i >= (int)catalog.size())
        return nullptr;
    return Archetypes::ShipCatalogue().FindDesign(catalog[i].stats.design);
}

const Ships::Design* CurrentDesign(const View& view)
{
    if (view.snapshot == nullptr)
        return nullptr;
    const Proto::PlayerView& p = view.snapshot->player;
    for (size_t i = 0; i < p.ownedShips.size() && i < p.shipDesigns.size(); i++)
        if (p.ownedShips[i] == p.shipIndex)
            if (const Ships::Design* d = Archetypes::FindDesign(p.shipDesigns[i]))
                return d;
    return CurrentHull(view);
}

bool AtShipyard(const View& view, float* markup)
{
    if (view.snapshot == nullptr || view.layout == nullptr || !view.snapshot->player.docked)
        return false;
    auto it = view.layout->find(view.snapshot->player.dockedStationId);
    if (it == view.layout->end())
        return false;
    const Archetype* a = Archetypes::Find(it->second.archetype);
    if (a == nullptr || !a->Has(Component::Shipyard))
        return false;
    if (markup != nullptr)
        *markup = a->refitMarkup;
    return true;
}

namespace
{
float Standing(const View& view, FactionId f)
{
    const Proto::PlayerView& p = view.snapshot->player;
    const size_t             i = (size_t)f.Index();
    return i < p.reputation.size() ? p.reputation[i] : 0.0f;
}

// A module's ore at this system's prices, times the yard's markup: what one costs fitted.
double ModulePrice(const View& view, const Ships::ModulePart& m, float markup)
{
    double v = 0.0;
    for (const auto& [r, n] : m.cost)
        if ((size_t)r.Index() < view.snapshot->marketPrices.size())
            v += (double)n * view.snapshot->marketPrices[(size_t)r.Index()];
    return v * markup * ShipPriceMultiplier(Factions::TierOf(Standing(view, DockedFaction(view))));
}

std::string Provides(const Ships::Provides& p)
{
    std::string out;
    auto        add = [&](const char* what, float v)
    {
        if (v != 0.0f)
            out += Fmt("%s%s %g", out.empty() ? "" : ", ", what, (double)v);
    };
    add("thrust", p.thrust);
    add("rcs", p.rcs);
    add("cargo", p.cargo);
    add("mining", p.mining);
    add("hull", p.hull);
    add("damage", p.damage);
    return out.empty() ? std::string("nothing") : out;
}
}  // namespace

FitQuote QuoteFit(const View& view, const std::vector<Ships::FitLine>& fit)
{
    FitQuote             q;
    const Ships::Design* hull = CurrentHull(view);
    const Ships::Design* now = CurrentDesign(view);
    if (hull == nullptr || now == nullptr)
    {
        q.problem = "no world state yet";
        return q;
    }
    q.hull = hull->name;
    std::string why;
    PlayerStats(*now, q.before, why);
    q.after = q.before;
    const Ships::Catalogue& ships = Archetypes::ShipCatalogue();
    if (!Ships::PlayerFit(ships, *hull, fit, q.design, q.problem))
        return q;
    // The server refuses a design it cannot derive numbers for, as a fit it cannot make.
    if (!PlayerStats(q.design, q.after, q.problem))
    {
        q.design = Ships::Design();
        q.after = q.before;
        return q;
    }
    q.changed = q.design.id != now->id;
    float      markup = 1.5f;
    const bool yard = AtShipyard(view, &markup);
    q.price = Ships::RefitPrice(ships, *now, q.design, view.snapshot->marketPrices, markup);
    if (q.price > 0.0)
        q.price *= ShipPriceMultiplier(Factions::TierOf(Standing(view, DockedFaction(view))));
    const Proto::PlayerView& p = view.snapshot->player;
    if (!q.changed)
        q.problem = "that is the fit the " + q.hull + " has";
    else if (!yard)
        q.problem = "refits are done docked at a station with a shipyard";
    else if (q.after.cargoCapacity < p.cargoUsed)
        q.problem = Fmt("the hold would take %d and carries %d; sell some first",
                        q.after.cargoCapacity, p.cargoUsed);
    else if (q.price > 0.0 && q.price > p.money)
        q.problem = Fmt("it costs %.0f cr and you have %.0f cr", q.price, p.money);
    q.ok = q.problem.empty();
    return q;
}

std::string DescribeQuote(const FitQuote& q)
{
    std::string out = q.hull.empty() ? std::string("REFIT\n") : "REFIT of the " + q.hull + "\n";
    auto        row = [&](const char* what, float a, float b, const char* fmt)
    {
        out += Fmt("  %-8s ", what) + Fmt(fmt, (double)a) + " -> " + Fmt(fmt, (double)b) +
               (a == b  ? ""
                : b > a ? "  (+)"
                        : "  (-)") +
               "\n";
    };
    row("speed", q.before.maxSpeed, q.after.maxSpeed, "%.0f");
    row("turn", q.before.turnSpeed, q.after.turnSpeed, "%.2f");
    row("cargo", (float)q.before.cargoCapacity, (float)q.after.cargoCapacity, "%.0f");
    row("mining", q.before.miningRate, q.after.miningRate, "%.1f");
    row("hull", q.before.hull, q.after.hull, "%.0f");
    row("guns", q.before.damage, q.after.damage, "%.0f");
    if (!q.design.id.empty())
        out += q.price >= 0.0 ? Fmt("  price %.0f cr\n", q.price)
                              : Fmt("  refund %.0f cr\n", -q.price);
    out += q.ok ? std::string("  the server would take it: call refit with this fit\n")
                : "  refused: " + q.problem + "\n";
    return out;
}

std::string DescribeFitting(const View& view)
{
    const Ships::Design* hull = CurrentHull(view);
    const Ships::Design* now = CurrentDesign(view);
    if (hull == nullptr || now == nullptr)
        return "No world state yet.\n";
    float       markup = 1.5f;
    const bool  yard = AtShipyard(view, &markup);
    std::string out =
        Fmt("FITTING of your %s%s -- %s\n", hull->name.c_str(),
            now->id == hull->id ? "" : " (refitted)",
            yard ? Fmt("docked at a shipyard, modules at %.2fx their ore", (double)markup).c_str()
                 : "refits are done docked at a station with a shipyard");
    out += "Fit now (pass a whole fit like this to preview_fit and refit; a fit sets no scale):\n";
    for (const Ships::FitLine& l : Ships::Normalized(now->fit))
        out += Fmt("  {\"module\": \"%s\", \"in\": \"%s\", \"on\": \"%s\", \"count\": %d}\n",
                   l.module.c_str(), l.in.c_str(), l.on.c_str(), l.count);
    const Ships::Catalogue& ships = Archetypes::ShipCatalogue();
    out += "Sockets (in/on: used/capacity -- modules that go there):\n";
    for (const Ships::SocketGroup& g : Ships::Sockets(ships, *hull, now->fit))
    {
        std::string mods;
        for (const std::string& m : g.modules)
            mods += (mods.empty() ? "" : ", ") + m;
        out += Fmt("  %s/%s: %d/%d -- %s\n", g.in.c_str(), g.on.c_str(), g.used, g.capacity,
                   mods.empty() ? "nothing" : mods.c_str());
    }
    out += "Modules (mass; what it adds; price fitted here, half its ore back when removed):\n";
    for (const Ships::ModulePart& m : ships.modules)
        out += Fmt("  %-16s mass %g; %s; %.0f cr\n", m.id.c_str(), (double)m.mass,
                   Provides(m.provides).c_str(), ModulePrice(view, m, markup));
    out += "Side and edge sockets take modules in pairs; drives go in the stern; a ship keeps a "
           "drive.\n";
    return out;
}

std::vector<SavedDesignRow> SavedDesigns(const View& view)
{
    std::vector<SavedDesignRow> out;
    if (view.snapshot == nullptr)
        return out;
    const Ships::Design*         now = CurrentDesign(view);
    const std::vector<ShipType>& catalog = GetShipCatalog();
    for (const Proto::SavedDesign& e : view.snapshot->player.library)
    {
        SavedDesignRow r;
        r.name = e.name;
        r.id = e.design;
        r.hullIndex = HullOf(e.design);
        const Ships::Design* d = Archetypes::FindDesign(e.design);
        std::string          why;
        r.known = d != nullptr && r.hullIndex >= 0 && r.hullIndex < (int)catalog.size() &&
                  PlayerStats(*d, r.stats, why);
        if (r.known)
        {
            r.hull = catalog[(size_t)r.hullIndex].name;
            r.thisHull = r.hullIndex == view.snapshot->player.shipIndex;
            r.flown = r.thisHull && now != nullptr && now->id == e.design;
            if (r.thisHull)
                r.quote = QuoteFit(view, Ships::PlayerLines(*d));
        }
        out.push_back(std::move(r));
    }
    return out;
}

std::string DescribeDesigns(const View& view)
{
    if (view.snapshot == nullptr)
        return "No world state yet.\n";
    const std::vector<SavedDesignRow> rows = SavedDesigns(view);
    std::string out = Fmt("DESIGNS saved: %d of %d (save_design adds one; apply_design refits "
                          "the ship you fly to one of its own hull, at a shipyard)\n",
                          (int)rows.size(), Ships::LIBRARY_MAX);
    if (rows.empty())
        out += "  none yet\n";
    for (const SavedDesignRow& r : rows)
    {
        if (!r.known)
        {
            out += Fmt("  \"%s\" -- %s, not known here yet\n", r.name.c_str(), r.id.c_str());
            continue;
        }
        std::string apply;
        if (r.flown)
            apply = "the ship you fly is this design";
        else if (!r.thisHull)
            apply = "for the " + r.hull + "; fly one to apply it";
        else if (r.quote.ok)
            apply = r.quote.price >= 0.0
                        ? Fmt("apply here for %.0f cr", r.quote.price)
                        : Fmt("apply here for a refund of %.0f cr", -r.quote.price);
        else
            apply = "cannot apply: " + r.quote.problem;
        out += Fmt("  \"%s\" -- %s (%s): speed %.0f  turn %.2f  cargo %d  mining %.1f/s  hull %.0f"
                   "  guns %.0f/shot -- %s\n",
                   r.name.c_str(), r.hull.c_str(), r.id.c_str(), r.stats.maxSpeed,
                   r.stats.turnSpeed, r.stats.cargoCapacity, r.stats.miningRate, r.stats.hull,
                   r.stats.damage, apply.c_str());
    }
    return out;
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
                           ResourceName(ResourceType::FromIndex(m.resource)).c_str(), held);
            break;
        }
        case MissionType::Delivery: break;
        case MissionType::Supply:
        {
            int held = 0;
            if (p != nullptr && m.resource >= 0 && m.resource < (int)p->cargoByType.size())
                held = p->cargoByType[m.resource];
            const std::string what = ResourceName(ResourceType::FromIndex(m.resource));
            if (held < m.targetCount)
                return Fmt("carry %d %s (the hold has %d) to the site in %s", m.targetCount,
                           what.c_str(), held, m.systemName.c_str());
            if (m.siteId != 0)
                return Fmt("fly beside the site (#%d), undocked, and hand it in", m.siteId);
            return Fmt("take the %s to the site in %s (system %s)", what.c_str(),
                       m.systemName.c_str(), m.systemId.c_str());
        }
        case MissionType::Escort:
            return Fmt("fly with the convoy until it reaches %s (system %s)", m.systemName.c_str(),
                       m.systemId.c_str());
        case MissionType::Siege:
            return Fmt("shoot down %d more besieger%s in %s (system %s)",
                       m.targetCount - m.progress, m.targetCount - m.progress == 1 ? "" : "s",
                       m.systemName.c_str(), m.systemId.c_str());
    }
    return "dock at " + StationName(view, HandInStation(m));
}

std::string JobDue(const View& view, const Proto::MissionView& m)
{
    if (m.deadline <= 0.0 || view.snapshot == nullptr)
        return std::string();
    const double left = m.deadline - view.snapshot->time;
    if (left <= 0.0)
        return "overdue";
    if (left < 120.0)
        return Fmt("due in %.0f s", left);
    return Fmt("due in %.0f min", left / 60.0);
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
        if ((MissionType)m.type == MissionType::Bounty || (MissionType)m.type == MissionType::Siege)
            out += Fmt("       progress %d/%d\n", m.progress, m.targetCount);
        if (m.completable)
            out += IsPlanJob((MissionType)m.type) ? "       READY TO HAND IN, anywhere\n"
                                                  : "       READY TO HAND IN here\n";
        else
            out += "       needs: " + MissionNeeds(view, m) + "\n";
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
    const float     standing =
        (size_t)sf.Index() < p.reputation.size() ? p.reputation[(size_t)sf.Index()] : 0.0f;
    const float mul = p.docked ? ShipPriceMultiplier(Factions::TierOf(standing)) : 1.0f;

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
        // The hull's numbers are its design's (#279 step 5), the ones the server flies it by
        // -- and an owned one's are its own, refitted or not (#279 fitting).
        ShipStats st = t.stats;
        for (size_t k = 0; k < p.ownedShips.size() && k < p.shipDesigns.size(); k++)
            if (p.ownedShips[k] == (int)i && p.shipDesigns[k] != t.stats.design &&
                StatsOfDesign(p.shipDesigns[k], st))
                status += ", refitted";
        out += Fmt("  [%d] %-8s speed %.0f  turn %.1f  cargo %d  mining %.1f/s  hull %.0f  "
                   "guns %.0f/shot  %s\n",
                   (int)i, t.name.c_str(), st.maxSpeed, st.turnSpeed, st.cargoCapacity,
                   st.miningRate, st.hull, st.damage, status.c_str());
        // What losing it would cost, and what insurance would do about it (#344).
        if (owns((int)i))
        {
            const InsuranceQuote q = QuoteInsurance(view, (int)i);
            std::string          line = Fmt("       worth %.0f cr here; ", q.value);
            if (q.cover > 0.0)
                line += Fmt("insured for %.0f cr (pays %.0f if lost)", q.cover,
                            InsurancePayout(q.cover, q.value));
            else if (q.value > 0.0)
                line += "NOT insured";
            else
                line += "free to replace";
            if (q.problem.empty())
                line += Fmt("; insure_ship covers %.0f for %.0f cr", q.value, q.premium);
            out += line + "\n";
        }
    }
    out += "  Losing a ship loses its hull, its fit and its cargo; the starter is free and "
           "always yours.\n";
    return out;
}

InsuranceQuote QuoteInsurance(const View& view, int hull)
{
    InsuranceQuote               q;
    const std::vector<ShipType>& catalog = GetShipCatalog();
    if (view.snapshot == nullptr || hull < 0 || hull >= (int)catalog.size())
    {
        q.problem = "no such hull";
        return q;
    }
    const Proto::PlayerView& p = view.snapshot->player;
    const ShipType&          t = catalog[(size_t)hull];
    q.hull = t.name;
    std::string design = t.stats.design;
    for (size_t k = 0; k < p.ownedShips.size(); k++)
        if (p.ownedShips[k] == hull)
        {
            q.owned = true;
            if (k < p.shipDesigns.size())
                design = p.shipDesigns[k];
            if (k < p.insured.size())
                q.cover = p.insured[k];
        }
    // The server's sum (ShipValue), at the prices the snapshot shows.
    const Ships::Catalogue& ships = Archetypes::ShipCatalogue();
    const Ships::Design*    stock = ships.FindDesign(t.stats.design);
    const Ships::Design*    flown = Archetypes::FindDesign(design);
    q.value = stock != nullptr
                  ? ShipValue(ships, t.price, *stock, flown != nullptr ? *flown : *stock,
                              view.snapshot->marketPrices)
                  : t.price;
    q.premium = std::ceil(InsurancePremium(q.value, q.cover));
    q.payout = InsurancePayout(q.value, q.value);
    // In the server's order (Simulation::InsureShip).
    if (!p.docked)
        q.problem = "insurance is bought docked at a station";
    else if (!q.owned)
        q.problem = "you do not own a " + t.name;
    else if (q.value <= 0.0)
        q.problem = "a stock " + t.name + " is free and needs no insurance";
    else if (q.cover >= q.value)
        q.problem = Fmt("the %s is covered for %.0f cr already", t.name.c_str(), q.cover);
    else if (p.money < q.premium)
        q.problem = Fmt("it costs %.0f cr and you have %.0f cr", q.premium, p.money);
    return q;
}

std::string DescribeScan(const View& view)
{
    if (view.snapshot == nullptr)
        return std::string();
    const Proto::ScanView& scan = view.snapshot->player.scan;
    const double           now = view.snapshot->time;
    auto                   name = [&](const std::string& id)
    {
        if (view.universe != nullptr)
            for (const WorldLoader::SystemInfo& si : view.universe->systems)
                if (si.id == id)
                    return si.name + " (" + id + ")";
        return id;
    };
    std::string out;
    if (scan.until > now)
        out += Fmt("SCAN   a pass is under way, %.0fs left\n", scan.until - now);
    if (!scan.sigs.empty())
    {
        out += "SIGNATURES\n";
        for (const Proto::SignatureView& g : scan.sigs)
            if (g.progress < 1.0f)
                out += Fmt("  %s  unresolved, %d%% -- scan again\n", g.code.c_str(),
                           (int)(g.progress * 100.0f));
            else
            {
                const std::string to = g.dest.empty() ? std::string() : " to " + name(g.dest);
                out +=
                    Fmt("  %s  %s%s  [id %d]\n", g.code.c_str(), g.kind.c_str(), to.c_str(), g.id);
            }
    }
    // The roaming holes this pilot knows, anywhere: routes of theirs.
    if (view.universe != nullptr)
        for (const WorldLoader::SystemLink& l : view.universe->links)
            if (l.kind == "hole")
                out += Fmt("HOLE   %s <-> %s  ships up to %.0f t, closes in %.1f h\n",
                           name(l.a).c_str(), name(l.b).c_str(), l.maxMass,
                           std::max(0.0, l.closesAt - now) / 3600.0);
    return out;
}

std::string DescribeLoss(const Proto::LossView& loss)
{
    if (loss.seq <= 0)
        return std::string();
    std::string out =
        Fmt("LAST LOSS  your %s, destroyed in %s", loss.hull.c_str(), loss.system.c_str());
    if (loss.value > 0.0)
        out += Fmt(", worth %.0f cr", loss.value);
    if (loss.cargo > 0)
        out += Fmt(", with %d cargo", loss.cargo);
    if (loss.paid > 0.0)
        out += Fmt("; insurance paid %.0f cr", loss.paid);
    else if (loss.value > 0.0)
        out += "; not insured";
    out += loss.wokeAt.empty() ? std::string("; you fly the starter now")
                               : "; you woke docked at " + loss.wokeAt + " in the starter";
    return out + "\n";
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
    // A keep is said as what it does (#309): this distance from that object.
    if (p.holdMode == 2)
        out += Fmt("       holding: %.0f from object %d\n", p.holdRange, p.holdTargetId);
    else if (p.holdMode != 0)
        out += Fmt("       holding: %s object %d at %.0f\n",
                   p.holdMode == 1 ? "orbiting" : "following", p.holdTargetId, p.holdRange);

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
    // The last ship lost (#344): an agent that wakes somewhere else in a smaller ship is told
    // why on every look, not only in the event that said it once.
    out += DescribeLoss(p.loss);
    out += DescribeScan(view);

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
        // What it pays / what it asks, and how many it holds (#343): what sell_cargo and buy
        // trade at, before standing, skill and the fee.
        std::string prices;
        const auto& types = AllResourceTypes();
        for (size_t i = 0; i < types.size() && i < snap.marketPrices.size(); i++)
            prices += Fmt("%s%s %.1f/%.1f x%d", prices.empty() ? "" : ", ",
                          ResourceName(types[i]).c_str(), snap.marketPrices[i],
                          i < snap.marketAsk.size() ? snap.marketAsk[i] : 0.0f,
                          i < snap.marketStock.size() ? snap.marketStock[i] : 0);
        out += Fmt("MARKET (bid/ask x stock; fee %.0f%% a trade) ", Goods::Rules().fee * 100.0) +
               prices + "\n";
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
        for (FactionId f : Factions::All())
        {
            const size_t i = (size_t)f.Index();
            if (i >= p.reputation.size())
                break;
            double owed = i < p.bounty.size() ? p.bounty[i] : 0.0;
            out += Fmt("  %-16s %+.1f%s\n", FactionName(f).c_str(), p.reputation[i],
                       owed > 0.0 ? Fmt("  WANTED, bounty %.0f cr", owed).c_str() : "");
        }
    }

    return out;
}

}  // namespace Obs
