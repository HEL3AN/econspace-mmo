// Delivery contracts a short market posts (#343, #407). One translation unit of Simulation (#17).
//
// A market runs short of a good when what its system uses outruns what is made and carried
// in. Its owner then posts a contract at its station: bring so many units by a deadline, for a
// price. The price is the market's own -- between what selling those units there would fetch
// and what buying them there would cost -- and further across that gap the shorter it is. So a
// contract always pays better than selling at the market, and never pays for units bought at
// the market itself. It is posted only where a market nearby sells the units cheaper than it
// pays: a shortage the whole neighbourhood shares has nothing to fill it from. And the owner
// pays from its treasury, setting the price aside when it posts and giving back what was not
// earned when the contract ends, so it posts nothing it cannot pay.
//
// A contract is shown on the boards of its owner's stations within the jobs' reach, like a
// plan's job (#345), and taken by one pilot. It is done docked at its station with the goods in
// the hold. Those go into the market, which is what the contract was for.
#include "sim/Simulation.h"
#include "sim/ClientSession.h"
#include "sim/WorldRules.h"

#include "core/Faction.h"
#include "economy/Market.h"
#include "economy/Resource.h"
#include "entities/Ship.h"
#include "entities/Station.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace
{
std::string Money(double cr)
{
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.0f cr", cr);
    return buf;
}

// Every system within `depth` jumps of `from`, by how many.
std::map<std::string, int> Within(const Simulation& sim, const std::string& from, int depth)
{
    std::map<std::string, int> found;
    if (!sim.HasSystem(from))
        return found;
    found[from] = 0;
    std::vector<std::string> ring{ from };
    for (int d = 1; d <= depth && !ring.empty(); d++)
    {
        std::vector<std::string> next;
        for (const std::string& id : ring)
            for (const std::string& n : sim.Neighbors(id))
                if (sim.HasSystem(n) && found.emplace(n, d).second)
                    next.push_back(n);
        ring = std::move(next);
    }
    return found;
}
}  // namespace

Simulation::MarketHolder Simulation::MarketStation(const SystemState& st) const
{
    // A faction's station that trades. The one of whoever controls the system if it has one --
    // its market is the system's -- else the first.
    MarketHolder h;
    for (const auto& e : st.entities)
    {
        if (e->GetKind() != EntityKind::Station || !e->Has(Component::Market))
            continue;
        const FactionId f = static_cast<const Station&>(*e).GetFaction();
        if (h.stationId == 0 || (f == st.agg.controller && h.faction != st.agg.controller))
        {
            h.stationId = e->GetId();
            h.faction = f;
        }
    }
    return h;
}

void Simulation::PayFee(const SystemState& st, double fee)
{
    if (fee <= 0.0)
        return;
    const MarketHolder h = MarketStation(st);
    if (h.stationId != 0)
        director_.Mind(h.faction).treasury += fee;
}

const Entity* Simulation::ContractStation(const Contract& c) const
{
    const SystemState* st = SystemById(c.system);
    if (st == nullptr || c.station.empty())
        return nullptr;
    for (const auto& kv : st->keys)
        if (kv.second == c.station)
            for (const auto& e : st->entities)
                if (e->GetId() == kv.first && e->GetKind() == EntityKind::Station)
                    return e.get();
    return nullptr;
}

