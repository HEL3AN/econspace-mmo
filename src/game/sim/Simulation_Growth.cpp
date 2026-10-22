// A new region's first hours (#342 S6): expeditions, stations grown by need, and the builder
// convoy both of the last are built from. One translation unit of Simulation (#17).
//
// A mouth opening is news. The factions holding space near it hear what is on the far side --
// intel on the entry system -- and for a while every step towards the mouth, and into the
// region beyond, is worth their expedition curiosity on top of what it is worth anyway. Nothing
// else is new: the survey, settle and outpost loop of #295 does the rest, drawn that way.
//
// An outpost grows by need, as stations do in X4. Where the nearest market is far, a faction
// makes its outpost a market with a dock; where a market stands and enough passes through or
// is mined, it makes the market a station -- a trade station or a mining station -- up to a
// number of stations in each region. Each step is a plan weighed in the faction's own turn
// against everything else it could do, paid from its stock, and built: the outpost becomes the
// site of what it is growing into, holding the claim while it does, and anyone there can see
// it -- and shoot it.
//
// What an upgrade or an Anchor is built from comes by convoy, from where the faction makes
// things -- its holding within reach with the most stations of its own (#407) -- a system at
// a time, by the way the faction fears least, with as much escort as it can lend for what it
// expects to meet. Where nobody is it is arithmetic: crossing each system it is attacked or
// not, by the danger there and the security, and a fight takes freight and escort by how far
// the danger outweighs its strength (#407, the owner's decision of 2026-10-16) -- so it
// arrives whole or damaged, turns back, or is destroyed, and a convoy no player helps still
// gets through where its faction judged the way well. Where somebody is, the same arithmetic
// has a hauler and its escort to show for it, which a player can meet, escort or shoot down.
// Losing one delays a plan and costs the faction what was lost; it never cancels it.
#include "sim/Simulation.h"
#include "sim/WorldRules.h"

#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "entities/JumpGate.h"
#include "entities/NpcShip.h"
#include "entities/Station.h"
#include "entities/Structure.h"
#include "gen/Region.h"
#include "gen/Rng.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace
{
// What keys where a convoy's ships are, besides which plan, which leg and the second.
constexpr uint64_t CONVOY_KEY = 0xC0470Bu;
// The most escorts a convoy shows: a picture of the escort, not every ship of it.
constexpr int MAX_ESCORTS = 4;
// A holding with less than this to spare sends no escort: the convoy goes alone.
constexpr float MIN_ESCORT = 0.25f;
// Less than this of an escort coming home is not worth the journey: it was lost.
constexpr float MIN_RETURN = 0.05f;
// What keys whether a convoy is attacked crossing a system, and how hard (#407).
constexpr uint64_t FIGHT_KEY = 0xF16E7u;
// What a faction fears in a system it has never looked at, in ships, on a convoy's way.
constexpr float  UNSEEN_DANGER = 1.0f;
constexpr double HOUR = 3600.0;

bool Against(FactionId a, FactionId b)
{
    const Stance s = Factions::Relation(a, b);
    return a != b && (s == Stance::War || s == Stance::Hostile);
}

// Every system within `depth` jumps of `from`, by how many: out a jump at a time, in the order
// the links are listed, so the same galaxy counts the same way.
std::map<std::string, int> Within(const Simulation& sim, const std::string& from, int depth)
{
    std::map<std::string, int> found;
    if (!sim.HasSystem(from))
        return found;
    found[from] = 0;
    std::vector<std::string> ring{ from };
    for (int d = 1; d <= depth && !ring.empty(); d++)
    {
        std::vector<std::string> next;
        for (const std::string& id : ring)
            for (const std::string& n : sim.Neighbors(id))
                if (sim.HasSystem(n) && found.emplace(n, d).second)
                    next.push_back(n);
        ring = std::move(next);
    }
    return found;
}

// "an outpost market", "a trade station": a blueprint's name as the history says it.
std::string Called(const Blueprint& bp)
{
    std::string name = bp.name;
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    const bool vowel = !name.empty() && std::string("aeiou").find(name[0]) != std::string::npos;
    return (vowel ? "an " : "a ") + name;
}

Structure* StructureNumbered(SystemState& st, int id)
{
    for (const auto& e : st.entities)
        if (e->GetId() == id && e->GetKind() == EntityKind::Structure)
            return static_cast<Structure*>(e.get());
    return nullptr;
}

// How many stations of its own `f` has in a system (#407): where what it builds is made.
int StationsOf(const SystemState& st, FactionId f)
{
    int n = 0;
    for (const auto& e : st.entities)
        if (e->GetKind() == EntityKind::Station &&
            static_cast<const Station&>(*e).GetFaction() == f)
            n++;
    return n;
}

bool IsStationBlueprint(const Blueprint* bp)
{
    const Archetype* a = bp != nullptr ? Archetypes::Find(bp->archetype) : nullptr;
    return a != nullptr && a->kind == EntityKind::Station;
}
}  // namespace

// --- Expeditions --------------------------------------------------------------------------

