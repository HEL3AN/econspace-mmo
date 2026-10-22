// Territory (#431 slice 5): influence, claims, and what holding a system brings.
//
// Every party -- a world faction, a player corporation, a pilot in none -- has an influence in
// each system: what its structures there weigh by tier, and a faction's presence (T1). A
// system is held by the party with the most influence AND a claim, a finished structure of an
// outpost's tier or above, which can itself be brought down (T2): strength alone claims
// nothing, and a claim nobody could attack would be a flag. A tie keeps the holder.
//
// Factions among themselves keep the rule they had (StepControl: strength held for
// ContestPasses(), an outpost to claim an empty system, a holder's outpost to be brought down
// first). This is the same contest with corporations as parties. A corporation that holds a
// system sets the fee on trades at its markets, which goes to its treasury, and who docks at
// its structures there (T3). Owning is not a ban on building: anybody may build in a held
// system, and what they build is influence of their own.
//
// It all reads the profile and the aggregate, never entities, so it runs in the coarse pass
// for a cold system exactly as for a hot one (#295).
#include "sim/Simulation.h"
#include "sim/WorldRules.h"

#include "core/Blueprint.h"
#include "economy/Resource.h"
#include "entities/Station.h"
#include "entities/Structure.h"

#include <algorithm>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace
{
std::string Tag(const Corps::Corp& c)
{
    return c.name + " [" + c.ticker + "]";
}
}  // namespace

float Simulation::InfluenceWeight(const Structure& t, bool& claim)
{
    claim = false;
    const WorldRules::Influence& r = WorldRules::InfluenceRules();
    const Blueprint*             bp = t.GetBlueprint();
    // An outpost being upgraded is the outpost it was until it is finished (#342 S6) -- its
    // claim stands meanwhile; any other site is nothing yet.
    const bool upgrading = t.IsBuilding() && bp != nullptr && !bp->onto.empty();
    if (t.IsBuilding() && !upgrading)
        return 0.0f;
    double w = 0.0;
    if (upgrading)
        w = r.outpost;
    else if (IsAnchor(t.GetBuilds()))
        w = r.anchor;
    else if (bp != nullptr && std::find(bp->onto.begin(), bp->onto.end(),
                                        Outposts::MARKET_BLUEPRINT) != bp->onto.end())
        w = r.station;  // grown from a market: a trade or a mining station, a shipyard
    else if (bp != nullptr && !bp->onto.empty())
        w = r.market;
    else if (Outposts::IsOutpost(t.GetBuilds()))
        w = r.outpost;
    claim = w > 0.0;
    return (float)w;
}

InfluenceMap Simulation::InfluenceIn(const SystemState& st) const
{
    InfluenceMap m;
    const float  perShip = (float)WorldRules::InfluenceRules().perShip;
    for (FactionId f : Factions::All())
        m.faction[f] = st.profile.factionWeight[f] + st.agg.presence[f] * perShip;
    // A pilot's structures count for their corporation (T1), as it is now.
    for (const auto& kv : st.profile.playerWeight)
    {
        const std::string id = corps_.IdOf(kv.first);
        m.corp[id.empty() ? InfluenceMap::PILOT + kv.first : id] += kv.second;
        if (!id.empty() && st.profile.playerClaims.count(kv.first) != 0)
            m.corpClaims.insert(id);
    }
    // Slice 12 adds a corporation's patrols here, as presence.
    return m;
}

