// Outposts over time (#295, slice 5): what keeping one costs, what becomes of one nobody
// keeps, and how a faction brings down another's. One translation unit of Simulation (#17).
//
// An outpost is a claim (slice 3), and from here on a claim has a price and can be lost. Each
// period a faction pays for every outpost it holds out of the same stock that built it; one it
// cannot pay for, or one standing in a system it no longer holds, is cut off and falls apart,
// and a ruin is all that is left. A supplied one mends what it was hit for.
//
// A siege is the other way to lose one. A faction that sees an enemy's outpost next door,
// worth taking and within its strength, sends ships to stand at it and wear it down. Where
// nobody is that is arithmetic -- the besiegers' fire, less what the garrison ties up, and
// losses on both sides -- and where somebody is, the same arithmetic with warships to show
// for it, so a player can watch, help, or shoot the besiegers down. Both read the same numbers
// in the same pass, which is what keeps a world where somebody looked the same as one where
// nobody did. Its outpost destroyed, the claim it anchored goes with it.
//
// A siege is pooled (#295, the owner's decision of 2026-10-11): one holding's surplus is no
// match for a full garrison, so a faction draws on every holding of its within reach. Each
// lends what it can spare and is that much weaker while it is away; the force musters on the
// way and comes in once its farthest contingent has, a jump at a time; and whatever is left of
// it when the siege is over goes home the same way, each holding its share of the survivors.
#include "sim/Simulation.h"

#include "core/Blueprint.h"
#include "entities/NpcShip.h"
#include "entities/Station.h"
#include "entities/Structure.h"
#include "gen/Rng.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
// The coarse pass a siege is stepped by: MaintainWorld's two seconds.
constexpr float PASS_SECONDS = 2.0f;
// What keys a siege's length and its ships' places, besides who, where and when.
constexpr uint64_t SIEGE_KEY = 0x5E1Eu;
constexpr uint64_t FIELD_KEY = 0xF1E1Du;
// The most warships one siege puts in a system: a picture of the force, not every ship of it.
constexpr int MAX_FIELDED = 8;
// Besiegers fewer than this are no siege: they are driven off.
constexpr float MIN_FORCE = 1.0f;
// How far from an outpost's edge its besiegers stand: inside a cruiser's reach of it.
constexpr float STAND_OFF = 150.0f;
// Less than this of a contingent coming home is not worth the journey: it was lost.
constexpr float MIN_RETURN = 0.05f;

Structure* StructureWithId(SystemState& st, int id)
{
    for (const auto& e : st.entities)
        if (e->GetId() == id && e->GetKind() == EntityKind::Structure)
            return static_cast<Structure*>(e.get());
    return nullptr;
}

// "watch post": what an outpost is, without the article, for "the Traders Guild watch post".
std::string NounOf(const Structure& t)
{
    if (Simulation::IsAnchor(t.GetBuilds()))
        return "Anchor";  // at a wormhole mouth (#342 S4)
    return Outposts::Noun(t.GetBuilds());
}

float CostOf(const Blueprint* bp)
{
    float cost = 0.0f;
    if (bp != nullptr)
        for (const auto& c : bp->cost)
            cost += (float)c.second;
    return cost;
}

bool IsSiegeShip(const Entity& e, FactionId f)
{
    if (e.GetKind() != EntityKind::Npc)
        return false;
    const NpcShip& n = static_cast<const NpcShip&>(e);
    return n.GetRole() == NpcRole::Warship && n.GetFaction() == f && n.GetTask() == 0;
}
}  // namespace

float Simulation::SiegeCost()
{
    return Blueprints::SiegeRules().cost * OutpostCost();
}

Structure* Simulation::StructureByKey(SystemState& st, const std::string& key) const
{
    if (key.empty())
        return nullptr;
    for (const auto& kv : st.keys)
        if (kv.second == key)
            return StructureWithId(st, kv.first);
    return nullptr;
}

