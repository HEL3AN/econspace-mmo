#include "economy/Market.h"

#include <algorithm>
#include <cmath>

namespace
{
// A market never holds more than this many times its target: a producer nobody buys from
// stops, rather than filling a number without end.
constexpr double STOCK_CAP = 10.0;
}  // namespace

Market::Market()
{
    Fit();
}

void Market::Fit()
{
    const size_t n = (size_t)Goods::Count();
    while (base_.size() < n)
        base_.push_back(Goods::Def(ResourceType::FromIndex((int)base_.size())).price);
    while (stock_.size() < n)
        stock_.push_back(Goods::Def(ResourceType::FromIndex((int)stock_.size())).stock * markets_);
    base_.resize(n);
    stock_.resize(n);
}

double Market::DefaultBasePrice(ResourceType type)
{
    return Goods::Def(type).price;
}

double Market::Target(ResourceType type) const
{
    return Goods::Def(type).stock * markets_;
}

void Market::SetMarkets(int markets)
{
    Fit();
    const int was = markets_;
    markets_ = std::max(1, markets);
    // A market new to the world is scaled to its targets, with whatever trade it has seen
    // since it was made. One that has been stepped, or was read from a save, keeps its stock:
    // a station added since raises the target, and the market is short until it catches up.
    if (fresh_ && was != markets_)
        for (double& s : stock_)
            s = s * markets_ / was;
    fresh_ = false;
}

double Market::PriceAt(ResourceType type, double stock) const
{
    const MarketRules& k = Goods::Rules();
    const size_t       i = (size_t)type.Index();
    if (i >= base_.size())
        return 0.0;
    // Server-side and never compared across machines (the snapshot carries the result), so
    // pow is fine here.
    const double short_ = Target(type) / std::max(stock, 0.5);
    return base_[i] * std::clamp(std::pow(short_, k.elasticity), k.floor, k.ceiling);
}

double Market::Mid(ResourceType type) const
{
    return PriceAt(type, Stock(type));
}

double Market::Bid(ResourceType type) const
{
    return Mid(type) * (1.0 - Goods::Rules().spread * 0.5);
}

double Market::Ask(ResourceType type) const
{
    return Mid(type) * (1.0 + Goods::Rules().spread * 0.5);
}

double Market::GetBasePrice(ResourceType type) const
{
    const size_t i = (size_t)type.Index();
    return i < base_.size() ? base_[i] : DefaultBasePrice(type);
}

void Market::SetBasePrice(ResourceType type, double base)
{
    Fit();
    if ((size_t)type.Index() < base_.size())
        base_[(size_t)type.Index()] = base;
}

double Market::Stock(ResourceType type) const
{
    const size_t i = (size_t)type.Index();
    return i < stock_.size() ? stock_[i] : Target(type);
}

void Market::SetStock(ResourceType type, double stock)
{
    Fit();
    if ((size_t)type.Index() < stock_.size())
        stock_[(size_t)type.Index()] = std::max(0.0, stock);
    fresh_ = false;
}

double Market::QuoteSell(ResourceType type, int amount) const
{
    const double spread = 1.0 - Goods::Rules().spread * 0.5;
    double       s = Stock(type), sum = 0.0;
    for (int i = 0; i < amount; i++, s += 1.0)
        sum += PriceAt(type, s) * spread;
    return sum;
}

double Market::QuoteBuy(ResourceType type, int amount) const
{
    const double spread = 1.0 + Goods::Rules().spread * 0.5;
    double       s = Stock(type), sum = 0.0;
    for (int i = 0; i < amount && s >= 1.0; i++, s -= 1.0)
        sum += PriceAt(type, s) * spread;
    return sum;
}

double Market::Sell(ResourceType type, int amount)
{
    if (amount <= 0)
        return 0.0;
    Fit();
    const double revenue = QuoteSell(type, amount);
    stock_[(size_t)type.Index()] += amount;
    return revenue;
}

double Market::Buy(ResourceType type, int amount, int& bought)
{
    Fit();
    bought = std::clamp(amount, 0, (int)std::floor(Stock(type)));
    if (bought <= 0)
        return 0.0;
    const double cost = QuoteBuy(type, bought);
    stock_[(size_t)type.Index()] -= bought;
    return cost;
}

void Market::Step(const MarketRates& rates, double hours)
{
    Fit();
    const size_t n = stock_.size();
    for (size_t i = 0; i < n && i < rates.source.size(); i++)
        stock_[i] += rates.source[i] * hours;
    // Recipes in the order the system lists them: an input two of them share goes to the
    // first while it lasts, the same way on every run.
    for (const Recipe& r : rates.recipes)
    {
        double cycles = r.rate * hours;
        for (const auto& in : r.in)
            if ((size_t)in.first.Index() < n)
                cycles = std::min(cycles, stock_[(size_t)in.first.Index()] / in.second);
        if (cycles <= 0.0)
            continue;
        for (const auto& in : r.in)
            if ((size_t)in.first.Index() < n)
                stock_[(size_t)in.first.Index()] -= cycles * in.second;
        for (const auto& out : r.out)
            if ((size_t)out.first.Index() < n)
                stock_[(size_t)out.first.Index()] += cycles * out.second;
    }
    for (size_t i = 0; i < n && i < rates.use.size(); i++)
        stock_[i] -= std::min(stock_[i], rates.use[i] * hours);
    for (size_t i = 0; i < n; i++)
        stock_[i] = std::clamp(stock_[i], 0.0, Target(ResourceType::FromIndex((int)i)) * STOCK_CAP);
}

int HaulAmount(const Market& from, const Market& to, ResourceType type, int cap)
{
    const MarketRules& k = Goods::Rules();
    const double       buyMul = (1.0 + k.spread * 0.5) * (1.0 + k.fee) * (1.0 + k.haulMargin);
    const double       sellMul = (1.0 - k.spread * 0.5) * (1.0 - k.fee);
    const double       here = from.Stock(type), there = to.Stock(type);
    // Whether the n-th unit still pays: it is bought at n - 1 below the stock here and sold at
    // n - 1 above the stock there. Paying less with every unit, so the answer is a search.
    auto pays = [&](int n)
    {
        return to.MidAt(type, there + (n - 1)) * sellMul >=
               from.MidAt(type, here - (n - 1)) * buyMul;
    };
    int lo = 0, hi = std::min(cap, (int)std::floor(here));
    if (hi <= 0 || !pays(1))
        return 0;
    lo = 1;
    while (lo < hi)
    {
        const int mid = (lo + hi + 1) / 2;
        if (pays(mid))
            lo = mid;
        else
            hi = mid - 1;
    }
    return lo;
}

Supply SupplyOf(ResourceType type)
{
    return Goods::Def(type).supply;
}

double RemotenessFactor(Supply supply, int jumps)
{
    if (jumps <= 0)
        return 1.0;
    if (supply == Supply::Local)
        return std::max(0.55, 1.0 - 0.03 * jumps);
    return std::min(2.0, 1.0 + 0.06 * jumps);
}