void Simulation::StepClaims(bool settling)
{
    for (auto& kv : systems_)
    {
        SystemState&     st = kv.second;
        SystemAggregate& a = st.agg;
        // Nothing of a corporation's here, and none holds it: the faction rule's alone.
        if (a.holderCorp.empty() && st.profile.playerClaims.empty())
        {
            a.claimant.clear();
            a.claimContest = 0;
            continue;
        }
        const InfluenceMap m = InfluenceIn(st);
        const std::string  where = SystemName(kv.first);

        // A holder whose last claim is gone -- destroyed, dismantled, or its owner gone from the
        // corporation -- or whose corporation is, holds nothing: destroying is not taking, so
        // the system is nobody's until someone claims it.
        if (!a.holderCorp.empty())
        {
            const Corps::Corp* c = corps_.Find(a.holderCorp);
            if (c == nullptr || m.corpClaims.count(a.holderCorp) == 0)
            {
                const std::string text = (c != nullptr ? Tag(*c) : std::string("A corporation")) +
                                         " lost its claim on " + where;
                if (c != nullptr && c->Ceo() != nullptr)
                    corps_.Record(c->Ceo()->account, Ev::Kind::Notice, text);
                a.holderCorp.clear();
                a.claimant.clear();
                a.claimContest = 0;
                if (!settling)
                    Announce("claim", std::nullopt, kv.first, text);
                continue;
            }
        }

        // The challenger: of the parties with a claim here, other than the holder, the one
        // with the most influence. A faction challenges this way only a corporation's system;
        // between factions their own rule stands. And a claim is laid only where the side's
        // front is open (#431 D3): a held system is taken from next door, so a core whose every
        // neighbour is its holder's own falls only once a border system has.
        std::string bestKey;
        float       best = 0.0f;
        bool        isFaction = false;
        FactionId   bestFaction = Factions::Unaffiliated();
        for (const std::string& id : m.corpClaims)
            if (id != a.holderCorp && (bestKey.empty() || m.corp.at(id) > best) &&
                FrontOpen(kv.first, id))
            {
                bestKey = id;
                best = m.corp.at(id);
            }
        if (!a.holderCorp.empty())
            for (FactionId f : st.profile.outposts)
                if ((bestKey.empty() || m.faction[f] > best) && FrontOpen(kv.first, f))
                {
                    bestKey = InfluenceMap::Key(f);
                    best = m.faction[f];
                    isFaction = true;
                    bestFaction = f;
                }
        // ...and it must outweigh every other party, the holder above all: a tie keeps it.
        float rest = 0.0f;
        for (FactionId f : Factions::All())
            if (InfluenceMap::Key(f) != bestKey)
                rest = std::max(rest, m.faction[f]);
        for (const auto& p : m.corp)
            if (p.first != bestKey)
                rest = std::max(rest, p.second);
        if (bestKey.empty() || best <= rest)
        {
            a.claimant.clear();
            a.claimContest = 0;
            continue;
        }
        if (a.claimant != bestKey)
        {
            a.claimant = bestKey;
            a.claimContest = 0;
        }
        if (++a.claimContest < ContestPasses())
            continue;

        // It changes hands.
        const Corps::Corp* was = a.holderCorp.empty() ? nullptr : corps_.Find(a.holderCorp);
        const std::string  from = was != nullptr ? Tag(*was)
                                  : a.claimed    ? FactionName(a.controller)
                                                 : std::string();
        if (isFaction)
        {
            TakeControl(kv.first, bestFaction);  // clears the holder
            const std::string text = FactionName(bestFaction) + " took " + where + " from " + from;
            if (was != nullptr && was->Ceo() != nullptr)
                corps_.Record(was->Ceo()->account, Ev::Kind::Notice, text);
            if (!settling)
                Announce("claim", bestFaction, kv.first, text);
            continue;
        }
        const Corps::Corp* now = corps_.Find(bestKey);
        a.holderCorp = bestKey;
        a.claimed = false;  // no faction holds it: its garrison and its law are not the world's
        a.controller = Factions::Unaffiliated();
        a.contested = 0;
        a.claimant.clear();
        a.claimContest = 0;
        const std::string text =
            Tag(*now) + (from.empty() ? " claim " + where : " took " + where + " from " + from);
        if (now->Ceo() != nullptr)
            corps_.Record(now->Ceo()->account, Ev::Kind::Notice, text);
        if (was != nullptr && was->Ceo() != nullptr)
            corps_.Record(was->Ceo()->account, Ev::Kind::Notice, text);
        if (!settling)
            Announce("claim", std::nullopt, kv.first, text);
    }
}

