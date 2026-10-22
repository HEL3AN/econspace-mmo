// A corporation's own AI patrols (#431 C1, slice 12).
//
// One translation unit of Simulation (#17). A patrol is presence, as a faction's ships are: a
// number of ships in a system's aggregate (`SystemAggregate::patrols`, by corporation), which
// the coarse pass pays for out of the corporation's treasury and makes good, and which fights
// and is lost by the same arithmetic as any NPC -- the cold pass against pirates, a siege's
// attrition at a member's structure -- from the design it flies (one combat model, #186).
// Where somebody is, the number flies as guards: NPC ships of the patrol's design that are
// no faction's but the corporation's, and that pick their fights by its rules of engagement,
// the same rules a member's structure keeps (#340).
//
// Why a role and not a faction: a corporation is not a faction (#347). A guard has no place
// in the relation matrix, no PerFaction slot and no doctrine of its own; it takes sides by
// its corporation's membership and settings, which only the corporation registry knows.

#include "sim/Simulation.h"
#include "core/Archetype.h"
#include "core/Faction.h"
#include "core/Fitting.h"
#include "core/ShipDesign.h"
#include "entities/NpcShip.h"
#include "entities/Structure.h"
#include "gen/Rng.h"
#include "sim/ClientSession.h"
#include "sim/FactionMind.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace
{
constexpr uint64_t PATROL_KEY = 0x9A7201u;
// Where guards fly: a ring this far out around each structure they guard.
constexpr float GUARD_RING = 600.0f;
// How far from a member's ship under fire a guard hears of it.
constexpr float GUARD_HEARS = 3500.0f;

std::string Tag(const Corps::Corp& c)
{
    return c.name + " [" + c.ticker + "]";
}

std::string Lower(std::string s)
{
    for (char& ch : s)
        ch = (char)std::tolower((unsigned char)ch);
    return s;
}

// The strength a design brings to a fight, for what keeping it costs: guns times hull.
double Strength(const Ships::Fighter& f)
{
    return (double)f.dps * (double)f.hull;
}
}  // namespace

const Corps::Patrol* Simulation::PatrolOf(const std::string& corpId,
                                          const std::string& systemId) const
{
    const Corps::Corp* c = corps_.Find(corpId);
    if (c == nullptr)
        return nullptr;
    const auto it = c->patrols.find(systemId);
    return it != c->patrols.end() ? &it->second : nullptr;
}

std::string Simulation::PatrolDesign(const Corps::Patrol& p)
{
    const Ships::Catalogue& cat = Archetypes::ShipCatalogue();
    if (!p.design.empty() && cat.FindDesign(p.design) != nullptr)
        return p.design;
    return cat.Pick("default", "guard", 0);
}

Ships::Fighter Simulation::PatrolFighter(const Corps::Patrol& p)
{
    return Ships::DesignFighter(Archetypes::ShipCatalogue(), PatrolDesign(p));
}

double Simulation::PatrolUpkeep(const Corps::Patrol& p)
{
    // A ship of the default guard costs the rules' `shipHour`; one that fights harder costs as
    // much more, so the choice of design is the corporation's and so is the bill.
    const Ships::Catalogue& cat = Archetypes::ShipCatalogue();
    const double base = Strength(Ships::DesignFighter(cat, cat.Pick("default", "guard", 0)));
    const double own = Strength(PatrolFighter(p));
    const double weight = base > 0.0 && own > 0.0 ? own / base : 1.0;
    return WorldRules::PatrolRules().shipHour * (double)std::max(0, p.ships) * weight;
}

bool Simulation::PatrolAllowed(const SystemState& st, const std::string& corpId) const
{
    if (corpId.empty() || corps_.Find(corpId) == nullptr)
        return false;
    if (st.agg.holderCorp == corpId)
        return true;
    for (const auto& e : st.entities)
        if (e->GetKind() == EntityKind::Structure && !e->GetOwner().empty() &&
            corps_.IdOf(e->GetOwner()) == corpId)
            return true;
    return false;
}

