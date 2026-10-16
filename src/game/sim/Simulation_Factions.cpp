// Factions acting on the galaxy (#231).
//
// Every faction runs the same step; only its Temperament, read from data/factions.json,
// differs. A faction's strength in a system is its "presence" -- the ships it has
// committed there, of which the spawn director then makes real ones. Presence grows in the
// systems a faction holds; a holding with strength to spare may reach into ONE neighbouring
// system per period, the one where what the faction values most outweighs the risk by its
// own appetite for risk; and a system changes hands only when a hostile faction has held
// the upper hand there for a sustained time (#225). Nothing here depends on whether a
// player is present: the world goes on without them, slowly and for reasons.
//
// A faction decides on what it knows, not on what is there (#295): what it holds and where
// its ships are it sees; everything else is what its surveyors last brought back, trusted
// less the older it is. Looking is one of the things it may choose to do with its turn.
//
// A system nobody held is taken by building there (#295, slice 3): an outpost, paid for
// from the stock the faction's holdings yield, standing in the system for anyone there to
// see while it goes up. Finished, it is the claim. Strength alone takes a system only from
// an enemy.
#include "sim/Simulation.h"
#include "sim/WorldRules.h"

#include "core/Blueprint.h"
#include "core/World.h"
#include "entities/Derelict.h"
#include "entities/JumpGate.h"
#include "entities/Station.h"
#include "entities/Structure.h"
#include "gen/Rng.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace
{
// How many macro passes make one faction period: a minute at the 2 s pass.
constexpr int FACTION_PERIOD = 30;
// The most one move takes out of a holding, as a share of the faction's capacity.
constexpr float MOVE_SHARE = 0.25f;
// Presence a faction has not kept up with fades by this share per period.
constexpr float FADE = 0.05f;
// Below this a presence is not worth calling one.
constexpr float SPARSE = 0.05f;
// A survey takes this long, plus up to SURVEY_JITTER more seconds of world time (#295).
constexpr double SURVEY_TIME = 90.0;
constexpr int    SURVEY_JITTER = 60;
// What keys a survey's randomness, besides who, where and when.
constexpr uint64_t SURVEY_KEY = 0x5E5u;
// At most this many plans come due in one pass; the rest wait for the next.
constexpr int RESOLVE_PER_PASS = 4;
// A faction may have one survey under way, and one more for every this many holdings.
constexpr int PLANS_PER_HOLDINGS = 8;
// How much history is kept, in memory and in the save.
constexpr size_t CHRONICLE_KEEP = 200;
// What one holding yields a faction's stock per period (#295): a base, plus as much again
// for a prosperous system and for one rich in ore. An outpost costs about ten periods of
// what three ordinary holdings yield.
constexpr float INCOME_BASE = 0.5f;
// A faction keeps no more than this many outposts' worth: what it cannot store or spend
// is not income.
constexpr float STOCK_OUTPOSTS = 2.0f;
// A faction settles only where it has really come: half its capacity committed, the bar
// a capture by strength has always had (StepControl). A gang is not a settlement.
constexpr float SETTLE_PRESENCE = 0.5f;
// A faction spends on something new only what it has beyond this many periods of keeping the
// outposts it has (#295, slice 5).
constexpr float UPKEEP_RESERVE = 10.0f;
// A holding with less than this to spare lends a siege nothing: not worth the journey.
constexpr float MIN_LEND = 0.5f;
// What bringing down a lawless base is worth to a lawful faction, in a system with no
// security at all (#295): the order it brings to its holdings next door.
constexpr float ORDER_WORTH = 0.5f;
// The most a convoy's expected losses add to the risk of what it is for (#407): past this, a
// way is simply bad, and a worse one is no worse a reason to wait.
constexpr float CONVOY_RISK_CAP = 2.0f;

bool Hostile(FactionId a, FactionId b)
{
    const Stance s = Factions::Relation(a, b);
    return a != b && (s == Stance::War || s == Stance::Hostile);
}

// Lawless places breed their own trouble: in a system the pirates do not hold, their
// presence drifts towards this however nobody sends them, and no further.
float Unrest(const SystemAggregate& a)
{
    const float cap = Factions::TemperamentOf(Factions::Pirates()).capacity;
    return std::max(0.0f, 0.6f - a.security) * cap * 0.4f;
}
}  // namespace

void Simulation::SeedPresence(SystemAggregate& a)
{
    a.presence.Clear();
    // Half strength to begin with: a fresh world grows into its numbers rather than
    // starting at them, so nothing is decided in its first minutes (#143, #225). Nobody's
    // system has nobody's garrison.
    if (a.claimed)
        a.presence[a.controller] = Factions::TemperamentOf(a.controller).capacity * 0.5f;
    if (a.controller != Factions::Pirates())
        a.presence[Factions::Pirates()] = Unrest(a);
}

// What an object does to its system's market (#343): a planet gives what goods.json says it
// grows, a market its kind's industry. A belt gives its ore too, but it can run out (#348), so
// that is the coarse pass's to ask of the belt itself (StepMarket).
static void AddToMarket(MarketRates& r, const Entity& e)
{
    r.source.resize((size_t)Goods::Count(), 0.0);
    r.use.resize((size_t)Goods::Count(), 0.0);
    const std::string kind = e.GetArchetype() != nullptr ? e.GetArchetype()->id : std::string();
    auto add = [](std::vector<double>& to, const std::vector<std::pair<ResourceType, double>>& v)
    {
        for (const auto& a : v)
            if ((size_t)a.first.Index() < to.size())
                to[(size_t)a.first.Index()] += a.second;
    };
    if (e.GetKind() == EntityKind::Planet)
    {
        if (const auto* grows = Goods::PlanetYield(kind))
            add(r.source, *grows);
    }
    else if (const Industry* ind = Goods::IndustryOf(kind))
    {
        add(r.use, ind->uses);
        r.recipes.insert(r.recipes.end(), ind->recipes.begin(), ind->recipes.end());
    }
}

