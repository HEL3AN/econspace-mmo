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

// "40 s", "12 min", "3 h 5 min": how long, as a pilot reads it.
std::string Span(double seconds)
{
    const long long s = seconds < 0.0 ? 0 : (long long)seconds;
    if (s < 60)
        return std::to_string(s) + " s";
    if (s < 3600)
        return std::to_string(s / 60) + " min";
    const long long m = (s % 3600) / 60;
    return std::to_string(s / 3600) + " h" + (m > 0 ? " " + std::to_string(m) + " min" : "");
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
    FactionId  law = Factions::Unaffiliated();
    const bool lawful = LawIn(st, law);
    s.RecordEvent(Ev::Kind::Notice, "Rules of engagement for " + t->GetName() + ": " +
                                        EngageLabel(d.engage) + ". " +
                                        DefenceCost(d.engage, lawful, law));
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
    // ...and their corporation's patrol near them (#431 slice 12).
    ProvokePatrol(st, corps_.IdOf(victim.accountName), at, bySession != victim.id ? bySession : 0,
                  byNpc);
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
    StepAttacks();
    // Who joined a corporation reads its journal from then on (#431 C2).
    for (auto& kv : sessions_)
        if (!kv.second.accountName.empty())
            ReadCorpJournal(kv.second);
    // A corporation's members' standings are noted while they are on: the corporation's
    // standing with a faction is theirs together, on or off (#431 slice 6).
    for (auto& kv : sessions_)
        if (!kv.second.accountName.empty() && owners_.count(kv.second.accountName) == 0 &&
            !corps_.IdOf(kv.second.accountName).empty())
            owners_[kv.second.accountName];
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
        if (b.owedMoney > 0.0)  // their markets' fees (#438)
        {
            s.account.AddMoney(b.owedMoney);
            s.RecordEvent(Ev::Kind::Notice, "While you were away your markets took " +
                                                std::to_string((long long)(b.owedMoney + 0.5)) +
                                                " cr in fees");
            b.owedMoney = 0.0;
        }
        else if (b.owedMoney < 0.0)  // ...or less than the rent their licences took (#431)
        {
            s.account.AddMoney(b.owedMoney);
            s.RecordEvent(Ev::Kind::Notice,
                          "While you were away " + std::to_string((long long)(-b.owedMoney + 0.5)) +
                              " cr went in licence rent, more than your markets took");
            b.owedMoney = 0.0;
        }
        b.seen = true;
        b.money = s.account.GetMoney();
        for (FactionId f : Factions::All())
            b.standing[f] = s.account.GetReputation(f);
    }
}

bool Simulation::OwnerSeen(const std::string& account) const
{
    for (const auto& kv : sessions_)
        if (kv.second.accountName == account)
            return true;
    const auto it = owners_.find(account);
    return it != owners_.end() && it->second.seen;
}

bool Simulation::NoteUnseenOwner(const std::string& account, const std::string& path,
                                 std::string& why)
{
    why.clear();
    if (account.empty() || OwnerSeen(account))
        return false;
    ClientSession      s;  // a reading, never in sessions_ and never saved
    const Save::Result r = LoadAccount(s, path);
    if (r == Save::Result::TooNew)
        why = path + " is from a newer build";
    else if (r == Save::Result::Corrupt)
        why = path + " is damaged";
    if (r != Save::Result::Ok)
        return false;
    OwnerBook& b = owners_[account];
    b.seen = true;
    b.money = s.account.GetMoney();
    for (FactionId f : Factions::All())
        b.standing[f] = s.account.GetReputation(f);
    return true;
}

void Simulation::NoteAttack(const std::string& owner, const std::string& key,
                            const std::string& name, const std::string& system,
                            const std::string& by, float hull, bool destroyed)
{
    auto       it = attacks_.find(key);
    const bool fresh = it == attacks_.end();
    if (fresh)
    {
        it = attacks_.emplace(key, AttackEpisode()).first;
        it->second.owner = owner;
        it->second.system = system;
        it->second.first = time_;
    }
    AttackEpisode& a = it->second;
    a.name = name;
    a.by = by;
    a.hits++;
    a.hull = hull;
    a.last = time_;
    if (destroyed)
    {
        const AttackEpisode over = a;
        attacks_.erase(it);
        EndAttack(over, true);
        return;
    }
    // Whoever is on hears of it when it starts; the hits after are the same attack.
    if (fresh)
    {
        const std::string what = name + " in " + SystemName(system) + " is under attack by " + by;
        TellOwner(owner, "Your " + what, owner + "'s " + what, true);
    }
}

void Simulation::EndAttack(const AttackEpisode& a, bool destroyed)
{
    std::string what = a.name + " in " + SystemName(a.system) + " was attacked by " + a.by + ": " +
                       std::to_string(a.hits) + (a.hits == 1 ? " hit" : " hits");
    if (a.last > a.first)
        what += " over " + Span(a.last - a.first);
    what += destroyed
                ? "; it was destroyed"
                : "; it stands at " + std::to_string((int)(a.hull * 100.0f + 0.5f)) + "% hull";
    TellOwner(a.owner, "Your " + what, a.owner + "'s " + what, false);
}

