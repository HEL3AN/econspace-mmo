#pragma once

#include <cstdint>
#include <string>
#include <vector>

// The goods the game trades (#343). They are data: a registry read from data/goods.json,
// as factions are (#355), and nothing in the code names one. A ResourceType is a handle: an
// index into this process's registry, meaningless outside it. A save names a good by its
// string id (`Goods::Id`); the wire sends goods in registry order, which both ends read from
// the same goods.json, as they read the stock designs from ships.json.
class ResourceType
{
public:
    // The first good in the registry.
    constexpr ResourceType() = default;
    static constexpr ResourceType FromIndex(int index) { return ResourceType((uint16_t)index); }

    constexpr int Index() const { return index_; }

    constexpr bool operator==(ResourceType o) const { return index_ == o.index_; }
    constexpr bool operator!=(ResourceType o) const { return index_ != o.index_; }
    constexpr bool operator<(ResourceType o) const { return index_ < o.index_; }

private:
    constexpr explicit ResourceType(uint16_t index) : index_(index) {}
    uint16_t index_ = 0;
};

// Frontier prices (#342 S5): which way remoteness from a centre moves a good's price.
enum class Supply
{
    Local,     // mined or found where it is sold: cheaper further out
    Imported,  // carried out from a centre: dearer further out
};

// One good as the registry holds it.
struct GoodDef
{
    std::string id;             // stable: what saves and other data files call it
    std::string name;           // what a player reads
    int         tier = 0;       // 0 raw, 1 refined, 2 components and consumables
    double      price = 10.0;   // a centre's price at the target stock
    double      stock = 200.0;  // what one market keeps of it, the target its price is set by
    Supply      supply = Supply::Local;
    bool        mined = false;  // an asteroid belt can hold it
};

// A station turning goods into goods: `rate` cycles an hour, each taking `in` and giving
// `out` -- as far as the inputs are in stock.
struct Recipe
{
    std::vector<std::pair<ResourceType, double>> in, out;
    double                                       rate = 0.0;
};

// What a kind of station does to its system's market (`industries` in goods.json): the
// recipes it runs and what its people and ships use up, per hour.
struct Industry
{
    std::vector<Recipe>                          recipes;
    std::vector<std::pair<ResourceType, double>> uses;
};

// How a market prices and keeps its stock (`market` in goods.json).
struct MarketRules
{
    double fee = 0.02;        // share of every trade's value, paid by whoever trades (#348)
    double spread = 0.1;      // between what a market asks and what it bids
    double elasticity = 0.7;  // price = base * (target / stock) ^ elasticity...
    double floor = 0.35;      // ...no lower than this times the base
    double ceiling = 3.0;     // ...and no higher than this
    // Belts (#348): the share of what is in a belt a system's miners take an hour, and the
    // share of a full belt that grows back an hour. A belt nobody else mines settles at
    // beltRegrow / beltMined of full, giving beltRegrow of full an hour for ever; a belt
    // players work too gives the miners less.
    double beltMined = 0.0;
    double beltRegrow = 0.0;
    // NPC haulers (#343): a system's traders carry goods to a neighbour that pays more, one
    // hauler every `haulEvery` seconds at most, `haulCargo` units at a time, and only while
    // the neighbour's bid beats the ask here, the fee both ways included, by `haulMargin`.
    // 0 seconds -- nobody hauls.
    double haulEvery = 0.0;
    double haulCargo = 0.0;
    double haulMargin = 0.1;
};

std::string  ResourceName(ResourceType type);            // what a player reads
ResourceType ResourceFromName(const std::string& name);  // by id or name; the first if neither

// Every good, in registry order -- for iterating in loops.
std::vector<ResourceType> AllResourceTypes();

namespace Goods
{
// Reads goods.json and replaces the registry with it. Strict (#191): a missing file, an
// unknown field, a good named twice, a recipe naming a good that is not there -- each is an
// error, and the registry is left as it was. Before any load the registry holds the three
// ores of the original game and no industry, so a tool or a test that loads nothing still
// has goods to work with.
bool               Load(const std::string& path);
const std::string& Error();
void               ResetToBuiltin();  // back to the three ores, for a test that loaded a file

int                       Count();
bool                      Find(const std::string& idOrName, ResourceType& out);  // false if none
const GoodDef&            Def(ResourceType g);
const std::string&        Id(ResourceType g);
std::vector<ResourceType> Mined();  // what a belt can hold, in registry order

const MarketRules& Rules();
// What an object of archetype `archetypeId` does to its market; nullptr when nothing.
const Industry* IndustryOf(const std::string& archetypeId);
// What a planet of archetype `archetypeId` grows for a market in its system, per hour.
const std::vector<std::pair<ResourceType, double>>* PlanetYield(const std::string& archetypeId);
}  // namespace Goods
