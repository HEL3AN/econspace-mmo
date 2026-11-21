// Simulation -- the NPC delivery service (#472, documents/holds_and_storage.md).
//
// A faction's freight office carries a pilot's goods from a hangar at one of its stations to
// where the pilot is docked, for a quote that comes from the route alone -- its length, how
// safe it is -- and never from what players charge. Today it is how a pilot gets out what a
// faction that now hates them still holds (at a surcharge, paid to that faction); it is the
// same quote the general service and #473's courier contracts show beside their prices.
// The goods leave the hangar and the fee is paid at the request; the room at the destination is
// reserved; the goods arrive when the world clock says, online or away. Part of the Simulation
// class.
#include "sim/Simulation.h"
#include "sim/WorldRules.h"

#include "economy/Market.h"
#include "entities/Station.h"
#include "entities/ShipType.h"
#include "entities/ShipType.h"
#include "entities/Structure.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace
{
constexpr double HOUR = 3600.0;

std::string Cr(double cr)
{
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.0f cr", std::ceil(cr - 0.005));
    return buf;
}
}  // namespace

Simulation::DeliveryQuote Simulation::QuoteDelivery(const std::string& from, const std::string& to,
                                                    int volume, double value,
                                                    double surcharge) const
{
    DeliveryQuote                  q;
    const WorldRules::Deliveries&  r = WorldRules::DeliveryRules();
    const std::vector<std::string> path = PlanRoute(from, to, Route::Preference::Safer);
    if (path.empty())
    {
        q.why = "there is no route from " + SystemName(from) + " to " + SystemName(to);
        return q;
    }
    double insecurity = 0.0;
    for (const std::string& id : path)
    {
        const SystemState* st = SystemById(id);
        if (st == nullptr)
        {
            q.why = "the route crosses a system nobody knows";
            return q;
        }
        for (const auto& p : plans_)
            if (p.second.kind == Plan::Kind::Siege && p.second.target == id)
            {
                q.why = "the route crosses " + SystemName(id) + ", where a siege is under way";
                return q;
            }
        insecurity += 1.0 - std::clamp((double)st->agg.security, 0.0, 1.0);
    }
    q.jumps = std::max(1, (int)path.size() - 1);
    q.risk = 1.0 + r.riskWeight * insecurity / (double)path.size();
    const double carry = std::max(r.minFee, r.rate * volume * q.jumps * q.risk);
    q.fee = std::ceil((carry + r.insure * value * (q.risk - 1.0)) * surcharge);
    q.hours = std::max(r.minHours, r.hoursPerJump * q.jumps);
    q.ok = true;
    return q;
}

// Goods a pilot keeps at the hostile station called `key`, who holds them, and whether the
// station is one whose faction refuses the pilot now.
const Entity* Simulation::HangarStation(const std::string& key) const
{
    const size_t       cut = key.find('|');
    const SystemState* st = cut == std::string::npos ? nullptr : SystemById(key.substr(0, cut));
    if (st == nullptr)
        return nullptr;
    for (const auto& e : st->entities)
        if (e->GetKind() == EntityKind::Station && HangarKey(*st, *e) == key)
            return e.get();
    return nullptr;
}

bool Simulation::HangarOutOfReach(const ClientSession& s, const std::string& key) const
{
    const Entity* station = HangarStation(key);
    if (station == nullptr)
        return false;
    const FactionId f = static_cast<const Station&>(*station).GetFaction();
    return Factions::TierOf(s.account.GetReputation(f)) == RepTier::Hated;
}