std::string Simulation::HolderOf(const SystemAggregate& a)
{
    if (!a.holderCorp.empty())
        return a.holderCorp;
    return a.claimed ? InfluenceMap::Key(a.controller) : std::string();
}

bool Simulation::FrontOpen(const std::string& system, const std::string& side) const
{
    const auto it = systems_.find(system);
    if (it == systems_.end())
        return false;
    const std::string holder = HolderOf(it->second.agg);
    if (holder.empty() || holder == side)
        return true;
    // A faction's allies open its front as its own holdings do; a corporation has none yet.
    FactionId  f = Factions::Unaffiliated();
    const bool faction = side.rfind("f:", 0) == 0 && Factions::Find(side.substr(2), f);
    for (const std::string& n : Neighbors(system))
    {
        const auto nb = systems_.find(n);
        if (nb == systems_.end())
            continue;
        const SystemAggregate& a = nb->second.agg;
        const std::string      k = HolderOf(a);
        if (k.empty() || k == side)
            return true;
        if (faction && a.holderCorp.empty() && Factions::Relation(f, a.controller) == Stance::Ally)
            return true;
    }
    return false;
}

std::vector<std::string> Simulation::HeldBy(const std::string& corpId) const
{
    std::vector<std::string> out;
    if (corpId.empty())
        return out;
    for (const auto& kv : systems_)
        if (kv.second.agg.holderCorp == corpId)
            out.push_back(kv.first);
    return out;
}

double Simulation::FeeRateIn(const SystemState& st) const
{
    const double       standard = Goods::Rules().fee;
    const Corps::Corp* c = st.agg.holderCorp.empty() ? nullptr : corps_.Find(st.agg.holderCorp);
    return c != nullptr ? c->TaxRate(standard, WorldRules::InfluenceRules().taxMax) : standard;
}

Simulation::FeeTo Simulation::FeeAt(const SystemState& st, const Entity& market) const
{
    FeeTo to;
    to.rate = FeeRateIn(st);
    // The holder's tax is the holder's, at every market in its system (T3).
    if (!st.agg.holderCorp.empty() && corps_.Find(st.agg.holderCorp) != nullptr)
    {
        to.corp = st.agg.holderCorp;
        return to;
    }
    if (market.GetKind() == EntityKind::Station)
    {
        to.toFaction = true;
        to.faction = static_cast<const Station&>(market).GetFaction();
    }
    else if (Outposts::FactionOf(market.GetOwner(), to.faction))
        to.toFaction = true;
    else if (!market.GetOwner().empty())
    {
        // A player's market (#438): its owner's corporation's, or the owner's own.
        to.corp = corps_.IdOf(market.GetOwner());
        if (to.corp.empty())
            to.account = market.GetOwner();
    }
    return to;
}

void Simulation::PayFee(const FeeTo& to, double fee)
{
    if (!(fee > 0.0))
        return;
    if (to.toFaction)
        minds_[to.faction].treasury += fee;
    else if (!to.corp.empty())
        corps_.Credit(to.corp, fee);
    else if (!to.account.empty())
    {
        for (auto& kv : sessions_)
            if (kv.second.accountName == to.account)
            {
                kv.second.account.AddMoney(fee);
                return;
            }
        owners_[to.account].owedMoney += fee;  // paid when they are next on (StepOwners)
    }
}

// --- Licences (#431 slice 6) ---
//
// Owning is not a ban on building (T3), but building in somebody's space is by their leave:
// the holder's standing gate and a licence, a rent paid each week from the party's money (the
// owner's decision of 2026-10-22, as EVE rents a system's infrastructure rather than selling
// it). A party is a corporation or a pilot in none, as for influence. The licence is kept by
// whoever pays it -- a corporation on its record (corps.json), a pilot in the world's book of
// owners -- and everything here reads that and the static layer, so the rent comes due in a
// cold system exactly as in a hot one.