float Simulation::SiegeForceIn(const std::string& systemId, FactionId f) const
{
    float force = 0.0f;
    for (const auto& kv : plans_)
        if (kv.second.kind == Plan::Kind::Siege && kv.second.faction == f &&
            kv.second.target == systemId && kv.second.arrived)
            force += kv.second.force;
    return force;
}

float Simulation::LentFrom(const std::string& systemId, FactionId f) const
{
    // Lent to a siege, the whole of it until the siege is over; on the way home, what is left.
    float away = 0.0f;
    for (const auto& kv : plans_)
    {
        const Plan& p = kv.second;
        if (p.faction != f)
            continue;
        if (p.kind == Plan::Kind::Siege || p.OnTheWay())  // a convoy's escort too (#342 S6)
            for (const Plan::Lent& l : p.lent)
                away += l.from == systemId ? l.force : 0.0f;
        else if (p.kind == Plan::Kind::Return && p.target == systemId)
            away += p.force;
    }
    return away;
}

// A claim rests on what the faction has standing in the system: an outpost, or a station of
// its own. The last of them gone, the system is nobody's -- whoever wants it must build there,
// as anyone settling a system nobody holds does. Either way the garrison is shaken.
void Simulation::ShakeClaim(SystemState& st, FactionId f)
{
    SystemAggregate& a = st.agg;
    if (!a.claimed || a.controller != f)
        return;  // not its claim: an outpost in somebody else's system anchored nothing
    a.presence[f] *= 1.0f - Blueprints::SiegeRules().shock;
    bool anchored = std::find(st.profile.outposts.begin(), st.profile.outposts.end(), f) !=
                    st.profile.outposts.end();
    for (const auto& e : st.entities)
        if (!anchored && e->GetKind() == EntityKind::Station)
            anchored = static_cast<const Station&>(*e).GetFaction() == f;
    const std::string where = SystemName(st.id);
    if (anchored)
    {
        Record("claim", f, st.id, FactionName(f) + "'s hold on " + where + " is shaken");
        return;
    }
    a.claimed = false;
    a.controller = Factions::Unaffiliated();
    a.contested = 0;
    Announce("claim", f, st.id, FactionName(f) + " no longer holds " + where);
}

void Simulation::StepUpkeep()
{
    const Blueprints::Upkeep& u = Blueprints::UpkeepRules();
    for (auto& kv : systems_)
    {
        SystemState& st = kv.second;
        if (st.profile.outpostIds.empty())
            continue;
        // Copied: an outpost that falls into ruin changes the profile under the loop.
        const std::vector<int>       ids = st.profile.outpostIds;
        const std::vector<FactionId> owners = st.profile.outposts;
        for (size_t i = 0; i < ids.size(); i++)
        {
            Structure* t = StructureWithId(st, ids[i]);
            if (t == nullptr)
                continue;
            const FactionId   f = owners[i];
            FactionMind&      mind = minds_[f];
            const float       due = u.share * CostOf(t->GetBlueprint());
            const float       hull = t->MaxHull(time_);
            const std::string key = kv.first + "/" + StaticKey(kv.first, ids[i]);
            const std::string what = NounOf(*t) + " in " + SystemName(kv.first);
            // Supplied only where it holds: the stock goes along its own lanes, not into a
            // system somebody else holds or nobody does.
            const bool held = st.agg.claimed && st.agg.controller == f;
            if (held && mind.stock >= due)
            {
                mind.stock -= due;
                if (mind.unsupplied.erase(key) > 0)
                    Record("upkeep", f, kv.first,
                           FactionName(f) + " supplies its " + what + " again");
                if (t->GetDamage() > 0.0f && u.repair > 0.0f)
                {
                    t->SetDamage(std::max(0.0f, t->GetDamage() - u.repair * hull));
                    MarkStaticChanged(st, ids[i]);
                }
                continue;
            }
            if (mind.unsupplied.insert(key).second)
                Record("upkeep", f, kv.first,
                       held ? FactionName(f) + " cannot pay for its " + what +
                                  "; unsupplied, it is falling apart"
                            : FactionName(f) + " has abandoned its " + what + ", held by " +
                                  (st.agg.claimed ? FactionName(st.agg.controller) : "nobody"));
            if (u.decay > 0.0f)
                HitStructure(st, ids[i], u.decay * hull, nullptr, Loss::Decay, f);
        }
    }
}