std::vector<Proto::LockedHangarView> Simulation::LockedHangarsFor(const ClientSession& s) const
{
    std::vector<Proto::LockedHangarView> out;
    if (s.accountName.empty() || !s.IsDocked() || !s.ship)
        return out;
    for (const auto& at : hangars_)
    {
        const auto it = at.second.find(s.accountName);
        if (it == at.second.end() || it->second.goods.Empty() || !HangarOutOfReach(s, at.first))
            continue;
        const Entity*           station = HangarStation(at.first);
        Proto::LockedHangarView v;
        v.key = at.first;
        v.name = station->GetName();
        v.system = SystemName(at.first.substr(0, at.first.find('|')));
        int          volume = 0;
        double       value = 0.0;
        const Market prices;
        for (ResourceType rt : AllResourceTypes())
        {
            const int n = it->second.goods.Amount(rt);
            v.goods.push_back(n);
            volume += n * Goods::Volume(rt);
            value += n * prices.GetBasePrice(rt);
        }
        const DeliveryQuote q =
            QuoteDelivery(at.first.substr(0, at.first.find('|')), s.systemId, volume, value,
                          WorldRules::DeliveryRules().hostileSurcharge);
        v.fee = q.ok ? q.fee : 0.0;
        v.hours = q.ok ? q.hours : 0.0;
        v.why = q.why;
        out.push_back(std::move(v));
    }
    return out;
}

bool Simulation::RequestDelivery(ClientSession& s, const std::string& key, int type, int amount)
{
    auto refuse = [&](const std::string& why)
    {
        s.RecordEvent(Ev::Kind::Notice, "Cannot request delivery: " + why);
        return false;
    };
    if (!s.ship || s.accountName.empty() || !s.IsDocked())
        return refuse("dock where the goods should go first");
    Hangar*       h = HangarOf(key, s.accountName, false);
    const Entity* station = HangarStation(key);
    if (h == nullptr || h->goods.Empty() || station == nullptr)
        return refuse("you keep nothing there");
    if (!HangarOutOfReach(s, key))
        return refuse(station->GetName() + " still lets you dock: go and get it yourself");
    // Where it goes: the pilot's hangar here, at a station or a corporation's structure.
    Container   dest;
    std::string why;
    if (!ContainerOf(s, "hangar", dest, why))
        return refuse("you have no hangar here to deliver to: " + why);
    Shipment sh;
    sh.account = s.accountName;
    for (const auto& g : h->goods.All())
        if (type < 0 || g.first.Index() == type)
            sh.goods.Add(g.first, amount > 0 ? std::min(g.second, amount) : g.second);
    if (sh.goods.Empty())
        return refuse("there is none of that to send");
    // The room is reserved: what is in transit to this hangar counts against it.
    const int reserved = ReservedFor(s.accountName, SystemOf(s)->id, dest.name);
    if (dest.goods->Used() + reserved + sh.goods.Used() > dest.capacity)
        return refuse(dest.name + " has room for " +
                      std::to_string(std::max(0, dest.capacity - dest.goods->Used() - reserved)) +
                      " more, with what is already on its way");
    double       value = 0.0;
    const Market prices;
    for (const auto& g : sh.goods.All())
        value += g.second * prices.GetBasePrice(g.first);
    const std::string   fromSystem = key.substr(0, key.find('|'));
    const DeliveryQuote q = QuoteDelivery(fromSystem, s.systemId, sh.goods.Used(), value,
                                          WorldRules::DeliveryRules().hostileSurcharge);
    if (!q.ok)
        return refuse(q.why);
    sh.holder = h->holder;
    if (!PayRent(InfluenceMap::PILOT + s.accountName, sh.holder, q.fee, why))
        return refuse(why);
    for (const auto& g : sh.goods.All())
        h->goods.Remove(g.first, g.second);
    sh.id = nextShipment_++;
    sh.fee = q.fee;
    sh.from = station->GetName() + ", " + SystemName(fromSystem);
    sh.toSystem = SystemOf(s)->id;
    sh.toName = dest.name;
    sh.due = time_ + q.hours * HOUR;
    s.RecordEvent(Ev::Kind::Notice, "Delivery from " + sh.from + " to " + dest.name + ": " +
                                        std::to_string(sh.goods.Used()) + " units, " + Cr(q.fee) +
                                        ", arriving in " + std::to_string((int)std::ceil(q.hours)) +
                                        " h");
    shipments_.push_back(std::move(sh));
    return true;
}