void Simulation::TellOwner(const std::string& owner, const std::string& yours,
                           const std::string& theirs, bool onlyOn, Ev::Kind kind)
{
    const Corps::Corp* corp = corps_.Of(owner);
    // The corporation's journal, which its members read (#347): whoever is on now, and the
    // rest at login. The owner is a member like any other.
    if (corp != nullptr && !onlyOn)
    {
        const int seq = corps_.Record(owner, kind, theirs);
        for (auto& kv : sessions_)
            if (seq != 0 && corps_.SameCorp(kv.second.accountName, owner))
                ReadCorpJournal(kv.second);
        return;
    }
    bool ownerOn = false;
    for (auto& kv : sessions_)
    {
        ClientSession& s = kv.second;
        if (s.accountName == owner)
            ownerOn = true;
        if (s.accountName == owner || (corp != nullptr && corps_.SameCorp(s.accountName, owner)))
            s.RecordEvent(kind, s.accountName == owner ? yours : theirs);
    }
    if (ownerOn || onlyOn)
        return;
    // Nobody on to read it: the pending part of the owner's journal, in their save.
    KeepForAbsent(owner, kind, yours);
}

void Simulation::TellAccount(const std::string& account, Ev::Kind kind, const std::string& text)
{
    if (account.empty())
        return;
    bool on = false;
    for (auto& kv : sessions_)
        if (kv.second.accountName == account)
        {
            kv.second.RecordEvent(kind, text);
            on = true;
        }
    if (!on)
        KeepForAbsent(account, kind, text);
}

void Simulation::KeepForAbsent(const std::string& account, Ev::Kind kind, const std::string& text)
{
    Ev::Event e;
    e.kind = kind;
    e.text = "While you were away: " + text;
    absent_.emplace_back(account, e);
    if (absent_.size() > 4 * MAX_PENDING_JOURNAL)  // nobody hosting takes them (a test)
        absent_.erase(absent_.begin());
}

void Simulation::ReadCorpJournal(ClientSession& s)
{
    const Corps::Corp* c = corps_.Of(s.accountName);
    if (c == nullptr)
    {
        s.corpJournalOf.clear();
        s.corpJournalSeen = 0;
        return;
    }
    if (s.corpJournalOf == c->id && s.corpJournalSeen >= c->nextJournalSeq - 1)
        return;  // nothing new: the usual case, every tick
    // Joined since they last read it: what came before is not theirs to read.
    if (s.corpJournalOf != c->id)
    {
        s.corpJournalOf = c->id;
        s.corpJournalSeen = c->nextJournalSeq - 1;
        return;
    }
    for (const Ev::Event& e : c->journal)
        if (e.seq > s.corpJournalSeen)
        {
            s.RecordEvent(e.kind, e.text);
            s.corpJournalSeen = e.seq;
        }
}

void Simulation::StepAttacks()
{
    for (auto it = attacks_.begin(); it != attacks_.end();)
        if (time_ - it->second.last > ATTACK_EPISODE_SECONDS)
        {
            const AttackEpisode over = it->second;
            it = attacks_.erase(it);
            EndAttack(over, false);
        }
        else
            ++it;
}

std::vector<std::pair<std::string, Ev::Event>> Simulation::TakeAbsentEvents()
{
    std::vector<std::pair<std::string, Ev::Event>> out;
    out.swap(absent_);
    return out;
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
        if (b.owedMoney != 0.0)
            o["owedMoney"] = b.owedMoney;
        // What they had when last on, and their licences (#431 slice 6): additions an older
        // build ignores.
        if (b.seen && b.money != 0.0)
            o["money"] = b.money;
        if (!b.licences.empty())
        {
            nlohmann::json licences = nlohmann::json::object();
            for (const auto& l : b.licences)
                licences[l.first] = { { "holder", l.second.holder },
                                      { "until", l.second.until },
                                      { "told", l.second.told } };
            o["licences"] = licences;
        }
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
        const auto money = it->find("owedMoney");
        if (money != it->end() && money->is_number())
            b.owedMoney = money->get<double>();
        const auto had = it->find("money");
        if (had != it->end() && had->is_number())
            b.money = had->get<double>();
        const auto licences = it->find("licences");
        if (licences != it->end() && licences->is_object())
            for (auto l = licences->begin(); l != licences->end(); ++l)
            {
                if (!l->is_object() || !l->contains("holder") || !(*l)["holder"].is_string() ||
                    !l->contains("until") || !(*l)["until"].is_number())
                    continue;
                Corps::Licence lc;
                lc.holder = (*l)["holder"].get<std::string>();
                lc.until = (*l)["until"].get<double>();
                lc.told = l->value("told", 0);
                b.licences[l.key()] = lc;
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
