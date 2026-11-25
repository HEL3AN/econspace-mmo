#pragma once

#include <string>
#include <vector>

// What one side of an exchange puts in (#473, documents/player_exchange.md): a *bundle*, the
// server's own description of goods, credits and charts. A direct trade has one per pilot;
// the exchange contracts that follow it have a giving and an asking one. Nothing in it is
// free text: every line is a thing the server checks and describes back.
namespace Exchange
{

// Goods and modules (modules are goods, #472 slice 10): `amount` units of resource `type`,
// taken from the container `from` as InventoryList names them -- "hold" when empty.
struct GoodLine
{
    int         type = -1;  // ResourceType as int
    int         amount = 0;
    std::string from;
};

// A chart on offer: a system, as its id. The side that is *receiving* is told a `text` instead
// of a name -- what is new to them -- because a name and a place are the goods themselves.
struct ChartLine
{
    std::string system;  // empty on the receiving side's list
    std::string text;
};

struct Bundle
{
    std::vector<GoodLine>    goods;
    long long                credits = 0;
    std::vector<std::string> charts;  // systems, by id or name when asked for; ids when held

    bool Empty() const { return goods.empty() && credits <= 0 && charts.empty(); }
};

// The most lines one bundle may carry on the wire, and the most a direct trade holds.
inline constexpr size_t MAX_LINES = 32;
// Credits a bundle may name: a double holds whole credits exactly to here.
inline constexpr long long MAX_CREDITS = 1000000000000LL;

}  // namespace Exchange
