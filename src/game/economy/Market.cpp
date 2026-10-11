#include "economy/Market.h"

#include <algorithm>
#include <cmath>

namespace
{
constexpr double RECOVERY = 0.1;  // share of the gap to the base price closed per second
}

Market::Market()
{
    for (ResourceType t : AllResourceTypes())
        basePrices_[t] = DefaultBasePrice(t);
    prices_ = basePrices_;
}

double Market::DefaultBasePrice(ResourceType type)
{
    switch (type)
    {
        case ResourceType::Iron: return 10.0;
        case ResourceType::Ice: return 15.0;
        case ResourceType::Crystal: return 40.0;
    }
    return 0.0;
}

void Market::SetBasePrice(ResourceType type, double base)
{
    const double was = GetBasePrice(type);
    const double price = GetPrice(type);
    basePrices_[type] = base;
    prices_[type] = was > 0.0 ? price * (base / was) : base;
}

Supply SupplyOf(ResourceType type)
{
    switch (type)
    {
        case ResourceType::Iron:
        case ResourceType::Ice:
        case ResourceType::Crystal: return Supply::Local;
    }
    return Supply::Local;
}

double RemotenessFactor(Supply supply, int jumps)
{
    if (jumps <= 0)
        return 1.0;
    if (supply == Supply::Local)
        return std::max(0.55, 1.0 - 0.03 * jumps);
    return std::min(2.0, 1.0 + 0.06 * jumps);
}

double Market::GetPrice(ResourceType type) const
{
    auto it = prices_.find(type);
    return (it != prices_.end()) ? it->second : 0.0;
}

double Market::GetBasePrice(ResourceType type) const
{
    auto it = basePrices_.find(type);
    return (it != basePrices_.end()) ? it->second : 0.0;
}

double Market::Sell(ResourceType type, int amount)
{
    double price = GetPrice(type);
    double revenue = price * amount;

    // Selling knocks the price down: ~2% per unit, but no lower than 30% per trade.
    double dropFactor = 1.0 - 0.02 * amount;
    if (dropFactor < 0.3)
        dropFactor = 0.3;
    prices_[type] = price * dropFactor;

    return revenue;
}

void Market::Update(float dt)
{
    // The recovery rate is proportional to the deviation from the base price.
    for (auto& [type, price] : prices_)
    {
        double diff = basePrices_[type] - price;
        price += RECOVERY * diff * dt;
    }
}

void Market::Recover(float seconds)
{
    // Update's step taken continuously: the gap closes by e^(-rate*t). Server-side and
    // never compared across machines, so exp is fine here.
    const double closed = 1.0 - std::exp(-RECOVERY * (double)seconds);
    for (auto& [type, price] : prices_)
        price += (basePrices_[type] - price) * closed;
}