void Simulation::BeginSiege(FactionId f, const std::vector<Plan::Lent>& lent,
                            const std::string& via, const std::string& to, FactionId against,
                            const std::string& site)
{
    SystemState* st = SystemById(to);
    if (st == nullptr || lent.empty())
        return;
    Structure* t = StructureByKey(*st, site);
    if (t == nullptr)
        return;
    const Blueprints::Siege& r = Blueprints::SiegeRules();
    // Each holding is short of what it lent from the moment it sets out; the force is counted
    // nowhere until it stands at the outpost.
    std::vector<Plan::Lent> sent;
    float                   force = 0.0f;
    int                     hops = 1;
    std::string             whence;
    for (const Plan::Lent& l : lent)
    {
        SystemState* home = SystemById(l.from);
        if (home == nullptr || l.force <= 0.0f)
            continue;
        float&     there = home->agg.presence[f];
        Plan::Lent took = l;
        took.force = std::min(l.force, there);
        if (took.force <= 0.0f)
            continue;
        there -= took.force;
        force += took.force;
        hops = std::max(hops, took.hops);
        whence += std::string(whence.empty() ? "" : ", ") + SystemName(took.from);
        sent.push_back(took);
    }
    if (sent.empty())
        return;
    minds_[f].stock -= SiegeCost();

    // How long they will stay is drawn from what is decided -- this faction, that system, this
    // minute of the world's clock -- never from how many draws came before.
    Gen::Rng rng(Gen::Key(SIEGE_KEY, (uint64_t)f.Index(), Intelligence::KeyOf(to),
                          (uint64_t)(time_ / 60.0)));
    Plan     p;
    p.id = nextPlanId_++;
    p.kind = Plan::Kind::Siege;
    p.faction = f;
    p.from = sent.front().from;  // the nearest
    p.via = via.empty() ? p.from : via;
    p.target = to;
    p.startedAt = time_;
    p.arrivesAt = time_ + hops * (double)r.hop;
    p.dueAt = p.arrivesAt + r.seconds + rng.Range(0, std::max(0, (int)r.jitter));
    p.site = site;
    p.against = against;
    p.force = force;
    p.lent = sent;
    plans_[p.id] = p;
    due_.insert({ p.dueAt, p.id });
    char ships[32];
    std::snprintf(ships, sizeof ships, "%.0f", std::max(1.0, std::round((double)force)));
    Record("siege", f, to,
           FactionName(f) + " sent " + ships + " ships from " + whence + " against the " +
               FactionName(against) + " " + NounOf(*t) + " in " + SystemName(to));
    if (p.arrivesAt <= time_ && !ArriveSiege(plans_[p.id]))
    {
        RecallSiege(plans_[p.id]);
        plans_.erase(p.id);
    }
}

bool Simulation::ArriveSiege(Plan& p)
{
    SystemState*     st = SystemById(p.target);
    const Structure* t = st != nullptr ? StructureByKey(*st, p.site) : nullptr;
    if (t == nullptr)
    {
        Record("siege", p.faction, p.target,
               FactionName(p.faction) + " turned back from " + SystemName(p.target) + ": the " +
                   FactionName(p.against) + " outpost is gone");
        return false;
    }
    p.arrived = true;
    st->agg.presence[p.faction] += p.force;
    Announce("siege", p.faction, p.target,
             FactionName(p.faction) + " laid siege to the " + FactionName(p.against) + " " +
                 NounOf(*t) + " in " + SystemName(p.target));
    FieldSiege(*st, p, true);
    return true;
}