void Simulation::SendWordOfMouth(int slot)
{
    const WorldRules::Expeditions& r = WorldRules::ExpeditionRules();
    const RegionOpening*           o = OpeningOf(slot);
    if (o == nullptr || o->mouth.empty() || r.hours <= 0.0)
        return;
    const std::map<std::string, int> near = Within(*this, o->mouth, r.reach);
    const std::string                entry = EntryOf(slot);
    const SystemState*               beyond = SystemById(entry);
    const SystemState*               mouth = SystemById(o->mouth);
    std::vector<std::string>         heard;
    for (FactionId f : Factions::All())
    {
        if (Factions::TemperamentOf(f).appetite <= 0.0f)
            continue;  // holds what it has and goes nowhere
        bool holds = false;
        for (const auto& kv : near)
        {
            const SystemAggregate& a = systems_.at(kv.first).agg;
            holds = holds || (a.claimed && a.controller == f);
        }
        if (!holds)
            continue;
        FactionMind& m = minds_[f];
        if (beyond != nullptr)
            m.intel[entry] = Observe(*beyond);
        if (mouth != nullptr)
            m.intel[o->mouth] = Observe(*mouth);
        m.expeditions[slot] = time_ + r.hours * HOUR;
        heard.push_back(FactionName(f));
    }
    if (heard.empty())
        return;
    std::string who;
    for (size_t i = 0; i < heard.size(); i++)
        who += (i == 0 ? "" : (i + 1 == heard.size() ? " and " : ", ")) + heard[i];
    Announce("expedition", std::nullopt, o->mouth,
             "Word of the mouth in " + SystemName(o->mouth) + " reaches " + who +
                 ": expeditions are fitting out for the space beyond");
}

Simulation::Drawn Simulation::DrawnBy(FactionId f)
{
    Drawn                  d;
    std::map<int, double>& heard = minds_[f].expeditions;
    for (auto it = heard.begin(); it != heard.end();)
    {
        const RegionOpening* o = OpeningOf(it->first);
        if (it->second <= time_ || o == nullptr || o->state == "drifted")
        {
            it = heard.erase(it);  // the word is old, or the way is gone
            continue;
        }
        d.regions.insert(it->first);
        for (const auto& kv : Within(*this, o->mouth, WorldRules::ExpeditionRules().reach))
        {
            const auto had = d.toward.find(kv.first);
            if (had == d.toward.end() || kv.second < had->second)
                d.toward[kv.first] = kv.second;
        }
        ++it;
    }
    return d;
}

float Simulation::DrawOf(const Drawn& d, const std::string& home, const std::string& to)
{
    if (d.regions.empty())
        return 0.0f;
    const float bonus = (float)WorldRules::ExpeditionRules().curiosity;
    if (d.regions.count(Gen::SlotOf(to)) != 0)
        return bonus;  // the space beyond, all of it
    const auto there = d.toward.find(to);
    if (there == d.toward.end())
        return 0.0f;
    const auto here = d.toward.find(home);
    return here == d.toward.end() || there->second < here->second ? bonus : 0.0f;
}

// --- Growing stations ---------------------------------------------------------------------

float Simulation::BlueprintCost(const std::string& blueprint)
{
    const Blueprint* bp = Blueprints::Find(blueprint);
    if (bp == nullptr || !bp->byFactions)
        return 0.0f;
    float cost = 0.0f;
    for (const auto& c : bp->cost)
        cost += (float)c.second;
    return cost;
}

int Simulation::StationsIn(int slot) const
{
    int n = 0;
    for (const auto& kv : systems_)
        if (Gen::SlotOf(kv.first) == slot)
            n += kv.second.profile.stations;
    for (const auto& kv : plans_)
        if (kv.second.kind == Plan::Kind::Upgrade && Gen::SlotOf(kv.second.target) == slot &&
            IsStationBlueprint(Blueprints::Find(kv.second.blueprint)))
            n++;
    return n;
}

int Simulation::MarketJumps(const std::string& systemId)
{
    // Counted afresh once a minute of world time, out from every market at once: no faction
    // turn walks the galaxy for itself.
    const int minute = (int)std::floor(time_ / 60.0);
    if (minute != marketPeriod_)
    {
        marketPeriod_ = minute;
        marketJumps_.clear();
        std::vector<std::string> ring;
        for (const auto& kv : systems_)
            if (kv.second.profile.markets > 0)
            {
                marketJumps_[kv.first] = 0;
                ring.push_back(kv.first);
            }
        for (int d = 1; d <= WorldRules::GrowthRules().marketReach && !ring.empty(); d++)
        {
            std::vector<std::string> next;
            for (const std::string& id : ring)
                for (const std::string& n : Neighbors(id))
                    if (systems_.count(n) != 0 && marketJumps_.emplace(n, d).second)
                        next.push_back(n);
            ring = std::move(next);
        }
    }
    const auto it = marketJumps_.find(systemId);
    return it == marketJumps_.end() ? -1 : it->second;
}