std::vector<Proto::AssetView> Simulation::AssetsFor(const ClientSession& s) const
{
    std::vector<Proto::AssetView> out;
    if (s.accountName.empty())
        return out;
    auto vectorOf = [](const Inventory& inv)
    {
        std::vector<int> v;
        for (ResourceType rt : AllResourceTypes())
            v.push_back(inv.Amount(rt));
        return v;
    };
    // The hangars at the stations.
    for (const auto& at : hangars_)
    {
        const auto it = at.second.find(s.accountName);
        if (it == at.second.end() || it->second.goods.Empty())
            continue;
        const std::string sysId = at.first.substr(0, at.first.find('|'));
        Proto::AssetView  a;
        a.kind = "hangar";
        a.name = "Hangar at " + PlaceName(at.first).substr(0, PlaceName(at.first).find(','));
        a.system = sysId;
        a.systemName = SystemName(sysId);
        a.goods = vectorOf(it->second.goods);
        if (it->second.owed > 0.0)
            a.note = "closed until the rent owed is paid";
        if (HangarOutOfReach(s, at.first))
        {
            a.locked = true;
            a.note =
                "out of reach: its faction refuses you; request a delivery where you are docked";
        }
        out.push_back(std::move(a));
    }
    // A structure's storage and a member's hangar, where the pilot's side may use them.
    for (const auto& kv : systems_)
        for (const auto& e : kv.second.entities)
        {
            if (e->GetKind() != EntityKind::Structure)
                continue;
            const Structure& t = static_cast<const Structure&>(*e);
            if (t.GetStorageCapacity() <= 0 ||
                !t.OwnersSide(s.accountName, corps_.SameCorp(t.GetOwner(), s.accountName)))
                continue;
            Inventory seen;  // what the pilot's role may look at
            for (const char* div : Corps::DIVISION_IDS)
                if (corps_.MayDivision(t.GetOwner(), s.accountName, div, "view"))
                    if (const Inventory* inv = t.Division(div))
                        for (const auto& g : inv->All())
                            seen.Add(g.first, g.second);
            if (!seen.Empty())
            {
                Proto::AssetView a;
                a.kind = "storage";
                a.name = t.GetName() + "'s storage";
                a.system = kv.first;
                a.systemName = SystemName(kv.first);
                a.goods = vectorOf(seen);
                out.push_back(std::move(a));
            }
            if (const Inventory* h = t.FindHangar(s.accountName); h != nullptr && !h->Empty())
            {
                Proto::AssetView a;
                a.kind = "hangar";
                a.name = "Your hangar in " + t.GetName();
                a.system = kv.first;
                a.systemName = SystemName(kv.first);
                a.goods = vectorOf(*h);
                out.push_back(std::move(a));
            }
        }
    // Ships parked away from the flown one, with what they carry.
    const std::vector<ShipType>& catalog = GetShipCatalog();
    for (const auto& b : s.berths)
    {
        Proto::AssetView a;
        a.kind = "ship";
        a.name = b.first >= 0 && b.first < (int)catalog.size() ? catalog[(size_t)b.first].name
                                                               : std::string("Ship");
        const size_t cut = b.second.place.find('|');
        if (cut != std::string::npos)
        {
            a.system = b.second.place.substr(0, cut);
            a.systemName = SystemName(a.system);
            a.note = "parked at " + PlaceName(b.second.place);
        }
        else
            a.note = "parked wherever you dock";
        a.goods = vectorOf(b.second.hold);
        out.push_back(std::move(a));
    }
    // Goods on their way.
    for (const Shipment& sh : shipments_)
        if (sh.account == s.accountName)
        {
            Proto::AssetView a;
            a.kind = "transit";
            a.name = "On its way to " + sh.toName;
            a.note = "from " + sh.from;
            a.goods = vectorOf(sh.goods);
            out.push_back(std::move(a));
        }
    return out;
}

