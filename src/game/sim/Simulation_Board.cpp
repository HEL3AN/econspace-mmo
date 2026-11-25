// Exchange contracts (#473, documents/player_exchange.md): a pilot, or a corporation, puts goods,
// credits and charts up at a station and names what it wants for them; anybody docked there it
// is open to takes it, and the server swaps. It is the faction Contract generalised -- the same
// record, deadline, board and saved list -- with `kind` "exchange".
//
// The giving bundle is *held*: taken from the issuer's containers when it is posted and kept in
// the contract, the way a trade holds nothing between confirms but a board holds everything for
// days. The server is the only escrow there is, and the contract is saved with the world, so a
// restart loses nothing. What the issuer is to receive has room reserved in its hangar (or its
// office) at posting, so a deal cannot fail at the last moment for want of space, and an
// offline pilot is paid: goods into their hangar there, credits in the book of owners.
//
// Posting costs a flat fee and a cap on how many a pilot keeps open; taking pays the market's
// own fee on the value that changes hands (FeeAt), so the holder's tax means something the
// moment two pilots talk. A chart on the giving side is a copy frozen when it was posted. Nothing
// is free text: the board is the server's description of every bundle.
//
// One translation unit of Simulation (#17).

#include "sim/Simulation.h"

#include "core/Faction.h"
#include "entities/Entity.h"
#include "entities/Ship.h"
#include "sim/WorldRules.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace
{
bool SameName(const std::string& a, const std::string& b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]))
            return false;
    return true;
}

std::string Cr(long long c)
{
    return std::to_string(c) + " cr";
}

std::string Place(const Exchange::GoodLine& g, bool corp)
{
    return g.from.empty() ? std::string(corp ? "storage" : "hold") : g.from;
}

int Volume(const Inventory& inv)
{
    return inv.Used();
}

// "5 Iron, 100 cr, the chart of Haven": what a bundle says, for a journal line.
std::string Say(const Inventory& goods, long long credits, const std::vector<std::string>& charts)
{
    std::string out;
    auto        add = [&](const std::string& part) { out += (out.empty() ? "" : ", ") + part; };
    for (const auto& [type, n] : goods.All())
        add(std::to_string(n) + " " + ResourceName(type));
    if (credits > 0)
        add(Cr(credits));
    for (const std::string& c : charts)
        add(c);
    return out.empty() ? std::string("nothing") : out;
}

Inventory ToInventory(const std::vector<Exchange::GoodLine>& lines)
{
    Inventory inv;
    for (const Exchange::GoodLine& g : lines)
        inv.Add(ResourceType::FromIndex(g.type), g.amount);
    return inv;
}

std::vector<Exchange::GoodLine> ToLines(const Inventory& inv)
{
    std::vector<Exchange::GoodLine> out;
    for (const auto& [type, n] : inv.All())
        out.push_back({ type.Index(), n, "" });
    return out;
}
}  // namespace

// --- Values and the board's words -----------------------------------------------------------

double Simulation::GoodsValue(const SystemState& st, const std::vector<Exchange::GoodLine>& g)
{
    double v = 0.0;
    for (const Exchange::GoodLine& l : g)
        v += l.amount * st.market.Mid(ResourceType::FromIndex(l.type));
    return v;
}

double Simulation::DealValue(const SystemState& st, const Inventory& goods, long long credits) const
{
    double v = (double)credits;
    for (const auto& [type, n] : goods.All())
        v += n * st.market.Mid(type);
    return v;
}

void Simulation::PayAccount(const std::string& account, double amount)
{
    if (account.empty() || !(amount > 0.0))
        return;
    for (auto& kv : sessions_)
        if (kv.second.accountName == account)
        {
            kv.second.account.AddMoney(amount);
            return;
        }
    owners_[account].owedMoney += amount;  // paid when they are next on (StepOwners)
}