std::string Simulation::GrowthFor(FactionId f, const SystemState& st, const Structure& outpost,
                                  float& worth)
{
    worth = 0.0f;
    const WorldRules::Growth& g = WorldRules::GrowthRules();
    const Temperament&        t = Factions::TemperamentOf(f);
    const float               traffic = std::clamp(st.agg.prosperity, 0.0f, 1.0f);
    const float               ore = std::min(1.0f, st.profile.belts / 3.0f);
    if (!Outposts::IsMarket(outpost.GetBuilds()))
    {
        // A market where there is none near: the farther the nearest, the more it is needed,
        // and the more so where things pass through or are mined.
        if (st.profile.markets > 0 || BlueprintCost(Outposts::MARKET_BLUEPRINT) <= 0.0f)
            return std::string();
        const int   jumps = MarketJumps(st.id);
        const float far = jumps < 0 ? 1.0f : std::min(1.0f, (float)jumps / (float)g.marketReach);
        const float need = far * (0.5f + 0.5f * std::max(traffic, ore));
        worth = (float)g.marketWorth * need * std::min(1.0f, t.traffic + t.ore);
        return need > 0.0f ? Outposts::MARKET_BLUEPRINT : std::string();
    }
    // A station where a market is, and enough passes through or is mined to keep one: whichever
    // of the two the faction values more here -- and only while the region has room for one.
    const int stations = StationsIn(Gen::SlotOf(st.id));
    if (stations >= g.stationsPerRegion)
        return std::string();
    const float busy = std::max(traffic, ore);
    const float leaning = std::min(1.0f, t.traffic + t.ore);
    std::string grow;
    if (busy >= (float)g.stationNeed)
    {
        const bool        mining = t.ore * ore > t.traffic * traffic;
        const std::string bp =
            mining ? Outposts::MINING_STATION_BLUEPRINT : Outposts::TRADE_STATION_BLUEPRINT;
        if (BlueprintCost(bp) > 0.0f)
        {
            worth = (float)g.stationWorth * busy * leaning;
            grow = bp;
        }
    }
    // ...or a shipyard (#432), by need and never first: where a station already stands in the
    // region to bring pilots, none is within reach to refit them, and the region has no other
    // -- standing or going up, the faction's or anyone's.
    if (stations < g.shipyardAfter || busy < (float)g.shipyardNeed ||
        BlueprintCost(Outposts::SHIPYARD_BLUEPRINT) <= 0.0f || !ShipyardWanted(st.id))
        return grow;
    const float yard = (float)g.shipyardWorth * busy * leaning;
    if (yard > worth)
    {
        worth = yard;
        grow = Outposts::SHIPYARD_BLUEPRINT;
    }
    return grow;
}

bool Simulation::ShipyardWanted(const std::string& systemId) const
{
    const int slot = Gen::SlotOf(systemId);
    for (const auto& kv : plans_)
        if (kv.second.kind == Plan::Kind::Upgrade &&
            kv.second.blueprint == Outposts::SHIPYARD_BLUEPRINT &&
            Gen::SlotOf(kv.second.target) == slot)
            return false;
    for (const auto& kv : systems_)
        if (Gen::SlotOf(kv.first) == slot && kv.second.profile.shipyards > 0)
            return false;
    for (const auto& kv : Within(*this, systemId, WorldRules::GrowthRules().shipyardReach))
    {
        const SystemState* near = SystemById(kv.first);
        if (near != nullptr && near->profile.shipyards > 0)
            return false;
    }
    return true;
}

int Simulation::BeginUpgrade(FactionId f, const std::string& systemId, const std::string& blueprint)
{
    SystemState*     st = SystemById(systemId);
    const Blueprint* bp = Blueprints::Find(blueprint);
    const float      cost = BlueprintCost(blueprint);
    FactionMind&     mind = minds_[f];
    if (st == nullptr || bp == nullptr || bp->onto.empty() || cost <= 0.0f || mind.stock < cost)
        return 0;
    // Its own finished outpost there, of a kind this is built onto -- and nothing growing there
    // already.
    for (const auto& kv : plans_)
        if (kv.second.kind == Plan::Kind::Upgrade && kv.second.target == systemId)
            return 0;
    Structure* onto = nullptr;
    for (size_t i = 0; i < st->profile.outpostIds.size(); i++)
    {
        Structure*       t = st->profile.outposts[i] == f
                                 ? StructureNumbered(*st, st->profile.outpostIds[i])
                                 : nullptr;
        const Blueprint* was = t != nullptr ? t->GetBlueprint() : nullptr;
        if (t != nullptr && !t->IsBuilding() && was != nullptr &&
            std::find(bp->onto.begin(), bp->onto.end(), was->id) != bp->onto.end())
            onto = t;
    }
    if (onto == nullptr)
        return 0;
    if (IsStationBlueprint(bp) &&
        StationsIn(Gen::SlotOf(systemId)) >= WorldRules::GrowthRules().stationsPerRegion)
        return 0;
    mind.stock -= cost;

    Plan p;
    p.id = nextPlanId_++;
    p.kind = Plan::Kind::Upgrade;
    p.faction = f;
    p.target = systemId;
    p.site = StaticKey(systemId, onto->GetId());
    p.blueprint = blueprint;
    p.startedAt = time_;
    p.legAt = time_;
    Plan& kept = plans_[p.id] = p;
    Record("upgrade", f, systemId,
           FactionName(f) + " is making its " + Outposts::Noun(onto->GetBuilds()) + " in " +
               SystemName(systemId) + " " + Called(*bp));
    DispatchConvoy(kept);
    return p.id;
}

int Simulation::BeginAnchor(FactionId f, int slot)
{
    RegionOpening* o = LiveOpening(slot);
    const float    cost = AnchorCost();
    FactionMind&   mind = minds_[f];
    if (o == nullptr || o->state != "unstable" || cost <= 0.0f || mind.stock < cost ||
        !AnchorAt(*o, false).empty() || !HasSystem(o->mouth))
        return 0;
    for (const auto& kv : plans_)
        if (kv.second.kind == Plan::Kind::Anchor && kv.second.faction == f &&
            kv.second.target == o->mouth)
            return 0;
    mind.stock -= cost;

    Plan p;
    p.id = nextPlanId_++;
    p.kind = Plan::Kind::Anchor;
    p.faction = f;
    p.target = o->mouth;
    p.slot = slot;
    p.startedAt = time_;
    p.legAt = time_;
    Plan& kept = plans_[p.id] = p;
    Announce("anchor", f, o->mouth,
             FactionName(f) + " sent for an Anchor to hold the mouth in " + SystemName(o->mouth));
    DispatchConvoy(kept);
    return p.id;
}

