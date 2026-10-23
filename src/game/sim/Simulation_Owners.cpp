// Whose a structure is, and handing it over (#468). One translation unit of Simulation (#17).
//
// Ownership works as EVE's does (the owner's decisions of 2026-10-23): a structure a member
// of a corporation builds is the corporation's, and stays with it when the member leaves; one
// a pilot built before joining stays theirs until they hand it over. The owner is one string
// on the entity, as it always was -- an account, "corp:<id>", or a faction's "faction:<id>" --
// so the save, the wire and every reader of Entity::GetOwner keep one field. Every lookup by
// account in the corporation registry takes "corp:<id>" too, which is why trust, patrols and
// the rules of engagement needed no change of their own.
//
// A disbanded corporation leaves its structures abandoned: the owner still names it, and it
// no longer exists. Nobody pays rent for them, no patrol guards them, no law covers them, and
// anyone may take one -- from a ship beside it, by the same rules as building there.

#include "sim/Simulation.h"
#include "sim/ClientSession.h"

#include "core/Blueprint.h"
#include "core/Faction.h"
#include "entities/Ship.h"
#include "entities/Structure.h"
#include "sim/FactionMind.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>

namespace
{
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

bool IsPilotParty(const std::string& party)
{
    return party.rfind(InfluenceMap::PILOT, 0) == 0;
}

std::string PilotIn(const std::string& party)
{
    return party.substr(std::string(InfluenceMap::PILOT).size());
}

Corps::Outcome Refuse(const std::string& why)
{
    Corps::Outcome o;
    o.error = why;
    o.told = why;
    return o;
}

Structure* StructureAt(SystemState& st, int id)
{
    for (const auto& e : st.entities)
        if (e->GetId() == id && e->GetKind() == EntityKind::Structure)
            return static_cast<Structure*>(e.get());
    return nullptr;
}
}  // namespace

std::string Simulation::StructureParty(const std::string& owner) const
{
    FactionId   f;
    std::string corp;
    if (owner.empty() || Outposts::FactionOf(owner, f))
        return std::string();
    if (Corps::CorpOwner(owner, corp))
        return corps_.Find(corp) != nullptr ? corp : std::string();
    return InfluenceMap::PILOT + owner;
}

std::string Simulation::BuildOwner(const std::string& account) const
{
    const std::string corp = corps_.IdOf(account);
    return corp.empty() ? account : Corps::OwnerOf(corp);
}

bool Simulation::Abandoned(const std::string& owner) const
{
    std::string corp;
    return Corps::CorpOwner(owner, corp) && corps_.Find(corp) == nullptr;
}

bool Simulation::OwnSpace(const std::string& party, const std::string& holder) const
{
    if (party == holder)
        return true;
    // A member's own structure, built before they joined, in their corporation's system.
    return IsPilotParty(party) && !holder.empty() && corps_.IdOf(PilotIn(party)) == holder;
}

std::string Simulation::OwnerName(const std::string& owner) const
{
    FactionId   f;
    std::string corp;
    if (owner.empty())
        return std::string();
    if (Outposts::FactionOf(owner, f))
        return FactionName(f);
    if (Corps::CorpOwner(owner, corp))
    {
        const Corps::Corp* c = corps_.Find(corp);
        return c != nullptr ? Tag(*c) : std::string("abandoned");
    }
    return owner;
}

void Simulation::HandOver(SystemState& st, Structure& t, const std::string& to)
{
    const std::string from = t.GetOwner();
    const std::string oldParty = StructureParty(from);
    const std::string newParty = StructureParty(to);
    const std::string holder = LicenceHolder(st);

    // The licence goes with it, paid to where it was paid and owing what it owed -- unless
    // the new party has one of its own here already, which covers this too.
    if (!holder.empty() && !oldParty.empty() && oldParty != newParty)
    {
        const Corps::Licence* was = LicenceOf(oldParty, st.id);
        const Corps::Licence  had = was != nullptr ? *was : Corps::Licence();
        const Corps::Licence* has = LicenceOf(newParty, st.id);
        if (was != nullptr && had.holder == holder && !OwnSpace(newParty, holder) &&
            (has == nullptr || has->holder != holder))
            SetLicence(newParty, st.id, had);
        // The old party keeps its own only while something else of its stands here.
        bool stands = false;
        for (const auto& e : st.entities)
            stands = stands || (e.get() != &t && e->GetKind() == EntityKind::Structure &&
                                StructureParty(e->GetOwner()) == oldParty);
        if (was != nullptr && !stands)
            DropLicence(oldParty, st.id);
    }

    t.SetOwner(to);
    MarkStaticChanged(st, t.GetId());  // everyone here sees whose it is now, and the save keeps it
    st.profile = ProfileOf(st);        // its influence is the new party's
    // Its goods and its rules of engagement are the structure's, and stay. An attack under way
    // is the new owner's to hear the end of.
    const auto attack = attacks_.find(st.id + "/" + StaticKey(st.id, t.GetId()));
    if (attack != attacks_.end())
        attack->second.owner = to;
    // A pilot's battery fires on their standings, which are noted from now on (StepOwners).
    std::string corp;
    if (!Corps::CorpOwner(to, corp))
        owners_[to];
    RefreshLicences(st);
}

