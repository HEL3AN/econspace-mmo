// Rules of engagement (#340): what a player's armed structure fires at, set by its owner, and
// what that costs them. One translation unit of Simulation (#17).
//
// The battery itself is StepStationDefence's; this file is the setting, and the owner's side
// of it -- their standings, which an outpost firing on their enemies needs while they are
// away, and the crimes it commits in their name, charged when they are back.

#include "sim/Simulation.h"
#include "sim/ClientSession.h"

#include "core/Archetype.h"
#include "core/Faction.h"
#include "entities/Structure.h"

#include <nlohmann/json.hpp>

namespace
{
const Structure* OwnedStructure(const SystemState& st, int id)
{
    for (const auto& e : st.entities)
        if (e->GetId() == id && e->GetKind() == EntityKind::Structure)
            return static_cast<const Structure*>(e.get());
    return nullptr;
}

// Whether what it is, or will be once built, has guns.
bool Armed(const Structure& t)
{
    const Archetype* a = Archetypes::Find(t.GetBuilds());
    return a != nullptr && a->Has(Component::Defensive) && a->weaponDamage > 0.0f;
}
}  // namespace

std::string Simulation::DefenceSetProblem(const ClientSession& s, int id, const Defence& d) const
{
    const SystemState* st = SystemOf(s);
    if (st == nullptr)
        return "you are nowhere";
    const Structure* t = OwnedStructure(*st, id);
    if (t == nullptr)
        return "there is no structure #" + std::to_string(id) + " in this system";
    // Its owner's, or their corporation's leadership's (#347).
    if (s.accountName.empty() || !corps_.Manages(s.accountName, t->GetOwner()))
        return t->GetName() + " is not yours";
    if (!Armed(*t))
        return t->GetName() + " has no guns";
    return DefenceProblem(d);
}

bool Simulation::SetDefence(ClientSession& s, int id, const Defence& d)
{
    const std::string why = DefenceSetProblem(s, id, d);
    if (!why.empty())
    {
        s.RecordEvent(Ev::Kind::Notice, "Cannot set the rules of engagement: " + why);
        return false;
    }
    SystemState& st = *SystemOf(s);
    Structure*   t = nullptr;
    for (const auto& e : st.entities)
        if (e->GetId() == id)
            t = static_cast<Structure*>(e.get());
    t->SetDefence(d);
    MarkStaticChanged(st, id);  // everyone in the system sees it, and the save keeps it
    owners_[t->GetOwner()];     // its owner's standings are noted from now on (StepOwners)
    StepOwners();

    // What the mode will cost here, said once, where the owner reads it.
    const bool lawful = st.agg.claimed && Factions::IsLawful(st.agg.controller);
    s.RecordEvent(Ev::Kind::Notice, "Rules of engagement for " + t->GetName() + ": " +
                                        EngageLabel(d.engage) + ". " +
                                        DefenceCost(d.engage, lawful, st.agg.controller));
    // Set by a director of the owner's corporation: the owner hears of it, since what the
    // battery does is still on their account.
    if (t->GetOwner() != s.accountName)
        for (auto& kv : sessions_)
            if (kv.second.accountName == t->GetOwner())
                kv.second.RecordEvent(Ev::Kind::Notice,
                                      s.accountName + " set the rules of engagement for " +
                                          t->GetName() + ": " + EngageLabel(d.engage));
    return true;
}

void Simulation::RetellAllowLists()
{
    for (auto& kv : systems_)
    {
        std::vector<int> ids;
        for (const auto& e : kv.second.entities)
            if (e->GetKind() == EntityKind::Structure && !e->GetOwner().empty())
            {
                const Defence& d = static_cast<const Structure&>(*e).GetDefence();
                if (!d.players.empty() || !d.factions.empty())
                    ids.push_back(e->GetId());
            }
        for (int id : ids)
            MarkStaticChanged(kv.second, id);
    }
}

void Simulation::ProvokeGuards(SystemState& st, const ClientSession& victim, Vector2 at,
                               int bySession, int byNpc)
{
    if (victim.accountName.empty())
        return;
    for (const auto& e : st.entities)
    {
        // The victim's own, and their corporation's (#347): a member's battery guards every
        // member's ship.
        if (e->GetKind() != EntityKind::Structure ||
            (e->GetOwner() != victim.accountName &&
             !corps_.SameCorp(e->GetOwner(), victim.accountName)) ||
            static_cast<const Structure&>(*e).IsBuilding())
            continue;
        const Archetype* a = e->GetArchetype();
        if (a == nullptr || !a->Has(Component::Defensive) || a->weaponDamage <= 0.0f)
            continue;
        // In its reach: where it could have answered from, measured as its own fire is.
        const float reach = e->GetSize() + a->weaponRange;
        const float dx = at.x - e->GetPosition().x, dy = at.y - e->GetPosition().y;
        if (dx * dx + dy * dy > reach * reach)
            continue;
        const double until = time_ + PROVOKED_SECONDS;
        if (bySession != 0 && bySession != victim.id)
            st.provokedBy[e->GetId()][bySession] = until;
        if (byNpc != 0)
            st.provokedByNpc[e->GetId()][byNpc] = until;
    }
}