bool Simulation::StartUpgrade(Plan& p)
{
    SystemState*     st = SystemById(p.target);
    Structure*       t = st != nullptr ? StructureByKey(*st, p.site) : nullptr;
    const Blueprint* bp = Blueprints::Find(p.blueprint);
    const Blueprint* was = t != nullptr ? t->GetBlueprint() : nullptr;
    if (t == nullptr || bp == nullptr || was == nullptr || t->IsBuilding() ||
        t->GetOwner() != Outposts::OwnerOf(p.faction) ||
        std::find(bp->onto.begin(), bp->onto.end(), was->id) == bp->onto.end())
    {
        // What it brought is put by: the stock is paid back.
        minds_[p.faction].stock += BlueprintCost(p.blueprint);
        Record("upgrade", p.faction, p.target,
               FactionName(p.faction) + "'s convoy came to " + SystemName(p.target) +
                   " to find nothing to build on");
        return false;
    }
    // The outpost is the site of what it grows into: as far along as a new site, as hurt as it
    // was, by the share of its hull.
    const float hurt = t->MaxHull(time_) > 0.0f ? t->GetDamage(time_) / t->MaxHull(time_) : 0.0f;
    const std::string what = Outposts::Noun(t->GetBuilds());
    t->Retask(bp->archetype, bp->id, FactionName(p.faction) + " " + bp->name);
    t->StartBuilding(time_, time_ + bp->buildSeconds);
    t->SetDamage(hurt * t->MaxHull(time_), time_);
    MarkStaticChanged(*st, t->GetId());
    st->profile = ProfileOf(*st);
    ScheduleStructure(st->id, *t);
    p.dueAt = time_ + bp->buildSeconds;
    due_.insert({ p.dueAt, p.id });
    const long long minutes = (long long)std::lround(bp->buildSeconds / 60.0);
    Announce("upgrade", p.faction, p.target,
             FactionName(p.faction) + " began making its " + what + " in " + SystemName(p.target) +
                 " " + Called(*bp) + ": done in " + std::to_string(minutes) + " min");
    return true;
}

void Simulation::ResolveUpgrade(const Plan& p)
{
    SystemState* st = SystemById(p.target);
    if (st == nullptr)
        return;
    const Structure* t = StructureByKey(*st, p.site);
    if (t == nullptr)
        return;  // a station now (FinishStation said so), or lost (HitStructure did)
    if (t->IsBuilding())
    {
        Plan later = p;
        later.dueAt = std::max(t->GetCompletesAt(), time_);
        plans_[later.id] = later;
        due_.insert({ later.dueAt, later.id });
        return;
    }
    if (Outposts::IsMarket(t->GetBuilds()))
        Announce("upgrade", p.faction, p.target,
                 FactionName(p.faction) + " opened a market at its outpost in " +
                     SystemName(p.target) + ": ships dock and trade there");
}

void Simulation::FinishStation(SystemState& st, int id)
{
    Structure*       t = StructureNumbered(st, id);
    const Archetype* a = t != nullptr ? Archetypes::Find(t->GetBuilds()) : nullptr;
    const Blueprint* bp = t != nullptr ? t->GetBlueprint() : nullptr;
    FactionId        f;
    // A player's stays the structure it is: a station belongs to a faction.
    if (a == nullptr || bp == nullptr || !Outposts::FactionOf(t->GetOwner(), f))
        return;
    StationRole role = StationRole::TradeHub;
    ParseStationRole(a->worldSubType, role);
    const Vector2              at = t->GetPosition();
    const std::optional<Orbit> orbit = t->GetOrbit();  // an orbital outpost's, still turning
    const std::string          owner = t->GetOwner();
    const std::string          name = FactionName(f) + " " + bp->name;
    if (!RemoveStatic(st.id, id))
        return;
    auto station = std::make_unique<Station>(at, a->defaultSize, name, f, role);
    if (orbit)
        station->SetOrbit(*orbit);
    AddStatic(st.id, std::move(station), owner);
    Announce("station", f, st.id,
             FactionName(f) + " opened " + Called(*bp) + " in " + SystemName(st.id) +
                 ", grown from its outpost market");
}

// --- The builder convoy -------------------------------------------------------------------

float Simulation::FreightPrice(const Plan& p)
{
    return p.kind == Plan::Kind::Anchor ? AnchorCost() : BlueprintCost(p.blueprint);
}

float Simulation::AmbushChance(float danger, float security)
{
    if (danger <= 0.0f)
        return 0.0f;
    const WorldRules::Convoys& r = WorldRules::ConvoyRules();
    return danger / (danger + (float)r.calm) *
           (1.0f - (float)r.policed * std::clamp(security, 0.0f, 1.0f));
}

float Simulation::AmbushHarm(float strength, float danger)
{
    if (danger <= 0.0f)
        return 0.0f;
    // The share of the fight that is the attackers': all of it against nothing, half against
    // as many -- and what that takes off the freight grows as its square, so an escort that
    // matches the danger saves most of what it carries.
    const float weak = danger / (danger + std::max(0.0f, strength));
    return (float)WorldRules::ConvoyRules().harm * weak * weak;
}