namespace
{
std::string Hours(double seconds)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.0f h", std::max(0.0, seconds) / 3600.0);
    return buf;
}

std::string Credits(double cr)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.0f cr", cr);
    return buf;
}

bool IsPilotParty(const std::string& party)
{
    return party.rfind(InfluenceMap::PILOT, 0) == 0;
}

std::string PilotOf(const std::string& party)
{
    return party.substr(std::string(InfluenceMap::PILOT).size());
}

bool FactionKey(const std::string& holder, FactionId& f)
{
    return holder.rfind("f:", 0) == 0 && Factions::Find(holder.substr(2), f);
}
}  // namespace

std::string Simulation::LicenceHolder(const SystemState& st) const
{
    if (!st.agg.holderCorp.empty() && corps_.Find(st.agg.holderCorp) != nullptr)
        return st.agg.holderCorp;
    // A faction's space is where the law is a faction's: the lawless license nobody, and what
    // the world's unaffiliated keep is nobody's to rent.
    if (st.agg.claimed && Factions::IsLawful(st.agg.controller) &&
        st.agg.controller != Factions::Unaffiliated())
        return InfluenceMap::Key(st.agg.controller);
    return std::string();
}

std::string Simulation::HolderName(const std::string& holder) const
{
    FactionId f;
    if (FactionKey(holder, f))
        return FactionName(f);
    const Corps::Corp* c = corps_.Find(holder);
    return c != nullptr ? Tag(*c) : holder;
}

std::string Simulation::PartyOf(const std::string& account) const
{
    const std::string id = corps_.IdOf(account);
    return id.empty() ? InfluenceMap::PILOT + account : id;
}

const Corps::Licence* Simulation::LicenceOf(const std::string& party,
                                            const std::string& system) const
{
    if (IsPilotParty(party))
    {
        const auto o = owners_.find(PilotOf(party));
        if (o == owners_.end())
            return nullptr;
        const auto l = o->second.licences.find(system);
        return l == o->second.licences.end() ? nullptr : &l->second;
    }
    const Corps::Corp* c = corps_.Find(party);
    if (c == nullptr)
        return nullptr;
    const auto l = c->licences.find(system);
    return l == c->licences.end() ? nullptr : &l->second;
}

float Simulation::PartyStanding(const std::string& party, FactionId f) const
{
    float s = 0.0f;
    if (IsPilotParty(party))
    {
        OwnerStanding(PilotOf(party), f, s);
        return s;
    }
    // A corporation's is the sum of its members' (Decided after the rounds, 2): each pilot's
    // stays their own, and the corporation is what they are together.
    if (const Corps::Corp* c = corps_.Find(party))
        for (const Corps::Member& m : c->members)
        {
            float one = 0.0f;
            if (OwnerStanding(m.account, f, one))
                s += one;
        }
    return s;
}

bool Simulation::Admits(const SystemState& st, const std::string& holder, const std::string& party,
                        std::string& why) const
{
    const bool corp = !IsPilotParty(party);
    FactionId  f;
    if (FactionKey(holder, f))
    {
        const float have = PartyStanding(party, f);
        const float need = (float)WorldRules::LicenceRules().standing;
        if (have >= need)
            return true;
        char buf[200];
        std::snprintf(buf, sizeof(buf),
                      "%s licenses only those it thinks well of: a standing of %.0f, and %s %.0f",
                      FactionName(f).c_str(), need,
                      corp ? "the corporation's (its members' together) is" : "yours is", have);
        why = buf;
        return false;
    }
    const Corps::Corp* c = corps_.Find(holder);
    if (c == nullptr || party == holder || c->docking == Corps::Docking::All)
        return true;
    // Allies are whoever its structures here hold fire for, as its docks take them (T3).
    if (c->docking == Corps::Docking::Allies)
    {
        std::vector<std::string> pilots;
        if (corp)
        {
            if (const Corps::Corp* pc = corps_.Find(party))
                for (const Corps::Member& m : pc->members)
                    pilots.push_back(m.account);
        }
        else
            pilots.push_back(PilotOf(party));
        for (const auto& e : st.entities)
            if (e->GetKind() == EntityKind::Structure && corps_.IdOf(e->GetOwner()) == holder)
                for (const std::string& p : pilots)
                    if (static_cast<const Structure&>(*e).GetDefence().Allows(p))
                        return true;
    }
    why = Tag(*c) + " licenses only " +
          (c->docking == Corps::Docking::Own ? std::string("its members")
                                             : std::string("its members and their friends"));
    return false;
}

