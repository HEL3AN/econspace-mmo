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
// a time. Where nobody is it is arithmetic: in each system on the way it fights whatever is
// hostile there, escort first and then the hauler. Where somebody is, the
// same arithmetic has a hauler and its escort to show for it, which a player can meet, escort
// or shoot down -- the hauler shot down, or worn down by the arithmetic, the convoy is lost
// and the plan waits for the next. Losing one delays a plan; it never cancels it.
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
#include <string>
#include <vector>

namespace
{
// The coarse pass a convoy is stepped by: MaintainWorld's two seconds.
constexpr float PASS_SECONDS = 2.0f;
// What keys where a convoy's ships are, besides which plan, which leg and the second.
constexpr uint64_t CONVOY_KEY = 0xC0470Bu;
// The most escorts a convoy shows: a picture of the escort, not every ship of it.
constexpr int MAX_ESCORTS = 4;
// A holding with less than this to spare sends no escort: the convoy goes alone.
constexpr float MIN_ESCORT = 0.25f;
// Less than this of an escort coming home is not worth the journey: it was lost.
constexpr float  MIN_RETURN = 0.05f;
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
    if (std::max(traffic, ore) < (float)g.stationNeed ||
        StationsIn(Gen::SlotOf(st.id)) >= g.stationsPerRegion)
        return std::string();
    const bool        mining = t.ore * ore > t.traffic * traffic;
    const std::string bp =
        mining ? Outposts::MINING_STATION_BLUEPRINT : Outposts::TRADE_STATION_BLUEPRINT;
    if (BlueprintCost(bp) <= 0.0f)
        return std::string();
    worth = (float)g.stationWorth * std::max(traffic, ore) * std::min(1.0f, t.traffic + t.ore);
    return bp;
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
    const float       hurt = t->MaxHull(time_) > 0.0f ? t->GetDamage() / t->MaxHull(time_) : 0.0f;
    const std::string what = Outposts::Noun(t->GetBuilds());
    t->Retask(bp->archetype, bp->id, FactionName(p.faction) + " " + bp->name);
    t->StartBuilding(time_, time_ + bp->buildSeconds);
    t->SetDamage(hurt * t->MaxHull(time_));
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

std::vector<std::string> Simulation::ConvoyRoute(FactionId f, const std::string& target) const
{
    // Out from the site a jump at a time, remembering the way back to it. What a site is built
    // from is made where the faction has stations of its own (#407) -- hubs, mining posts,
    // stations grown from outpost markets -- not at an outpost, which holds a claim and makes
    // nothing; and most is made where most of them are. Of its holdings within the convoy
    // rules' reach, the one with the most stations of its own -- the nearest of those, the
    // lowest id among the nearest -- is where the convoy sets out from: a civilisation's
    // heart, for the frontier at its door, so that frontier is built from the space behind it
    // and its convoys come out through it. With no station within reach, the nearest holding
    // of any kind, as before.
    std::map<std::string, std::string> toward;  // id -> the next system on the way to target
    std::vector<std::string>           ring{ target };
    toward[target] = std::string();
    std::string home, held;  // where most is made; the nearest holding of any kind
    int         homeAt = 0, heldAt = 0, most = 0;
    for (int d = 1; d <= WorldRules::ConvoyRules().reach && !ring.empty(); d++)
    {
        std::vector<std::string> next;
        for (const std::string& id : ring)
            for (const std::string& n : Neighbors(id))
            {
                const auto it = systems_.find(n);
                if (it == systems_.end() || !toward.emplace(n, id).second)
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
        return { target };  // nothing of its own nearer: from the site itself
    std::vector<std::string> route{ home };
    while (route.back() != target)
        route.push_back(toward[route.back()]);
    return route;
}

bool Simulation::DispatchConvoy(Plan& p)
{
    if (!HasSystem(p.target))
        return false;
    const WorldRules::Convoys& r = WorldRules::ConvoyRules();
    const Temperament&         t = Factions::TemperamentOf(p.faction);
    p.route = ConvoyRoute(p.faction, p.target);
    p.from = p.route.front();
    p.leg = 0;
    p.legAt = time_;
    p.freight = 1.0f;
    p.force = 0.0f;
    p.lent.clear();
    p.dueAt = time_ + r.hop * (double)p.route.size();  // when it would arrive, if nothing stops it
    // Its escort is lent by the holding it sets out from, as a siege's is: that much short
    // until it is home again.
    SystemAggregate& home = systems_.at(p.from).agg;
    if (home.claimed && home.controller == p.faction)
    {
        const float spare = std::min(home.presence[p.faction] - t.capacity * HOME_GUARD,
                                     t.capacity * (float)r.escort);
        if (spare >= MIN_ESCORT)
        {
            home.presence[p.faction] -= spare;
            p.force = spare;
            p.lent.push_back({ p.from, spare, std::max(1, (int)p.route.size() - 1) });
        }
    }
    char escort[48];
    std::snprintf(escort, sizeof escort, "%.0f ships of escort",
                  std::max(1.0, std::round((double)p.force)));
    Record("convoy", p.faction, p.target,
           FactionName(p.faction) + " sent a convoy from " + SystemName(p.from) + " to " +
               SystemName(p.target) + ", " + (p.force > 0.0f ? escort : "unescorted"));
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
    p.leg = -1;
    p.legAt = time_ + retry;
    p.route.clear();
    p.force = 0.0f;
    p.lent.clear();
    p.freight = 1.0f;
    p.setbacks++;
}

bool Simulation::ArriveConvoy(Plan& p)
{
    UnfieldConvoy(p);
    SendHome(p, std::max(1, (int)p.route.size() - 1));
    p.arrived = true;
    p.force = 0.0f;
    p.lent.clear();
    p.route.clear();
    p.leg = -1;
    return p.kind == Plan::Kind::Anchor ? LayAnchorSite(p) : StartUpgrade(p);
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
            // Waiting to set out: the first, or the next after one was lost.
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

        // Whatever is hostile here falls on it: the escort first, then the hauler -- and the
        // escort takes its toll of them as it goes.
        // Pilots flying with it on its escort jobs (#345) are escort strength too: they fire
        // back with the rest and stand between it and the first of the blows, each as many
        // ships as the jobs rules say, and are not worn down by the sums -- where they are, the
        // system is hot, and what hits them, or the hauler, is real.
        float hostile = 0.0f;
        for (FactionId g : Factions::All())
            if (Against(p.faction, g))
                hostile += st->agg.presence[g];
        if (hostile > 0.0f)
        {
            const float pilots = EscortsWith(p, st->id);
            float       blow = (float)r.ambush * hostile * PASS_SECONDS;
            const float back = (float)r.ambush * (p.force + pilots) * PASS_SECONDS;
            blow -= std::min(blow, pilots);
            for (FactionId g : Factions::All())
                if (Against(p.faction, g) && st->agg.presence[g] > 0.0f)
                {
                    float& there = st->agg.presence[g];
                    there = std::max(0.0f, there - back * there / hostile);
                }
            const float taken = std::min(p.force, blow);
            p.force -= taken;
            blow -= taken;
            p.freight -= blow;
            if (p.freight <= 0.0f)
            {
                FactionId worst = Factions::Pirates();
                float     most = -1.0f;
                for (FactionId g : Factions::All())
                    if (Against(p.faction, g) && st->agg.presence[g] > most)
                    {
                        most = st->agg.presence[g];
                        worst = g;
                    }
                LoseConvoy(p, worst == Factions::Pirates() ? "the pirates took it"
                                                           : FactionName(worst) + " ships took it");
                continue;
            }
        }

        if (time_ < p.legAt + r.hop)
            continue;
        if ((size_t)p.leg + 1 >= p.route.size())
        {
            if (!ArriveConvoy(p))
                over.push_back(p.id);
            continue;
        }
        // On to the next system, if the way is still there.
        const std::string& next = p.route[(size_t)p.leg + 1];
        const auto&        links = Neighbors(p.route[(size_t)p.leg]);
        if (!HasSystem(next) || std::find(links.begin(), links.end(), next) == links.end())
        {
            LoseConvoy(p, "its way was cut");
            continue;
        }
        UnfieldConvoy(p);
        p.leg++;
        p.legAt = time_;
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