float Simulation::ConvoyDanger(FactionId f, const std::string& systemId, bool truth,
                               float* security) const
{
    const SystemState* st = SystemById(systemId);
    if (st == nullptr)
        return 0.0f;
    float danger = 0.0f;
    // What it holds, or has ships in, it sees for itself: never out of date (Think).
    const bool sees = (st->agg.claimed && st->agg.controller == f) || st->agg.presence[f] >= 1.0f;
    if (truth || sees)
    {
        for (FactionId g : Factions::All())
            if (Against(f, g))
                danger += st->agg.presence[g];
        if (security != nullptr)
            *security = st->agg.security;
        if (truth)
            return danger;
    }
    // What it believes: what it saw there, or -- where it has never looked -- a ship's worth of
    // whatever might be there, at middling security; and where it lost something, the memory
    // of that.
    const FactionMind& mind = minds_[f];
    if (!sees)
    {
        const auto seen = mind.intel.find(systemId);
        if (seen == mind.intel.end())
            danger = UNSEEN_DANGER;
        else
            for (FactionId g : Factions::All())
                if (Against(f, g))
                    danger += seen->second.presence[g];
        if (security != nullptr)
            *security = seen == mind.intel.end() ? 0.5f : seen->second.security;
    }
    return danger + Intelligence::LossRisk(mind.lostAt, systemId, time_);
}

Simulation::ConvoyChart Simulation::ChartConvoy(FactionId f, const std::string& target,
                                                float pilots, int waits) const
{
    ConvoyChart                c;
    const WorldRules::Convoys& r = WorldRules::ConvoyRules();
    if (!HasSystem(target))
        return c;
    // Out from the site a jump at a time. What a site is built from is made where the faction
    // has stations of its own (#407) -- hubs, mining posts, stations grown from outpost
    // markets -- not at an outpost, which holds a claim and makes nothing; and most is made
    // where most of them are. Of its holdings within the convoy rules' reach, the one with the
    // most stations of its own -- the nearest of those, the lowest id among the nearest -- is
    // where the convoy sets out from: a civilisation's heart, for the frontier at its door, so
    // that frontier is built from the space behind it and its convoys come out through it.
    // With no station within reach, the nearest holding of any kind.
    std::set<std::string>    within{ target };
    std::vector<std::string> ring{ target };
    std::string              home, held;  // where most is made; the nearest holding of any kind
    int                      homeAt = 0, heldAt = 0, most = 0;
    for (int d = 1; d <= r.reach && !ring.empty(); d++)
    {
        std::vector<std::string> next;
        for (const std::string& id : ring)
            for (const std::string& n : Neighbors(id))
            {
                const auto it = systems_.find(n);
                if (it == systems_.end() || !within.insert(n).second)
                    continue;
                next.push_back(n);
                if (!it->second.agg.claimed || it->second.agg.controller != f)
                    continue;
                if (held.empty() || (d == heldAt && n < held))
                {
                    held = n;
                    heldAt = d;
                }
                const int k = StationsOf(it->second, f);
                if (k > most || (k > 0 && k == most && d == homeAt && n < home))
                {
                    most = k;
                    home = n;
                    homeAt = d;
                }
            }
        ring = std::move(next);
    }
    if (home.empty())
        home = held;
    if (home.empty())
        c.route = { target };  // nothing of its own nearer: from the site itself
    else
    {
        // The way it fears least, among the systems within reach of the site: a jump for every
        // system entered, and `avoid` more for every ship it believes waits there (#407). Ties
        // go to the lower id, so the same galaxy is crossed the same way.
        std::map<std::string, float>            cost{ { home, 0.0f } };
        std::map<std::string, std::string>      back;
        std::set<std::pair<float, std::string>> open{ { 0.0f, home } };
        while (!open.empty())
        {
            const std::pair<float, std::string> at = *open.begin();
            open.erase(open.begin());
            if (at.second == target)
                break;
            for (const std::string& n : Neighbors(at.second))
            {
                if (within.count(n) == 0)
                    continue;
                const float step = 1.0f + (float)r.avoid * ConvoyDanger(f, n, false);
                const auto  had = cost.find(n);
                if (had != cost.end() && had->second <= at.first + step)
                    continue;
                if (had != cost.end())
                    open.erase({ had->second, n });
                cost[n] = at.first + step;
                back[n] = at.second;
                open.insert({ at.first + step, n });
            }
        }
        std::vector<std::string> way{ target };
        while (way.back() != home && back.count(way.back()) != 0)
            way.push_back(back[way.back()]);
        if (way.back() != home)
            way = { target };  // within reach of the site, so never: from the site, then
        c.route.assign(way.rbegin(), way.rend());
    }

    // The escort it asks for: enough for the worst it believes it will meet, more for every
    // time one was held back or turned back -- and what it gets is what its holding can spare.
    for (const std::string& id : c.route)
        c.worst = std::max(c.worst, ConvoyDanger(f, id, false));
    const Temperament& t = Factions::TemperamentOf(f);
    const float want = std::clamp((float)r.escortPerDanger * c.worst * (1.0f + 0.5f * (float)waits),
                                  (float)r.escort * t.capacity, (float)r.escortMax * t.capacity);
    const SystemState* from = SystemById(c.route.front());
    if (from != nullptr && from->agg.claimed && from->agg.controller == f)
    {
        const float spare = std::min(from->agg.presence[f] - t.capacity * HOME_GUARD, want);
        c.escort = spare >= MIN_ESCORT ? spare : 0.0f;
    }
    // What it expects to lose on the way, system by system, as it believes them to be.
    for (size_t i = 0; i < c.route.size(); i++)
    {
        float       security = 0.0f;
        const float danger = ConvoyDanger(f, c.route[i], false, &security);
        const float strength = (float)r.hauler + c.escort + (i == 0 ? pilots : 0.0f);
        c.loss += AmbushChance(danger, security) * AmbushHarm(strength, danger);
    }
    return c;
}