bool Simulation::OwnerStanding(const std::string& account, FactionId f, float& out) const
{
    for (const auto& kv : sessions_)
        if (kv.second.accountName == account)
        {
            out = kv.second.account.GetReputation(f);
            return true;
        }
    const auto it = owners_.find(account);
    if (it == owners_.end() || !it->second.seen)
        return false;
    out = it->second.standing[f];
    return true;
}

void Simulation::ChargeOwner(const std::string& account, FactionId victim, bool killed)
{
    for (auto& kv : sessions_)
        if (kv.second.accountName == account)
        {
            ChargeAttack(kv.second, victim, killed);
            return;
        }
    OwnerBook& b = owners_[account];
    b.owedStanding[victim] += killed ? Crime::KILL_STANDING : Crime::HIT_STANDING;
    b.owedBounty[victim] += killed ? Crime::KILL_BOUNTY : Crime::HIT_BOUNTY;
}

float Simulation::OwedStanding(const std::string& account, FactionId f) const
{
    const auto it = owners_.find(account);
    return it == owners_.end() ? 0.0f : it->second.owedStanding[f];
}

void Simulation::StepOwners()
{
    if (owners_.empty())
        return;
    for (auto& kv : sessions_)
    {
        ClientSession& s = kv.second;
        const auto     it = owners_.find(s.accountName);
        if (s.accountName.empty() || it == owners_.end())
            continue;
        OwnerBook& b = it->second;
        bool       owed = false;
        for (FactionId f : Factions::All())
            if (b.owedStanding[f] != 0.0f || b.owedBounty[f] != 0.0)
            {
                s.account.AddReputation(f, -b.owedStanding[f]);
                s.account.AddBounty(f, b.owedBounty[f]);
                owed = true;
            }
        if (owed)
            s.RecordEvent(Ev::Kind::Notice,
                          "While you were away your structures fired first where that is a "
                          "crime: the standing and bounty it cost are on your account");
        b.owedStanding.Clear();
        b.owedBounty.Clear();
        b.seen = true;
        for (FactionId f : Factions::All())
            b.standing[f] = s.account.GetReputation(f);
    }
}

nlohmann::json Simulation::OwnersJson() const
{
    nlohmann::json out = nlohmann::json::object();
    for (const auto& kv : owners_)
    {
        const OwnerBook& b = kv.second;
        nlohmann::json   standing = nlohmann::json::object(), owed = nlohmann::json::object();
        for (FactionId f : Factions::All())
        {
            if (b.seen && b.standing[f] != 0.0f)
                standing[Factions::Id(f)] = b.standing[f];
            if (b.owedStanding[f] != 0.0f || b.owedBounty[f] != 0.0)
                owed[Factions::Id(f)] = { b.owedStanding[f], b.owedBounty[f] };
        }
        nlohmann::json o = nlohmann::json::object();
        if (b.seen)
            o["standing"] = standing;
        if (!owed.empty())
            o["owed"] = owed;
        out[kv.first] = o;
    }
    return out;
}

void Simulation::OwnersFrom(const nlohmann::json& j)
{
    owners_.clear();
    if (!j.is_object())
        return;
    for (auto it = j.begin(); it != j.end(); ++it)
    {
        if (!it->is_object())
            continue;
        OwnerBook& b = owners_[it.key()];
        FactionId  f;
        const auto standing = it->find("standing");
        if (standing != it->end() && standing->is_object())
        {
            b.seen = true;
            for (auto s = standing->begin(); s != standing->end(); ++s)
                if (s->is_number() && Factions::Find(s.key(), f))
                    b.standing[f] = s->get<float>();
        }
        const auto owed = it->find("owed");
        if (owed != it->end() && owed->is_object())
            for (auto o = owed->begin(); o != owed->end(); ++o)
                if (o->is_array() && o->size() == 2 && (*o)[0].is_number() && (*o)[1].is_number() &&
                    Factions::Find(o.key(), f))
                {
                    b.owedStanding[f] = (*o)[0].get<float>();
                    b.owedBounty[f] = (*o)[1].get<double>();
                }
    }
}
