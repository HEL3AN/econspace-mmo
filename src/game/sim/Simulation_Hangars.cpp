// Simulation -- personal hangars at NPC stations (#472, documents/holds_and_storage.md).
//
// Every station whose archetype keeps `storage` gives each pilot a hangar there: a container
// like the hold, moved into and out of by the one `move` verb. The first `freeUnits` are free;
// beyond them the pilot rents as many units as they choose, up to the archetype's `storage`,
// paid every `periodDays` days of world time to the station's faction through the same path a
// licence's rent takes (PayRent), online or away. Unpaid, nothing is thrown away: the rent
// shrinks to what is held above the free units, the period becomes a debt, and the hangar is
// closed -- nothing in, nothing out -- until the debt is paid, which docking there does when
// the money is there. Part of the Simulation class.
#include "sim/Simulation.h"
#include "sim/WorldRules.h"

#include "core/Archetype.h"
#include "entities/Station.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace
{
constexpr double DAY = 86400.0;

std::string Cr(double cr)
{
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.0f cr", std::ceil(cr - 0.005));
    return buf;
}

std::string Days(double seconds)
{
    char buf[48];
    if (seconds >= 2.0 * DAY)
        std::snprintf(buf, sizeof(buf), "%.0f days", seconds / DAY);
    else
        std::snprintf(buf, sizeof(buf), "%.0f hours", std::max(0.0, seconds / 3600.0));
    return buf;
}

// A station's key: its system, its name, and which of the stations of that name it is -- an id
// is good for one run only, and a station's name is what the save can find again.
constexpr char SEP = '|';

std::string StationNameOf(const std::string& key)
{
    const size_t a = key.find(SEP), b = key.rfind(SEP);
    return a == std::string::npos || b <= a ? key : key.substr(a + 1, b - a - 1);
}
}  // namespace

std::string Simulation::HangarKey(const SystemState& st, const Entity& station) const
{
    int nth = 0;
    for (const auto& e : st.entities)
    {
        if (e.get() == &station)
            break;
        if (e->GetKind() == EntityKind::Station && e->GetName() == station.GetName())
            nth++;
    }
    return st.id + SEP + station.GetName() + SEP + std::to_string(nth);
}

const Entity* Simulation::DockedHangarStation(const ClientSession& s, std::string* why) const
{
    auto no = [why](const std::string& text) -> const Entity*
    {
        if (why != nullptr)
            *why = text;
        return nullptr;
    };
    const SystemState* st = SystemOf(s);
    if (st == nullptr || !s.ship || !s.IsDocked() || s.accountName.empty())
        return no("dock at a station first");
    for (const auto& e : st->entities)
        if (e->GetId() == s.dockedStationId)
        {
            if (e->GetKind() != EntityKind::Station || !e->Has(Component::Storage))
                return no(e->GetName() + " keeps no hangars");
            return e.get();
        }
    return no("dock at a station first");
}

Simulation::Hangar* Simulation::HangarOf(const std::string& key, const std::string& account,
                                         bool create)
{
    auto at = hangars_.find(key);
    if (at == hangars_.end())
    {
        if (!create)
            return nullptr;
        at = hangars_.emplace(key, std::map<std::string, Hangar>()).first;
    }
    auto it = at->second.find(account);
    if (it == at->second.end())
    {
        if (!create)
            return nullptr;
        it = at->second.emplace(account, Hangar()).first;
    }
    return &it->second;
}

const Simulation::Hangar* Simulation::FindHangar(const std::string& key,
                                                 const std::string& account) const
{
    const auto at = hangars_.find(key);
    if (at == hangars_.end())
        return nullptr;
    const auto it = at->second.find(account);
    return it == at->second.end() ? nullptr : &it->second;
}

int Simulation::HangarMost(const Entity& station)
{
    const Archetype* a = station.GetArchetype();
    const int        most = a != nullptr ? (int)a->storageCapacity : 0;
    return std::max(most, WorldRules::HangarRules().freeUnits);
}

int Simulation::HangarCapacity(const Hangar& h, const Entity& station)
{
    return std::min(WorldRules::HangarRules().freeUnits + h.rented, HangarMost(station));
}