bool Simulation::DispatchConvoy(Plan& p)
{
    if (!HasSystem(p.target))
        return false;
    const WorldRules::Convoys& r = WorldRules::ConvoyRules();
    FactionMind&               mind = minds_[p.faction];
    const double               retry = r.retryMinutes * 60.0;
    // Pilots already waiting with it where it would set out stand in for escort it lacks.
    ConvoyChart c = ChartConvoy(p.faction, p.target, 0.0f, p.waits);
    if (const float pilots = EscortsWith(p, c.route.front()); pilots > 0.0f)
        c = ChartConvoy(p.faction, p.target, pilots, p.waits);
    p.route = c.route;
    p.from = p.route.front();
    p.leg = -1;
    p.force = 0.0f;
    p.lent.clear();
    p.guard = 0.0f;
    const auto hold = [&](const std::string& why)
    {
        p.legAt = time_ + retry;
        p.dueAt = p.legAt + r.hop * (double)p.route.size();
        TallyConvoy(p, &ConvoyTally::held);
        Record("convoy", p.faction, p.from,
               FactionName(p.faction) + " held back its convoy for " + SystemName(p.target) +
                   " in " + SystemName(p.from) + ": " + why);
        return true;
    };
    // What the last one lost has to be made again before another can carry it.
    const float refill = std::max(0.0f, 1.0f - p.freight) * FreightPrice(p);
    if (mind.stock < refill)
        return hold("it cannot yet make good what the last one lost");
    // Value against risk (#407): while it expects to lose too much of what it sends, it waits
    // for a stronger escort, pilots to fly with it, or the way to clear -- but not for ever.
    if (p.waits < r.maxWaits && c.loss > (float)r.wait)
    {
        p.waits++;
        return hold("the way is too dangerous for the escort it can send");
    }
    mind.stock -= refill;
    p.freight = 1.0f;
    p.leg = 0;
    p.legAt = time_;
    p.escorted = false;
    p.dueAt = time_ + r.hop * (double)p.route.size();  // when it would arrive, if nothing stops it
    // Its escort is lent by the holding it sets out from, as a siege's is: that much short
    // until it is home again.
    if (c.escort > 0.0f)
    {
        systems_.at(p.from).agg.presence[p.faction] -= c.escort;
        p.force = c.escort;
        p.lent.push_back({ p.from, c.escort, std::max(1, (int)p.route.size() - 1) });
    }
    char escort[48];
    std::snprintf(escort, sizeof escort, "%.0f ships of escort",
                  std::max(1.0, std::round((double)p.force)));
    Record("convoy", p.faction, p.target,
           FactionName(p.faction) + " sent a convoy from " + SystemName(p.from) + " to " +
               SystemName(p.target) + ", " + (p.force > 0.0f ? escort : "unescorted"));
    TallyConvoy(p, &ConvoyTally::sent);
    if (IsHot(systems_.at(p.from)))
        FieldConvoy(systems_.at(p.from), p, false);
    return true;
}

void Simulation::SendHome(const Plan& p, int hops)
{
    if (p.force < MIN_RETURN || p.lent.empty() || !HasSystem(p.lent.front().from))
        return;
    Plan r;
    r.id = nextPlanId_++;
    r.kind = Plan::Kind::Return;
    r.faction = p.faction;
    r.from = p.route.empty() || p.leg < 0 ? p.target : p.route[(size_t)p.leg];
    r.target = p.lent.front().from;
    r.startedAt = time_;
    r.dueAt = time_ + std::max(1, hops) * WorldRules::ConvoyRules().hop;
    r.force = p.force;
    plans_[r.id] = r;
    due_.insert({ r.dueAt, r.id });
}

void Simulation::TallyConvoy(const Plan& p, long long ConvoyTally::* what)
{
    convoyTally_.*what += 1;
    convoyRegions_[Gen::SlotOf(p.target)].*what += 1;
}

void Simulation::LoseConvoy(Plan& p, const std::string& how)
{
    const std::string where = p.route.empty() || p.leg < 0 ? p.target : p.route[(size_t)p.leg];
    UnfieldConvoy(p);
    SendHome(p, std::max(1, p.leg));
    minds_[p.faction].lostAt[where] = time_;  // it thinks twice before sending one that way
    const double retry = WorldRules::ConvoyRules().retryMinutes * 60.0;
    Announce("convoy", p.faction, where,
             FactionName(p.faction) + " lost its convoy bound for " + SystemName(p.target) +
                 " in " + SystemName(where) + ": " + how + "; another sets out in " +
                 std::to_string((long long)std::lround(retry / 60.0)) + " min");
    TallyConvoy(p, &ConvoyTally::destroyed);
    p.leg = -1;
    p.legAt = time_ + retry;
    p.route.clear();
    p.force = 0.0f;
    p.lent.clear();
    p.freight = 0.0f;  // all of it, to be made again
    p.guard = 0.0f;
    p.setbacks++;
    p.waits++;
}