void Simulation::ResolveSiege(const Plan& p)
{
    SiegeOver(p);  // its jobs (#345)
    RecallSiege(p);
    SystemState*      st = SystemById(p.target);
    const Structure*  t = st != nullptr ? StructureByKey(*st, p.site) : nullptr;
    const std::string what = t != nullptr ? FactionName(p.against) + " " + NounOf(*t) : "outpost";
    Record("siege", p.faction, p.target,
           FactionName(p.faction) + " gave up the siege of the " + what + " in " +
               SystemName(p.target));
}

void Simulation::RecallSiege(const Plan& p)
{
    SystemState* st = SystemById(p.target);
    if (st != nullptr && p.arrived)
    {
        // What is left of the force is no longer the besieger's strength here; the warships
        // that stood for it go. (A siege from an older save was lent by nobody, and what is
        // left of it stays, as presence that fades like any other.)
        float& there = st->agg.presence[p.faction];
        if (!p.lent.empty())
            there = std::max(0.0f, there - p.force);
        if (IsHot(*st))
            st->entities.erase(std::remove_if(st->entities.begin(), st->entities.end(),
                                              [&](const std::unique_ptr<Entity>& e)
                                              { return IsSiegeShip(*e, p.faction); }),
                               st->entities.end());
    }
    // ...and goes home, each holding its share of what survived, as many jumps away as it came.
    float lent = 0.0f;
    for (const Plan::Lent& l : p.lent)
        lent += l.force;
    const double hop = Blueprints::SiegeRules().hop;
    for (const Plan::Lent& l : p.lent)
    {
        const float back = lent > 0.0f ? p.force * l.force / lent : 0.0f;
        if (back < MIN_RETURN || !HasSystem(l.from))
            continue;
        Plan r;
        r.id = nextPlanId_++;
        r.kind = Plan::Kind::Return;
        r.faction = p.faction;
        r.from = p.target;
        r.target = l.from;
        r.startedAt = time_;
        r.dueAt = time_ + l.hops * hop;
        r.force = back;
        plans_[r.id] = r;
        due_.insert({ r.dueAt, r.id });
    }
}

void Simulation::ResolveReturn(const Plan& p)
{
    // Home, whoever holds it now: strength a faction no longer keeps there fades like any other.
    if (SystemState* home = SystemById(p.target))
        home->agg.presence[p.faction] += p.force;
}

void Simulation::FieldSieges(SystemState& st)
{
    for (auto& kv : plans_)
        if (kv.second.kind == Plan::Kind::Siege && kv.second.target == st.id)
            FieldSiege(st, kv.second, false);
}

void Simulation::FieldSiege(SystemState& st, Plan& p, bool arriving)
{
    if (!IsHot(st) || p.fielded > 0 || !p.arrived)
        return;
    Structure* t = StructureByKey(st, p.site);
    if (t == nullptr)
        return;
    const int n = std::min(MAX_FIELDED, (int)std::lround(p.force));
    // Where they stand is drawn from the siege and the second, like a warming system's ships:
    // two players arriving together see the same thing.
    Gen::Rng rng(Gen::Key(FIELD_KEY, (uint64_t)p.id, (uint64_t)std::max(0.0, std::floor(time_))));
    const Vector2 at = t->GetPosition();
    const float   ring = t->GetSize() + STAND_OFF;
    auto          around = [&](Vector2 c, float r0, float r1) -> Vector2
    {
        const float turn = (float)(rng.Unit() * 2.0 * PI);
        const float d = (float)rng.Between((double)r0, (double)r1);
        return Vector2{ c.x + d * std::cos(turn), c.y + d * std::sin(turn) };
    };
    // They circle the outpost, within reach of it.
    std::vector<Vector2> beat;
    for (int k = 0; k < 4; k++)
        beat.push_back(around(at, ring, ring + 100.0f));
    // Just arrived, they come in through the gate from where they set out, and fly to it;
    // found there by somebody arriving later, they are at it already.
    const Vector2 gate = arriving ? ArrivalFrom(st.id, p.via) : at;
    for (int i = 0; i < n; i++)
        SpawnNpcInto(st, arriving ? around(gate, 0.0f, 400.0f) : around(at, ring, ring + 400.0f),
                     p.faction, NpcRole::Warship, beat, i);
    p.fielded = n;
}

