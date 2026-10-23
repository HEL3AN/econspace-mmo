#include "sim/InventoryList.h"

#include <algorithm>
#include <cctype>

namespace InventoryList
{

int Container::Amount(ResourceType type) const
{
    const size_t i = (size_t)type.Index();
    return i < goods.size() ? std::max(0, goods[i]) : 0;
}

int Container::Used() const
{
    int used = 0;
    for (int n : goods)
        used += std::max(0, n);
    return used;
}

int Container::Room() const
{
    return std::max(0, capacity - Used());
}

std::vector<Container> Reachable(const Proto::PlayerView& p)
{
    std::vector<Container> out;
    Container              hold;
    hold.id = "hold";
    hold.name = "Hold";
    hold.goods = p.cargoByType;
    hold.capacity = p.cargoCap;
    out.push_back(hold);
    if (p.docked && p.storageCap > 0)
    {
        Container store;
        store.id = "storage";
        store.name = "Storage";
        store.goods = p.storedByType;
        store.capacity = p.storageCap;
        out.push_back(store);
    }
    return out;
}

const Container* Find(const std::vector<Container>& all, std::string_view id)
{
    for (const Container& c : all)
        if (c.id == id)
            return &c;
    return nullptr;
}

namespace
{
bool Contains(std::string_view text, std::string_view part)
{
    if (part.empty())
        return true;
    auto low = [](char ch) { return (char)std::tolower((unsigned char)ch); };
    return std::search(text.begin(), text.end(), part.begin(), part.end(),
                       [&](char a, char b) { return low(a) == low(b); }) != text.end();
}
}  // namespace

std::vector<Row> Rows(const Container& c, const std::vector<float>& prices, std::string_view filter)
{
    std::vector<Row> rows;
    for (ResourceType type : AllResourceTypes())
    {
        const int n = c.Amount(type);
        if (n <= 0)
            continue;
        Row r;
        r.type = type;
        r.name = ResourceName(type);
        if (!Contains(r.name, filter))
            continue;
        r.amount = n;
        const size_t i = (size_t)type.Index();
        r.price = i < prices.size() ? std::max(0.0f, prices[i]) : 0.0f;
        r.value = (double)r.price * n;
        rows.push_back(r);
    }
    return rows;
}

int Movable(const Container& from, const Container& to, ResourceType type, int wanted)
{
    if (from.id == to.id)
        return 0;
    return std::max(0, std::min({ wanted, from.Amount(type), to.Room() }));
}

int PresetAmount(Preset preset, int available)
{
    if (available <= 0)
        return 0;
    switch (preset)
    {
        case Preset::One: return 1;
        case Preset::Ten: return std::min(10, available);
        case Preset::Half: return std::max(1, available / 2);
        case Preset::All: return available;
    }
    return available;
}

}  // namespace InventoryList