void Simulation::TurnBackConvoy(Plan& p, const std::string& where, const std::string& why)
{
    const std::string at = p.ConvoyAt();
    UnfieldConvoy(p);
    SendHome(p, std::max(1, p.leg));
    minds_[p.faction].lostAt[where] = time_;
    const double retry = WorldRules::ConvoyRules().retryMinutes * 60.0;
    char         left[96];
    std::snprintf(left, sizeof left,
                  "; %.0f%% of its freight goes home, another sets out in %lld min",
                  100.0 * std::max(0.0f, p.freight), (long long)std::lround(retry / 60.0));
    Record("convoy", p.faction, at,
           FactionName(p.faction) + "'s convoy bound for " + SystemName(p.target) +
               " turned back in " + SystemName(at) + ": " + why + left);
    TallyConvoy(p, &ConvoyTally::turnedBack);
    p.leg = -1;
    p.legAt = time_ + retry;
    p.route.clear();
    p.force = 0.0f;
    p.lent.clear();
    p.guard = 0.0f;
    p.setbacks++;
    p.waits++;
}

bool Simulation::ArriveConvoy(Plan& p)
{
    UnfieldConvoy(p);
    SendHome(p, std::max(1, (int)p.route.size() - 1));
    TallyConvoy(p, p.freight >= 0.999f ? &ConvoyTally::intact : &ConvoyTally::damaged);
    if (p.escorted)
        TallyConvoy(p, &ConvoyTally::escorted);
    p.arrived = true;
    p.force = 0.0f;
    p.lent.clear();
    p.route.clear();
    p.leg = -1;
    const bool begun = p.kind == Plan::Kind::Anchor ? LayAnchorSite(p) : StartUpgrade(p);
    // What it lost on the way is made up from the faction's stock, as far as that goes.
    if (begun && p.freight < 1.0f)
    {
        float& stock = minds_[p.faction].stock;
        stock = std::max(0.0f, stock - (1.0f - p.freight) * FreightPrice(p));
    }
    return begun;  // `freight` stays what it came with: the record of how it went
}

bool Simulation::CrossSystem(Plan& p, SystemState& st)
{
    const WorldRules::Convoys& r = WorldRules::ConvoyRules();
    float                      security = 0.0f;
    const float                danger = ConvoyDanger(p.faction, st.id, true, &security);
    if (danger <= 0.0f)
        return true;
    // Whether it is attacked, and how hard the fight goes, are drawn from which convoy and
    // where on its way -- never from how many draws came before -- and from the numbers alone,
    // the same where somebody is to see it as where nobody is (#295). Pilots with it are
    // escort strength the sums do not wear down: what hits them is real.
    Gen::Rng    rng(Gen::Key(FIGHT_KEY, (uint64_t)p.id, (uint64_t)p.setbacks, (uint64_t)p.leg));
    const float strength = (float)r.hauler + p.force + p.guard;
    if ((float)rng.Unit() >= AmbushChance(danger, security))
        return true;  // it slipped through
    const float weak = danger / (danger + strength);
    const float hard = 0.5f + (float)rng.Unit();
    const float taken = AmbushHarm(strength, danger) * hard;
    const float escortLost = std::min(p.force, (float)r.bite * danger * weak * hard);
    const float dealt = std::min(danger, (float)r.bite * strength * (1.0f - weak) * hard);
    FactionId   worst = Factions::Pirates();
    float       most = -1.0f;
    for (FactionId g : Factions::All())
        if (Against(p.faction, g) && st.agg.presence[g] > 0.0f)
        {
            float& there = st.agg.presence[g];
            if (there > most)
            {
                most = there;
                worst = g;
            }
            there = std::max(0.0f, there - dealt * there / danger);
        }
    p.force -= escortLost;
    p.freight -= taken;
    const std::string who =
        worst == Factions::Pirates() ? std::string("the pirates") : FactionName(worst) + " ships";
    if (p.freight <= 0.0f)
    {
        LoseConvoy(p, who + " took it");
        return false;
    }
    const bool last = (size_t)p.leg + 1 >= p.route.size();
    if (!last && p.freight < (float)r.turnBack)
    {
        TurnBackConvoy(p, st.id, "mauled by " + who);
        return false;
    }
    char lost[48];
    std::snprintf(lost, sizeof lost, "%.0f%% of its freight", 100.0 * std::min(1.0f, taken));
    Record("convoy", p.faction, st.id,
           FactionName(p.faction) + "'s convoy bound for " + SystemName(p.target) + " fought off " +
               who + " in " + SystemName(st.id) + ", losing " + lost);
    return true;
}