// What an issuer is given, at the station: a pilot's hangar there, a corporation's office, or --
// where that office is gone -- the pilot who posted it.
void Simulation::GiveToIssuer(const Contract& c, const Inventory& goods, long long credits)
{
    std::string corpId;
    if (Corps::CorpOwner(c.issuer, corpId) && corps_.Find(corpId) != nullptr)
    {
        Inventory* office = corps_.OfficeOf(corpId, c.place) != nullptr
                                ? corps_.OfficeGoods(corpId, c.place, "shared")
                                : nullptr;
        if (office != nullptr)
        {
            for (const auto& [type, n] : goods.All())
                office->Add(type, n);
            corps_.Touch();
        }
        else if (!goods.Empty())
        {
            Hangar* h = HangarOf(c.place, c.poster, true);
            for (const auto& [type, n] : goods.All())
                h->goods.Add(type, n);
        }
        if (credits > 0)
            corps_.Credit(corpId, (double)credits, "deal", c.poster);
        return;
    }
    if (!goods.Empty())
    {
        Hangar* h = HangarOf(c.place, c.issuer, true);
        for (const auto& [type, n] : goods.All())
            h->goods.Add(type, n);
    }
    PayAccount(c.issuer, (double)credits);
}

std::string Simulation::DealProblem(const ClientSession& s, const Contract& c, bool forCorp) const
{
    if (!c.IsExchange())
        return "not an exchange contract";
    if (!c.to.empty())
    {
        std::string corpId;
        const bool  toCorp = Corps::CorpOwner(c.to, corpId);
        if (toCorp ? corps_.IdOf(s.accountName) != corpId : !SameName(c.to, s.accountName))
            return "it is not offered to you";
    }
    if (c.poster == s.accountName || c.issuer == s.accountName)
        return "it is yours: cancel it to take it back";
    std::string issuerCorp;
    if (Corps::CorpOwner(c.issuer, issuerCorp) && corps_.IdOf(s.accountName) == issuerCorp)
        return "it is your corporation's: cancel it to take it back";
    const Entity*      station = DockedHangarStation(s);
    const SystemState* st = SystemOf(s);
    if (station == nullptr || st == nullptr || HangarKey(*st, *station) != c.place)
        return "dock at " + c.placeName + " to take it";
    if (forCorp)
    {
        const Corps::Corp*   corp = corps_.Of(s.accountName);
        std::string          key;
        const Corps::Office* office = OfficeHere(s, &key);
        if (corp == nullptr || !corps_.Commands(s.accountName, Corps::OwnerOf(corp->id)))
            return "only your corporation's CEO or a director acts for it";
        if (office == nullptr || key != c.place || OfficeLocked(*office))
            return "your corporation has no open office here";
        return std::string();
    }
    // What is asked, in the hold and the hangar here together.
    const auto    hangars = hangars_.find(c.place);
    const Hangar* mine = nullptr;
    if (hangars != hangars_.end())
    {
        const auto h = hangars->second.find(s.accountName);
        if (h != hangars->second.end())
            mine = &h->second;
    }
    for (const Exchange::GoodLine& g : c.asking.goods)
    {
        const ResourceType type = ResourceType::FromIndex(g.type);
        int                have = s.ship ? s.ship->GetCargoAmount(type) : 0;
        if (mine != nullptr)
            have += mine->goods.Amount(type);
        int need = 0;
        for (const Exchange::GoodLine& o : c.asking.goods)
            if (o.type == g.type)
                need += o.amount;
        if (have < need)
            return "you lack " + std::to_string(need) + " " + ResourceName(type);
    }
    if (std::floor(s.account.GetMoney()) < (double)c.asking.credits)
        return "you lack " + Cr(c.asking.credits);
    return std::string();
}