SystemProfile Simulation::ProfileOf(const SystemState& st)
{
    SystemProfile p;
    for (const auto& e : st.entities)
        switch (e->GetKind())
        {
            case EntityKind::Field: p.belts++; break;
            case EntityKind::Derelict: p.wrecks++; break;
            case EntityKind::Planet:
                p.planets++;
                AddToMarket(p.rates, *e);
                break;
            case EntityKind::Gate:
                if (SignatureOf(*e) <= 0.0f)  // a roaming hole is nobody's way (#342 S9)
                    p.gates++;
                break;
            case EntityKind::Station:
                p.stations++;
                p.markets += e->Has(Component::Market) ? 1 : 0;
                if (e->Has(Component::Market))
                    AddToMarket(p.rates, *e);
                if (e->Has(Component::Defensive))
                    p.defenders.push_back(static_cast<const Station*>(e.get())->GetFaction());
                break;
            case EntityKind::Structure:
            {
                const Structure& t = static_cast<const Structure&>(*e);
                FactionId        anchorer;
                if (IsAnchor(t.GetBuilds()) && Outposts::FactionOf(t.GetOwner(), anchorer))
                {
                    p.anchors.push_back(anchorer);  // a site too: it is what a rival stops
                    p.anchorIds.push_back(t.GetId());
                }
                p.markets += !t.IsBuilding() && e->Has(Component::Market) ? 1 : 0;
                if (!t.IsBuilding() && e->Has(Component::Market))
                    AddToMarket(p.rates, *e);
                // An outpost being upgraded (#342 S6) -- into a market, or into a station -- is
                // still the outpost it was until it is finished: the claim, the upkeep, a siege.
                const Blueprint* bp = t.GetBlueprint();
                const bool       upgrading = t.IsBuilding() && bp != nullptr && !bp->onto.empty();
                if (!Outposts::IsOutpost(t.GetBuilds()) && !upgrading)
                    break;
                p.outpostSite = true;
                FactionId owner;
                if ((!t.IsBuilding() || upgrading) && Outposts::FactionOf(t.GetOwner(), owner))
                {
                    p.outposts.push_back(owner);
                    p.outpostIds.push_back(t.GetId());
                }
                break;
            }
            default: break;
        }
    return p;
}

float Simulation::OutpostCost()
{
    const Blueprint* bp = Blueprints::Find(Outposts::BLUEPRINT);
    if (bp == nullptr || !bp->byFactions)
        return 0.0f;
    float cost = 0.0f;
    for (const auto& c : bp->cost)
        cost += (float)c.second;
    return cost;
}

void Simulation::TakeControl(SystemAggregate& a, FactionId f)
{
    const FactionId was = a.controller;
    a.controller = f;
    a.claimed = true;
    a.contested = 0;
    if (f == Factions::Pirates())
        a.baseSecurity = std::min(a.baseSecurity, 0.15f);
    else if (was == Factions::Pirates())
        a.baseSecurity = std::max(a.baseSecurity, 0.5f);
}

// What a system offers, seen by anyone: how much passes through, how much ore there is,
// how much lies about to be salvaged, and who defends it. The last three are what the
// system is, read from its profile (#295) -- a faction weighing a hundred neighbours does
// not walk a hundred lists of ships to count the belts.
Simulation::SystemOffer Simulation::OfferOf(const SystemState& st) const
{
    return OfferOf(Observe(st));
}

Simulation::SystemOffer Simulation::OfferOf(const Intel& seen)
{
    SystemOffer o;
    o.traffic = std::clamp(seen.traffic, 0.0f, 1.0f);
    o.ore = std::min(1.0f, seen.belts / 3.0f);
    o.salvage = std::min(1.0f, seen.wrecks / 4.0f);
    o.defenders = seen.defenders;
    return o;
}

Intel Simulation::Observe(const SystemState& st) const
{
    Intel i;
    i.seenAt = time_;
    i.traffic = st.agg.prosperity;
    i.security = st.agg.security;
    i.belts = st.profile.belts;
    i.wrecks = st.profile.wrecks;
    i.stations = st.profile.stations;
    i.planets = st.profile.planets;
    i.gates = st.profile.gates;
    i.defenders = st.profile.defenders;
    i.outposts = st.profile.outposts;
    i.presence = st.agg.presence;
    i.controller = st.agg.controller;
    i.claimed = st.agg.claimed;
    return i;
}

void Simulation::Record(const std::string& kind, std::optional<FactionId> faction,
                        const std::string& system, const std::string& text)
{
    ChronicleEntry e;
    e.seq = ++chronicleSeq_;
    e.time = time_;
    e.kind = kind;
    e.faction = faction;
    e.system = system;
    e.text = text;
    chronicle_.push_back(std::move(e));
    if (chronicle_.size() > CHRONICLE_KEEP)
        chronicle_.erase(chronicle_.begin(),
                         chronicle_.begin() + (long)(chronicle_.size() - CHRONICLE_KEEP));
}

// What every faction knows when the world begins (#295): the systems it holds and the ones
// a gate from them leads to. Everything beyond is a name on a chart at most -- and beyond
// the wormhole, not even that.
void Simulation::SeedMinds()
{
    mindsSeeded_ = true;
    for (auto& kv : systems_)
    {
        const SystemAggregate& a = kv.second.agg;
        if (!a.claimed)
            continue;
        FactionMind& m = minds_[a.controller];
        m.intel[kv.first] = Observe(kv.second);
        for (const std::string& nid : Neighbors(kv.first))
        {
            const auto n = systems_.find(nid);
            if (n != systems_.end() && m.intel.count(nid) == 0)
                m.intel[nid] = Observe(n->second);
        }
    }
}

// "3 belts, held by Syndicate, pirates": what a survey found, as somebody reading the
// history would want it put.
std::string Simulation::DescribeSurvey(const Intel& seen, FactionId by) const
{
    std::string text;
    auto        add = [&](const std::string& part) { text += (text.empty() ? "" : ", ") + part; };
    auto        count = [&](int n, const char* one, const char* many)
    {
        if (n > 0)
            add(n == 1 ? std::string("1 ") + one : std::to_string(n) + " " + many);
    };
    count(seen.belts, "belt", "belts");
    count(seen.wrecks, "wreck", "wrecks");
    count(seen.stations, "station", "stations");
    if (!seen.claimed)
        add("held by nobody");
    else if (seen.controller != by)
        add("held by " + FactionName(seen.controller));
    for (FactionId g : Factions::All())
        if (g != by && seen.presence[g] >= 1.0f && !(seen.claimed && g == seen.controller))
            add(g == Factions::Pirates() ? std::string("pirates") : FactionName(g) + " ships");
    return text.empty() ? "nothing of note" : text;
}

void Simulation::ResolveSurvey(const Plan& p)
{
    const auto st = systems_.find(p.target);
    if (st == systems_.end())
        return;  // the galaxy no longer has it; the survey comes back with nothing
    const Intel seen = Observe(st->second);
    minds_[p.faction].intel[p.target] = seen;
    NoteSurvey(p.target);  // the region's tide feels it (#342 S3)
    Record("survey", p.faction, p.target,
           FactionName(p.faction) + " surveyed " + SystemName(p.target) + ": " +
               DescribeSurvey(seen, p.faction));
}

