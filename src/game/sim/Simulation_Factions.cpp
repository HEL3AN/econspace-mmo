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
#include "sim/Simulation.h"

#include "entities/AsteroidField.h"
#include "entities/Derelict.h"
#include "entities/Station.h"
#include "gen/Rng.h"
#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <vector>

namespace
{
// How many macro passes make one faction period: a minute at the 2 s pass.
constexpr int FACTION_PERIOD = 30;
// A holding keeps this share of its capacity at home; only what is above it can reach out.
constexpr float HOME_GUARD = 0.6f;
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

bool Hostile(FactionId a, FactionId b)
{
    const Stance s = Factions::Relation(a, b);
    return a != b && (s == Stance::War || s == Stance::Hostile);
}

// Lawless places breed their own trouble: in a system the pirates do not hold, their
// presence drifts towards this however nobody sends them, and no further.
float Unrest(const SystemAggregate& a)
{
    const float cap = Factions::TemperamentOf(FactionId::Pirates).capacity;
    return std::max(0.0f, 0.6f - a.security) * cap * 0.4f;
}
}  // namespace

void Simulation::SeedPresence(SystemAggregate& a)
{
    a.presence.fill(0.0f);
    // Half strength to begin with: a fresh world grows into its numbers rather than
    // starting at them, so nothing is decided in its first minutes (#143, #225). Nobody's
    // system has nobody's garrison.
    if (a.claimed)
        a.presence[(int)a.controller] = Factions::TemperamentOf(a.controller).capacity * 0.5f;
    if (a.controller != FactionId::Pirates)
        a.presence[(int)FactionId::Pirates] = Unrest(a);
}

SystemProfile Simulation::ProfileOf(const SystemState& st)
{
    SystemProfile p;
    for (const auto& e : st.entities)
        switch (e->GetKind())
        {
            case EntityKind::Field: p.belts++; break;
            case EntityKind::Derelict: p.wrecks++; break;
            case EntityKind::Planet: p.planets++; break;
            case EntityKind::Gate: p.gates++; break;
            case EntityKind::Station:
                p.stations++;
                if (e->Has(Component::Defensive))
                    p.defenders.push_back(static_cast<const Station*>(e.get())->GetFaction());
                break;
            default: break;
        }
    return p;
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
    i.defenders = st.profile.defenders;
    i.presence = st.agg.presence;
    i.controller = st.agg.controller;
    i.claimed = st.agg.claimed;
    return i;
}

void Simulation::Record(const std::string& kind, int faction, const std::string& system,
                        const std::string& text)
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
        FactionMind& m = minds_[(int)a.controller];
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
    for (int g = 0; g < FACTION_COUNT; g++)
        if ((FactionId)g != by && seen.presence[g] >= 1.0f &&
            !(seen.claimed && (FactionId)g == seen.controller))
            add((FactionId)g == FactionId::Pirates ? std::string("pirates")
                                                   : FactionName((FactionId)g) + " ships");
    return text.empty() ? "nothing of note" : text;
}

void Simulation::ResolveSurvey(const Plan& p)
{
    const auto st = systems_.find(p.target);
    if (st == systems_.end())
        return;  // the galaxy no longer has it; the survey comes back with nothing
    const Intel seen = Observe(st->second);
    minds_[(int)p.faction].intel[p.target] = seen;
    Record("survey", (int)p.faction, p.target,
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
        }
    }
}