double Simulation::ContractPrice(const SystemState& st, ResourceType good, int amount,
                                 double shortness) const
{
    const WorldRules::Contracts& r = WorldRules::ContractRules();
    const double                 fee = Goods::Rules().fee;
    // Selling here and buying here, both at the first unit's price and the fee: the price of
    // a contract lies between, so it beats the one and never pays for the other.
    const double lo = amount * st.market.Bid(good) * (1.0 - fee);
    const double hi = amount * st.market.Ask(good) * (1.0 + fee);
    // Lower security pays better (#431 L5, slice 11): the premium grows, never past the price
    // of buying here.
    const double across = std::clamp((r.premium + r.premiumShort * shortness) *
                                         WorldRules::SecurityRules().Pay(st.agg.security),
                                     0.0, 1.0);
    const double price = lo + (hi - lo) * across;
    // Somewhere to fill it from: the cheapest market near that has the units, for less.
    double cheapest = -1.0;
    for (const auto& kv : Within(*this, st.id, r.sourceReach))
    {
        const SystemState* from = SystemById(kv.first);
        if (kv.second == 0 || from == nullptr || from->profile.markets <= 0 ||
            from->market.Stock(good) < amount)
            continue;
        const double cost = from->market.QuoteBuy(good, amount) * (1.0 + fee);
        if (cheapest < 0.0 || cost < cheapest)
            cheapest = cost;
    }
    if (cheapest < 0.0 || cheapest * (1.0 + r.sourceMargin) > price)
        return 0.0;
    return price;
}

void Simulation::PostContracts()
{
    const WorldRules::Contracts& r = WorldRules::ContractRules();
    if (r.perMarket <= 0)
        return;
    std::map<std::string, int>            open;
    std::set<std::pair<std::string, int>> asked;  // (system, good) already under contract
    for (const auto& kv : contracts_)
    {
        if (kv.second.IsExchange())  // pilots' (#473): not the market's to count
            continue;
        open[kv.second.system]++;
        asked.insert({ kv.second.system, kv.second.good.Index() });
    }
    for (auto& kv : systems_)
    {
        SystemState& st = kv.second;
        if (st.profile.markets <= 0 || open[st.id] >= r.perMarket)
            continue;
        const MarketHolder h = MarketStation(st);
        const auto         key = st.keys.find(h.stationId);
        if (h.stationId == 0 || key == st.keys.end())
            continue;
        // What it is short of, the shortest first: how far below the line, as a share of it.
        std::vector<std::pair<double, ResourceType>> shortOf;
        for (ResourceType g : AllResourceTypes())
        {
            // A module is asked for only where it is made or used: nobody posts a contract
            // for a part their own yard cannot build or use (#472 slice 10).
            if (Goods::Def(g).IsModule() && !st.profile.rates.MakesOrUses(g))
                continue;
            const double line = r.shortAt * st.market.Target(g);
            const double stock = st.market.Stock(g);
            if (line > 0.0 && stock < line && asked.count({ st.id, g.Index() }) == 0)
                shortOf.push_back({ 1.0 - stock / line, g });
        }
        std::stable_sort(shortOf.begin(), shortOf.end(),
                         [](const auto& a, const auto& b) { return a.first > b.first; });
        double& treasury = director_.Mind(h.faction).treasury;
        for (const auto& sg : shortOf)
        {
            if (open[st.id] >= r.perMarket)
                break;
            const ResourceType g = sg.second;
            // Sized in room (#572), as a hold is: `minAmount` and `maxAmount` are volume, so a
            // bulky good is asked for in fewer units, and one that does not fit at all is not.
            const int vol = Goods::Volume(g);
            const int most = r.maxAmount / vol;
            if (most < 1)
                continue;
            const int amount =
                std::clamp((int)std::lround(r.fillTo * st.market.Target(g) - st.market.Stock(g)),
                           std::min(most, (r.minAmount + vol - 1) / vol), most);
            const double price = ContractPrice(st, g, amount, sg.first);
            if (price <= 0.0 || price > treasury)
                continue;
            treasury -= price;  // set aside until it is paid out or the contract ends
            Contract c;
            c.id = nextContractId_++;
            c.faction = h.faction;
            c.system = st.id;
            c.station = key->second;
            c.good = g;
            c.amount = amount;
            c.reward = price;
            c.postedAt = time_;
            c.deadline = time_ + r.hours * 3600.0;
            contracts_[c.id] = c;
            open[st.id]++;
        }
    }
}

void Simulation::EndContract(int id)
{
    const auto it = contracts_.find(id);
    if (it == contracts_.end() || it->second.IsExchange())  // a pilot's goes back by EndDeal
        return;
    director_.Mind(it->second.faction).treasury +=
        it->second.reward;  // not earned: the owner's again
    contracts_.erase(it);
}