void Simulation::SettleHangar(ClientSession& s)
{
    const Entity* station = DockedHangarStation(s);
    if (station == nullptr)
        return;
    Hangar*   h = HangarOf(HangarKey(*SystemOf(s), *station), s.accountName, false);
    FactionId f = static_cast<const Station&>(*station).GetFaction();
    if (h == nullptr)
        return;
    h->holder = InfluenceMap::Key(f);  // the rent goes to whoever owns it now
    if (h->owed <= 0.0)
        return;
    std::string why;
    if (!PayRent(InfluenceMap::PILOT + s.accountName, h->holder, h->owed, why))
    {
        s.RecordEvent(Ev::Kind::Notice, "Your hangar at " + station->GetName() + " stays closed: " +
                                            Cr(h->owed) + " of rent is owed, and " + why);
        return;
    }
    s.RecordEvent(Ev::Kind::Notice, "Paid the " + Cr(h->owed) +
                                        " of rent owed for your hangar at " + station->GetName() +
                                        ": it is open again");
    h->owed = 0.0;
}

bool Simulation::RentHangar(ClientSession& s, int units)
{
    auto refuse = [&](const std::string& why)
    {
        s.RecordEvent(Ev::Kind::Notice, "Cannot rent: " + why);
        return false;
    };
    std::string   why;
    const Entity* station = DockedHangarStation(s, &why);
    if (station == nullptr)
        return refuse(why);
    const WorldRules::Hangars& r = WorldRules::HangarRules();
    const int                  most = HangarMost(*station) - r.freeUnits;
    if (units < 0 || units > most)
        return refuse("at most " + std::to_string(most) + " units can be rented at " +
                      station->GetName() + ", on top of the " + std::to_string(r.freeUnits) +
                      " free");
    Hangar& h = *HangarOf(HangarKey(*SystemOf(s), *station), s.accountName, true);
    h.holder = InfluenceMap::Key(static_cast<const Station&>(*station).GetFaction());
    if (h.owed > 0.0)
    {
        SettleHangar(s);  // says either way
        if (h.owed > 0.0)
            return false;
        if (units == h.rented)
            return true;  // what was asked was to pay it
    }
    const int above = std::max(0, h.goods.Used() - r.freeUnits);
    if (units < above)
        return refuse("the hangar holds " + std::to_string(above) + " units above the " +
                      std::to_string(r.freeUnits) + " free; move them out first");
    if (units == h.rented)
        return refuse("that is what is rented now");
    const std::string party = InfluenceMap::PILOT + s.accountName;
    const double      period = r.periodDays * DAY;
    const bool        running = h.rented > 0 && h.paidUntil > time_;
    if (units > h.rented)
    {
        // Added units pay for what is left of the period; a rent starting now pays a whole one.
        const double cost =
            running ? (units - h.rented) * r.rentPerUnit * (h.paidUntil - time_) / period
                    : units * r.rentPerUnit;
        if (!PayRent(party, h.holder, cost, why))
            return refuse(why);
        if (!running)
            h.paidUntil = time_ + period;
        h.rented = units;
        s.RecordEvent(Ev::Kind::Notice,
                      "Rented " + std::to_string(units) + " units at " + station->GetName() +
                          " on top of the " + std::to_string(r.freeUnits) + " free, for " +
                          Cr(cost) + "; " + Cr(units * r.rentPerUnit) + " is taken every " +
                          Days(period) + ", next in " + Days(h.paidUntil - time_));
        return true;
    }
    // Less: at once, and nothing back.
    h.rented = units;
    if (units == 0)
        h.paidUntil = 0.0;
    s.RecordEvent(Ev::Kind::Notice, units == 0
                                        ? "The rent at " + station->GetName() + " is ended: the " +
                                              std::to_string(r.freeUnits) + " free units stay yours"
                                        : "The rent at " + station->GetName() + " is now " +
                                              std::to_string(units) + " units, " +
                                              Cr(units * r.rentPerUnit) + " every " + Days(period));
    return true;
}

