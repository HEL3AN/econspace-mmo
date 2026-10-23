// Fleets (#464, slice 2): what each side in a system flies, design by design.
//
// A faction's presence and the role populations are floats -- how many ships -- and a fleet
// says which: design id -> ships, per (faction, role), in SystemAggregate::fleets. Every
// fight reads it (FighterIn), and the spawn director draws from it (FleetDesign). The float
// stays the truth for the count: each macro pass scales a fleet to it, so a cold loss, a
// migration or a siege's ships lent and given back -- all of them proportional -- leave its
// mix alone, and only what knows which ship it was edits a design's count. See
// documents/npc_fits_living_world.md.

#include "sim/Simulation.h"
#include "core/Archetype.h"
#include "core/Faction.h"
#include "core/Fitting.h"
#include "core/ShipDesign.h"
#include "entities/NpcShip.h"
#include <algorithm>
#include <cmath>

namespace
{
// Below this a fleet's number is no ship at all.
constexpr float FLEET_EPSILON = 1e-4f;

using Fleet = std::map<std::string, float>;

// The doctrine's designs for `role` in equal shares of `total`: what Pick spreads them as.
void SeedFromDoctrine(Fleet& fleet, FactionId f, NpcRole role, float total)
{
    fleet.clear();
    const std::vector<std::string>* list =
        Archetypes::ShipCatalogue().DesignsFor(Factions::Id(f), NpcRoleId(role));
    if (list == nullptr || list->empty())
        return;
    for (const std::string& id : *list)
        fleet[id] += total / (float)list->size();
}
// FleetKey, made once per faction and role: every macro pass asks for a few in every system,
// and the macro pass must stay cheap. A key whose faction's id is no longer its prefix -- a
// registry loaded again -- is made again.
const std::string& KeyFor(FactionId f, NpcRole role)
{
    static std::vector<std::string> keys;
    constexpr size_t                ROLES = 6;
    const size_t                    at = (size_t)f.Index() * ROLES + (size_t)role;
    if (at >= keys.size())
        keys.resize(at + 1);
    std::string&       k = keys[at];
    const std::string& id = Factions::Id(f);
    if (k.size() <= id.size() || k.compare(0, id.size(), id) != 0 || k[id.size()] != '/')
        k = Simulation::FleetKey(f, role);
    return k;
}
}  // namespace

std::string Simulation::FleetKey(FactionId f, NpcRole role)
{
    return Factions::Id(f) + "/" + NpcRoleId(role);
}

Ships::Fighter Simulation::FighterIn(const SystemState& st, FactionId f, NpcRole role) const
{
    const auto it = st.agg.fleets.find(KeyFor(f, role));
    if (it == st.agg.fleets.end())
        return FighterOf(f, role);
    if (designFightersFrom_ != Archetypes::Generation())
    {
        designFighters_.clear();
        designFightersFrom_ = Archetypes::Generation();
    }
    Ships::Fighter mean;
    float          ships = 0.0f;
    for (const auto& [design, n] : it->second)
    {
        if (n <= 0.0f)
            continue;
        auto d = designFighters_.find(design);
        if (d == designFighters_.end())
            d = designFighters_
                    .emplace(design, Ships::DesignFighter(Archetypes::ShipCatalogue(), design))
                    .first;
        // A design that does not derive counts as nothing, as in the doctrine's average.
        mean.dps += d->second.dps * n;
        mean.hull += d->second.hull * n;
        ships += n;
    }
    if (ships <= 0.0f)
        return FighterOf(f, role);
    mean.dps /= ships;
    mean.hull /= ships;
    return mean;
}

std::string Simulation::FleetDesign(const SystemState& st, FactionId f, NpcRole role,
                                    unsigned key) const
{
    const auto it = st.agg.fleets.find(KeyFor(f, role));
    float      total = 0.0f;
    if (it != st.agg.fleets.end())
        for (const auto& kv : it->second)
            total += std::max(0.0f, kv.second);
    if (total <= 0.0f)
        return Archetypes::ShipCatalogue().Pick(Factions::Id(f), NpcRoleId(role), key);
    // Consecutive keys -- the n-th ship of a role as a system warms -- spread evenly over the
    // fleet (the golden ratio's sequence), so a fleet of three cutters and a cruiser warms as
    // about that, and the same key always finds the same design.
    const double golden = 0.6180339887498949;
    const double u = std::fmod((double)key * golden, 1.0) * (double)total;
    double       at = 0.0;
    std::string  last;
    for (const auto& [design, n] : it->second)
    {
        if (n <= 0.0f)
            continue;
        at += (double)n;
        last = design;
        if (u < at)
            return design;
    }
    return last;
}