void Simulation::StepPatrols(double seconds)
{
    const WorldRules::Patrols&                        r = WorldRules::PatrolRules();
    std::map<std::string, std::map<std::string, int>> kept;   // system -> corp -> ships paid
    std::set<std::pair<std::string, std::string>>     broke;  // (system, corp) unpaid
    std::vector<std::string>                          ids;    // Debit changes a corporation
    for (const auto& kv : corps_.All())
        ids.push_back(kv.first);
    for (const std::string& id : ids)
    {
        const Corps::Corp* c = corps_.Find(id);
        if (c == nullptr)
            continue;
        const std::map<std::string, Corps::Patrol> patrols = c->patrols;  // a copy, as Debit runs
        for (const auto& p : patrols)
        {
            const SystemState* st = SystemById(p.first);
            if (st == nullptr || p.second.ships <= 0 || !PatrolAllowed(*st, id))
                continue;  // nothing to guard there now: it stands down, unpaid
            if (corps_.Debit(id, PatrolUpkeep(p.second) * seconds / 3600.0))
                kept[p.first][id] = std::min(p.second.ships, r.systemMax);
            else
                broke.insert({ p.first, id });
        }
    }
    // A paid patrol makes good what it lost, a ship every `regrowHours`; the rest stand down.
    const float regrow = (float)(seconds / (r.regrowHours * 3600.0));
    for (auto& kv : systems_)
    {
        std::map<std::string, float>& here = kv.second.agg.patrols;
        const auto                    paid = kept.find(kv.first);
        if (paid != kept.end())
            for (const auto& pk : paid->second)
            {
                float& n = here[pk.first];
                n = std::min((float)pk.second, n + regrow);
            }
        for (auto it = here.begin(); it != here.end();)
        {
            const bool keep = paid != kept.end() && paid->second.count(it->first) != 0;
            if (keep)
            {
                ++it;
                continue;
            }
            // Out of money: said once, in the corporation's journal, which its members read.
            const Corps::Corp* c = corps_.Find(it->first);
            if (it->second > 0.0f && broke.count({ kv.first, it->first }) != 0 && c != nullptr &&
                c->Ceo() != nullptr)
            {
                const int seq = corps_.Record(c->Ceo()->account, Ev::Kind::Notice,
                                              Tag(*c) + " can no longer pay for its patrol in " +
                                                  SystemName(kv.first) +
                                                  ": the treasury is empty, and it stands down");
                for (auto& sv : sessions_)
                    if (seq != 0 && corps_.IdOf(sv.second.accountName) == c->id)
                        ReadCorpJournal(sv.second);
            }
            it = here.erase(it);
        }
    }
}