void Simulation::StepHangars()
{
    const WorldRules::Hangars& r = WorldRules::HangarRules();
    const double               period = r.periodDays * DAY;
    for (auto& at : hangars_)
        for (auto& kv : at.second)
        {
            Hangar& h = kv.second;
            if (h.rented <= 0 || h.paidUntil <= 0.0 || time_ < h.paidUntil)
                continue;
            const std::string party = InfluenceMap::PILOT + kv.first;
            const std::string where = StationNameOf(at.first);
            const double      rent = h.rented * r.rentPerUnit;
            h.paidUntil += period;
            std::string why;
            if (h.owed <= 0.0 && PayRent(party, h.holder, rent, why))
            {
                TellParty(party, "Paid " + Cr(rent) + " rent for " + std::to_string(h.rented) +
                                     " units of hangar at " + where + ", for " +
                                     Days(h.paidUntil - time_) + " more");
                continue;
            }
            // Unpaid: the rent shrinks to what is held above the free units, and that is owed.
            const int above = std::max(0, h.goods.Used() - r.freeUnits);
            h.rented = above;
            if (above == 0)
            {
                h.paidUntil = 0.0;
                TellParty(party, "The rent of your hangar at " + where +
                                     " was not paid, and has ended; the " +
                                     std::to_string(r.freeUnits) + " free units are still yours");
                continue;
            }
            h.owed += above * r.rentPerUnit;
            TellParty(party, "The rent of your hangar at " + where + " was not paid (" + why +
                                 "): " + Cr(h.owed) + " is owed, and the hangar is closed until " +
                                 "it is paid -- dock there with the money");
        }
}

bool Simulation::RentsHangar(const std::string& account) const
{
    for (const auto& at : hangars_)
    {
        const auto it = at.second.find(account);
        if (it != at.second.end() && (it->second.rented > 0 || it->second.owed > 0.0))
            return true;
    }
    return false;
}

Proto::HangarView Simulation::HangarViewFor(const ClientSession& s) const
{
    Proto::HangarView v;
    const Entity*     station = DockedHangarStation(s);
    if (station == nullptr)
        return v;
    const WorldRules::Hangars& r = WorldRules::HangarRules();
    const Hangar*              h = FindHangar(HangarKey(*SystemOf(s), *station), s.accountName);
    const Hangar               none;
    const Hangar&              hh = h != nullptr ? *h : none;
    v.capacity = HangarCapacity(hh, *station);
    v.free = r.freeUnits;
    v.rented = hh.rented;
    v.most = HangarMost(*station);
    v.rentPerUnit = r.rentPerUnit;
    v.periodDays = r.periodDays;
    v.paidUntil = hh.paidUntil;
    v.owed = hh.owed;
    for (ResourceType rt : AllResourceTypes())
        v.goods.push_back(hh.goods.Amount(rt));
    return v;
}

nlohmann::json Simulation::HangarsJson() const
{
    nlohmann::json out = nlohmann::json::array();
    for (const auto& at : hangars_)
        for (const auto& kv : at.second)
        {
            const Hangar& h = kv.second;
            if (h.goods.Empty() && h.rented <= 0 && h.owed <= 0.0)
                continue;  // nothing to keep
            nlohmann::json goods = nlohmann::json::object();
            for (const auto& g : h.goods.All())
                goods[Goods::Id(g.first)] = g.second;
            out.push_back({ { "station", at.first },
                            { "account", kv.first },
                            { "goods", goods },
                            { "rented", h.rented },
                            { "paidUntil", h.paidUntil },
                            { "owed", h.owed },
                            { "holder", h.holder } });
        }
    return out;
}

void Simulation::HangarsFrom(const nlohmann::json& j)
{
    hangars_.clear();
    if (!j.is_array())
        return;
    for (const nlohmann::json& e : j)
    {
        if (!e.is_object())
            continue;
        const std::string key = e.value("station", std::string());
        const std::string account = e.value("account", std::string());
        if (key.empty() || account.empty())
            continue;
        Hangar&    h = *HangarOf(key, account, true);
        const auto goods = e.find("goods");
        if (goods != e.end() && goods->is_object())
            for (auto it = goods->begin(); it != goods->end(); ++it)
            {
                ResourceType g;
                if (Goods::Find(it.key(), g) && it->is_number_integer())
                    h.goods.Add(g, it->get<int>());
            }
        h.rented = std::max(0, e.value("rented", 0));
        h.paidUntil = e.value("paidUntil", 0.0);
        h.owed = std::max(0.0, e.value("owed", 0.0));
        h.holder = e.value("holder", std::string());
    }
}
