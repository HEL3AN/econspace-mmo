#include "sim/InventoryList.h"

#include "core/Archetype.h"

#include <algorithm>
#include <cctype>
#include <cmath>

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
    if (p.docked && p.hangar.capacity > 0)  // (#472)
    {
        Container h;
        h.id = "hangar";
        h.name = "Hangar";
        h.goods = p.hangar.goods;
        h.capacity = p.hangar.capacity;
        h.closed = p.hangar.owed > 0.0;
        out.push_back(h);
    }
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

std::vector<Container> Reachable(const Proto::Snapshot&                    snap,
                                 const std::map<int, Proto::EntityLayout>& layout,
                                 const std::string&                        account)
{
    std::vector<Container> out = Reachable(snap.player);
    if (snap.player.docked)
        return out;
    for (const Proto::EntitySnapshot& e : snap.entities)
    {
        if (e.kind != Proto::EntityKind::Derelict || Total(e.holds) <= 0)
            continue;
        const auto l = layout.find(e.id);
        if (l == layout.end())
            continue;
        const Archetype* a = Archetypes::Find(l->second.archetype);
        if (a == nullptr || !a->Has(Component::Salvageable))
            continue;
        const float reach = e.size + a->salvageRange;
        const float dx = e.pos.x - snap.player.pos.x, dy = e.pos.y - snap.player.pos.y;
        if (dx * dx + dy * dy > reach * reach)
            continue;
        Container c;
        c.id = "can:" + std::to_string(e.id);
        c.name = l->second.name;
        c.goods = e.holds;
        const bool mine = !account.empty() && l->second.owner == account;
        c.takeOnly = !mine;
        c.capacity = mine ? std::max(CAN_UNITS, Total(e.holds)) : Total(e.holds);
        out.push_back(c);
    }
    return out;
}

int Total(const std::vector<int>& goods)
{
    int n = 0;
    for (int g : goods)
        n += std::max(0, g);
    return n;
}

std::string Describe(const std::vector<int>& goods)
{
    std::string                     out;
    const std::vector<ResourceType> types = AllResourceTypes();
    for (size_t i = 0; i < goods.size() && i < types.size(); i++)
        if (goods[i] > 0)
            out +=
                (out.empty() ? "" : ", ") + std::to_string(goods[i]) + " " + ResourceName(types[i]);
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
    if (from.id == to.id || from.closed || to.closed || to.takeOnly)
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