void Simulation::StepContracts()
{
    const WorldRules::Contracts& r = WorldRules::ContractRules();
    // A pilot's: out of time, or over.
    for (auto& kv : sessions_)
    {
        ClientSession&        s = kv.second;
        std::vector<Mission>& active = s.missions.Active();
        for (int i = (int)active.size() - 1; i >= 0; i--)
        {
            const Mission& m = active[(size_t)i];
            if (m.type != MissionType::Contract)
                continue;
            if (contracts_.count(m.contractId) == 0)
                FailJob(s, i, "the contract is over", false);
            else if (time_ > m.deadline)
                FailJob(s, i, "its time ran out", true);
        }
    }
    // The market's: out of time, or -- nobody having taken it -- no longer short.
    std::vector<int> over;
    for (const auto& kv : contracts_)
    {
        const Contract& c = kv.second;
        if (c.IsExchange())
            continue;
        const SystemState* st = SystemById(c.system);
        const bool filled = !c.taken && st != nullptr &&
                            st->market.Stock(c.good) >= r.fillTo * st->market.Target(c.good);
        if (time_ > c.deadline || st == nullptr || filled)
            over.push_back(kv.first);
    }
    for (int id : over)
        EndContract(id);
    StepBoard();  // pilots' contracts out of time go back to whoever posted them (#473)
    // And once every `every` seconds, what the markets are short of now.
    const double slot = std::floor(time_ / r.every);
    if (slot != contractSlot_)
    {
        contractSlot_ = slot;
        PostContracts();
    }
}

