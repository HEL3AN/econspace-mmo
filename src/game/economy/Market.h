#pragma once

#include "economy/Resource.h"
#include <map>

// Market: the price of each resource. prices_ fluctuate, basePrices_ is what
// prices drift toward over time.
class Market
{
public:
    Market();

    double GetPrice(ResourceType type) const;
    double GetBasePrice(ResourceType type) const;

    // Sells amount units, returns the revenue. Side effect: the price drops.
    double Sell(ResourceType type, int amount);

    void SetPrice(ResourceType type, double price) { prices_[type] = price; }

    // Prices gradually return to their base values.
    void Update(float dt);
    // The same recovery over `seconds` at once, as the ticks of a cold system would have
    // made it (#295): one call where there would have been sixty a second.
    void Recover(float seconds);

private:
    std::map<ResourceType, double> prices_;
    std::map<ResourceType, double> basePrices_;
};
