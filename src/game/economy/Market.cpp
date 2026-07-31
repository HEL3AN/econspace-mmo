#include "economy/Market.h"

Market::Market()
{
    basePrices_[ResourceType::Iron] = 10.0;
    basePrices_[ResourceType::Ice] = 15.0;
    basePrices_[ResourceType::Crystal] = 40.0;
    prices_ = basePrices_;
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
        price += 0.1 * diff * dt;
    }
}