void Simulation::StepSieges()
{
    const Blueprints::Siege& r = Blueprints::SiegeRules();
    std::vector<int>         over;
    for (auto& kv : plans_)
    {
        Plan& p = kv.second;
        if (p.kind != Plan::Kind::Siege)
            continue;
        SystemState* st = SystemById(p.target);
        if (st == nullptr)
        {
            over.push_back(p.id);
            continue;
        }
        // On the way: nothing to fight yet. Arriving to find the outpost gone, it turns back.
        if (!p.arrived)
        {
            if (time_ >= p.arrivesAt && !ArriveSiege(p))
                over.push_back(p.id);
            continue;
        }
        SystemAggregate& a = st->agg;
        const FactionId  fi = p.faction, gi = p.against;

        // Where it is real, a besieger a player shot down is one fewer: what the arithmetic
        // below does not know about, and the one way a player changes a siege's course.
        if (IsHot(*st))
        {
            FieldSiege(*st, p, false);
            int alive = 0;
            for (const auto& e : st->entities)
                if (IsSiegeShip(*e, p.faction) && static_cast<const NpcShip&>(*e).IsAlive())
                    alive++;
            if (alive < p.fielded)
            {
                const float lost = (float)(p.fielded - alive);
                p.force = std::max(0.0f, p.force - lost);
                a.presence[fi] = std::max(0.0f, a.presence[fi] - lost);
            }
            p.fielded = alive;
        }
        else
            p.fielded = 0;

        Structure* t = StructureByKey(*st, p.site);
        if (t == nullptr)
        {
            // Brought down by somebody else, or fallen into ruin: nothing left to besiege.
            Record("siege", fi, p.target,
                   FactionName(p.faction) + " ended the siege in " + SystemName(p.target) +
                       ": the " + FactionName(p.against) + " outpost is gone");
            over.push_back(p.id);
            continue;
        }

        // Both sides bleed, each by the other's numbers.
        const float defenders = a.presence[gi];
        const float lostHere = std::min(p.force, r.attrition * defenders * PASS_SECONDS);
        const float lostThere = std::min(defenders, r.attrition * p.force * PASS_SECONDS);
        p.force -= lostHere;
        a.presence[fi] = std::max(0.0f, a.presence[fi] - lostHere);
        a.presence[gi] = std::max(0.0f, a.presence[gi] - lostThere);
        if (p.force < MIN_FORCE)
        {
            Announce("siege", gi, p.target,
                     FactionName(p.against) + " drove the " + FactionName(p.faction) +
                         " besiegers from its " + NounOf(*t) + " in " + SystemName(p.target));
            over.push_back(p.id);
            continue;
        }
        // What the garrison does not tie up fires at the outpost.
        const float firing = std::max(0.0f, p.force - r.defence * a.presence[gi]);
        if (firing > 0.0f && HitStructure(*st, t->GetId(), firing * r.dps * PASS_SECONDS, nullptr,
                                          Loss::Siege, p.faction))
            over.push_back(p.id);
    }
    for (int id : over)
    {
        const auto it = plans_.find(id);
        if (it == plans_.end())
            continue;
        SiegeOver(it->second);  // its jobs (#345)
        RecallSiege(it->second);
        plans_.erase(it);  // its due entry finds nothing
    }
}