std::string Simulation::LicenceProblem(const std::string& account, const SystemState& st) const
{
    const std::string holder = LicenceHolder(st);
    const std::string party = PartyOf(account);
    if (holder.empty() || party == holder)
        return std::string();
    const Corps::Licence* l = LicenceOf(party, st.id);
    if (l != nullptr && l->holder == holder && time_ < l->until)
        return std::string();
    const std::string where = SystemName(st.id);
    const std::string from = HolderName(holder);
    std::string       why;
    if (!Admits(st, holder, party, why))
        return "building in " + where + " takes a licence from " + from + ", and " + why;
    FactionId f;
    return "building in " + where + " takes a licence from " + from + " (" +
           Credits(WorldRules::LicenceRules().rent) + " a week" +
           (l != nullptr && l->holder == holder ? ", and yours is unpaid" : std::string()) +
           "): buy one " +
           (FactionKey(holder, f) ? "docked at one of its stations here" : std::string("here"));
}

void Simulation::SetLicence(const std::string& party, const std::string& system,
                            const Corps::Licence& l)
{
    if (IsPilotParty(party))
        owners_[PilotOf(party)].licences[system] = l;
    else
        corps_.SetLicence(party, system, l);
}

void Simulation::DropLicence(const std::string& party, const std::string& system)
{
    if (IsPilotParty(party))
    {
        const auto o = owners_.find(PilotOf(party));
        if (o != owners_.end())
            o->second.licences.erase(system);
    }
    else
        corps_.DropLicence(party, system);
}

void Simulation::TellParty(const std::string& party, const std::string& text)
{
    // The pilot's journal, kept for them while away (#431 C2); a corporation's, which its
    // members read.
    if (IsPilotParty(party))
    {
        TellOwner(PilotOf(party), text, text, false, Ev::Kind::Notice);
        return;
    }
    const Corps::Corp* c = corps_.Find(party);
    if (c == nullptr || c->Ceo() == nullptr)
        return;
    TellOwner(c->Ceo()->account, text, text, false, Ev::Kind::Notice);
}

bool Simulation::PayRent(const std::string& party, const std::string& holder, double rent,
                         std::string& why)
{
    if (IsPilotParty(party))
    {
        const std::string account = PilotOf(party);
        ClientSession*    on = nullptr;
        for (auto& kv : sessions_)
            if (kv.second.accountName == account)
                on = &kv.second;
        if (on != nullptr)
        {
            if (on->account.GetMoney() < rent)
            {
                why = "the rent is " + Credits(rent) + " and you have " +
                      Credits(on->account.GetMoney());
                return false;
            }
            on->account.AddMoney(-rent);
        }
        else
        {
            // Away: within what they had when last on, and what came in since; it is taken
            // from their money when they are next on (StepOwners).
            OwnerBook& b = owners_[account];
            if (!b.seen || b.money + b.owedMoney < rent)
            {
                why = "the rent is " + Credits(rent) + " and you had " +
                      Credits(b.money + b.owedMoney);
                return false;
            }
            b.owedMoney -= rent;
        }
    }
    else if (!corps_.Debit(party, rent))
    {
        const Corps::Corp* c = corps_.Find(party);
        why = "the rent is " + Credits(rent) + " and the treasury holds " +
              Credits(c != nullptr ? c->treasury : 0.0);
        return false;
    }
    FactionId f;
    if (FactionKey(holder, f))
        minds_[f].treasury += rent;
    else
        corps_.Credit(holder, rent);
    return true;
}