// Plans whose time has come, a few per pass (#295). One that misses its pass because the
// budget ran out is simply a pass late; nothing is lost.
void Simulation::ResolveDuePlans()
{
    for (int budget = RESOLVE_PER_PASS; budget > 0 && !due_.empty(); budget--)
    {
        const auto next = due_.begin();
        if (next->first > time_)
            break;
        const int id = next->second;
        due_.erase(next);
        const auto it = plans_.find(id);
        if (it == plans_.end())
            continue;
        const Plan p = it->second;
        plans_.erase(it);
        switch (p.kind)
        {
            case Plan::Kind::Survey: ResolveSurvey(p); break;
            case Plan::Kind::Settle: ResolveSettle(p); break;
            case Plan::Kind::Siege: ResolveSiege(p); break;
            case Plan::Kind::Return: ResolveReturn(p); break;
            case Plan::Kind::Anchor: ResolveAnchor(p); break;
            case Plan::Kind::Upgrade: ResolveUpgrade(p); break;
        }
    }
}

// Lays the site down (#295). Everything the faction believed is checked against what is
// there first, as a move is on arrival: a system somebody has claimed since, or where an
// outpost already stands or is going up, is not one to settle -- and nothing is spent.
bool Simulation::Settle(FactionId f, const std::string& from, const std::string& to)
{
    SystemState* st = SystemById(to);
    const float  cost = OutpostCost();
    FactionMind& mind = minds_[f];
    if (st == nullptr || cost <= 0.0f || mind.stock < cost)
        return false;
    const Intel& seen = mind.intel[to] = Observe(*st);
    if (st->agg.claimed || st->profile.outpostSite)
        return false;

    // What it came for decides what it builds and where (#318); a purpose with nothing to
    // build by here, or no room left for it, is an open point instead.
    Outposts::Purpose purpose =
        Outposts::ChoosePurpose(Factions::TemperamentOf(f), Factions::IsLawful(f), seen);
    OutpostSpot spot = FindOutpostSpot(f, purpose, from, to);
    if (!spot.found && purpose != Outposts::Purpose::Open)
    {
        purpose = Outposts::Purpose::Open;
        spot = FindOutpostSpot(f, purpose, from, to);
    }
    const Blueprint* bp = Blueprints::Find(Outposts::BlueprintOf(purpose));
    if (!spot.found || bp == nullptr)
        return false;

    auto site =
        std::make_unique<Structure>(spot.at, 0.0f, FactionName(f) + " " + bp->name, bp->archetype);
    if (spot.orbit)
        site->SetOrbit(*spot.orbit);
    site->StartBuilding(time_, time_ + bp->buildSeconds);
    if (bp->lifetime > 0.0f)
        site->SetExpiresAt(time_ + bp->buildSeconds + bp->lifetime);
    // A faction's, not an account's: the per-account cap is not its limit, the stock is.
    const int id = AddStatic(to, std::move(site), Outposts::OwnerOf(f));
    if (id == 0)
        return false;
    mind.stock -= cost;

    Plan p;
    p.id = nextPlanId_++;
    p.kind = Plan::Kind::Settle;
    p.faction = f;
    p.from = from;
    p.target = to;
    p.startedAt = time_;
    p.dueAt = time_ + bp->buildSeconds;
    p.site = StaticKey(to, id);
    plans_[p.id] = p;
    due_.insert({ p.dueAt, p.id });
    Record("settle", f, to,
           FactionName(f) + " began " + Outposts::Called(purpose) + " in " + SystemName(to));
    return true;
}

// A settlement's time has come (#295). The site finished on the tick it was due --
// StepStructures runs every tick and saw to it -- and a finished outpost in a system nobody
// holds is the claim. One that is gone was lost, and the stock with it.
void Simulation::ResolveSettle(const Plan& p)
{
    SystemState* st = SystemById(p.target);
    if (st == nullptr)
        return;
    int siteId = 0;
    for (const auto& kv : st->keys)
        if (kv.second == p.site)
            siteId = kv.first;
    const Structure* site = nullptr;
    for (const auto& e : st->entities)
        if (siteId != 0 && e->GetId() == siteId && e->GetKind() == EntityKind::Structure)
            site = static_cast<const Structure*>(e.get());
    const std::string who = FactionName(p.faction), where = SystemName(p.target);
    if (site == nullptr)
    {
        Record("settle", p.faction, p.target,
               who + " lost the outpost it was building in " + where);
        return;
    }
    if (site->IsBuilding())
    {
        // Not yet after all -- its time line says later. Wait for it.
        Plan later = p;
        later.dueAt = std::max(site->GetCompletesAt(), time_);
        plans_[later.id] = later;
        due_.insert({ later.dueAt, later.id });
        return;
    }
    SystemAggregate& a = st->agg;
    if (a.claimed)
    {
        Record("settle", p.faction, p.target,
               who + " finished an outpost in " + where + ", but " +
                   (a.controller == p.faction ? std::string("it holds it already")
                                              : FactionName(a.controller) + " holds it"));
        return;
    }
    TakeControl(a, p.faction);
    minds_[p.faction].intel[p.target] = Observe(*st);
    Outposts::Purpose purpose = Outposts::Purpose::Open;
    Outposts::PurposeOf(site->GetBuilds(), purpose);
    Announce("settle", p.faction, p.target,
             who + " founded " + Outposts::Called(purpose) + " in " + where);
}

