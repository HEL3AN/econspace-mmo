// NPC haulers (#343, slice 2). One translation unit of Simulation (#17).
//
// What moves goods between systems is the traders a system already has (#295): no stock is
// pulled towards a target by trade nobody flies any more. Once in every `haulEvery` seconds, a
// system with a market and a trader to send looks next door for the good whose bid there beats
// the ask here -- the fee both ways included -- by the haul margin, and a trader sets out with
// as much of it as still pays, `haulCargo` at most. A trader that leaves for any other reason
// (migration) carries what pays on its way too. Every unit moves both prices, so a hauler
// narrows a gap and never closes it: what is left between neighbours is the margin, and further
// apart it compounds.
//
// Conserved, as ships are (#295): the goods leave the market when the hauler sets out, are on
// the trip -- counted nowhere -- while it travels, and arrive with it. Where nobody is, that is
// a number taken off one market and, `travel` seconds later, added to another. Where somebody
// is, it is a real trader: one turning for the gate with the goods aboard, or one come through
// it and flying to a market to unload. Shot down, either leaves its cargo in a wreck for
// whoever searches it first.
#include "sim/Simulation.h"

#include "core/Blueprint.h"
#include "economy/Market.h"
#include "entities/Derelict.h"
#include "entities/NpcShip.h"
#include "entities/Structure.h"
#include "gen/Rng.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace
{
// What keys when a system's hauler sets out, besides where and when.
constexpr uint64_t HAUL_KEY = 0x4A017u;
// The coarse pass hauling is stepped by: MaintainWorld's two seconds.
constexpr double PASS_SECONDS = 2.0;
// A trader to send: a whole one, as for migration.
constexpr float WHOLE = 0.999f;
// A hauler come into a hot system has unloaded by this long after it arrived, wherever it is:
// a crossing at warp and a pause at the station.
constexpr double UNLOAD_SECONDS = 180.0;
// ...or as soon as it is this near a market's edge.
constexpr float UNLOAD_REACH = 300.0f;

// Whether an object is a market a hauler can unload at: a station's, or a finished structure's.
bool IsMarket(const Entity& e)
{
    if (!e.Has(Component::Market))
        return false;
    if (e.GetKind() == EntityKind::Structure)
        return !static_cast<const Structure&>(e).IsBuilding();
    return e.GetKind() == EntityKind::Station;
}
}  // namespace

Simulation::Haul Simulation::BestHaul(const SystemState& from, const std::string& to) const
{
    Haul               best;
    const SystemState* dest = SystemById(to);
    const MarketRules& k = Goods::Rules();
    if (dest == nullptr || from.profile.markets <= 0 || dest->profile.markets <= 0 ||
        k.haulCargo < 1.0)
        return best;
    for (ResourceType g : AllResourceTypes())
    {
        const int n = HaulAmount(from.market, dest->market, g, (int)k.haulCargo);
        if (n <= 0)
            continue;
        // What the trip makes, the fee both ways included: the good that makes the most goes,
        // the first of them on a tie.
        const double gain = dest->market.QuoteSell(g, n) * (1.0 - k.fee) -
                            from.market.QuoteBuy(g, n) * (1.0 + k.fee);
        if (gain > best.gain)
        {
            best.good = g;
            best.amount = n;
            best.gain = gain;
        }
    }
    return best;
}

void Simulation::StepHauls()
{
    const MarketRules& k = Goods::Rules();
    if (k.haulEvery <= 0.0 || k.haulCargo < 1.0)
        return;
    const double every = k.haulEvery;
    const double slot = std::floor(time_ / every);
    size_t       nth = 0;
    for (auto& kv : systems_)
    {
        const Population& flags = passTargets_[nth++];
        SystemState&      st = kv.second;
        if (st.profile.markets <= 0 || st.agg.traders < WHOLE)
            continue;
        // One moment in each stretch of the clock, drawn from what is decided -- this system,
        // this stretch -- never from what came before.
        Gen::Rng     rng(Gen::Key(HAUL_KEY, Intelligence::KeyOf(kv.first), (uint64_t)slot));
        const double at = slot * every + rng.Unit() * every;
        if (at > time_ || at <= time_ - PASS_SECONDS)
            continue;
        // Where it goes: the neighbour the best haul is for, the first of them on a tie.
        std::string to;
        Haul        best;
        for (const std::string& n : Neighbors(kv.first))
        {
            const Haul h = BestHaul(st, n);
            if (h.amount > 0 && h.gain > best.gain)
            {
                best = h;
                to = n;
            }
        }
        if (!to.empty())
            SetOut(st, to, NpcRole::Trader, flags.tradeFaction, best);
    }
}

