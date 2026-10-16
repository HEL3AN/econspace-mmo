#pragma once

#include "economy/Resource.h"
#include <vector>

// What a system does to its market every hour (#343): what its belts and planets give, the
// recipes its stations run (each already times how many stations run it) and what they use
// up. Counted from the static layer with the rest of SystemProfile, so a cold system and a
// hot one keep the same books.
struct MarketRates
{
    std::vector<double> source;  // by good index, units an hour
    std::vector<double> use;     // by good index, units an hour
    std::vector<Recipe> recipes;
    int markets = 0;  // what trades here: the target stock is this many times a good's
};

// A system's market (#343): a stock of every good, and prices that follow it. The price of a
// good is its base -- the centre's, times how far this market is from one (#342 S5) -- times
// how short of its target stock the market is, so every unit bought or sold moves it. The
// stock moves by the coarse pass's arithmetic (`Step`) and by trade, never per frame.
class Market
{
public:
    Market();  // every good at its target stock and its centre's price

    // What the market pays a seller for one unit now, and what it asks a buyer.
    double GetPrice(ResourceType type) const { return Bid(type); }
    double Bid(ResourceType type) const;
    double Ask(ResourceType type) const;
    double Mid(ResourceType type) const;
    // The price at the target stock: what prices return to.
    double GetBasePrice(ResourceType type) const;
    // Moves the base -- a market whose place in the galaxy changed (#342 S5). How far trade
    // has pushed the price off it stays, since that is the stock's.
    void SetBasePrice(ResourceType type, double base);
    // The centre's price, from the goods registry.
    static double DefaultBasePrice(ResourceType type);

    double Stock(ResourceType type) const;
    double Target(ResourceType type) const;
    void   SetStock(ResourceType type, double stock);
    // How many markets this one stands for: its target stock is that many times a good's. A
    // market nothing has happened to yet starts at the new targets.
    void SetMarkets(int markets);

    // Sells `amount` units into the market and returns what they fetch at the bid, unit by
    // unit, each lowering the price of the next. Buy takes them out at the ask, as many as
    // are in stock, and returns what they cost; `bought` says how many that was.
    double Sell(ResourceType type, int amount);
    double Buy(ResourceType type, int amount, int& bought);
    // The same sums without trading.
    double QuoteSell(ResourceType type, int amount) const;
    double QuoteBuy(ResourceType type, int amount) const;

    // `hours` of what the system does: sources add, recipes turn inputs into outputs as far
    // as the inputs are in stock, uses take away, and the trade nobody flies pulls every
    // stock toward its target at the registry's restock rate times `traffic`.
    void Step(const MarketRates& rates, double hours, double traffic);

private:
    void   Fit();  // sized to the registry
    double PriceAt(ResourceType type, double stock) const;

    std::vector<double> stock_;
    std::vector<double> base_;
    int                 markets_ = 1;
    bool                fresh_ = true;  // never stepped or loaded
};

// What a good's base price follows (#342 S5): mined where it is sold, or carried there.
Supply SupplyOf(ResourceType type);
// The factor at `jumps` from the nearest centre: 1 at a centre (and for a negative count -- a
// system no centre reaches, or a world without centres), falling to 0.55 for what is local
// and rising to 2 for what is imported.
double RemotenessFactor(Supply supply, int jumps);