void Simulation::StepFactions()
{
    // Losses in battle are losses of presence: whatever the director spawned and somebody
    // destroyed was strength the faction had committed there.
    for (auto& kv : systems_)
    {
        SystemAggregate& a = kv.second.agg;
        if (a.lostPirates > 0.0f)
            a.presence[Factions::Pirates()] =
                std::max(0.0f, a.presence[Factions::Pirates()] - a.lostPirates);
        if (a.lostPolice > 0.0f)
        {
            const FactionId law = a.policeFaction;
            a.presence[law] = std::max(0.0f, a.presence[law] - a.lostPolice);
        }
        a.lostPirates = a.lostPolice = 0.0f;
    }

    if (!mindsSeeded_)
        SeedMinds();
    ResolveDuePlans();
    StepSieges();
    StepConvoys();  // the builder convoys on their way (#342 S6)
    StepJobs();     // and the jobs those plans posted (#345)

    // Each faction thinks once a period, on a pass of its own (#295): spread across the
    // period rather than all on one pass, so no pass carries more than one of them -- while
    // there are no more factions than passes in a period; past that, some share one.
    const int phase = ++factionPasses_ % FACTION_PERIOD;
    const int count = Factions::Count();
    for (FactionId f : Factions::All())
        if (phase == f.Index() * FACTION_PERIOD / count)
            Think(f);
    if (phase != 0)
        return;

    // Once a period, what factions have built is paid for out of what they saved, or is not
    // (slice 5) -- the bill comes before this period's yield, which is next period's to spend.
    StepUpkeep();

    // Once a period, holdings yield (#295): something for being held, more for trade and
    // for ore. What a faction cannot store is not income -- and it stores, besides, what it
    // sets by to keep the outposts it has (#342 S6): a faction with many is not kept from ever
    // affording anything new by the reserve that keeping them asks of it.
    std::map<FactionId, int> kept;
    for (const auto& kv : systems_)
        for (FactionId o : kv.second.profile.outposts)
            kept[o]++;
    const float reserve = UPKEEP_RESERVE * Blueprints::UpkeepRules().share * OutpostCost();
    for (const auto& kv : systems_)
    {
        const SystemAggregate& a = kv.second.agg;
        if (!a.claimed)
            continue;
        const float ore = std::min(1.0f, kv.second.profile.belts / 3.0f);
        const auto  n = kept.find(a.controller);
        const float storable =
            STOCK_OUTPOSTS * OutpostCost() + reserve * (n == kept.end() ? 0.0f : (float)n->second);
        float& stock = minds_[a.controller].stock;
        stock = std::min(storable, stock + INCOME_BASE * (1.0f + a.prosperity + ore));
    }

    // Once a period, holdings recover towards capacity -- less what they have lent a siege and
    // not had back (#295): a holding does not make good ships that are only away -- and
    // everything else fades towards what the place breeds by itself, unless its faction keeps
    // sending more.
    // By system, what its holder has away: LentFrom for every holding at once.
    std::map<std::string, float> lent;
    auto                         owes = [&](const std::string& id, FactionId f, float force)
    {
        const auto home = systems_.find(id);
        if (home != systems_.end() && home->second.agg.claimed && home->second.agg.controller == f)
            lent[id] += force;
    };
    for (const auto& kv : plans_)
    {
        if (kv.second.kind == Plan::Kind::Siege || kv.second.OnTheWay())
        {
            // A siege's contingents, and a convoy's escort (#342 S6).
            for (const Plan::Lent& l : kv.second.lent)
                owes(l.from, kv.second.faction, l.force);
        }
        else if (kv.second.kind == Plan::Kind::Return)
            owes(kv.second.target, kv.second.faction, kv.second.force);
    }
    for (auto& kv : systems_)
    {
        SystemAggregate& a = kv.second.agg;
        for (FactionId f : Factions::All())
        {
            const Temperament& t = Factions::TemperamentOf(f);
            float&             p = a.presence[f];
            if (f == a.controller && a.claimed)
            {
                const auto  away = lent.find(kv.first);
                const float keep =
                    std::max(0.0f, t.capacity - (away != lent.end() ? away->second : 0.0f));
                p += t.growth * (keep - p);
            }
            else
            {
                const float floor = f == Factions::Pirates() ? Unrest(a) : 0.0f;
                p = p > floor ? p - (p - floor) * FADE : p + (floor - p) * t.growth;
            }
            if (p < SPARSE)
                p = 0.0f;
        }
    }
}