int Simulation::ReservedFor(const std::string& account, const std::string& system,
                            const std::string& toName) const
{
    int n = 0;
    for (const Shipment& sh : shipments_)
        if (sh.account == account && sh.toSystem == system && sh.toName == toName)
            n += sh.goods.Used();
    return n;
}

void Simulation::StepShipments()
{
    for (size_t i = 0; i < shipments_.size();)
    {
        Shipment& sh = shipments_[i];
        if (time_ < sh.due)
        {
            i++;
            continue;
        }
        // Into the hangar the pilot named: a station's, or a corporation structure's.
        Inventory*   into = nullptr;
        SystemState* st = SystemById(sh.toSystem);
        if (st != nullptr)
            for (const auto& e : st->entities)
            {
                if (e->GetKind() == EntityKind::Station &&
                    "your hangar at " + e->GetName() == sh.toName)
                    into = &HangarOf(HangarKey(*st, *e), sh.account, true)->goods;
                else if (e->GetKind() == EntityKind::Structure &&
                         "your hangar in " + e->GetName() == sh.toName)
                    into = &static_cast<Structure&>(*e).HangarOf(sh.account);
                if (into != nullptr)
                    break;
            }
        if (into == nullptr)  // the place is gone: the goods wait, and the pilot is told
        {
            i++;
            continue;
        }
        for (const auto& g : sh.goods.All())
            into->Add(g.first, g.second);
        TellParty(InfluenceMap::PILOT + sh.account, "Your delivery from " + sh.from +
                                                        " has arrived at " + sh.toName + ": " +
                                                        std::to_string(sh.goods.Used()) + " units");
        shipments_.erase(shipments_.begin() + (long)i);
    }
}

nlohmann::json Simulation::ShipmentsJson() const
{
    nlohmann::json out = nlohmann::json::array();
    for (const Shipment& sh : shipments_)
    {
        nlohmann::json goods = nlohmann::json::object();
        for (const auto& g : sh.goods.All())
            goods[Goods::Id(g.first)] = g.second;
        out.push_back({ { "account", sh.account },
                        { "goods", goods },
                        { "from", sh.from },
                        { "toSystem", sh.toSystem },
                        { "toName", sh.toName },
                        { "due", sh.due },
                        { "fee", sh.fee },
                        { "holder", sh.holder } });
    }
    return out;
}

void Simulation::ShipmentsFrom(const nlohmann::json& j)
{
    shipments_.clear();
    nextShipment_ = 1;
    if (!j.is_array())
        return;
    for (const nlohmann::json& e : j)
    {
        if (!e.is_object() || e.value("account", std::string()).empty())
            continue;
        Shipment sh;
        sh.id = nextShipment_++;
        sh.account = e.value("account", std::string());
        sh.from = e.value("from", std::string());
        sh.toSystem = e.value("toSystem", std::string());
        sh.toName = e.value("toName", std::string());
        sh.due = e.value("due", 0.0);
        sh.fee = e.value("fee", 0.0);
        sh.holder = e.value("holder", std::string());
        if (e.contains("goods") && e["goods"].is_object())
            for (auto g = e["goods"].begin(); g != e["goods"].end(); ++g)
            {
                ResourceType good;
                if (g.value().is_number_integer() && Goods::Find(g.key(), good))
                    sh.goods.Add(good, g.value().get<int>());
            }
        if (!sh.goods.Empty())
            shipments_.push_back(std::move(sh));
    }
}

std::vector<Proto::ShipmentView> Simulation::ShipmentsFor(const std::string& account) const
{
    std::vector<Proto::ShipmentView> out;
    for (const Shipment& sh : shipments_)
        if (sh.account == account)
        {
            Proto::ShipmentView v;
            v.from = sh.from;
            v.to = sh.toName;
            v.hours = std::max(0.0, (sh.due - time_) / HOUR);
            for (ResourceType rt : AllResourceTypes())
                v.goods.push_back(sh.goods.Amount(rt));
            out.push_back(std::move(v));
        }
    return out;
}