bool Simulation::BuyLicence(ClientSession& s)
{
    auto refuse = [&](const std::string& why)
    {
        s.RecordEvent(Ev::Kind::Notice, "No licence: " + why);
        return false;
    };
    SystemState* st = SystemOf(s);
    if (st == nullptr || s.accountName.empty())
        return refuse("only an account can hold a licence");
    const std::string holder = LicenceHolder(*st);
    const std::string party = PartyOf(s.accountName);
    const std::string where = SystemName(st->id);
    if (holder.empty())
        return refuse("nobody here licenses building -- " + where + " is open to anyone");
    if (party == holder)
        return refuse(where + " is your corporation's: it needs no licence");
    FactionId f;
    if (FactionKey(holder, f))
    {
        // Bought where the faction does business: docked at one of its stations here.
        const Entity* at = nullptr;
        for (const auto& e : st->entities)
            if (s.dockedStationId != 0 && e->GetId() == s.dockedStationId)
                at = e.get();
        if (at == nullptr || at->GetKind() != EntityKind::Station ||
            static_cast<const Station&>(*at).GetFaction() != f)
            return refuse("dock at a " + FactionName(f) + " station in " + where + " to buy one");
    }
    if (!IsPilotParty(party))
    {
        const Corps::Corp*   c = corps_.Find(party);
        const Corps::Member* m = c != nullptr ? c->Find(s.accountName) : nullptr;
        if (m == nullptr || m->role == Corps::Role::Member)
            return refuse("your corporation's licences are paid from its treasury, by its CEO or "
                          "a director");
    }
    std::string why;
    if (!Admits(*st, holder, party, why))
        return refuse(why);
    const WorldRules::Licences& r = WorldRules::LicenceRules();
    const double                period = r.periodDays * 86400.0;
    const double                grace = r.graceHours * 3600.0;
    const Corps::Licence*       had = LicenceOf(party, st->id);
    // A licence from this holder, not lapsed, runs on from where it was paid to; anything else
    // starts now.
    double from = time_;
    if (had != nullptr && had->holder == holder && time_ < had->until + grace)
    {
        if (had->until - time_ >= period)
            return refuse("it is paid for " + Hours(had->until - time_) + " more already");
        from = had->until;
    }
    if (!PayRent(party, holder, r.rent, why))
        return refuse(why);
    Corps::Licence l;
    l.holder = holder;
    l.until = from + period;
    SetLicence(party, st->id, l);
    RefreshLicences(*st);
    const std::string text = "Licensed by " + HolderName(holder) + " to build in " + where +
                             " for " + Hours(l.until - time_) + ": " + Credits(r.rent) + " paid" +
                             (IsPilotParty(party) ? "" : " from the treasury") +
                             ", and taken each week from then on";
    if (IsPilotParty(party))
        s.RecordEvent(Ev::Kind::Notice, text);
    else
        TellParty(party, s.accountName + ": " + text);
    return true;
}

namespace
{
// A licence's state as a player reads it, and the instant that goes with it.
void StateOf(const Corps::Licence& l, double now, double grace, std::string& state, double& until)
{
    state = now < l.until ? "licensed" : now < l.until + grace ? "unpaid" : "trespass";
    until = now < l.until ? l.until : now < l.until + grace ? l.until + grace : 0.0;
}
}  // namespace