void Simulation::FieldPatrols(SystemState& st)
{
    if (!IsHot(st))
        return;
    // The living, by corporation.
    std::map<std::string, std::vector<NpcShip*>> alive;
    for (const auto& e : st.entities)
        if (e->GetKind() == EntityKind::Npc)
        {
            NpcShip* n = static_cast<NpcShip*>(e.get());
            if (n->GetRole() == NpcRole::Guard && n->IsAlive())
                alive[n->GetCorp()].push_back(n);
        }
    // A guard shot down since the last look is one ship fewer: what the arithmetic does not
    // know about, as a besieger a player shot down is.
    SystemAggregate& a = st.agg;
    for (const auto& f : st.patrolFielded)
    {
        const int  now = (int)alive[f.first].size();
        const auto it = a.patrols.find(f.first);
        if (now < f.second && it != a.patrols.end())
            it->second = std::max(0.0f, it->second - (float)(f.second - now));
    }
    st.patrolFielded.clear();

    std::set<std::string> corps;
    for (const auto& kv : a.patrols)
        corps.insert(kv.first);
    for (const auto& kv : alive)
        corps.insert(kv.first);
    std::set<const Entity*> letGo;
    for (const std::string& corp : corps)
    {
        const auto             it = a.patrols.find(corp);
        const Corps::Patrol*   set = PatrolOf(corp, st.id);
        const int              want = it == a.patrols.end() || set == nullptr
                                          ? 0
                                          : std::min(WorldRules::PatrolRules().systemMax,
                                                     (int)std::floor(it->second + 1e-4f));
        std::vector<NpcShip*>& have = alive[corp];
        // Fewer than fly: the last made go first, the same ones on every server.
        std::sort(have.begin(), have.end(),
                  [](const NpcShip* x, const NpcShip* y) { return x->GetId() < y->GetId(); });
        while ((int)have.size() > want)
        {
            letGo.insert(have.back());
            have.pop_back();
        }
        if (want > 0)
            st.patrolFielded[corp] = want;
        if ((int)have.size() >= want)
            continue;
        // More: made around what they guard -- the members' structures here, else the
        // holder's beat -- drawn from the corporation and the second, as a siege's are.
        const Corps::Corp*   c = corps_.Find(corp);
        std::vector<Vector2> around;
        for (const auto& e : st.entities)
            if (e->GetKind() == EntityKind::Structure && !e->GetOwner().empty() &&
                corps_.IdOf(e->GetOwner()) == corp)
                around.push_back(e->GetPosition());
        if (around.empty())
            around.push_back(SafeArrival(st.id));
        Gen::Rng rng(Gen::Key(PATROL_KEY, Intelligence::KeyOf(corp), Intelligence::KeyOf(st.id),
                              (uint64_t)std::max(0.0, std::floor(time_))));
        auto     ring = [&](Vector2 at, float r0, float r1)
        {
            const float turn = (float)(rng.Unit() * 2.0 * PI);
            const float d = (float)rng.Between((double)r0, (double)r1);
            return Vector2{ at.x + d * std::cos(turn), at.y + d * std::sin(turn) };
        };
        std::vector<Vector2> beat;
        for (const Vector2& at : around)
            for (int k = 0; k < 2; k++)
                beat.push_back(ring(at, GUARD_RING, GUARD_RING + 300.0f));
        const std::string design = PatrolDesign(*set);
        const std::string label = c != nullptr ? "[" + c->ticker + "] patrol" : "patrol";
        for (int i = (int)have.size(); i < want; i++)
        {
            const Vector2 at = around[(size_t)i % around.size()];
            auto          g =
                std::make_unique<NpcShip>(ring(at, GUARD_RING, GUARD_RING + 300.0f),
                                          Factions::Unaffiliated(), NpcRole::Guard, beat, design);
            g->SetId(NextAgentId());
            g->SetCorp(corp, label);
            // Whose word its first shots are: the CEO's, as a structure's are its owner's.
            if (c != nullptr && c->Ceo() != nullptr)
                g->SetOwner(c->Ceo()->account);
            st.entities.push_back(std::move(g));
        }
    }
    if (!letGo.empty())
        st.entities.erase(std::remove_if(st.entities.begin(), st.entities.end(),
                                         [&](const std::unique_ptr<Entity>& e)
                                         { return letGo.count(e.get()) != 0; }),
                          st.entities.end());
}

bool Simulation::GuardProvoked(const SystemState& st, const NpcShip& g, int session, int npc) const
{
    auto lately = [&](const std::map<int, std::map<int, double>>& book, int entity, int who)
    {
        const auto of = book.find(entity);
        if (of == book.end() || who == 0)
            return false;
        const auto when = of->second.find(who);
        return when != of->second.end() && time_ < when->second;
    };
    if (lately(st.provokedBy, g.GetId(), session) || lately(st.provokedByNpc, g.GetId(), npc))
        return true;
    // ...or a member's structure here was shot by them, or a member's ship in its reach.
    for (const auto& e : st.entities)
        if (e->GetKind() == EntityKind::Structure && !e->GetOwner().empty() &&
            corps_.IdOf(e->GetOwner()) == g.GetCorp() &&
            (lately(st.provokedBy, e->GetId(), session) ||
             lately(st.provokedByNpc, e->GetId(), npc)))
            return true;
    return false;
}

void Simulation::ProvokePatrol(SystemState& st, const std::string& corp, Vector2 at, int bySession,
                               int byNpc)
{
    if (corp.empty())
        return;
    const double until = time_ + PROVOKED_SECONDS;
    for (const auto& e : st.entities)
    {
        if (e->GetKind() != EntityKind::Npc)
            continue;
        const NpcShip& g = static_cast<const NpcShip&>(*e);
        if (g.GetRole() != NpcRole::Guard || g.GetCorp() != corp)
            continue;
        const float dx = at.x - g.GetPosition().x, dy = at.y - g.GetPosition().y;
        if (dx * dx + dy * dy > GUARD_HEARS * GUARD_HEARS)
            continue;
        if (bySession != 0)
            st.provokedBy[g.GetId()][bySession] = until;
        if (byNpc != 0 && byNpc != g.GetId())
            st.provokedByNpc[g.GetId()][byNpc] = until;
    }
}