std::vector<Proto::DealView> Simulation::BoardFor(const ClientSession& s) const
{
    std::vector<Proto::DealView> out;
    if (s.accountName.empty())
        return out;
    const std::string  myCorp = corps_.IdOf(s.accountName);
    const Entity*      here = DockedHangarStation(s);
    const SystemState* mySt = SystemOf(s);
    const std::string  hereKey = here != nullptr && mySt != nullptr ? HangarKey(*mySt, *here) : "";
    const WorldRules::Exchange& rules = WorldRules::ExchangeRules();
    for (const auto& kv : contracts_)
    {
        const Contract& c = kv.second;
        if (!c.IsExchange() || out.size() >= 40)
            continue;
        std::string issuerCorp;
        const bool  corpIssued = Corps::CorpOwner(c.issuer, issuerCorp);
        const bool  mine =
            c.poster == s.accountName || (corpIssued && corps_.Commands(s.accountName, c.issuer));
        std::string toCorp;
        const bool  toMyCorp = Corps::CorpOwner(c.to, toCorp) && toCorp == myCorp;
        const bool  addressed = !c.to.empty() && (toMyCorp || SameName(c.to, s.accountName));
        const bool  here_ = !hereKey.empty() && c.place == hereKey;
        // On the board here when it is open or is for this pilot; the pilot's own and what is
        // addressed to them wherever they are.
        if (!(mine || addressed || (here_ && c.to.empty())))
            continue;
        Proto::DealView d;
        d.id = c.id;
        if (corpIssued)
        {
            const Corps::Corp* corp = corps_.Find(issuerCorp);
            d.issuer = corp != nullptr ? corp->name + " [" + corp->ticker + "]" : "a corporation";
        }
        else
            d.issuer = c.issuer;
        if (!c.to.empty())
        {
            std::string        t;
            const Corps::Corp* corp = Corps::CorpOwner(c.to, t) ? corps_.Find(t) : nullptr;
            d.to = corp != nullptr ? "[" + corp->ticker + "]" : c.to;
        }
        d.place = c.placeName;
        d.system = c.system;
        d.here = here_;
        d.mine = mine;
        d.deadline = c.deadline;
        d.give = ToLines(c.giveGoods);
        d.ask = c.asking.goods;
        d.giveCredits = c.giveCredits;
        d.askCredits = c.asking.credits;
        for (const ChartCopy& copy : c.giveCharts)
            d.giveCharts.push_back(ChartGiftText(ChartGiftOfCopy(copy, s)));
        if (const SystemState* st = SystemById(c.system))
        {
            const double vg = DealValue(*st, c.giveGoods, c.giveCredits);
            const double va = DealValue(*st, ToInventory(c.asking.goods), c.asking.credits);
            if (const Entity* station = OfficeStation(c.place))
                d.fee = FeeAt(*st, *station).rate * std::max(vg, va);
            // A price far from the market's: a typo is a scam as much as a deal.
            if (c.giveCharts.empty() && vg > 0.0 && va > 0.0)
            {
                if (va > rules.warnRatio * vg)
                    d.note = "asks " + std::to_string((int)std::lround(va / vg)) +
                             " times the market value of what it gives";
                else if (vg > rules.warnRatio * va)
                    d.note = "gives " + std::to_string((int)std::lround(vg / va)) +
                             " times the market value of what it asks";
            }
        }
        if (here_ && !mine)
        {
            d.why = DealProblem(s, c, false);
            d.canAccept = d.why.empty();
        }
        else if (!mine)
            d.why = "dock at " + c.placeName + " to take it";
        out.push_back(std::move(d));
    }
    return out;
}

// --- Posting ----------------------------------------------------------------------------------

namespace
{
// A container's goods on a copy, and where it came from: a trade's way of being all or nothing.
struct Box
{
    Simulation::Container c;
    Inventory             inv;
};
}  // namespace

