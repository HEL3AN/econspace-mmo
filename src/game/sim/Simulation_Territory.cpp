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
        // between factions their own rule stands.
        std::string bestKey;
        float       best = 0.0f;
        bool        isFaction = false;
        FactionId   bestFaction = Factions::Unaffiliated();
        for (const std::string& id : m.corpClaims)
            if (id != a.holderCorp && (bestKey.empty() || m.corp.at(id) > best))
            {
                bestKey = id;
                best = m.corp.at(id);
            }
        if (!a.holderCorp.empty())
            for (FactionId f : st.profile.outposts)
                if (bestKey.empty() || m.faction[f] > best)
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
            TakeControl(a, bestFaction);  // clears the holder
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
