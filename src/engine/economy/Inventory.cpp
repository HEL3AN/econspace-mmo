#include "economy/Inventory.h"

#include <algorithm>

int Inventory::Amount(ResourceType type) const
{
    const auto it = goods_.find(type);
    return it == goods_.end() ? 0 : it->second;
}

int Inventory::Used() const
{
    int n = 0;
    for (const auto& kv : goods_)
        n += kv.second;
    return n;
}

void Inventory::Add(ResourceType type, int amount)
{
    if (amount > 0)
        goods_[type] += amount;
}

int Inventory::Remove(ResourceType type, int amount)
{
    const auto it = goods_.find(type);
    if (it == goods_.end() || amount <= 0)
        return 0;
    const int took = std::min(amount, it->second);
    it->second -= took;
    if (it->second <= 0)
        goods_.erase(it);
    return took;
}