bool Simulation::PostDeal(ClientSession& s, const Proto::Act::Board& a)
{
    const WorldRules::Exchange& rules = WorldRules::ExchangeRules();
    auto                        refuse = [&](const std::string& why)
    {
        s.RecordEvent(Ev::Kind::Notice, "Contract not posted: " + why);
        return false;
    };
    std::string   why;
    const Entity* station = DockedHangarStation(s, &why);
    SystemState*  st = SystemOf(s);
    if (station == nullptr || st == nullptr)
        return refuse("dock at a station that keeps hangars: " + why);
    const std::string place = HangarKey(*st, *station);

    // Whose it is.
    std::string        issuer = s.accountName, corpId;
    const Corps::Corp* corp = nullptr;
    if (a.corp)
    {
        corp = corps_.Of(s.accountName);
        if (corp == nullptr || !corps_.Commands(s.accountName, Corps::OwnerOf(corp->id)))
            return refuse("only a corporation's CEO or a director posts for it");
        std::string          key;
        const Corps::Office* office = OfficeHere(s, &key);
        if (office == nullptr || key != place || OfficeLocked(*office))
            return refuse("your corporation has no open office at this station");
        corpId = corp->id;
        issuer = Corps::OwnerOf(corp->id);
    }
    int open = 0;
    for (const auto& kv : contracts_)
        if (kv.second.IsExchange() && kv.second.poster == s.accountName)
            open++;
    if (open >= rules.maxOpen)
        return refuse("you keep " + std::to_string(open) + " open already, the most there may be");

    // What it is for.
    if (a.giving.goods.empty() && a.giving.credits <= 0 && a.giving.charts.empty())
        return refuse("it gives nothing: put something up");
    if (!a.asking.charts.empty())
        return refuse("a contract may ask for goods and credits, not charts");
    for (const Exchange::GoodLine& g : a.giving.goods)
        if (g.type < 0 || g.type >= Goods::Count() || g.amount <= 0)
            return refuse("no such good");
    for (const Exchange::GoodLine& g : a.asking.goods)
        if (g.type < 0 || g.type >= Goods::Count() || g.amount <= 0)
            return refuse("no such good");
    std::string to;
    if (!a.to.empty())
    {
        if (a.to.rfind("corp:", 0) == 0)
        {
            const Corps::Corp* target = corps_.FindTicker(a.to.substr(5));
            if (target == nullptr)
                return refuse("no corporation has the ticker " + a.to.substr(5));
            to = Corps::OwnerOf(target->id);
        }
        else
        {
            if (a.to.size() > 32 || SameName(a.to, s.accountName))
                return refuse("name another pilot, or corp:<ticker>");
            to = a.to;
        }
    }
    double days = a.days > 0 ? (double)a.days : rules.defaultDays;
    days = std::min(days, rules.maxDays);

    // Room for what it asks, kept for it: the hangar or the office that will receive it.
    const Inventory askGoods = ToInventory(a.asking.goods);
    int             reserved = 0;
    for (const auto& kv : contracts_)
        if (kv.second.IsExchange() && kv.second.issuer == issuer && kv.second.place == place)
            reserved += Volume(ToInventory(kv.second.asking.goods));
    int room = 0;
    if (corp != nullptr)
    {
        const Corps::Office* office = OfficeHere(s);
        int                  used = 0;
        for (const auto& d : office->goods)
            used += d.second.Used();
        room = WorldRules::OfficeRules().units - used;
    }
    else
    {
        Hangar* h = HangarOf(place, s.accountName, true);
        room = HangarCapacity(*h, *station) - h->goods.Used();
    }
    if (Volume(askGoods) > room - reserved)
        return refuse("there is no room here to receive what it asks, " +
                      std::to_string(std::max(0, room - reserved)) + " free of " +
                      std::to_string(Volume(askGoods)));

    // The escrow, on copies: all of it can be taken, or none of it.
    std::map<std::string, Box> boxes;
    auto                       open_ = [&](const std::string& id) -> Box*
    {
        const auto it = boxes.find(id);
        if (it != boxes.end())
            return &it->second;
        Box b;
        if (!ContainerOf(s, id, b.c, why))
            return nullptr;
        b.inv = *b.c.goods;
        return &boxes.emplace(id, std::move(b)).first->second;
    };
    Inventory escrow;
    for (const Exchange::GoodLine& g : a.giving.goods)
    {
        const std::string id = Place(g, corp != nullptr);
        if (id.rfind("can:", 0) == 0 || id == "can")
            return refuse("goods in space are not part of a contract");
        if (corp != nullptr ? id.rfind("storage", 0) != 0 : id.rfind("storage", 0) == 0)
            return refuse(corp != nullptr ? "a corporation gives from its office's storage"
                                          : "you give from your hold or your hangar");
        Box* box = open_(id);
        if (box == nullptr)
            return refuse(why);
        const ResourceType type = ResourceType::FromIndex(g.type);
        if (!box->c.gives)
            return refuse("your role may not take from " + box->c.name);
        if (box->inv.Amount(type) < g.amount)
            return refuse(box->c.name + " has only " + std::to_string(box->inv.Amount(type)) + " " +
                          ResourceName(type));
        box->inv.Remove(type, g.amount);
        escrow.Add(type, g.amount);
    }
    std::set<const Inventory*> stores;
    for (const auto& kv : boxes)
        if (!stores.insert(kv.second.c.goods).second)
            return refuse("the same storage is used twice");
    std::vector<ChartCopy>   charts;
    std::vector<std::string> chartNames;
    for (const std::string& which : a.giving.charts)
    {
        std::string id = which.rfind("system:", 0) == 0 ? which.substr(7) : which;
        if (!HasSystem(id))
            id = SystemNamed(which);
        ChartCopy copy = id.empty() ? ChartCopy() : MakeChartCopy(s, id);
        if (!copy.charted || s.known.count(ChartKey(id)) == 0)
            return refuse("the chart of " + which + " is not on your own charts");
        chartNames.push_back("the chart of " + SystemName(id));
        charts.push_back(std::move(copy));
    }
    const double fee = rules.postingFee;
    if (corp != nullptr)
    {
        if (corp->treasury < (double)a.giving.credits + fee)
            return refuse("the treasury holds less than " +
                          Cr(a.giving.credits + (long long)std::ceil(fee)));
    }
    else if (std::floor(s.account.GetMoney()) < (double)a.giving.credits + fee)
        return refuse("you lack " + Cr(a.giving.credits + (long long)std::ceil(fee)) +
                      " for it and the posting fee");

    // --- Taken and held. ---
    for (auto& kv : boxes)
    {
        *kv.second.c.goods = kv.second.inv;
        if (!kv.second.c.takeLog.empty())
            corps_.Touch();
    }
    if (corp != nullptr)
    {
        corps_.Debit(corpId, (double)a.giving.credits + fee, "deal", s.accountName,
                     "contract posted");
    }
    else
        s.account.AddMoney(-((double)a.giving.credits + fee));
    PayFee(FeeAt(*st, *station), fee);

    Contract c;
    c.id = nextContractId_++;
    c.kind = "exchange";
    c.issuer = issuer;
    c.poster = s.accountName;
    c.to = to;
    c.system = st->id;
    c.place = place;
    c.placeName = station->GetName() + ", " + SystemName(st->id);
    c.giveGoods = escrow;
    c.giveCredits = a.giving.credits;
    c.giveCharts = std::move(charts);
    c.asking.goods = a.asking.goods;
    c.asking.credits = a.asking.credits;
    c.postedAt = time_;
    c.deadline = time_ + days * 86400.0;
    contracts_[c.id] = c;
    const std::string said = "posted contract #" + std::to_string(c.id) + " at " + c.placeName +
                             ": gives " + Say(c.giveGoods, c.giveCredits, chartNames) + " for " +
                             Say(askGoods, c.asking.credits, {}) +
                             (to.empty() ? "" : ", offered to " + a.to) + " (" +
                             Cr((long long)std::ceil(fee)) + " to post)";
    s.RecordEvent(Ev::Kind::Notice, "Contract " + said);
    if (corp != nullptr)
        corps_.Record(s.accountName, Ev::Kind::Notice, s.accountName + " " + said);
    return true;
}