void Simulation::ContractsPostedAt(const ClientSession& s, const Station& giver,
                                   const std::map<std::string, int>& near,
                                   std::vector<Mission>&             out) const
{
    const WorldRules::Contracts& r = WorldRules::ContractRules();
    const FactionId              f = giver.GetFaction();
    const float repMul = MissionRewardMultiplier(Factions::TierOf(s.account.GetReputation(f)));
    std::vector<std::pair<int, const Contract*>> found;  // by how far, then by when posted
    for (const auto& kv : contracts_)
    {
        const Contract& c = kv.second;
        const auto      at = near.find(c.system);
        if (c.IsExchange() || c.taken || c.faction != f || at == near.end())
            continue;
        found.push_back({ at->second, &c });
    }
    std::stable_sort(found.begin(), found.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    int shown = 0;
    for (const auto& jc : found)
    {
        if (shown >= r.perBoard)
            break;
        const Contract& c = *jc.second;
        const Entity*   station = ContractStation(c);
        if (station == nullptr)
            continue;
        Mission m;
        m.type = MissionType::Contract;
        m.faction = f;
        m.giverStationId = giver.GetId();
        m.destStationId = station->GetId();
        m.resource = c.good;
        m.targetCount = c.amount;
        // The price is the market's and was set aside as it is: a pilot's standing does not
        // change it, only what the work is worth to their name.
        m.rewardMoney = c.reward;
        m.rewardRep = (float)r.rep * repMul;
        m.contractId = c.id;
        m.offerId = Offers::ContractOffer(c.id);  // the same on every board (#425)
        m.systemId = c.system;
        m.systemName = SystemName(c.system);
        m.deadline = c.deadline;
        const std::string good = ResourceName(c.good);
        m.title =
            "Contract: " + std::to_string(c.amount) + " " + good + " to " + station->GetName();
        m.description = "The market at " + station->GetName() + " in " + m.systemName +
                        " is short of " + good + ". Bring " + std::to_string(c.amount) +
                        " and hand them over docked there, before the deadline. It pays more "
                        "than selling them there.";
        out.push_back(std::move(m));
        shown++;
    }
}

bool Simulation::TakeContract(ClientSession& s, const Mission& m)
{
    const auto it = contracts_.find(m.contractId);
    if (it == contracts_.end() || it->second.taken)
    {
        s.RecordEvent(Ev::Kind::Notice,
                      "Contract not taken: somebody has taken it, or it is over -- " + m.title);
        return false;
    }
    it->second.taken = true;
    return true;
}

bool Simulation::ContractReadyNow(const ClientSession& s, const Mission& m) const
{
    return m.type == MissionType::Contract && s.ship && s.dockedStationId != 0 &&
           s.dockedStationId == m.destStationId && contracts_.count(m.contractId) != 0 &&
           s.ship->GetCargoAmount(m.resource) >= m.targetCount;
}

bool Simulation::CompleteContract(ClientSession& s, int index)
{
    std::vector<Mission>& active = s.missions.Active();
    const Mission         m = active[(size_t)index];
    if (!ContractReadyNow(s, m))
    {
        s.RecordEvent(Ev::Kind::Notice, "Hand-in refused -- " + m.title +
                                            ": dock at its station with the goods in the hold");
        return false;
    }
    const auto   it = contracts_.find(m.contractId);
    const double paid = it->second.reward;  // set aside when it was posted
    contracts_.erase(it);
    s.ship->RemoveCargo(m.resource, m.targetCount);
    // What the market was short of, in its stock: the contract's whole point.
    if (SystemState* st = SystemOf(s))
        st->market.SetStock(m.resource, st->market.Stock(m.resource) + m.targetCount);
    s.account.AddMoney(paid);
    s.account.AddReputation(m.faction, m.rewardRep);
    s.account.GetSkills().AddXp(SkillType::Trading, (float)(paid * 0.05));
    active.erase(active.begin() + index);
    s.RecordEvent(Ev::Kind::Notice, "Contract done -- " + m.title + ", paid " + Money(paid) +
                                        " by " + FactionName(m.faction));
    return true;
}

// An exchange contract (#473) as the world saves it: what is held -- goods by their id, credits,
// and each chart's copy -- and what it asks, so nothing a restart could lose is only in memory.
nlohmann::json Simulation::ExchangeJson(const Contract& c)
{
    using nlohmann::json;
    json goods = json::array();
    for (const auto& [type, n] : c.giveGoods.All())
        goods.push_back({ Goods::Id(type), n });
    json ask = json::array();
    for (const Exchange::GoodLine& g : c.asking.goods)
        ask.push_back({ Goods::Id(ResourceType::FromIndex(g.type)), g.amount });
    json charts = json::array();
    for (const ChartCopy& k : c.giveCharts)
        charts.push_back({ { "system", k.system },
                           { "observed", k.observed },
                           { "at", k.at },
                           { "stations", k.stations },
                           { "structures", k.structures },
                           { "finds", k.finds },
                           { "seen", WorldLoader::ToJson(k.seen) },
                           { "hops", k.hops },
                           { "from", k.from } });
    return { { "id", c.id },
             { "kind", "exchange" },
             { "issuer", c.issuer },
             { "poster", c.poster },
             { "to", c.to },
             { "system", c.system },
             { "place", c.place },
             { "placeName", c.placeName },
             { "give", goods },
             { "giveCredits", c.giveCredits },
             { "giveCharts", charts },
             { "ask", ask },
             { "askCredits", c.asking.credits },
             { "postedAt", c.postedAt },
             { "deadline", c.deadline } };
}

bool Simulation::ExchangeFrom(const nlohmann::json& cj, Contract& c) const
{
    using nlohmann::json;
    c.kind = "exchange";
    c.id = cj.value("id", 0);
    c.issuer = cj.value("issuer", std::string());
    c.poster = cj.value("poster", std::string());
    c.to = cj.value("to", std::string());
    c.system = cj.value("system", std::string());
    c.place = cj.value("place", std::string());
    c.placeName = cj.value("placeName", std::string());
    c.giveCredits = cj.value("giveCredits", 0LL);
    c.asking.credits = cj.value("askCredits", 0LL);
    c.postedAt = cj.value("postedAt", 0.0);
    c.deadline = cj.value("deadline", 0.0);
    if (c.id <= 0 || c.issuer.empty() || c.poster.empty() || c.place.empty())
        return false;
    // A good this world no longer has cannot be given back: its line is dropped. The rest of
    // what was held still is -- the contract is kept as long as anything of it can be.
    auto goodAt = [](const json& l, ResourceType& g, int& n)
    {
        return l.is_array() && l.size() >= 2 && l[0].is_string() && l[1].is_number_integer() &&
               Goods::Find(l[0].get<std::string>(), g) && (n = l[1].get<int>()) > 0;
    };
    if (cj.contains("give") && cj["give"].is_array())
        for (const json& l : cj["give"])
        {
            ResourceType g;
            int          n = 0;
            if (goodAt(l, g, n))
                c.giveGoods.Add(g, n);
        }
    if (cj.contains("ask") && cj["ask"].is_array())
        for (const json& l : cj["ask"])
        {
            ResourceType g;
            int          n = 0;
            if (goodAt(l, g, n))
                c.asking.goods.push_back({ g.Index(), n, "" });
        }
    if (cj.contains("giveCharts") && cj["giveCharts"].is_array())
        for (const json& k : cj["giveCharts"])
        {
            if (!k.is_object())
                continue;
            ChartCopy copy;
            copy.system = k.value("system", std::string());
            if (!HasSystem(copy.system))
                continue;
            copy.charted = true;
            copy.observed = k.value("observed", false);
            copy.at = k.value("at", -1.0);
            copy.stations = k.value("stations", 0);
            copy.structures = k.value("structures", 0);
            copy.finds = k.value("finds", std::vector<std::string>{});
            copy.seen = WorldLoader::SightingFrom(k.value("seen", json::object()));
            copy.hops = k.value("hops", 0);
            copy.from = k.value("from", std::string());
            c.giveCharts.push_back(std::move(copy));
        }
    return true;
}

void Simulation::SaveContracts(nlohmann::json& j) const
{
    using nlohmann::json;
    json list = json::array();
    for (const auto& kv : contracts_)
    {
        const Contract& c = kv.second;
        if (c.IsExchange())
        {
            list.push_back(ExchangeJson(c));
            continue;
        }
        list.push_back({ { "id", c.id },
                         { "faction", Factions::Id(c.faction) },
                         { "system", c.system },
                         { "station", c.station },
                         { "good", Goods::Id(c.good) },
                         { "amount", c.amount },
                         { "reward", c.reward },
                         { "postedAt", c.postedAt },
                         { "deadline", c.deadline },
                         { "taken", c.taken } });
    }
    j["contracts"] = std::move(list);
    j["nextContract"] = nextContractId_;
}

void Simulation::ReadContracts(const nlohmann::json& j)
{
    contracts_.clear();
    nextContractId_ = std::max(1, j.value("nextContract", 1));
    if (!j.contains("contracts") || !j["contracts"].is_array())
        return;
    for (const nlohmann::json& cj : j["contracts"])
    {
        if (!cj.is_object())
            continue;
        if (cj.value("kind", std::string()) == "exchange")
        {
            Contract e;
            if (ExchangeFrom(cj, e) && e.id > 0 && contracts_.count(e.id) == 0)
            {
                nextContractId_ = std::max(nextContractId_, e.id + 1);
                contracts_[e.id] = std::move(e);
            }
            continue;
        }
        Contract c;
        c.id = cj.value("id", 0);
        c.system = cj.value("system", std::string());
        c.station = cj.value("station", std::string());
        c.amount = cj.value("amount", 0);
        c.reward = cj.value("reward", 0.0);
        c.postedAt = cj.value("postedAt", 0.0);
        c.deadline = cj.value("deadline", 0.0);
        c.taken = cj.value("taken", false);
        // What it cannot place is let go -- and with it what was set aside, which was the
        // treasury's when the world was saved and is gone with the faction or the good.
        if (c.id <= 0 || contracts_.count(c.id) != 0 || !HasSystem(c.system) || c.amount <= 0 ||
            !Factions::Find(cj.value("faction", std::string()), c.faction) ||
            !Goods::Find(cj.value("good", std::string()), c.good))
            continue;
        contracts_[c.id] = c;
        nextContractId_ = std::max(nextContractId_, c.id + 1);
    }
}
