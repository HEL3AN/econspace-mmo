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
#include "entities/Structure.h"

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

std::string Tag(const Corps::Corp& c)
{
    return c.name + " [" + c.ticker + "]";
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

std::string Simulation::PlaceKeyOf(const SystemState& st, const Entity& e) const
{
    int nth = 0;
    for (const auto& o : st.entities)
    {
        if (o.get() == &e)
            break;
        if (o->GetKind() == e.GetKind() && o->GetName() == e.GetName())
            nth++;
    }
    return st.id + SEP + e.GetName() + SEP + std::to_string(nth);
}

std::string Simulation::PlaceKeyHere(const ClientSession& s) const
{
    const SystemState* st = SystemOf(s);
    if (st == nullptr || !s.IsDocked())
        return std::string();
    for (const auto& e : st->entities)
        if (e->GetId() == s.dockedStationId)
            return PlaceKeyOf(*st, *e);
    return std::string();
}

std::string Simulation::PlaceName(const std::string& key) const
{
    const size_t a = key.find(SEP);
    if (a == std::string::npos)
        return std::string();
    return StationNameOf(key) + ", " + SystemName(key.substr(0, a));
}

bool Simulation::ShipIsHere(const ClientSession& s, int hull) const
{
    const auto it = s.berths.find(hull);
    if (it == s.berths.end() || it->second.place.empty())
        return true;
    return it->second.place == PlaceKeyHere(s);
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
        return refuse("at most " + std::to_string(most) + " volume can be rented at " +
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
        return refuse("the hangar holds " + std::to_string(above) + " volume above the " +
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
    s.RecordEvent(Ev::Kind::Notice,
                  units == 0
                      ? "The rent at " + station->GetName() + " is ended: the " +
                            std::to_string(r.freeUnits) + " free volume stays yours"
                      : "The rent at " + station->GetName() + " is now " + std::to_string(units) +
                            " volume, " + Cr(units * r.rentPerUnit) + " every " + Days(period));
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
                                     " volume of hangar at " + where + ", for " +
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
                                     std::to_string(r.freeUnits) + " free volume is still yours");
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
    if (const Structure* t = CorpHangarStructure(s))  // a member's own at a corporation's (#472)
    {
        const Inventory* mine = t->FindHangar(s.accountName);
        const Inventory  none;
        const Inventory& inv = mine != nullptr ? *mine : none;
        v.free = std::min(WorldRules::HangarRules().freeUnits, t->GetStorageCapacity());
        v.capacity = PooledCapacity(*t, inv, v.free);
        v.most = v.free;
        for (ResourceType rt : AllResourceTypes())
            v.goods.push_back(inv.Amount(rt));
        return v;
    }
    const Entity* station = DockedHangarStation(s);
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

// --- A corporation's office (#572, slice 12) ---
//
// Rented from the treasury for an NPC station, paid to the station's faction by the rent path
// a licence takes. It keeps what the corporation puts in it in the three divisions of a
// structure's storage, under the same role rules, and a take from it is a line in the
// corporation's journal. Unpaid, it is locked after a grace day -- goods out, none in -- and it
// is never emptied: a locked office is a debt, not a seizure.

const Entity* Simulation::OfficeStation(const std::string& key, const SystemState** in) const
{
    const size_t       a = key.find(SEP);
    const SystemState* st = a == std::string::npos ? nullptr : SystemById(key.substr(0, a));
    if (st == nullptr)
        return nullptr;
    for (const auto& e : st->entities)
        if (e->GetKind() == EntityKind::Station && HangarKey(*st, *e) == key)
        {
            if (in != nullptr)
                *in = st;
            return e.get();
        }
    return nullptr;
}

bool Simulation::OfficeLocked(const Corps::Office& o) const
{
    return time_ >= o.until + WorldRules::OfficeRules().graceHours * 3600.0;
}

const Corps::Office* Simulation::OfficeHere(const ClientSession& s, std::string* key) const
{
    const SystemState* st = SystemOf(s);
    const Corps::Corp* c = corps_.Of(s.accountName);
    if (st == nullptr || c == nullptr || !s.ship || !s.IsDocked() || c->offices.empty())
        return nullptr;
    for (const auto& e : st->entities)
        if (e->GetId() == s.dockedStationId && e->GetKind() == EntityKind::Station)
        {
            const std::string k = HangarKey(*st, *e);
            const auto        it = c->offices.find(k);
            if (it == c->offices.end())
                return nullptr;
            if (key != nullptr)
                *key = k;
            return &it->second;
        }
    return nullptr;
}

bool Simulation::OfficeContainer(ClientSession& s, const std::string& id, Container& out,
                                 std::string& why)
{
    std::string          key;
    const Corps::Office* office = OfficeHere(s, &key);
    if (office == nullptr)
        return false;
    const std::string div = id == "storage" ? "shared" : id.substr(8);
    if (!Corps::ValidDivisionId(div))
    {
        why = "a storage division is shared, stock or command";
        return false;
    }
    const Corps::Corp* c = corps_.Of(s.accountName);
    const std::string  owner = "corp:" + c->id;
    const std::string  label = corps_.DivisionLabel(owner, div);
    if (!corps_.MayDivision(owner, s.accountName, div, "view"))
    {
        why = "your role does not see " + label + " in the office";
        return false;
    }
    Inventory* goods = corps_.OfficeGoods(c->id, key, div);
    int        others = 0;
    for (const auto& d : office->goods)
        if (d.first != div)
            others += d.second.Used();
    // One room for the three divisions, as a structure's storage has.
    out.goods = goods;
    out.capacity = std::max(0, WorldRules::OfficeRules().units - others);
    out.name = label + " in the office at " + office->station;
    out.accepts = !OfficeLocked(*office) && corps_.MayDivision(owner, s.accountName, div, "put");
    out.gives = corps_.MayDivision(owner, s.accountName, div, "take");
    out.takeLog = out.name;
    return true;
}

Corps::Outcome Simulation::ApplyOfficeAct(const std::string& account, const Corps::Act& act)
{
    auto no = [](const std::string& why)
    {
        Corps::Outcome o;
        o.error = o.told = why;
        return o;
    };
    const Corps::Corp*   c = corps_.Of(account);
    const Corps::Member* me = c != nullptr ? c->Find(account) : nullptr;
    if (me == nullptr)
        return no("you are in no corporation");
    if (me->role == Corps::Role::Member)
        return no("only the CEO or a director rents an office");
    std::string close = act.value;
    for (char& ch : close)
        ch = (char)std::tolower((unsigned char)ch);
    const bool closing = close == "close";
    // The station by its name -- in the named system, or where the pilot is -- or, with no
    // name, the one they are docked at.
    std::string    where = act.system;
    ClientSession* on = nullptr;
    for (auto& kv : sessions_)
        if (kv.second.accountName == account)
            on = &kv.second;
    if (where.empty() && on != nullptr)
        where = on->systemId;
    std::string sysId = SystemById(where) != nullptr ? where : std::string();
    if (sysId.empty())
        for (const auto& kv : systems_)
            if (!where.empty() && SystemName(kv.first) == where)
                sysId = kv.first;
    const SystemState* st = sysId.empty() ? nullptr : SystemById(sysId);
    if (st == nullptr)
        return no("name the system the station is in");
    std::string want = act.name;
    for (char& ch : want)
        ch = (char)std::tolower((unsigned char)ch);
    const Entity* station = nullptr;
    for (const auto& e : st->entities)
    {
        if (e->GetKind() != EntityKind::Station)
            continue;
        std::string name = e->GetName();
        for (char& ch : name)
            ch = (char)std::tolower((unsigned char)ch);
        const bool docked =
            want.empty() && on != nullptr && on->IsDocked() && on->dockedStationId == e->GetId();
        if (docked || (!want.empty() && name == want))
            station = e.get();
    }
    if (station == nullptr)
        return no("there is no station of that name there; name it, or dock at it");
    const std::string key = HangarKey(*st, *station);
    const auto&       rules = WorldRules::OfficeRules();
    if (closing)
    {
        const auto it = c->offices.find(key);
        if (it == c->offices.end())
            return no(Tag(*c) + " has no office at " + station->GetName());
        int held = 0;
        for (const auto& d : it->second.goods)
            held += d.second.Used();
        if (held > 0)
            return no("the office at " + station->GetName() + " still holds " +
                      std::to_string(held) + " volume of goods: take them out first");
        const std::string corp = c->id;
        corps_.DropOffice(corp, key);
        Corps::Outcome out;
        out.corp = corp;
        out.told = "You close the office at " + station->GetName();
        out.announce = account + " closed the corporation's office at " + station->GetName();
        return out;
    }
    if (!station->Has(Component::Market))
        return no(station->GetName() + " has no market: offices are rented at trading stations");
    const FactionId faction = static_cast<const Station&>(*station).GetFaction();
    if (!Factions::IsLawful(faction) || faction == Factions::Unaffiliated())
        return no(station->GetName() + " is nobody's to rent an office from");
    if (c->offices.count(key) != 0)
        return no(Tag(*c) + " has an office at " + station->GetName() + " already");
    std::string       why;
    const std::string corp = c->id;
    if (!PayRent(corp, InfluenceMap::Key(faction), rules.rent, why))
        return no(why);
    Corps::Office o;
    o.system = st->id;
    o.station = station->GetName();
    o.until = time_ + rules.periodDays * DAY;
    corps_.SetOffice(corp, key, o);
    Corps::Outcome out;
    out.corp = corp;
    out.told = "You rent an office at " + station->GetName() + " for " + Cr(rules.rent) +
               " every " + std::to_string((int)rules.periodDays) +
               " days: its divisions are in the inventory when you dock there";
    out.announce = account + " rented an office at " + station->GetName();
    return out;
}

void Simulation::StepOffices()
{
    const auto&                                      rules = WorldRules::OfficeRules();
    const double                                     period = rules.periodDays * DAY;
    std::vector<std::pair<std::string, std::string>> all;
    for (const auto& kv : corps_.All())
        for (const auto& o : kv.second.offices)
            all.emplace_back(kv.first, o.first);
    for (const auto& co : all)
    {
        Corps::Office* had = corps_.OfficeOf(co.first, co.second);
        const Entity*  station = OfficeStation(co.second);
        if (had == nullptr || station == nullptr || time_ < had->until)
            continue;
        Corps::Office   o = *had;
        const FactionId f = static_cast<const Station&>(*station).GetFaction();
        std::string     why;
        if (PayRent(co.first, InfluenceMap::Key(f), rules.rent, why))
        {
            o.until += period;
            o.told = 0;
            corps_.SetOffice(co.first, co.second, o);
            TellParty(co.first, "Paid " + Cr(rules.rent) + " for the office at " + o.station);
            continue;
        }
        const bool locked = OfficeLocked(o);
        const int  now = locked ? 2 : 1;
        if (o.told < now)
        {
            o.told = now;
            corps_.SetOffice(co.first, co.second, o);
            TellParty(co.first, locked ? "The office at " + o.station +
                                             " is locked, its rent unpaid (" + why +
                                             "): goods may be taken out, none put in"
                                       : "The office at " + o.station + " is unpaid: " + why);
        }
    }
}

std::vector<Proto::OfficeView> Simulation::PartyOffices(const std::string& corpId) const
{
    std::vector<Proto::OfficeView> out;
    const Corps::Corp*             c = corps_.Find(corpId);
    if (c == nullptr)
        return out;
    const auto& rules = WorldRules::OfficeRules();
    for (const auto& kv : c->offices)
    {
        Proto::OfficeView v;
        v.system = kv.second.system;
        v.station = kv.second.station;
        v.rent = rules.rent;
        v.periodDays = rules.periodDays;
        v.until = kv.second.until;
        v.state = OfficeLocked(kv.second) ? "locked" : time_ >= kv.second.until ? "unpaid" : "paid";
        for (const auto& d : kv.second.goods)
            v.held += d.second.Used();
        v.units = rules.units;
        out.push_back(std::move(v));
    }
    return out;
}