// --- Taking -----------------------------------------------------------------------------------

bool Simulation::AcceptDeal(ClientSession& s, int id, bool forCorp)
{
    auto refuse = [&](const std::string& why)
    {
        s.RecordEvent(Ev::Kind::Notice, "Contract not taken: " + why);
        return false;
    };
    const auto it = contracts_.find(id);
    if (it == contracts_.end() || !it->second.IsExchange())
        return refuse("there is no contract #" + std::to_string(id) +
                      " any more: it was taken, cancelled or ran out");
    const Contract    c = it->second;  // a copy: erasing it must not undo what follows
    const std::string problem = DealProblem(s, c, forCorp);
    if (!problem.empty())
        return refuse(problem);
    SystemState*  st = SystemOf(s);
    const Entity* station = DockedHangarStation(s);

    std::string        corpId;
    const Corps::Corp* corp = forCorp ? corps_.Of(s.accountName) : nullptr;
    if (corp != nullptr)
        corpId = corp->id;
    std::string                why;
    std::map<std::string, Box> boxes;
    auto                       open_ = [&](const std::string& bid) -> Box*
    {
        const auto b = boxes.find(bid);
        if (b != boxes.end())
            return &b->second;
        Box box;
        if (!ContainerOf(s, bid, box.c, why))
            return nullptr;
        box.inv = *box.c.goods;
        return &boxes.emplace(bid, std::move(box)).first->second;
    };
    const std::vector<std::string> sources =
        corp != nullptr ? std::vector<std::string>{ "storage", "storage:stock", "storage:command" }
                        : std::vector<std::string>{ "hold", "hangar" };
    const std::vector<std::string> sinks = corp != nullptr
                                               ? std::vector<std::string>{ "storage" }
                                               : std::vector<std::string>{ "hold", "hangar" };
    // What it asks, out of the hold and then the hangar -- or the office's divisions.
    for (const Exchange::GoodLine& g : c.asking.goods)
    {
        const ResourceType type = ResourceType::FromIndex(g.type);
        int                left = g.amount;
        for (const std::string& src : sources)
        {
            if (left <= 0)
                break;
            why.clear();
            Box* box = open_(src);
            if (box == nullptr || !box->c.gives)
                continue;
            const int n = std::min(left, box->inv.Amount(type));
            if (n > 0)
            {
                box->inv.Remove(type, n);
                left -= n;
            }
        }
        if (left > 0)
            return refuse("you lack " + std::to_string(g.amount) + " " + ResourceName(type));
    }
    // What it gives, into the hold and then the hangar -- or the office.
    for (const auto& [type, n] : c.giveGoods.All())
    {
        int left = n;
        for (const std::string& dst : sinks)
        {
            if (left <= 0)
                break;
            why.clear();
            Box* box = open_(dst);
            if (box == nullptr || !box->c.accepts)
                continue;
            const int fits =
                box->inv.Room(box->c.capacity, box->c.ore, type) / std::max(1, Goods::Volume(type));
            const int put = std::min(left, std::max(0, fits));
            if (put > 0)
            {
                box->inv.Add(type, put);
                left -= put;
            }
        }
        if (left > 0)
            return refuse("there is no room for " + std::to_string(n) + " " + ResourceName(type));
    }
    std::set<const Inventory*> stores;
    for (const auto& kv : boxes)
        if (!stores.insert(kv.second.c.goods).second)
            return refuse("the same storage is used twice");

    // The fee: the market's, on the dearer side, paid by whoever takes it.
    const double fee = FeeAt(*st, *station).rate *
                       std::max(DealValue(*st, c.giveGoods, c.giveCredits),
                                DealValue(*st, ToInventory(c.asking.goods), c.asking.credits));
    const double owes = (double)c.asking.credits + fee;
    if (corp != nullptr ? corp->treasury < owes : std::floor(s.account.GetMoney()) < owes)
        return refuse((corp != nullptr ? "the treasury lacks " : "you lack ") +
                      Cr((long long)std::ceil(owes)) + ": its price and the market's fee");

    // --- Done: written all at once. ---
    for (auto& kv : boxes)
    {
        *kv.second.c.goods = kv.second.inv;
        if (!kv.second.c.takeLog.empty())
            corps_.Touch();
    }
    if (corp != nullptr)
    {
        corps_.Debit(corpId, owes, "fees", s.accountName, "contract taken");
        corps_.Credit(corpId, (double)c.giveCredits, "deal", s.accountName);
    }
    else
    {
        s.account.AddMoney(-owes);
        s.account.AddMoney((double)c.giveCredits);
    }
    PayFee(FeeAt(*st, *station), fee);
    // The issuer is paid: goods into the hangar or office reserved for them, credits to them.
    GiveToIssuer(c, ToInventory(c.asking.goods), c.asking.credits);
    std::vector<std::string> chartNames;
    for (const ChartCopy& copy : c.giveCharts)
    {
        ApplyChartCopy(s, copy);
        chartNames.push_back("the chart of " + SystemName(copy.system));
    }
    // The poster's own count of sales, when they are on.
    for (auto& kv : sessions_)
        if (kv.second.accountName == c.poster)
            for (const ChartCopy& copy : c.giveCharts)
                kv.second.chartSales[copy.system]++;
    contracts_.erase(id);
    const std::string gave = Say(c.giveGoods, c.giveCredits, chartNames);
    const std::string got = Say(ToInventory(c.asking.goods), c.asking.credits, {});
    const std::string who =
        (corp != nullptr ? corp->name + " [" + corp->ticker + "] (" + s.accountName + ")"
                         : s.accountName);
    s.RecordEvent(
        Ev::Kind::Notice,
        "Took contract #" + std::to_string(id) + " from " + c.issuer + ": received " + gave +
            ", gave " + got +
            (fee > 0.0 ? ", market fee " + Cr((long long)std::ceil(fee)) : std::string()) + ".");
    TellAccount(c.poster, Ev::Kind::Notice,
                "Your contract #" + std::to_string(id) + " at " + c.placeName + " was taken by " +
                    who + ": you gave " + gave + ", you received " + got + ".");
    if (Corps::CorpOwner(c.issuer, corpId))
        corps_.Record(c.poster, Ev::Kind::Notice,
                      "Contract #" + std::to_string(id) + " was taken by " + who + ": gave " +
                          gave + ", received " + got);
    // The ledger an admin reads: who gave what to whom, where, when.
    tradeLog_.push_back("trade: contract #" + std::to_string(id) + ": " + c.issuer + " gave " +
                        gave + " to " + who + " for " + got + " at " + c.placeName);
    return true;
}