Corps::Outcome Simulation::TransferStructure(const std::string& account, const std::string& system,
                                             int id, const std::string& pilot,
                                             const std::string&           ticker,
                                             const std::set<std::string>& online)
{
    SystemState* st = SystemById(system);
    Structure*   t = st != nullptr ? StructureAt(*st, id) : nullptr;
    if (t == nullptr)
        return Refuse("there is no structure #" + std::to_string(id) +
                      (st != nullptr ? " in " + SystemName(st->id) : std::string()));
    const std::string from = t->GetOwner();
    const std::string name = t->GetName();
    if (Abandoned(from))
        return Refuse(name + " is abandoned: take it, from a ship beside it");
    // A pilot hands over their own; a corporation's, its CEO or a director.
    if (!corps_.Commands(account, from))
    {
        std::string corp;
        if (Corps::CorpOwner(from, corp) && corps_.Of(account) == corps_.Find(corp))
            return Refuse("only the CEO or a director hands over " + OwnerName(from) + "'s " +
                          name);
        return Refuse(name + " is not yours to hand over");
    }

    // To a pilot, who is there to hear of it as an invite's is, or to a corporation.
    std::string to;
    if (!pilot.empty() && !ticker.empty())
        return Refuse("hand it to a pilot or to a corporation, not both");
    if (!pilot.empty())
    {
        to = Lower(pilot);
        if (online.count(to) == 0)
            return Refuse(pilot.substr(0, 32) + " is not online: a structure is handed to a "
                                                "pilot who is there to take it");
    }
    else if (!ticker.empty())
    {
        const Corps::Corp* c = corps_.FindTicker(ticker);
        if (c == nullptr)
            return Refuse("there is no corporation [" + ticker.substr(0, 8) + "]");
        to = Corps::OwnerOf(c->id);
    }
    else
        return Refuse("hand it to whom? name a pilot or a corporation's ticker");
    if (to == from)
        return Refuse(name + " is " + OwnerName(to) + "'s already");

    // The new party asks the holder's leave again: a structure is not a way past a refusal.
    const std::string holder = LicenceHolder(*st);
    const std::string party = StructureParty(to);
    std::string       why;
    if (!holder.empty() && !OwnSpace(party, holder) && !Admits(*st, holder, party, why))
        return Refuse(HolderName(holder) + " would not license " + OwnerName(to) + " in " +
                      SystemName(st->id) + ": " + why);

    HandOver(*st, *t, to);
    const std::string where = name + " in " + SystemName(st->id);
    const std::string text = OwnerName(from) + " handed " + where + " over to " + OwnerName(to);
    TellOwner(from, "You handed " + where + " over to " + OwnerName(to), text, false,
              Ev::Kind::Notice);
    TellOwner(to, OwnerName(from) + " handed " + where + " over to you", text, false,
              Ev::Kind::Notice);
    Corps::Outcome o;
    o.told = "Handed " + where + " over to " + OwnerName(to) +
             ": its licence, goods and rules of engagement go with it";
    return o;
}

Corps::Outcome Simulation::TakeStructure(const std::string& account, const std::string& system,
                                         int id)
{
    ClientSession* s = nullptr;
    for (auto& kv : sessions_)
        if (kv.second.accountName == account && !account.empty())
            s = &kv.second;
    if (s == nullptr || !s->ship)
        return Refuse("there is no ship to take it from");
    SystemState* st = SystemById(system.empty() ? s->systemId : system);
    Structure*   t = st != nullptr ? StructureAt(*st, id) : nullptr;
    if (t == nullptr || st->id != s->systemId)
        return Refuse("there is no structure #" + std::to_string(id) + " here");
    const std::string name = t->GetName();
    if (!Abandoned(t->GetOwner()))
        return Refuse(name + " is " + OwnerName(t->GetOwner()) +
                      "'s: only what a disbanded corporation left is free to take");
    if (s->IsDocked())
        return Refuse("undock first: a structure is taken from space");
    // From beside it, as it would be taken apart.
    const Blueprint* bp = t->GetBlueprint();
    const float      reach = bp != nullptr ? bp->reach : 1000.0f;
    const float      d = std::hypot(t->GetPosition().x - s->ship->GetPosition().x,
                                    t->GetPosition().y - s->ship->GetPosition().y) -
                    t->GetSize();
    if (d > reach)
        return Refuse("too far from " + name + " (" + std::to_string((int)d) + " of at most " +
                      std::to_string((int)reach) + ")");
    // Taking is building's rules: the taker's party, by the holder's leave and licence.
    const std::string licence = LicenceProblem(account, *st);
    if (!licence.empty())
        return Refuse(licence);

    const std::string to = BuildOwner(account);
    attacks_.erase(st->id + "/" + StaticKey(st->id, id));  // its old owner is nobody now
    HandOver(*st, *t, to);
    const std::string where = name + " in " + SystemName(st->id);
    if (to != account)
        TellOwner(to, "", account + " took the abandoned " + where + " for " + OwnerName(to), false,
                  Ev::Kind::Notice);
    Corps::Outcome o;
    o.told = "Took the abandoned " + where + (to != account ? " for " + OwnerName(to) : "");
    return o;
}