std::vector<double> Simulation::GoodsInTransit() const
{
    std::vector<double> out((size_t)Goods::Count(), 0.0);
    for (const auto& kv : migrants_)
        if (kv.second.cargo > 0 && (size_t)kv.second.good.Index() < out.size())
            out[(size_t)kv.second.good.Index()] += kv.second.cargo;
    return out;
}

void Simulation::Unload(SystemState& st, ResourceType good, int amount)
{
    if (amount <= 0)
        return;
    // Sold, and the fee is the market's owner's, as a pilot's is (#348).
    PayFee(st, st.market.QuoteSell(good, amount) * Goods::Rules().fee);
    st.market.SetStock(good, st.market.Stock(good) + amount);
}

void Simulation::LoadArrival(SystemState& st, NpcShip& ship, ResourceType good, int amount)
{
    std::vector<Vector2> markets;
    for (const auto& e : st.entities)
        if (IsMarket(*e))
            markets.push_back(e->GetPosition());
    // Nowhere here to sell to any more: what it carried is sold the moment it is in.
    if (markets.empty())
    {
        Unload(st, good, amount);
        return;
    }
    ship.SetCargo(good, amount, time_ + UNLOAD_SECONDS);
    ship.SetRoute(markets);
}

void Simulation::DeliverCargo(SystemState& st, bool all)
{
    if (!IsHot(st))
        return;
    std::vector<const Entity*> markets;
    bool                       any = false;
    for (const auto& e : st.entities)
    {
        if (IsMarket(*e))
            markets.push_back(e.get());
        else if (e->GetKind() == EntityKind::Npc &&
                 static_cast<const NpcShip&>(*e).GetCargoAmount() > 0)
            any = true;
    }
    if (!any)
        return;
    std::vector<Vector2> lanes;  // where a trader goes once it has unloaded: its usual work
    for (auto& e : st.entities)
    {
        if (e->GetKind() != EntityKind::Npc)
            continue;
        NpcShip& n = static_cast<NpcShip&>(*e);
        // One on its way out carries what its trip took: the trip has it, not this market.
        if (n.GetCargoAmount() <= 0 || n.IsLeaving() || !n.IsAlive())
            continue;
        bool there = all || time_ >= n.GetCargoDue();
        for (const Entity* m : markets)
        {
            const float dx = m->GetPosition().x - n.GetPosition().x;
            const float dy = m->GetPosition().y - n.GetPosition().y;
            const float reach = m->GetSize() + UNLOAD_REACH;
            there = there || dx * dx + dy * dy <= reach * reach;
        }
        if (!there)
            continue;
        Unload(st, n.GetCargo(), n.GetCargoAmount());
        n.SetCargo(n.GetCargo(), 0);
        if (all)
            continue;
        if (lanes.empty())
        {
            const SpawnNodes nd = GatherNodes(st);
            lanes = nd.stations;
            lanes.insert(lanes.end(), nd.gates.begin(), nd.gates.end());
        }
        if (!lanes.empty())
            n.SetRoute(lanes);
    }
}

void Simulation::LeaveCargoWreck(SystemState& st, const NpcShip& ship)
{
    if (ship.GetCargoAmount() <= 0)
        return;
    auto wreck =
        std::make_unique<Derelict>(ship.GetPosition(), std::max(30.0f, ship.GetSize() * 3.0f),
                                   "Wreck of " + ship.GetName(), 0.0);
    wreck->SetCargo(ship.GetCargo(), ship.GetCargoAmount());
    // ...for a while, like any wreck (#41): an instant on the clock, so it goes on time in a
    // system nobody is in.
    if (Blueprints::Rules().wreckSeconds > 0.0f)
        wreck->SetExpiresAt(time_ + Blueprints::Rules().wreckSeconds);
    AddStatic(st.id, std::move(wreck), std::string());
}
