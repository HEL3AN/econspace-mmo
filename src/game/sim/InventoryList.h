#pragma once

#include "economy/Resource.h"
#include "sim/Protocol.h"

#include <string>
#include <string_view>
#include <vector>

// The inventory window as lists (#472): the containers reachable from where the pilot is, and
// what each holds. The half of the window with no drawing in it, as MissionList is for the
// missions window; the agent's `inventory` tool reads the same lists, so a container has one
// name and one count in both.
//
// A container is (place, holder, division) -- documents/holds_and_storage.md. What is
// reachable comes from the snapshot: the hold always, and, docked at a structure whose storage
// is the pilot's side's (#438), that storage.
namespace InventoryList
{

struct Container
{
    std::string      id;     // what a move names it by: "hold", "storage"
    std::string      name;   // what the window calls it: "Hold", "Storage"
    std::vector<int> goods;  // how many of each good, in AllResourceTypes order
    int              capacity = 0;

    int Amount(ResourceType type) const;
    int Used() const;
    int Room() const;
};

// The hold first, then what is reachable here.
std::vector<Container> Reachable(const Proto::PlayerView& p);
// The one with this id, or null.
const Container* Find(const std::vector<Container>& all, std::string_view id);

struct Row
{
    ResourceType type;
    std::string  name;
    int          amount = 0;
    float        price = 0.0f;  // what the market here pays a unit; 0 where there is none
    double       value = 0.0;   // amount times price
};

// What a container holds, in AllResourceTypes order, with nothing that is not there. `filter`
// keeps the goods whose name contains it, in any case; `prices` is the snapshot's market here,
// empty when there is none.
std::vector<Row> Rows(const Container& c, const std::vector<float>& prices,
                      std::string_view filter = {});

// How many of a good a move can carry: no more than wanted, than `from` has, than `to` has
// room for. The server counts again; this is what a button offers.
int Movable(const Container& from, const Container& to, ResourceType type, int wanted);

// The amount buttons beside the field: one, ten, half of what there is (at least one), all.
enum class Preset
{
    One,
    Ten,
    Half,
    All,
};
int PresetAmount(Preset preset, int available);

}  // namespace InventoryList