bool Simulation::GuardHunts(const SystemState& st, const NpcShip& g, const ClientSession& p) const
{
    const Corps::Patrol* set = PatrolOf(g.GetCorp(), st.id);
    if (set == nullptr || set->rules.engage == Engage::Hold)
        return false;
    // A member is a friend in every mode (#347): never fired on, not even back -- the
    // corporation's own ships are its trust made solid.
    if (!p.accountName.empty() && corps_.IdOf(p.accountName) == g.GetCorp())
        return false;
    if (GuardProvoked(st, g, p.id, 0))
        return true;
    const Defence& r = set->rules;
    switch (r.engage)
    {
        case Engage::Hold:
        case Engage::Return: return false;
        case Engage::Enemies:
            // A flagged criminal is anyone's to shoot (#431 L4), and one the law wants or a
            // faction the corporation stands badly with holds hostile is fair game.
            if (p.account.IsCriminal())
                return true;
            for (FactionId f : Factions::All())
                if (Factions::IsLawful(f) && p.account.IsWanted(f))
                    return true;
            return false;
        case Engage::Strangers: return !r.Allows(p.accountName);
    }
    return false;
}

bool Simulation::GuardEngages(const SystemState& st, const NpcShip& g, const NpcShip& n) const
{
    if (n.GetRole() == NpcRole::Guard && n.GetCorp() == g.GetCorp())
        return false;
    if (n.IsPirate())
        return true;  // everyone's enemy, and they are the guards'
    const Corps::Patrol* set = PatrolOf(g.GetCorp(), st.id);
    if (set == nullptr || set->rules.engage == Engage::Hold)
        return false;
    if (GuardProvoked(st, g, 0, n.GetId()))
        return true;
    const Defence& r = set->rules;
    switch (r.engage)
    {
        case Engage::Hold:
        case Engage::Return: return false;
        case Engage::Enemies:
            // A faction the corporation, its members together, stands badly with
            // (Decided after the rounds, 2); another corporation's guard is nobody's enemy.
            return n.GetRole() != NpcRole::Guard &&
                   PartyStanding(g.GetCorp(), n.GetFaction()) < r.threshold;
        case Engage::Strangers: return n.GetRole() == NpcRole::Guard || !r.Allows(n.GetFaction());
    }
    return false;
}

Corps::Outcome Simulation::ApplyPatrolAct(const std::string& account, const Corps::Act& act,
                                          const std::string& systemId)
{
    const WorldRules::Patrols& r = WorldRules::PatrolRules();
    const Corps::Corp*         c = corps_.Of(account);
    const SystemState*         st = systemId.empty() ? nullptr : SystemById(systemId);
    const bool                 allowed = c != nullptr && st != nullptr && PatrolAllowed(*st, c->id);
    // The design: a name in the officer's own library (#279), or a stock design's id. It
    // must fight -- a patrol that cannot shoot is a trader in a uniform.
    std::string design, designName;
    if (!act.design.empty())
    {
        for (const auto& kv : sessions_)
            if (kv.second.accountName == account)
            {
                const auto lib = kv.second.designLibrary.find(act.design);
                if (lib != kv.second.designLibrary.end())
                    design = lib->second;
            }
        if (design.empty() && Archetypes::ShipCatalogue().FindDesign(Lower(act.design)) != nullptr)
            design = Lower(act.design);
        if (design.empty())
        {
            Corps::Outcome o;
            o.error = o.told = "there is no design '" + act.design.substr(0, 32) +
                               "' in your library, nor a stock one by that id";
            return o;
        }
        // ...as a doctrine's armed role must (Ships::ArmedRole): guns fitted, not the base gun
        // every hull has.
        const Ships::Catalogue& cat = Archetypes::ShipCatalogue();
        const Ships::Design*    d = cat.FindDesign(design);
        Ships::Stats            stats;
        std::string             why;
        if (d == nullptr || !Ships::Derive(cat, *d, stats, why) || stats.damage <= 0.0f)
        {
            Corps::Outcome o;
            o.error = o.told = "'" + act.design.substr(0, 32) + "' carries no guns";
            return o;
        }
        designName = act.design;
    }
    else if (c != nullptr)
    {
        // The design it flies now, when one is set and none is named.
        const auto was = c->patrols.find(systemId);
        if (was != c->patrols.end())
            design = was->second.design;
    }
    return corps_.SetPatrol(account, systemId.empty() ? act.system : systemId,
                            systemId.empty() ? act.system : SystemName(systemId), act.value, design,
                            designName, act.engage, r.systemMax, r.shipsMax, allowed);
}