void Simulation::ReconcileFleets(SystemState& st, bool verify) const
{
    SystemAggregate& a = st.agg;
    // The numbers fleets describe: who flies each role here now, and each faction's presence.
    struct Want
    {
        FactionId f;
        NpcRole   role;
        float     n;
    };
    const FactionId trade =
        Factions::IsLawful(a.controller) ? a.controller : Factions::Unaffiliated();
    // Reused from call to call: this runs for every system on every pass.
    static thread_local std::vector<Want> wants;
    wants.assign({ { Factions::Pirates(), NpcRole::Pirate, a.pirates },
                   { a.policeFaction, NpcRole::Police, a.police },
                   { trade, NpcRole::Trader, a.traders },
                   { Factions::Unaffiliated(), NpcRole::Miner, a.miners } });
    for (FactionId f : Factions::All())
        if (a.presence[f] > FLEET_EPSILON)
            wants.push_back({ f, NpcRole::Warship, a.presence[f] });

    const Ships::Catalogue& cat = Archetypes::ShipCatalogue();
    // The fleets wanted, by their node in the map: a node does not move until it is erased.
    static thread_local std::vector<const Fleet*> kept;
    kept.clear();
    for (const Want& w : wants)
    {
        if (w.n <= FLEET_EPSILON)
            continue;
        Fleet& fleet = a.fleets[KeyFor(w.f, w.role)];
        kept.push_back(&fleet);
        // A design this catalogue does not have (a variant rule changed) is the role's stock
        // design now: counted, never dropped.
        float sum = 0.0f;
        for (auto it = fleet.begin(); it != fleet.end();)
        {
            if (!(it->second > 0.0f) || !std::isfinite(it->second))
            {
                it = fleet.erase(it);
                continue;
            }
            if (verify && cat.FindDesign(it->first) == nullptr)
            {
                const float       n = it->second;
                const std::string stock = cat.Pick(Factions::Id(w.f), NpcRoleId(w.role), 0);
                it = fleet.erase(it);
                if (!stock.empty())
                    fleet[stock] += n;  // a map insert never moves `it`
                continue;
            }
            ++it;
        }
        for (const auto& kv : fleet)
            sum += kv.second;
        if (sum <= FLEET_EPSILON)
            SeedFromDoctrine(fleet, w.f, w.role, w.n);
        else if (std::fabs(sum - w.n) > FLEET_EPSILON)
            for (auto& kv : fleet)
                kv.second *= w.n / sum;
    }
    // Whatever no number describes any more -- and anything left empty -- goes.
    for (auto it = a.fleets.begin(); it != a.fleets.end();)
    {
        const bool wanted =
            !it->second.empty() && std::find(kept.begin(), kept.end(), &it->second) != kept.end();
        it = wanted ? std::next(it) : a.fleets.erase(it);
    }
}

bool Simulation::DoctrineMix(const std::string& key, const std::map<std::string, float>& fleet)
{
    const size_t slash = key.rfind('/');
    if (slash == std::string::npos || fleet.empty())
        return false;
    const std::vector<std::string>* list =
        Archetypes::ShipCatalogue().DesignsFor(key.substr(0, slash), key.substr(slash + 1));
    if (list == nullptr || list->empty())
        return false;
    float total = 0.0f;
    for (const auto& kv : fleet)
        total += kv.second;
    std::map<std::string, float> seed;
    for (const std::string& id : *list)
        seed[id] += total / (float)list->size();
    if (seed.size() != fleet.size())
        return false;
    for (const auto& [id, n] : fleet)
    {
        const auto s = seed.find(id);
        if (s == seed.end() || std::fabs(s->second - n) > 1e-3f * std::max(1.0f, total))
            return false;
    }
    return true;
}

void Simulation::RecountFleets(SystemState& st) const
{
    // The four role populations are what flies here, so their fleets are what flies here:
    // a ship shot down is gone from its own design's count, whichever it was.
    SystemAggregate& a = st.agg;
    for (auto it = a.fleets.begin(); it != a.fleets.end();)
    {
        const std::string role = it->first.substr(it->first.rfind('/') + 1);
        const bool        counted =
            role == NpcRoleId(NpcRole::Trader) || role == NpcRoleId(NpcRole::Miner) ||
            role == NpcRoleId(NpcRole::Police) || role == NpcRoleId(NpcRole::Pirate);
        it = counted ? a.fleets.erase(it) : std::next(it);
    }
    for (const auto& e : st.entities)
    {
        if (e->GetKind() != EntityKind::Npc)
            continue;
        const NpcShip& n = static_cast<const NpcShip&>(*e);
        if (!n.IsAlive() || n.IsLeaving() || n.GetTask() != 0 || n.GetDesign().empty())
            continue;
        const NpcRole r = n.GetRole();
        if (r == NpcRole::Trader || r == NpcRole::Miner || r == NpcRole::Police ||
            r == NpcRole::Pirate)
            a.fleets[FleetKey(n.GetFaction(), r)][n.GetDesign()] += 1.0f;
    }
}
