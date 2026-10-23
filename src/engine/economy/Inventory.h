#pragma once

#include "economy/Resource.h"

#include <map>

// Goods by type, as a container holds them (#472): a ship's hold, a structure's storage, a
// pilot's hangar at a station, a can in space. One class for all of them, so moving goods from
// one to another is one rule rather than a pair of verbs per kind of place
// (documents/holds_and_storage.md).
//
// How much it may hold is not its own: a hold's room comes from the ship's design, a storage's
// from its archetype, a hangar's from what is rented. Whoever holds the inventory says, and the
// server checks the room before it adds.
class Inventory
{
public:
    int  Amount(ResourceType type) const;
    int  Used() const;  // every unit of every good
    bool Empty() const { return goods_.empty(); }

    void Add(ResourceType type, int amount);  // nothing for an amount of none
    // Takes up to `amount`; returns how many it took.
    int  Remove(ResourceType type, int amount);
    void Clear() { goods_.clear(); }

    // Every good it holds, none at zero.
    const std::map<ResourceType, int>& All() const { return goods_; }

private:
    std::map<ResourceType, int> goods_;
};