void Simulation::StepConvoys()
{
    const WorldRules::Convoys& r = WorldRules::ConvoyRules();
    std::vector<int>           over;
    for (auto& kv : plans_)
    {
        Plan& p = kv.second;
        if (!p.OnTheWay())
            continue;
        if (p.leg < 0)
        {
            // Waiting to set out: the first, or the next after one did not get through.
            if (time_ >= p.legAt && !DispatchConvoy(p))
                over.push_back(p.id);
            continue;
        }
        SystemState* st = SystemById(p.route[(size_t)p.leg]);
        if (st == nullptr)
        {
            LoseConvoy(p, "its way was cut");
            continue;
        }

        // Where it is real, what a player shot down is gone: the one thing the arithmetic below
        // does not know about.
        if (IsHot(*st))
        {
            FieldConvoy(*st, p, false);
            int  alive = 0;
            bool hauler = false;
            for (const auto& e : st->entities)
            {
                if (e->GetKind() != EntityKind::Npc)
                    continue;
                const NpcShip& n = static_cast<const NpcShip&>(*e);
                if (n.GetTask() != p.id || !n.IsAlive())
                    continue;
                if (n.GetId() == p.hauler)
                    hauler = true;
                else
                    alive++;
            }
            if (alive < p.fielded)
                p.force = std::max(0.0f, p.force - (float)(p.fielded - alive));
            p.fielded = alive;
            if (p.hauler != 0 && !hauler)
            {
                LoseConvoy(p, "its hauler was shot down");
                continue;
            }
        }
        // Pilots flying with it on its escort jobs (#345) are escort strength wherever they were
        // with it while it crossed the system.
        const float pilots = EscortsWith(p, st->id);
        p.guard = std::max(p.guard, pilots);
        p.escorted = p.escorted || pilots > 0.0f;

        if (time_ < p.legAt + r.hop)
            continue;
        // Across: whatever is hostile here falls on it, or does not.
        if (!CrossSystem(p, *st))
            continue;
        if ((size_t)p.leg + 1 >= p.route.size())
        {
            if (!ArriveConvoy(p))
                over.push_back(p.id);
            continue;
        }
        // On to the next system, if the way is still there -- and if what waits there is not
        // more than it could hope to get through, unless it has waited long enough.
        const std::string next = p.route[(size_t)p.leg + 1];
        const auto&       links = Neighbors(p.route[(size_t)p.leg]);
        if (!HasSystem(next) || std::find(links.begin(), links.end(), next) == links.end())
        {
            TurnBackConvoy(p, st->id, "its way was cut");
            continue;
        }
        const float ahead = ConvoyDanger(p.faction, next, true);
        if (p.waits < r.maxWaits && ahead > (float)r.balk * ((float)r.hauler + p.force + pilots))
        {
            TurnBackConvoy(p, next, SystemName(next) + " ahead is held against it");
            continue;
        }
        UnfieldConvoy(p);
        p.leg++;
        p.legAt = time_;
        p.guard = 0.0f;
        SystemState* there = SystemById(next);
        if (IsHot(*there))
            FieldConvoy(*there, p, true);
    }
    for (int id : over)
        plans_.erase(id);  // in no due entry: a convoy is stepped, not due
}

void Simulation::FieldConvoys(SystemState& st)
{
    for (auto& kv : plans_)
        if (kv.second.OnTheWay() && kv.second.leg >= 0 &&
            kv.second.route[(size_t)kv.second.leg] == st.id)
            FieldConvoy(st, kv.second, false);
}

void Simulation::FieldConvoy(SystemState& st, Plan& p, bool arriving)
{
    if (!IsHot(st) || p.leg < 0 || p.route[(size_t)p.leg] != st.id || p.fieldedIn == st.id)
        return;
    const size_t leg = (size_t)p.leg;
    // Where it came in -- the gate from the last system, or beside the holding it set out from
    // -- and where it goes out: the gate to the next, or the site.
    const Vector2 in = leg > 0 ? ArrivalFrom(st.id, p.route[leg - 1]) : SafeArrival(st.id);
    Vector2       out = in;
    if (leg + 1 < p.route.size())
    {
        for (const auto& e : st.entities)
            if (e->GetKind() == EntityKind::Gate &&
                static_cast<const JumpGate&>(*e).GetDestination() == p.route[leg + 1])
                out = e->GetPosition();
    }
    else if (const Structure* site = StructureByKey(st, p.site))
        out = site->GetPosition();
    else if (p.kind == Plan::Kind::Anchor)
    {
        float size = 0.0f;
        CrossingGateAt(st.id, EntryOf(p.slot), out, size);
    }
    // Found part way along its leg, it is that far along -- an instant on the clock, like
    // everything that moves, so two players arriving together see the same thing.
    const float   span = (float)WorldRules::ConvoyRules().hop;
    const float   done = arriving ? 0.0f : std::clamp((float)(time_ - p.legAt) / span, 0.0f, 1.0f);
    const Vector2 at{ in.x + (out.x - in.x) * done, in.y + (out.y - in.y) * done };
    Gen::Rng      rng(Gen::Key(CONVOY_KEY, (uint64_t)p.id, (uint64_t)leg,
                               (uint64_t)std::max(0.0, std::floor(time_))));
    auto          around = [&](float r0, float r1) -> Vector2
    {
        const float turn = (float)(rng.Unit() * 2.0 * PI);
        const float d = (float)rng.Between((double)r0, (double)r1);
        return Vector2{ at.x + d * std::cos(turn), at.y + d * std::sin(turn) };
    };
    SpawnNpcInto(st, around(0.0f, 120.0f), p.faction, NpcRole::Trader, { out }, 0);
    static_cast<NpcShip&>(*st.entities.back()).SetTask(p.id);
    p.hauler = st.entities.back()->GetId();
    const int n = std::min(MAX_ESCORTS, (int)std::lround(p.force));
    for (int i = 0; i < n; i++)
    {
        SpawnNpcInto(st, around(200.0f, 450.0f), p.faction, NpcRole::Warship, { out }, i);
        static_cast<NpcShip&>(*st.entities.back()).SetTask(p.id);
    }
    p.fielded = n;
    p.fieldedIn = st.id;
}

void Simulation::UnfieldConvoy(Plan& p)
{
    if (SystemState* st = p.fieldedIn.empty() ? nullptr : SystemById(p.fieldedIn))
        st->entities.erase(std::remove_if(st->entities.begin(), st->entities.end(),
                                          [&](const std::unique_ptr<Entity>& e)
                                          {
                                              return e->GetKind() == EntityKind::Npc &&
                                                     static_cast<const NpcShip&>(*e).GetTask() ==
                                                         p.id;
                                          }),
                           st->entities.end());
    p.fielded = 0;
    p.fieldedIn.clear();
    p.hauler = 0;
}