// One faction's turn (#231, #295). It sees what it holds and wherever its ships are; of
// anything else it knows only what it saw. Then it does at most one thing: it reaches into
// a neighbour of a holding where what it values outweighs the risk by its appetite for
// risk -- judged on what it saw there, and the older that is the riskier -- or it sends
// surveyors to a neighbour it knows nothing, or nothing recent, about, or it builds an
// outpost in a neighbour nobody holds where its ships already are.
void Simulation::Think(FactionId f)
{
    const Temperament& t = Factions::TemperamentOf(f);
    FactionMind&       mind = minds_[f];

    // What it sees for itself is never out of date -- and is not saved, for that reason.
    int holdings = 0, outposts = 0;
    for (auto& kv : systems_)
    {
        const SystemAggregate& a = kv.second.agg;
        const bool             held = a.claimed && a.controller == f;
        holdings += held ? 1 : 0;
        outposts += (int)std::count(kv.second.profile.outposts.begin(),
                                    kv.second.profile.outposts.end(), f);
        if (held || a.presence[f] >= 1.0f)
            mind.intel[kv.first] = Observe(kv.second);
    }
    if (t.appetite <= 0.0f)
        return;  // holds what it has and reaches for nothing
    // Word of a mouth near its space draws it that way for a while (#342 S6).
    const Drawn drawn = DrawnBy(f);

    // What it already has under way: one survey of a system at a time, and no more at once
    // than its holdings can send.
    std::set<std::string> surveying, settling, upgrading;
    bool                  besieging = false, anchoring = false;
    float                 owed = 0.0f;  // what convoys waiting to set out have yet to make good
    for (const auto& kv : plans_)
        if (kv.second.faction == f && kv.second.OnTheWay() && kv.second.leg < 0)
            owed += std::max(0.0f, 1.0f - kv.second.freight) * FreightPrice(kv.second);
    for (const auto& kv : plans_)
        if (kv.second.faction == f && kv.second.kind == Plan::Kind::Survey)
            surveying.insert(kv.second.target);
        else if (kv.second.faction == f && kv.second.kind == Plan::Kind::Settle)
            settling.insert(kv.second.target);
        else if (kv.second.faction == f && kv.second.kind == Plan::Kind::Siege)
            besieging = true;
        else if (kv.second.faction == f && kv.second.kind == Plan::Kind::Anchor)
            anchoring = true;
        else if (kv.second.faction == f && kv.second.kind == Plan::Kind::Upgrade)
            upgrading.insert(kv.second.target);
    // A small faction does one thing of a kind at a time; a civilisation of many holdings
    // builds in more than one place at once (#407) -- one more outpost going up, and one more
    // growing, for every so many holdings, as it sends surveyors.
    const int  works = 1 + holdings / PLANS_PER_HOLDINGS;
    const bool canSurvey = (int)surveying.size() < works;
    // Only with the stock to pay for it -- over and above what keeping the outposts it has will
    // cost for a while (slice 5): a faction that spends its upkeep on a new one loses an old one.
    const float cost = OutpostCost();
    // ...and over what a convoy that did not get through must be paid again (#407).
    const float reserve =
        UPKEEP_RESERVE * Blueprints::UpkeepRules().share * cost * (float)outposts + owed;
    const bool canSettle =
        (int)settling.size() < works && cost > 0.0f && mind.stock >= cost + reserve;
    // One siege at a time, paid for like a settlement (slice 5).
    const Blueprints::Siege& siege = Blueprints::SiegeRules();
    const bool canSiege = !besieging && cost > 0.0f && mind.stock >= SiegeCost() + reserve;
    // What it believes an outpost takes to bring down: it cannot see which kind it is from
    // next door, so the ordinary one's hull, mended as a supplied one is.
    const Blueprint* plain = Blueprints::Find(Outposts::BLUEPRINT);
    const float      wall = plain != nullptr ? plain->hull : 0.0f;
    const float      mend = Blueprints::UpkeepRules().repair * wall / (FACTION_PERIOD * 2.0f);
    // One Anchor at a time, paid for like an outpost (#342 S4) -- and an Anchor's hull, for a
    // rival weighing whether it could bring one down.
    const float anchorCost = AnchorCost();
    const bool  canAnchor = WorldRules::MouthRules().anchors && !anchoring && anchorCost > 0.0f &&
                            mind.stock >= anchorCost + reserve;
    const Blueprint* anchorBp = Blueprints::Find(ANCHOR_BLUEPRINT);
    const float      anchorWall = anchorBp != nullptr ? anchorBp->hull : 0.0f;

    enum class Act
    {
        None,
        Move,
        Survey,
        Settle,
        Siege,
        Anchor,
        Upgrade
    };
    Act         act = Act::None;
    float       bestScore = 0.0f;
    std::string from, to, why;
    FactionId   enemy = Factions::Unaffiliated();
    // A siege's force, by the system it would be against: who would lend it, and the way in.
    std::map<std::string, std::pair<std::vector<Plan::Lent>, std::string>> pools;
    std::vector<Plan::Lent>                                                lend;
    std::string                                                            via;
    int         anchorSlot = -1;        // the mouth an Anchor would hold
    std::string grow;                   // what an upgrade would make its outpost (#342 S6)
    bool        againstAnchor = false;  // the siege is of an Anchor rather than an outpost
    // What a region behind a mouth promises, from what was seen of its entry system (#342
    // S4): what the place offers, and a whole region nobody holds.
    auto promise = [&](const Intel& seen)
    {
        const SystemOffer o = OfferOf(seen);
        return t.traffic * o.traffic + t.ore * o.ore + t.salvage * o.salvage + t.unclaimed +
               0.5f * t.curiosity;
    };
    for (auto& kv : systems_)
    {
        const SystemAggregate& home = kv.second.agg;
        if (!home.claimed || home.controller != f)
            continue;
        const float surplus = home.presence[f] - t.capacity * HOME_GUARD;
        for (const std::string& nid : Neighbors(kv.first))
        {
            if (systems_.count(nid) == 0)
                continue;
            const auto   k = mind.intel.find(nid);
            const Intel* seen = k == mind.intel.end() ? nullptr : &k->second;
            // What stands next door is no secret (slice 5): an outpost is built to be seen,
            // and word of one comes through the gate. Who is there in strength, and how much
            // of it, a faction still has to go and look at.
            if (k != mind.intel.end())
                k->second.outposts = systems_.at(nid).profile.outposts;

            bool rival = false;  // an enemy's outpost stands there
            if (seen != nullptr)
                for (FactionId g : seen->outposts)
                    rival = rival || Hostile(f, g);

            // Looking: worth its curiosity where it has never been, and again as what it
            // saw goes stale -- unless what it saw was a friend's or a neutral's, which it
            // could not take whatever has changed there. The risk is the unknown, plus
            // whatever hostile it saw there.
            const bool closed = seen != nullptr && seen->claimed &&
                                (seen->controller == f || !Hostile(f, seen->controller));
            // To the law, a lawless system next door -- nobody's, or the lawless' own -- is
            // what keeps its holdings unsafe (#407): bringing order to it is worth what bringing
            // down a lawless base is (ORDER_WORTH), to look at, to reach into and to settle.
            // It is what keeps a civilisation pressing on the frontier at its door once the
            // easy systems there are taken, rather than sitting on a full purse.
            const float order = Factions::IsLawful(f) && seen != nullptr && !closed &&
                                        (!seen->claimed || !Factions::IsLawful(seen->controller))
                                    ? ORDER_WORTH * (1.0f - std::clamp(seen->security, 0.0f, 1.0f))
                                    : 0.0f;
            if (canSurvey && !closed && surveying.count(nid) == 0)
            {
                // An enemy's outpost next door is worth watching: what it would take to
                // bring down is what its garrison is now, not what it was (slice 5).
                const float value =
                    Intelligence::SurveyValue(t.curiosity + DrawOf(drawn, kv.first, nid) +
                                                  (rival && canSiege ? t.unclaimed : 0.0f) + order,
                                              seen, time_);
                float hostile = 0.0f;
                if (seen != nullptr)
                    for (FactionId g : Factions::All())
                        if (Hostile(f, g))
                            hostile += seen->presence[g];
                const float risk = Intelligence::UNSEEN_RISK + 0.5f * hostile / t.capacity;
                const float score = value - risk / t.appetite;
                if (value > 0.0f && score > bestScore)
                {
                    bestScore = score;
                    act = Act::Survey;
                    from = kv.first;
                    to = nid;
                }
            }

            if (seen == nullptr || closed)
                continue;
            const SystemOffer o = OfferOf(*seen);
            const float       unclaimed = seen->claimed ? 0.0f : 1.0f;
            // ...and the way to a mouth it has heard of, and the space beyond (#342 S6).
            const float value = t.traffic * o.traffic + t.ore * o.ore + t.salvage * o.salvage +
                                t.unclaimed * unclaimed + DrawOf(drawn, kv.first, nid) + order;

            // Settling (#295): building an outpost in a system nobody holds, where it has
            // already come in strength -- it sees the place for itself, so what it knows is
            // fresh. Worth what the place is worth; the risk is what is there against it,
            // net of its own ships, which is why settling beats sending more of them.
            if (canSettle && settling.count(nid) == 0 && !seen->claimed &&
                seen->presence[f] >= t.capacity * SETTLE_PRESENCE)
            {
                float hostile = 0.0f;
                for (FactionId g : Factions::All())
                    if (Hostile(f, g))
                        hostile += seen->presence[g];
                int guns = 0;
                for (FactionId d : o.defenders)
                    if (d != f)
                        guns++;
                // ...and where it lost an outpost not long ago, the memory of that (slice 5).
                const float risk = std::max(0.0f, hostile - seen->presence[f]) / t.capacity +
                                   0.5f * guns + (Factions::IsLawful(f) ? 0.0f : seen->security) +
                                   Intelligence::LossRisk(mind.lostAt, nid, time_);
                const float score = value - Intelligence::BelievedRisk(
                                                risk, Intelligence::Staleness(*seen, time_)) /
                                                t.appetite;
                if (score > bestScore)
                {
                    bestScore = score;
                    act = Act::Settle;
                    from = kv.first;
                    to = nid;
                }
            }

            // Besieging (slice 5): an enemy's outpost next door, which it has seen, can reach
            // with what its holdings within reach spare (#295: pooled), and can bring down
            // before its besiegers give up. Worth what the system is worth with nobody's claim
            // on it and one more wreck in it -- which is what is left when the outpost falls;
            // the risk is the garrison against what it brings, the guns, and the time it would
            // take to get there and bring it down.
            if (canSiege && wall > 0.0f)
                for (FactionId g : seen->outposts)
                {
                    if (!Hostile(f, g) || (seen->claimed && seen->controller != g))
                        continue;
                    const float garrison = seen->presence[g];
                    // It asks its holdings within reach for all the rules let a siege have.
                    auto pool = pools.find(nid);
                    if (pool == pools.end())
                    {
                        std::string in;
                        auto        lent = PoolFor(f, nid, siege.pool * t.capacity, &in);
                        pool = pools.emplace(nid, std::make_pair(std::move(lent), in)).first;
                    }
                    float sent = 0.0f;
                    int   hops = 0;
                    for (const Plan::Lent& l : pool->second.first)
                    {
                        sent += l.force;
                        hops = std::max(hops, l.hops);
                    }
                    const float travel = hops * siege.hop;
                    const float firing = sent - siege.defence * garrison;
                    const float rate = firing * siege.dps - (seen->claimed ? mend : 0.0f);
                    if (sent < 1.5f || firing <= 0.0f || rate <= 0.0f ||
                        wall / rate > siege.seconds)
                        break;
                    int guns = 0;
                    for (FactionId d : o.defenders)
                        if (d != f)
                            guns++;
                    // ...and to the law, a lawless base next door is what keeps its own systems
                    // unsafe: bringing one down is worth the order it brings, as the lawless
                    // fear the law in a system they would reach into.
                    const float order = Factions::IsLawful(f) && !Factions::IsLawful(g)
                                            ? ORDER_WORTH * (1.0f - seen->security)
                                            : 0.0f;
                    const float worth = t.traffic * o.traffic + t.ore * o.ore +
                                        t.salvage * std::min(1.0f, o.salvage + 0.25f) +
                                        t.unclaimed + order;
                    // The share of its force the garrison would tie up, the guns, and how much
                    // of its time there it would take to bring the outpost down -- all as it
                    // saw them, and that sighting will be older by the time it has got there.
                    const float risk =
                        siege.defence * garrison / sent + 0.5f * guns + wall / rate / siege.seconds;
                    const float score =
                        worth - Intelligence::BelievedRisk(
                                    risk, Intelligence::Staleness(*seen, time_ + travel)) /
                                    t.appetite;
                    if (score > bestScore)
                    {
                        bestScore = score;
                        act = Act::Siege;
                        from = kv.first;
                        to = nid;
                        enemy = g;
                        lend = pool->second.first;
                        via = pool->second.second;
                    }
                    break;  // one outpost in a system is what it would besiege
                }

            // Reaching: only where it has looked, and only with strength to spare -- and
            // only into a system it may take: nobody's, or an enemy's. Never a friend's or
            // a neutral power's (`closed`) -- that would be a war nobody declared.
            if (surplus < 0.5f)
                continue;
            float hostile = 0.0f;
            for (FactionId g : Factions::All())
                if (Hostile(f, g) || (g == seen->controller && seen->controller != f))
                    hostile += seen->presence[g];
            int guns = 0;
            for (FactionId d : o.defenders)
                if (d != f)
                    guns++;
            // The lawless also fear the law itself; the law does not fear a quiet system.
            const float risk = hostile / t.capacity + 0.5f * guns +
                               (Factions::IsLawful(f) ? 0.0f : seen->security);
            const float score =
                value - Intelligence::BelievedRisk(risk, Intelligence::Staleness(*seen, time_)) /
                            t.appetite;
            if (score > bestScore)
            {
                bestScore = score;
                act = Act::Move;
                from = kv.first;
                to = nid;
                why.clear();
                auto add = [&](float w, float v, const char* word)
                {
                    if (w * v >= 0.3f)
                        why += std::string(why.empty() ? "" : ", ") + word;
                };
                add(t.traffic, o.traffic, "traffic");
                add(t.ore, o.ore, "ore");
                add(t.salvage, o.salvage, "salvage");
                add(t.unclaimed, unclaimed, "nobody holds it");
                if (hostile < 1.0f && guns == 0)
                    why += std::string(why.empty() ? "" : ", ") + "nobody to stop them";
            }
        }
    }

    // Besieging a rival's Anchor at a wormhole mouth (#342 S4), built or going up: brought down,
    // the mouth is open to anyone again. Wherever it stands within reach -- in a system it holds
    // or next door to one, never a friend's or a neutral power's. Worth what the region behind
    // the mouth promises; the risk is reckoned as for an outpost, against the Anchor's hull.
    if (canSiege && anchorWall > 0.0f)
        for (auto& kv : systems_)
        {
            const SystemProfile& here = kv.second.profile;
            if (here.anchors.empty())
                continue;
            const std::string&     nid = kv.first;
            const SystemAggregate& a = kv.second.agg;
            if (a.claimed && a.controller != f && !Hostile(f, a.controller))
                continue;
            auto holds = [&](const std::string& id)
            {
                const auto it = systems_.find(id);
                return it != systems_.end() && it->second.agg.claimed &&
                       it->second.agg.controller == f;
            };
            bool near = holds(nid);
            for (const std::string& n : Neighbors(nid))
                near = near || holds(n);
            const auto k = mind.intel.find(nid);
            if (!near || k == mind.intel.end())
                continue;
            const Intel& seen = k->second;
            for (FactionId g : here.anchors)
            {
                if (!Hostile(f, g))
                    continue;
                const RegionOpening* o = nullptr;
                for (const RegionOpening& r : openings_)
                    if (r.mouth == nid && (r.state == "unstable" || r.state == "anchored"))
                        o = &r;
                const auto entry =
                    o != nullptr ? mind.intel.find(EntryOf(o->slot)) : mind.intel.end();
                auto pool = pools.find(nid);
                if (pool == pools.end())
                {
                    std::string in;
                    auto        lent = PoolFor(f, nid, siege.pool * t.capacity, &in);
                    pool = pools.emplace(nid, std::make_pair(std::move(lent), in)).first;
                }
                float sent = 0.0f;
                int   hops = 0;
                for (const Plan::Lent& l : pool->second.first)
                {
                    sent += l.force;
                    hops = std::max(hops, l.hops);
                }
                const float garrison = seen.presence[g];
                const float firing = sent - siege.defence * garrison;
                const float rate = firing * siege.dps;
                if (sent < 1.5f || firing <= 0.0f || anchorWall / rate > siege.seconds)
                    break;
                const float worth =
                    entry != mind.intel.end() ? promise(entry->second) : t.unclaimed + t.curiosity;
                const float risk =
                    siege.defence * garrison / sent + anchorWall / rate / siege.seconds;
                const float score =
                    worth - Intelligence::BelievedRisk(
                                risk, Intelligence::Staleness(seen, time_ + hops * siege.hop)) /
                                t.appetite;
                if (score > bestScore)
                {
                    bestScore = score;
                    act = Act::Siege;
                    againstAnchor = true;
                    from = pool->second.first.front().from;
                    to = nid;
                    enemy = g;
                    lend = pool->second.first;
                    via = pool->second.second;
                }
                break;  // one Anchor in a system is what it would besiege
            }
        }

    // Anchoring (#342 S4): an unstable mouth in a system it holds, or has come to in strength,
    // or next door to one it holds. Worth what the region behind it promises, judged on what
    // its surveyors saw of the entry system -- it looks first, if it has not; the risk is who
    // else is at the mouth, enemies the more, and the memory of losing something there.
    for (const RegionOpening& o : openings_)
    {
        if (o.state != "unstable" || (!canAnchor && !canSurvey))
            continue;
        const auto mouth = systems_.find(o.mouth);
        if (mouth == systems_.end() || !AnchorAt(o, false).empty())
            continue;
        auto holds = [&](const std::string& id)
        {
            const auto it = systems_.find(id);
            return it != systems_.end() && it->second.agg.claimed && it->second.agg.controller == f;
        };
        std::string base;
        if (holds(o.mouth) || mouth->second.agg.presence[f] >= t.capacity * SETTLE_PRESENCE)
            base = o.mouth;
        else
            for (const std::string& n : Neighbors(o.mouth))
                if (base.empty() && holds(n))
                    base = n;
        if (base.empty())
            continue;
        const std::string entry = EntryOf(o.slot);
        const auto        seen = mind.intel.find(entry);
        if (seen == mind.intel.end())
        {
            if (canSurvey && surveying.count(entry) == 0)
            {
                const float score =
                    t.curiosity + t.unclaimed - Intelligence::UNSEEN_RISK / t.appetite;
                if (score > bestScore)
                {
                    bestScore = score;
                    act = Act::Survey;
                    from = base;
                    to = entry;
                }
            }
            continue;
        }
        if (!canAnchor)
            continue;
        float others = 0.0f;
        for (FactionId g : Factions::All())
            if (g != f)
                others += mouth->second.agg.presence[g] * (Hostile(f, g) ? 2.0f : 0.5f);
        // ...and what it expects its convoy to lose on the way there (#407).
        const float risk = others / t.capacity +
                           Intelligence::LossRisk(mind.lostAt, o.mouth, time_) +
                           std::min(CONVOY_RISK_CAP, ChartConvoy(f, o.mouth).loss);
        const float score =
            promise(seen->second) -
            Intelligence::BelievedRisk(risk, Intelligence::Staleness(seen->second, time_)) /
                t.appetite;
        if (score > bestScore)
        {
            bestScore = score;
            act = Act::Anchor;
            from = base;
            anchorSlot = o.slot;
        }
    }

    // Growing an outpost (#342 S6): a market where none is near, a station where a market
    // stands and enough passes through or is mined -- by what it needs, against what is there
    // against it. The lawful build markets; a hidden base is not one to trade at.
    if ((int)upgrading.size() < works && Factions::IsLawful(f))
        for (auto& kv : systems_)
        {
            const SystemState& st = kv.second;
            if (!st.agg.claimed || st.agg.controller != f || upgrading.count(kv.first) != 0)
                continue;
            for (size_t i = 0; i < st.profile.outpostIds.size(); i++)
            {
                if (st.profile.outposts[i] != f)
                    continue;
                const Structure* outpost = nullptr;
                for (const auto& e : st.entities)
                    if (e->GetId() == st.profile.outpostIds[i] &&
                        e->GetKind() == EntityKind::Structure)
                        outpost = static_cast<const Structure*>(e.get());
                if (outpost == nullptr || outpost->IsBuilding())
                    continue;
                float             worth = 0.0f;
                const std::string bp = GrowthFor(f, st, *outpost, worth);
                const float       price = BlueprintCost(bp);
                if (bp.empty() || price <= 0.0f || mind.stock < price + reserve)
                    continue;
                float hostile = 0.0f;
                for (FactionId g : Factions::All())
                    if (Hostile(f, g))
                        hostile += st.agg.presence[g];
                // ...and what its convoy would expect to lose bringing what it is built from
                // (#407): a way it fears makes it weigh the upgrade against everything else.
                const float risk = hostile / t.capacity +
                                   Intelligence::LossRisk(mind.lostAt, kv.first, time_) +
                                   std::min(CONVOY_RISK_CAP, ChartConvoy(f, kv.first).loss);
                const float score = worth - risk / t.appetite;
                if (score > bestScore)
                {
                    bestScore = score;
                    act = Act::Upgrade;
                    from = kv.first;
                    grow = bp;
                }
            }
        }

    if (act == Act::Upgrade)
    {
        BeginUpgrade(f, from, grow);
        return;
    }
    if (act == Act::Anchor)
    {
        BeginAnchor(f, anchorSlot);
        return;
    }
    if (act == Act::Survey)
    {
        // How long it takes is drawn from what it is -- this faction, that system, this
        // minute of the world's clock -- never from how many draws came before.
        Gen::Rng rng(Gen::Key(SURVEY_KEY, (uint64_t)f.Index(), Intelligence::KeyOf(to),
                              (uint64_t)(time_ / 60.0)));
        Plan     p;
        p.id = nextPlanId_++;
        p.kind = Plan::Kind::Survey;
        p.faction = f;
        p.from = from;
        p.target = to;
        p.startedAt = time_;
        p.dueAt = time_ + SURVEY_TIME + rng.Range(0, SURVEY_JITTER);
        plans_[p.id] = p;
        due_.insert({ p.dueAt, p.id });
        return;
    }
    if (act == Act::Settle)
    {
        Settle(f, from, to);
        return;
    }
    if (act == Act::Siege)
    {
        // Arriving, it sees the place as it is: the outpost may have gone since, or the
        // system be a friend's now. Then it does nothing this period.
        SystemState& dest = systems_[to];
        mind.intel[to] = Observe(dest);
        // An Anchor may be besieged in a system it holds itself; an outpost never is.
        if (dest.agg.claimed && dest.agg.controller != f && !Hostile(f, dest.agg.controller))
            return;
        if (!againstAnchor && dest.agg.claimed && dest.agg.controller == f)
            return;
        const std::vector<FactionId>& whose =
            againstAnchor ? dest.profile.anchors : dest.profile.outposts;
        const std::vector<int>& ids =
            againstAnchor ? dest.profile.anchorIds : dest.profile.outpostIds;
        for (size_t i = 0; i < whose.size(); i++)
            if (whose[i] == enemy)
            {
                BeginSiege(f, lend, via, to, enemy, StaticKey(to, ids[i]));
                return;
            }
        return;
    }
    if (act != Act::Move)
        return;

    SystemAggregate& home = systems_[from].agg;
    SystemState&     dest = systems_[to];
    // Arriving, it sees the place as it is -- and if that is a friend's or a neutral's
    // after all, it turns back rather than start a war on old news.
    mind.intel[to] = Observe(dest);
    if (dest.agg.claimed && (dest.agg.controller == f || !Hostile(f, dest.agg.controller)))
        return;
    const float move =
        std::min(home.presence[f] - t.capacity * HOME_GUARD, t.capacity * MOVE_SHARE);
    const bool first = dest.agg.presence[f] < 1.0f && dest.agg.presence[f] + move >= 1.0f;
    home.presence[f] -= move;
    dest.agg.presence[f] += move;
    if (first)
        PushEvent(FactionName(f) + " move into " + SystemName(to) +
                  (why.empty() ? std::string() : ": " + why));
}

