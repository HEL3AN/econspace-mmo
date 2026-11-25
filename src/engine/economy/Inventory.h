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
    int Amount(ResourceType type) const;
    int Used() const;   // the room it takes: every unit times its good's volume (#472)
    int Units() const;  // every unit of every good, whatever room it takes
    // An ore hold (#572, slice 12): of a container's `capacity`, `ore` is room only mined goods
    // may take; the rest takes anything. Mined goods may also fill the general room.
    int OreUsed() const;  // the room its mined goods take
    // How much room a unit of `type` may still take, in the same unit as Used(): what is left
    // of the whole, and for a good that is not mined no more than what the ore hold leaves out.
    int  Room(int capacity, int ore, ResourceType type) const;
    bool Fits(int capacity, int ore) const;  // all it holds is where it may be
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