Proto::LicenceView Simulation::LicenceViewFor(const ClientSession& s) const
{
    Proto::LicenceView v;
    const SystemState* st = SystemOf(s);
    if (st == nullptr || s.accountName.empty())
        return v;
    const std::string holder = LicenceHolder(*st);
    const std::string party = PartyOf(s.accountName);
    if (holder.empty() || party == holder)
        return v;
    const WorldRules::Licences& r = WorldRules::LicenceRules();
    v.holder = HolderName(holder);
    const Corps::Corp* c = IsPilotParty(party) ? nullptr : corps_.Find(party);
    v.party = c != nullptr ? Tag(*c) : std::string("you");
    v.admitted = Admits(*st, holder, party, v.why);
    v.rent = r.rent;
    v.periodDays = r.periodDays;
    const Corps::Member* m = c != nullptr ? c->Find(s.accountName) : nullptr;
    v.canPay = c == nullptr || (m != nullptr && m->role != Corps::Role::Member);
    if (const Corps::Licence* l = LicenceOf(party, st->id))
        if (l->holder == holder)
            StateOf(*l, time_, r.graceHours * 3600.0, v.state, v.until);
    return v;
}

std::vector<Proto::LicenceView> Simulation::CorpLicences(const std::string& corpId) const
{
    std::vector<Proto::LicenceView> out;
    const Corps::Corp*              c = corps_.Find(corpId);
    if (c == nullptr)
        return out;
    const WorldRules::Licences& r = WorldRules::LicenceRules();
    for (const auto& kv : c->licences)
    {
        Proto::LicenceView v;
        v.system = kv.first;
        v.holder = HolderName(kv.second.holder);
        v.party = Tag(*c);
        v.admitted = true;
        v.rent = r.rent;
        v.periodDays = r.periodDays;
        StateOf(kv.second, time_, r.graceHours * 3600.0, v.state, v.until);
        out.push_back(std::move(v));
    }
    return out;
}

void Simulation::GrantLicence(const std::string& account, const std::string& system)
{
    SystemState* st = SystemById(system);
    if (st == nullptr)
        return;
    const std::string holder = LicenceHolder(*st);
    const std::string party = PartyOf(account);
    if (holder.empty() || holder == party)
        return;
    Corps::Licence l;
    l.holder = holder;
    l.until = time_ + WorldRules::LicenceRules().periodDays * 86400.0;
    SetLicence(party, system, l);  // what stands there is told on the next pass
}

void Simulation::RefreshLicences(SystemState& st)
{
    const std::string holder = LicenceHolder(st);
    const double      grace = WorldRules::LicenceRules().graceHours * 3600.0;
    st.trespass.clear();
    for (const auto& e : st.entities)
    {
        if (e->GetKind() != EntityKind::Structure || e->GetOwner().empty())
            continue;
        Structure& t = static_cast<Structure&>(*e);
        FactionId  owner;
        if (Outposts::FactionOf(t.GetOwner(), owner))
            continue;  // a faction's own builds where it likes
        LicenceState      state = LicenceState::None;
        double            until = 0.0;
        const std::string party = PartyOf(t.GetOwner());
        if (!holder.empty() && party != holder)
        {
            const Corps::Licence* l = LicenceOf(party, st.id);
            if (l == nullptr || l->holder != holder)
            {
                // It stands in somebody's space without their licence -- built before
                // licences were, or its owner changed corporations. It has the grace a lapsing
                // licence has: the rent is asked from now, and taken if the party can pay it
                // and is admitted (StepLicences).
                Corps::Licence fresh;
                fresh.holder = holder;
                fresh.until = time_;
                SetLicence(party, st.id, fresh);
                TellParty(party, t.GetName() + " in " + SystemName(st.id) + " stands in " +
                                     HolderName(holder) + "'s space: a licence is " +
                                     Credits(WorldRules::LicenceRules().rent) +
                                     " a week, asked from now. Unpaid, it trespasses in " +
                                     Hours(grace));
                l = LicenceOf(party, st.id);
            }
            if (time_ < l->until)
            {
                state = LicenceState::Licensed;
                until = l->until;
            }
            else if (time_ < l->until + grace)
            {
                state = LicenceState::Unpaid;
                until = l->until + grace;
            }
            else
                state = LicenceState::Trespass;
        }
        if (state == LicenceState::Trespass)
            st.trespass.push_back(t.GetId());
        if (t.GetLicence() != state || t.GetLicenceUntil() != until)
        {
            t.SetLicence(state, until);
            MarkStaticChanged(st, t.GetId());  // everyone here sees where it stands
        }
    }
}

