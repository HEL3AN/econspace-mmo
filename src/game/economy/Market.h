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
    // Moves what the price drifts toward, and the price with it in proportion: a market whose
    // place in the galaxy changed (#342 S5) keeps how far a sale has pushed it off its base.
    void SetBasePrice(ResourceType type, double base);
    // What a market at the heart of a civilisation pays (the base everywhere before #342 S5).
    static double DefaultBasePrice(ResourceType type);

    // Prices gradually return to their base values.
    void Update(float dt);
    // The same recovery over `seconds` at once, as the ticks of a cold system would have
    // made it (#295): one call where there would have been sixty a second.
    void Recover(float seconds);

private:
    std::map<ResourceType, double> prices_;
    std::map<ResourceType, double> basePrices_;
};

// Frontier prices (#342 S5). A station's base price is the centre's times a factor that grows
// with how far it is, in jumps, along its supply route to the nearest civilisation centre.
// What is dug up where it is sold -- ore, finds -- is cheaper the further out it is, since
// there it is common and the buyers few; what has to be carried there from a centre -- food,
// medicine -- is dearer. Every resource the game has today is ore; the other side of the rule
// is here for the goods that are carried, so a delivery has a reason without a script.
enum class Supply
{
    Local,     // mined or found where it is sold
    Imported,  // carried out from a centre
};
Supply SupplyOf(ResourceType type);
// The factor at `jumps` from the nearest centre: 1 at a centre (and for a negative count -- a
// system no centre reaches, or a world without centres), falling to 0.55 for what is local
// and rising to 2 for what is imported.
double RemotenessFactor(Supply supply, int jumps);