// 3. Who holds a system: the controller, until a hostile faction has had the upper hand
//    there for CONTEST_PASSES in a row (#225) -- unless the holder has an outpost there, which
//    has to fall first (#295's slice 5).
void Simulation::StepControl(bool settling)
{
    for (auto& kv : systems_)
    {
        SystemAggregate& a = kv.second.agg;
        // Nobody's system goes to whoever has held it longest in strength; a held one only
        // to an enemy of its holder.
        const float held = a.claimed ? a.presence[a.controller] : 0.0f;
        FactionId   challenger = a.controller;
        float       strongest = 0.0f;
        for (FactionId g : Factions::All())
            if ((!a.claimed || Hostile(g, a.controller)) && (g != a.controller || !a.claimed) &&
                a.presence[g] > strongest)
            {
                strongest = a.presence[g];
                challenger = g;
            }
        // And a faction must really have come: half its own capacity committed, which the
        // local trouble a lawless system breeds by itself (Unrest) never reaches. A gang is
        // not a faction moving in. Nor is strength alone a claim on a system nobody held: a
        // faction claims one by building there (#295), so it needs a finished outpost of its
        // own -- a settlement claims on completion, and this is the same rule for one whose
        // plan did not see it through.
        const auto& outposts = kv.second.profile.outposts;
        const bool  anchored =
            a.claimed || std::find(outposts.begin(), outposts.end(), challenger) != outposts.end();
        // And a held system whose holder has an outpost standing in it is not taken by
        // strength at all (slice 5): the outpost is the claim, and it has to be brought down
        // first -- by a siege, or by anyone's guns.
        const bool fortified = a.claimed && std::find(outposts.begin(), outposts.end(),
                                                      a.controller) != outposts.end();
        const bool upper = (challenger != a.controller || !a.claimed) && anchored && !fortified &&
                           strongest > held * 1.5f + 1.0f &&
                           strongest >= Factions::TemperamentOf(challenger).capacity * 0.5f;
        a.contested = upper ? a.contested + 1 : 0;
        if (a.contested < ContestPasses())
            continue;
        const bool wasClaimed = a.claimed;
        TakeControl(a, challenger);
        if (!settling)
            PushEvent(!wasClaimed ? FactionName(challenger) + " claim " + SystemName(kv.first)
                      : challenger == Factions::Pirates()
                          ? "Pirates seized " + SystemName(kv.first)
                          : SystemName(kv.first) + " taken by " + FactionName(challenger));
    }
}