// --- Ending -----------------------------------------------------------------------------------

void Simulation::EndDeal(int id, const std::string& why)
{
    const auto it = contracts_.find(id);
    if (it == contracts_.end() || !it->second.IsExchange())
        return;
    const Contract c = it->second;
    contracts_.erase(it);
    // Everything held goes back; the posting fee was the price of the board and is not.
    GiveToIssuer(c, c.giveGoods, c.giveCredits);
    std::string corpId;
    const bool  corpIssued = Corps::CorpOwner(c.issuer, corpId);
    TellAccount(c.poster, Ev::Kind::Notice,
                "Your contract #" + std::to_string(id) + " at " + c.placeName + " " + why + ": " +
                    Say(c.giveGoods, c.giveCredits, {}) + " went back to " +
                    (corpIssued ? "its corporation" : "your hangar there") + ".");
    if (corpIssued)
        corps_.Record(c.poster, Ev::Kind::Notice,
                      "Contract #" + std::to_string(id) + " " + why + ": " +
                          Say(c.giveGoods, c.giveCredits, {}) + " went back");
}

bool Simulation::CancelDeal(ClientSession& s, int id)
{
    const auto it = contracts_.find(id);
    if (it == contracts_.end() || !it->second.IsExchange())
    {
        s.RecordEvent(Ev::Kind::Notice,
                      "Contract not cancelled: there is no contract #" + std::to_string(id));
        return false;
    }
    const Contract& c = it->second;
    if (c.poster != s.accountName && !corps_.Commands(s.accountName, c.issuer))
    {
        s.RecordEvent(Ev::Kind::Notice, "Contract not cancelled: it is not yours");
        return false;
    }
    EndDeal(id, "was cancelled");
    return true;
}

void Simulation::StepBoard()
{
    std::vector<int> over;
    for (const auto& kv : contracts_)
        if (kv.second.IsExchange() && time_ > kv.second.deadline)
            over.push_back(kv.first);
    for (int id : over)
        EndDeal(id, "ran out of time");
}

void Simulation::ApplyBoard(ClientSession& s, const Proto::Act::Board& a)
{
    if (a.action == "post")
        PostDeal(s, a);
    else if (a.action == "accept")
        AcceptDeal(s, a.id, a.corp);
    else if (a.action == "cancel")
        CancelDeal(s, a.id);
    else
        s.RecordEvent(Ev::Kind::Notice, "Contract: unknown action '" + a.action + "'");
}