void Simulation::StepLicences()
{
    const WorldRules::Licences& r = WorldRules::LicenceRules();
    const double                period = r.periodDays * 86400.0;
    const double                grace = r.graceHours * 3600.0;
    // Every licence, by its party: listed first, since paying one changes where they are kept.
    std::vector<std::pair<std::string, std::string>> all;
    for (const auto& kv : corps_.All())
        for (const auto& l : kv.second.licences)
            all.emplace_back(kv.first, l.first);
    for (const auto& kv : owners_)
        for (const auto& l : kv.second.licences)
            all.emplace_back(InfluenceMap::PILOT + kv.first, l.first);
    std::set<std::string> touched;
    for (const auto& pl : all)
    {
        const std::string&    party = pl.first;
        const std::string&    sys = pl.second;
        const Corps::Licence* had = LicenceOf(party, sys);
        if (had == nullptr)
            continue;
        Corps::Licence    l = *had;
        SystemState*      st = SystemById(sys);
        const std::string holder = st != nullptr ? LicenceHolder(*st) : std::string();
        const std::string where = SystemName(sys);
        bool              stands = false;
        if (st != nullptr)
            for (const auto& e : st->entities)
                stands = stands || (e->GetKind() == EntityKind::Structure &&
                                    !e->GetOwner().empty() && PartyOf(e->GetOwner()) == party);
        // Nobody to pay any more, or nothing of the party's there once what it paid for runs
        // out: the licence ends.
        if (holder.empty() || holder == party || (!stands && time_ >= l.until))
        {
            DropLicence(party, sys);
            touched.insert(sys);
            TellParty(party, "The licence to build in " + where + " has ended: " +
                                 (holder.empty() || holder == party
                                      ? std::string("it needs none now")
                                      : std::string("nothing of yours stands there")));
            continue;
        }
        if (l.holder != holder)
        {
            // The system changed hands: the new holder asks its rent from now.
            TellParty(party, where + " is " + HolderName(holder) + "'s now: the licence from " +
                                 HolderName(l.holder) + " is void, and " + HolderName(holder) +
                                 " asks " + Credits(r.rent) + " a week from now");
            l.holder = holder;
            l.until = time_;
            l.told = 0;
            SetLicence(party, sys, l);
            touched.insert(sys);
        }
        if (time_ < l.until)
            continue;
        if (time_ >= l.until + grace)
        {
            if (l.told < 2)
            {
                l.told = 2;
                SetLicence(party, sys, l);
                touched.insert(sys);
                TellParty(party, "The licence to build in " + where +
                                     " has lapsed unpaid: what you have there is trespassing -- "
                                     "anyone may shoot it, and " +
                                     HolderName(holder) +
                                     " may evict it. Buy a licence again to stand there lawfully");
            }
            continue;
        }
        // Due: taken if the holder still admits the party and it can pay.
        std::string why;
        if (Admits(*st, holder, party, why) && PayRent(party, holder, r.rent, why))
        {
            l.until += period;
            l.told = 0;
            SetLicence(party, sys, l);
            touched.insert(sys);
            TellParty(party, "Paid " + HolderName(holder) + " " + Credits(r.rent) +
                                 " for the licence to build in " + where + ", for " +
                                 Hours(l.until - time_) + " more");
            continue;
        }
        if (l.told < 1)
        {
            l.told = 1;
            SetLicence(party, sys, l);
            touched.insert(sys);
            TellParty(party, "The licence to build in " + where + " is unpaid: " + why +
                                 ". It lapses in " + Hours(l.until + grace - time_));
        }
    }
    // Where every player's structure stands: wherever any player has built.
    for (auto& kv : systems_)
        if (!kv.second.profile.builders.empty() || touched.count(kv.first) != 0)
            RefreshLicences(kv.second);
}