// Who would lend a siege (#295): the faction's holdings within reach of the target, by the
// fewest jumps and then by id, each with what it has over and above its home guard -- at most
// what one holding would send -- until the siege has what it asked for.
std::vector<Plan::Lent> Simulation::PoolFor(FactionId f, const std::string& target, float want,
                                            std::string* via) const
{
    const Blueprints::Siege& r = Blueprints::SiegeRules();
    const Temperament&       t = Factions::TemperamentOf(f);
    const int                reach = std::max(1, (int)r.reach);
    // Out from the target a jump at a time, remembering which of its neighbours each system
    // is reached through: that is the gate a force from there comes in by.
    std::map<std::string, std::pair<int, std::string>> found;  // id -> (jumps, way in)
    std::vector<std::string>                           ring{ target };
    found[target] = { 0, std::string() };
    for (int hops = 1; hops <= reach && !ring.empty(); hops++)
    {
        std::vector<std::string> next;
        for (const std::string& id : ring)
            for (const std::string& n : Neighbors(id))
                if (systems_.count(n) != 0 && found.count(n) == 0)
                {
                    const std::string in = hops == 1 ? n : found[id].second;
                    found[n] = { hops, in };
                    next.push_back(n);
                }
        ring = std::move(next);
    }
    struct Offer
    {
        int         hops;
        std::string id;
        float       spare;
    };
    std::vector<Offer> offers;
    for (const auto& kv : found)
    {
        const SystemAggregate& a = systems_.at(kv.first).agg;
        if (kv.second.first == 0 || !a.claimed || a.controller != f)
            continue;
        const float spare =
            std::min(a.presence[f] - t.capacity * HOME_GUARD, t.capacity * r.commit);
        if (spare >= MIN_LEND)
            offers.push_back({ kv.second.first, kv.first, spare });
    }
    std::sort(offers.begin(), offers.end(), [](const Offer& a, const Offer& b)
              { return a.hops != b.hops ? a.hops < b.hops : a.id < b.id; });
    std::vector<Plan::Lent> lent;
    float                   total = 0.0f;
    for (const Offer& o : offers)
    {
        if (total >= want)
            break;
        Plan::Lent l;
        l.from = o.id;
        l.hops = o.hops;
        l.force = std::min(o.spare, want - total);
        total += l.force;
        lent.push_back(l);
    }
    if (via != nullptr)
        *via = lent.empty() ? std::string() : found[lent.front().from].second;
    return lent;
}
