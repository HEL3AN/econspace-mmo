#pragma once

#include "economy/Resource.h"
#include <vector>

// What a system does to its market every hour (#343): what its planets give, the recipes its
// stations run (each already times how many stations run it) and what they use up. Counted
// from the static layer with the rest of SystemProfile, so a cold system and a hot one keep
// the same books. What its belts give is not here: a belt runs out (#348), so the coarse pass
// asks each one.
struct MarketRates
{
    std::vector<double> source;  // by good index, units an hour
    std::vector<double> use;     // by good index, units an hour
    std::vector<Recipe> recipes;
    int markets = 0;  // what trades here: the target stock is this many times a good's

    // Whether the industries make `g` or use it up (#572): a module is asked for, and listed
    // empty, only where this is so.
    bool MakesOrUses(ResourceType g) const
    {
        if ((size_t)g.Index() < use.size() && use[(size_t)g.Index()] > 0.0)
            return true;
        for (const Recipe& r : recipes)
            for (const auto& out : r.out)
                if (out.first == g)
                    return true;
        return false;
    }
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
    // The mid price were the stock `stock`: what the next unit goes for after a trade.
    double MidAt(ResourceType type, double stock) const { return PriceAt(type, stock); }
    // The price at the target stock: what prices return to.
    double GetBasePrice(ResourceType type) const;
    // Moves the base -- a market whose place in the galaxy changed (#342 S5). How far trade
    // has pushed the price off it stays, since that is the stock's.
    void SetBasePrice(ResourceType type, double base);
    // The centre's price, from the goods registry.
    static double DefaultBasePrice(ResourceType type);

    double Stock(ResourceType type) const;
    // How much more of a good the shelf takes before its stock cap clips it.
    double Room(ResourceType type) const;
    double Target(ResourceType type) const;
    void   SetStock(ResourceType type, double stock);
    // How many markets this one stands for: its target stock is that many times a good's. A
    // market nothing has happened to yet starts at the new targets.
    void SetMarkets(int markets, const MarketRates* rates = nullptr);

    // Sells `amount` units into the market and returns what they fetch at the bid, unit by
    // unit, each lowering the price of the next. Buy takes them out at the ask, as many as
    // are in stock, and returns what they cost; `bought` says how many that was.
    double Sell(ResourceType type, int amount);
    double Buy(ResourceType type, int amount, int& bought);
    // The same sums without trading.
    double QuoteSell(ResourceType type, int amount) const;
    double QuoteBuy(ResourceType type, int amount) const;

    // `hours` of what the system does: sources add, recipes turn inputs into outputs as far
    // as the inputs are in stock, and uses take away. Nothing else moves a stock: what
    // arrives from other systems comes in a hauler's hold (#343).
    void Step(const MarketRates& rates, double hours);

    // What a holder's open ship order needs of a good (#584): a recipe does not take its input
    // below it, so that the Iron a fleet is waiting for is not made into Steel as it arrives.
    // Not saved: the faction pass sets it again every period.
    void ClearReserve() { reserve_.clear(); }
    void Reserve(ResourceType type, double amount);

    // The target stock of a good here: what the curve is drawn about (carried in the snapshot).
    double TargetOf(ResourceType type) const { return Target(type); }

    // What has moved through this market since it was made, per good, for a bench to read and
    // nothing else: not saved, never used by a rule. `wanted` is what the recipes and uses
    // would draw at their rates if their inputs were there; `clipped` is what the stock cap
    // took away; `mined` is what the system's crews put on the shelf, `fromFields` of it out of
    // fields with a life; `dispersed` is ore that left such a field unmined (#617).
    struct Flow
    {
        double mined = 0.0, wanted = 0.0, clipped = 0.0, dispersed = 0.0, fromFields = 0.0;
    };
    Flow FlowOf(ResourceType type) const;
    void NoteMined(ResourceType type, double amount, bool fromField = false);
    void NoteDispersed(ResourceType type, double amount);

private:
    void Fit();  // sized to the registry
    // Whether a recipe's output is worth more here than its input, fee included (#566).
    bool   Pays(const Recipe& r) const;
    double PriceAt(ResourceType type, double stock) const;

    std::vector<double> stock_;
    std::vector<double> base_;
    std::vector<double> reserve_;  // by good index
    std::vector<Flow>   flow_;     // by good index
    int                 markets_ = 1;
    bool                fresh_ = true;  // never stepped or loaded
};

// NPC haulers (#343): how many units of `type` are worth carrying from `from` to `to`, at most
// `cap` -- the most for which the last unit, bought here at the ask with the fee, still sells
// there at the bid with the fee for the registry's haul margin more. Every unit moves both
// prices, so a hauler never carries a gap away entirely. 0 -- nothing worth the trip.
int HaulAmount(const Market& from, const Market& to, ResourceType type, int cap);

// What a good's base price follows (#342 S5): mined where it is sold, or carried there.
Supply SupplyOf(ResourceType type);
// The factor at `jumps` from the nearest centre: 1 at a centre (and for a negative count -- a
// system no centre reaches, or a world without centres), falling to 0.55 for what is local
// and rising to 2 for what is imported.
double RemotenessFactor(Supply supply, int jumps);