void Simulation::StepFactions()
{
    // Losses in battle are losses of presence: whatever the director spawned and somebody
    // destroyed was strength the faction had committed there.
    for (auto& kv : systems_)
    {
        SystemAggregate& a = kv.second.agg;
        if (a.lostPirates > 0.0f)
            a.presence[(int)FactionId::Pirates] =
                std::max(0.0f, a.presence[(int)FactionId::Pirates] - a.lostPirates);
        if (a.lostPolice > 0.0f)
        {
            const FactionId law = a.policeFaction;
            a.presence[(int)law] = std::max(0.0f, a.presence[(int)law] - a.lostPolice);
        }
        a.lostPirates = a.lostPolice = 0.0f;
    }

    if (!mindsSeeded_)
        SeedMinds();
    ResolveDuePlans();

    // Each faction thinks once a period, on a pass of its own (#295): spread across the
    // period rather than all on one pass, so no pass carries more than one of them.
    static_assert(FACTION_COUNT <= FACTION_PERIOD, "more than one faction would think per pass");
    const int phase = ++factionPasses_ % FACTION_PERIOD;
    for (int fi = 0; fi < FACTION_COUNT; fi++)
        if (phase == fi * FACTION_PERIOD / FACTION_COUNT)
            Think((FactionId)fi);
    if (phase != 0)
        return;

    // Once a period, holdings recover towards capacity; everything else fades towards what
    // the place breeds by itself, unless its faction keeps sending more.
    for (auto& kv : systems_)
    {
        SystemAggregate& a = kv.second.agg;
        for (int f = 0; f < FACTION_COUNT; f++)
        {
            const Temperament& t = Factions::TemperamentOf((FactionId)f);
            float&             p = a.presence[f];
            if ((FactionId)f == a.controller && a.claimed)
                p += t.growth * (t.capacity - p);
            else
            {
                const float floor = (FactionId)f == FactionId::Pirates ? Unrest(a) : 0.0f;
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
// surveyors to a neighbour it knows nothing, or nothing recent, about.
void Simulation::Think(FactionId f)
{
    const int          fi = (int)f;
    const Temperament& t = Factions::TemperamentOf(f);
    if (t.appetite <= 0.0f)
        return;  // holds what it has and reaches for nothing
    FactionMind& mind = minds_[fi];

    int holdings = 0;
    for (auto& kv : systems_)
    {
        const SystemAggregate& a = kv.second.agg;
        const bool             held = a.claimed && a.controller == f;
        holdings += held ? 1 : 0;
        if (held || a.presence[fi] >= 1.0f)
            mind.intel[kv.first] = Observe(kv.second);
    }

    // What it already has under way: one survey of a system at a time, and no more at once
    // than its holdings can send.
    std::set<std::string> surveying;
    for (const auto& kv : plans_)
        if (kv.second.faction == f && kv.second.kind == Plan::Kind::Survey)
            surveying.insert(kv.second.target);
    const bool canSurvey = (int)surveying.size() < 1 + holdings / PLANS_PER_HOLDINGS;

    enum class Act
    {
        None,
        Move,
        Survey
    };
    Act         act = Act::None;
    float       bestScore = 0.0f;
    std::string from, to, why;
    for (auto& kv : systems_)
    {
        const SystemAggregate& home = kv.second.agg;
        if (!home.claimed || home.controller != f)
            continue;
        const float surplus = home.presence[fi] - t.capacity * HOME_GUARD;
        for (const std::string& nid : Neighbors(kv.first))
        {
            if (systems_.count(nid) == 0)
                continue;
            const auto   k = mind.intel.find(nid);
            const Intel* seen = k == mind.intel.end() ? nullptr : &k->second;

            // Looking: worth its curiosity where it has never been, and again as what it
            // saw goes stale -- unless what it saw was a friend's or a neutral's, which it
            // could not take whatever has changed there. The risk is the unknown, plus
            // whatever hostile it saw there.
            const bool closed = seen != nullptr && seen->claimed &&
                                (seen->controller == f || !Hostile(f, seen->controller));
            if (canSurvey && !closed && surveying.count(nid) == 0)
            {
                const float value = Intelligence::SurveyValue(t.curiosity, seen, time_);
                float       hostile = 0.0f;
                if (seen != nullptr)
                    for (int g = 0; g < FACTION_COUNT; g++)
                        if (Hostile(f, (FactionId)g))
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

            // Reaching: only where it has looked, and only with strength to spare.
            if (seen == nullptr || surplus < 0.5f)
                continue;
            // Only into a system it may take: nobody's, or an enemy's. Never a friend's
            // or a neutral power's -- that would be a war nobody declared.
            if (closed)
                continue;

            const SystemOffer o = OfferOf(*seen);
            const float       unclaimed = seen->claimed ? 0.0f : 1.0f;
            const float value = t.traffic * o.traffic + t.ore * o.ore + t.salvage * o.salvage +
                                t.unclaimed * unclaimed;
            float       hostile = 0.0f;
            for (int g = 0; g < FACTION_COUNT; g++)
                if (Hostile(f, (FactionId)g) ||
                    ((FactionId)g == seen->controller && seen->controller != f))
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

    if (act == Act::Survey)
    {
        // How long it takes is drawn from what it is -- this faction, that system, this
        // minute of the world's clock -- never from how many draws came before.
        Gen::Rng rng(
            Gen::Key(SURVEY_KEY, (uint64_t)fi, Intelligence::KeyOf(to), (uint64_t)(time_ / 60.0)));
        Plan p;
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
        std::min(home.presence[fi] - t.capacity * HOME_GUARD, t.capacity * MOVE_SHARE);
    const bool first = dest.agg.presence[fi] < 1.0f && dest.agg.presence[fi] + move >= 1.0f;
    home.presence[fi] -= move;
    dest.agg.presence[fi] += move;
    if (first)
        PushEvent(FactionName(f) + " move into " + SystemName(to) +
                  (why.empty() ? std::string() : ": " + why));
}

// 3. Who holds a system: the controller, until a hostile faction has had the upper hand
//    there for CONTEST_PASSES in a row (#225).
void Simulation::StepControl(bool settling)
{
    for (auto& kv : systems_)
    {
        SystemAggregate& a = kv.second.agg;
        // Nobody's system goes to whoever has held it longest in strength; a held one only
        // to an enemy of its holder.
        const float held = a.claimed ? a.presence[(int)a.controller] : 0.0f;
        FactionId   challenger = a.controller;
        float       strongest = 0.0f;
        for (int g = 0; g < FACTION_COUNT; g++)
            if ((!a.claimed || Hostile((FactionId)g, a.controller)) &&
                ((FactionId)g != a.controller || !a.claimed) && a.presence[g] > strongest)
            {
                strongest = a.presence[g];
                challenger = (FactionId)g;
            }
        // And a faction must really have come: half its own capacity committed, which the
        // local trouble a lawless system breeds by itself (Unrest) never reaches. A gang is
        // not a faction moving in.
        const bool upper = (challenger != a.controller || !a.claimed) &&
                           strongest > held * 1.5f + 1.0f &&
                           strongest >= Factions::TemperamentOf(challenger).capacity * 0.5f;
        a.contested = upper ? a.contested + 1 : 0;
        if (a.contested < ContestPasses())
            continue;
        a.contested = 0;
        const FactionId was = a.controller;
        const bool      wasClaimed = a.claimed;
        a.controller = challenger;
        a.claimed = true;
        if (challenger == FactionId::Pirates)
            a.baseSecurity = std::min(a.baseSecurity, 0.15f);
        else if (was == FactionId::Pirates)
            a.baseSecurity = std::max(a.baseSecurity, 0.5f);
        if (!settling)
            PushEvent(!wasClaimed ? FactionName(challenger) + " claim " + SystemName(kv.first)
                      : challenger == FactionId::Pirates
                          ? "Pirates seized " + SystemName(kv.first)
                          : SystemName(kv.first) + " taken by " + FactionName(challenger));
    }
}
