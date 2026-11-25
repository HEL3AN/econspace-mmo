// Exchange between pilots (#473, documents/player_exchange.md): the server standing between two
// parties so that neither can cheat. This file is the direct trade -- two pilots docked at one
// station, each putting in a bundle of goods, credits and charts, both confirming what they
// saw -- and what a chart given to another pilot is.
//
// The server is the only escrow there is, and it is perfect: nothing leaves a container until
// both have confirmed, and then it all moves in one step (every container is copied, changed
// on the copy and written back together, so a failed check leaves the world as it was). What
// a side sees of the other's offer is the server's description, never a title; and a confirm
// carries the revision of what it was shown, so an offer that moved after it was looked at
// cannot be confirmed.
//
// One translation unit of Simulation (#17).

#include "sim/Simulation.h"

#include "core/Faction.h"
#include "entities/Entity.h"
#include "entities/Ship.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace
{

constexpr const char* HOLD = "hold";

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

std::string HoursAgo(double seconds)
{
    const double h = std::max(0.0, seconds) / 3600.0;
    char         buf[40];
    if (h < 1.0)
        std::snprintf(buf, sizeof buf, "%.0f min", h * 60.0);
    else if (h < 48.0)
        std::snprintf(buf, sizeof buf, "%.0f h", h);
    else
        std::snprintf(buf, sizeof buf, "%.0f days", h / 24.0);
    return buf;
}

std::string Times(int n)
{
    return n == 1 ? std::string("once") : std::to_string(n) + " times";
}

std::string Place(const Exchange::GoodLine& g)
{
    return g.from.empty() ? std::string(HOLD) : g.from;
}

// What a list of goods and credits says, as a player reads it: "5 Iron, 3 Ice, 100 cr".
std::string Say(const std::vector<Exchange::GoodLine>& goods, long long credits,
                const std::vector<std::string>& charts)
{
    std::string out;
    auto        add = [&](const std::string& part) { out += (out.empty() ? "" : ", ") + part; };
    std::map<int, int> sum;
    for (const Exchange::GoodLine& g : goods)
        sum[g.type] += g.amount;
    for (const auto& [type, n] : sum)
        add(std::to_string(n) + " " + ResourceName(ResourceType::FromIndex(type)));
    if (credits > 0)
        add(Cr(credits));
    for (const std::string& c : charts)
        add(c);
    return out.empty() ? std::string("nothing") : out;
}

}  // namespace

// --- Who is trading with whom -----------------------------------------------------------------

namespace
{
// Both docked at one station, so a trade is a thing between two people standing side by side.
bool SameDock(const ClientSession& a, const ClientSession& b)
{
    return a.IsDocked() && b.IsDocked() && a.systemId == b.systemId &&
           a.dockedStationId == b.dockedStationId;
}
}  // namespace

void Simulation::CancelTrade(ClientSession& s, const std::string& why)
{
    ClientSession* p = s.trade.with != 0 ? Session(s.trade.with) : nullptr;
    const bool     live = p != nullptr && p->trade.with == s.id;
    s.trade = ClientSession::Trade{};
    if (live)
    {
        p->trade = ClientSession::Trade{};
        if (!why.empty())
        {
            p->RecordEvent(Ev::Kind::Notice, "Trade with " + s.accountName + " ended: " + why);
            s.RecordEvent(Ev::Kind::Notice, "Trade with " + p->accountName + " ended: " + why);
        }
    }
}

void Simulation::StepTrades()
{
    for (auto& kv : sessions_)
    {
        ClientSession& s = kv.second;
        if (s.trade.with == 0)
            continue;
        const ClientSession* p = Session(s.trade.with);
        if (p == nullptr)
            s.trade = ClientSession::Trade{};
        else if (!SameDock(s, *p))
            CancelTrade(s, "you are no longer docked at the same station");
    }
}

Proto::TradeView Simulation::TradeViewFor(const ClientSession& s) const
{
    Proto::TradeView     v;
    const ClientSession* p = s.trade.with != 0 ? Session(s.trade.with) : nullptr;
    const bool           live = p != nullptr && p->trade.with == s.id && SameDock(s, *p);
    for (const auto& kv : sessions_)
    {
        const ClientSession& o = kv.second;
        if (o.id != s.id && o.trade.with == s.id && s.trade.with != o.id && SameDock(s, o))
            v.invites.push_back(o.accountName);
    }
    if (p != nullptr && !live && SameDock(s, *p))
        v.asking = p->accountName;
    if (!live)
        return v;
    v.with = p->accountName;
    v.rev = s.trade.version + p->trade.version;
    v.fee = TradeFee(s, *p) / 2.0;
    v.problem = s.trade.note;
    auto side = [&](const ClientSession& who, const ClientSession& reader, bool mine,
                    Proto::TradeSideView& out)
    {
        out.name = who.accountName;
        out.goods = who.trade.offer.goods;
        out.credits = who.trade.offer.credits;
        out.confirmed = who.trade.confirmed;
        for (const std::string& sys : who.trade.offer.charts)
        {
            const ChartGift g = ChartGiftFor(who, reader, sys);
            if (mine)
            {
                std::string       text = SystemName(sys);
                const ChartReport r = ReportOf(who, sys, false);
                text += r.observed ? " (observed " + HoursAgo(time_ - r.at) + " ago" : " (charted";
                if (g.sold > 0)
                    text += "; sold " + Times(g.sold);
                if (g.hops > 0)
                    text += "; second-hand";
                text += "; " + ChartGiftText(g) + ")";
                out.charts.push_back({ sys, text });
            }
            else
                out.charts.push_back({ "", ChartGiftText(g) });
        }
    };
    side(s, *p, true, v.mine);
    side(*p, s, false, v.theirs);
    return v;
}

// --- What a chart given to another pilot is ---------------------------------------------------

Simulation::ChartGift Simulation::ChartGiftFor(const ClientSession& from, const ClientSession& to,
                                               const std::string& systemId) const
{
    ChartGift g;
    g.system = systemId;
    const ChartReport r = ReportOf(from, systemId, false);
    g.charted = r.charted;
    if (!r.charted)
        return g;
    g.geography = !Knows(to, systemId);
    if (r.observed)
    {
        const WorldLoader::Sighting theirs = LastSeen(to, systemId);
        g.fresher = !theirs.Observed() || r.at > theirs.at || theirs.outdated;
        g.seenAt = r.at;
        g.stations = r.stations;
        g.structures = r.structures;
    }
    for (const std::string& key : r.finds)
        if (to.known.count(key) == 0)
        {
            g.finds.push_back(key);
            const RoamingHole* h = key.rfind("hole:", 0) == 0 ? HoleById(key.substr(5)) : nullptr;
            if (h != nullptr && h->closesAt > time_ &&
                (g.closing < 0.0 || h->closesAt - time_ < g.closing))
                g.closing = h->closesAt - time_;
        }
    const auto how = from.secondHand.find(systemId);
    g.hops = how == from.secondHand.end() ? 0 : how->second.hops;
    const auto sold = from.chartSales.find(systemId);
    g.sold = sold == from.chartSales.end() ? 0 : sold->second;
    return g;
}

std::string Simulation::ChartGiftText(const ChartGift& g) const
{
    if (!g.charted)
        return "not on the giver's charts";
    if (g.Worthless())
        return "known to you";
    std::string parts;
    auto        add = [&](const std::string& part) { parts += (parts.empty() ? "" : ", ") + part; };
    if (g.geography)
        add("1 system");
    if (g.fresher && g.stations > 0)
        add(std::to_string(g.stations) + (g.stations == 1 ? " station" : " stations"));
    if (g.fresher && g.structures > 0)
        add(std::to_string(g.structures) + (g.structures == 1 ? " structure" : " structures"));
    if (!g.finds.empty())
        add(std::to_string(g.finds.size()) + (g.finds.size() == 1 ? " find" : " finds"));
    std::string text = "new to you: " + (parts.empty() ? std::string("a newer look") : parts);
    if (g.fresher && g.seenAt >= 0.0)
        text += "; observed " + HoursAgo(time_ - g.seenAt) + " ago";
    if (g.closing >= 0.0)
        text += "; a hole among the finds closes in about " + HoursAgo(g.closing);
    if (g.hops > 0)
        text += "; second-hand, copied " + Times(g.hops);
    if (g.sold > 0)
        text += "; sold " + Times(g.sold) + " by them";
    return text;
}

// --- The trade --------------------------------------------------------------------------------

namespace
{
// A trade is closed once and ended once; what is put into it is bounded, so a pilot cannot
// make the server hold an unbounded list.
bool Merge(Exchange::Bundle& into, const Exchange::GoodLine& line)
{
    for (Exchange::GoodLine& g : into.goods)
        if (g.type == line.type && Place(g) == Place(line))
        {
            g.amount += line.amount;
            return true;
        }
    if (into.goods.size() >= Exchange::MAX_LINES)
        return false;
    into.goods.push_back(line);
    return true;
}
}  // namespace

const Entity* Simulation::DockEntity(const ClientSession& s) const
{
    const SystemState* st = SystemOf(s);
    if (st == nullptr || !s.IsDocked())
        return nullptr;
    for (const auto& e : st->entities)
        if (e->GetId() == s.dockedStationId)
            return e.get();
    return nullptr;
}

double Simulation::TradeFee(const ClientSession& a, const ClientSession& b) const
{
    // A station with no market has no holder to be paid: the fee is the market's, and it
    // stays the market's however two pilots settle between them.
    const SystemState* st = SystemOf(a);
    const Entity*      at = DockEntity(a);
    if (st == nullptr || at == nullptr || !at->Has(Component::Market))
        return 0.0;
    auto value = [&](const ClientSession& s)
    {
        double v = (double)s.trade.offer.credits;
        for (const Exchange::GoodLine& g : s.trade.offer.goods)
            v += g.amount * st->market.Mid(ResourceType::FromIndex(g.type));
        return v;
    };
    return FeeAt(*st, *at).rate * std::max(value(a), value(b));
}

// Everything a trade needs to be true, checked on copies; with `commit`, written back. Returns
// why it cannot be done, or empty.
std::string Simulation::SettleTrade(ClientSession& a, ClientSession& b, bool commit)
{
    if (!SameDock(a, b))
        return "you are no longer docked at the same station";
    SystemState* st = SystemOf(a);
    if (st == nullptr || a.accountName.empty() || b.accountName.empty())
        return "the station is gone";

    struct Box
    {
        Container      c;
        Inventory      inv;
        ClientSession* owner = nullptr;
    };
    std::map<std::pair<int, std::string>, Box> boxes;
    std::string                                why;
    auto open = [&](ClientSession& who, const std::string& id) -> Box*
    {
        const auto key = std::make_pair(who.id, id);
        const auto it = boxes.find(key);
        if (it != boxes.end())
            return &it->second;
        Box box;
        if (!ContainerOf(who, id, box.c, why))
            return nullptr;
        box.inv = *box.c.goods;
        box.owner = &who;
        return &boxes.emplace(key, std::move(box)).first->second;
    };

    ClientSession* sides[2] = { &a, &b };
    // What leaves each side's containers.
    for (ClientSession* give : sides)
    {
        for (const Exchange::GoodLine& g : give->trade.offer.goods)
        {
            const std::string id = Place(g);
            if (id.rfind("can:", 0) == 0 || id == "can")
                return "goods in space are not part of a trade";
            Box* box = open(*give, id);
            if (box == nullptr)
                return give->accountName + ": " + why;
            const ResourceType type = ResourceType::FromIndex(g.type);
            if (!box->c.gives)
                return give->accountName + "'s role may not take from " + box->c.name;
            if (box->inv.Amount(type) < g.amount)
                return give->accountName + " no longer has " + std::to_string(g.amount) + " " +
                       ResourceName(type) + " in " + box->c.name;
            box->inv.Remove(type, g.amount);
        }
        if ((double)give->trade.offer.credits > std::floor(give->account.GetMoney()))
            return give->accountName + " no longer has " + Cr(give->trade.offer.credits);
        for (const std::string& sys : give->trade.offer.charts)
            if (give->known.count(ChartKey(sys)) == 0)
                return give->accountName + " no longer has the chart of " + SystemName(sys);
    }
    // What arrives: into the hold, and what the hold has no room for into the hangar.
    for (int i = 0; i < 2; ++i)
    {
        ClientSession& give = *sides[i];
        ClientSession& get = *sides[1 - i];
        for (const Exchange::GoodLine& g : give.trade.offer.goods)
        {
            const ResourceType type = ResourceType::FromIndex(g.type);
            int                left = g.amount;
            for (const char* id : { HOLD, "hangar" })
            {
                if (left <= 0)
                    break;
                why.clear();
                Box* box = open(get, id);
                if (box == nullptr || !box->c.accepts)
                    continue;
                const int fits = box->inv.Room(box->c.capacity, box->c.ore, type) /
                                 std::max(1, Goods::Volume(type));
                const int n = std::min(left, std::max(0, fits));
                if (n > 0)
                {
                    box->inv.Add(type, n);
                    left -= n;
                }
            }
            if (left > 0)
                return get.accountName + " has no room for " + std::to_string(g.amount) + " " +
                       ResourceName(type);
        }
    }
    // Two containers that are one store (a corporation's shared division, seen twice) would
    // each write their own copy over the other.
    std::set<const Inventory*> stores;
    for (const auto& kv : boxes)
        if (!stores.insert(kv.second.c.goods).second)
            return "the same storage is used twice in this trade";

    // The money, the fee and the charts.
    const double fee = TradeFee(a, b) / 2.0;
    for (ClientSession* who : sides)
        if (std::floor(who->account.GetMoney()) < (double)who->trade.offer.credits + fee)
            return who->accountName + " cannot pay " + Cr(who->trade.offer.credits) + " and the " +
                   std::to_string((long long)std::ceil(fee)) + " cr fee";
    if (!commit)
        return std::string();

    // --- It is all true: write it. ---
    for (auto& kv : boxes)
    {
        Box& box = kv.second;
        *box.c.goods = box.inv;
        if (!box.c.takeLog.empty())
            corps_.Touch();
    }
    const SystemState* sys = st;
    const Entity*      at = DockEntity(a);
    const FeeTo feeTo = at != nullptr && at->Has(Component::Market) ? FeeAt(*sys, *at) : FeeTo{};
    struct Said
    {
        std::vector<Exchange::GoodLine> goods;
        long long                       credits = 0;
        std::vector<std::string>        charts;
    } said[2];
    for (int i = 0; i < 2; ++i)
    {
        ClientSession& give = *sides[i];
        ClientSession& get = *sides[1 - i];
        give.account.AddMoney(-(double)give.trade.offer.credits);
        get.account.AddMoney((double)give.trade.offer.credits);
        said[i].goods = give.trade.offer.goods;
        said[i].credits = give.trade.offer.credits;
        for (const std::string& sysId : give.trade.offer.charts)
        {
            GrantChart(give, get, sysId);
            said[i].charts.push_back("the chart of " + SystemName(sysId));
        }
    }
    if (fee > 0.0)
    {
        a.account.AddMoney(-fee);
        b.account.AddMoney(-fee);
        PayFee(feeTo, fee * 2.0);
    }
    const std::string feeText =
        fee > 0.0 ? " Fee " + std::to_string((long long)std::ceil(fee)) + " cr each." : "";
    a.RecordEvent(Ev::Kind::Notice,
                  "Trade with " + b.accountName + " done: you gave " +
                      Say(said[0].goods, said[0].credits, said[0].charts) + "; you received " +
                      Say(said[1].goods, said[1].credits, said[1].charts) + "." + feeText);
    b.RecordEvent(Ev::Kind::Notice,
                  "Trade with " + a.accountName + " done: you gave " +
                      Say(said[1].goods, said[1].credits, said[1].charts) + "; you received " +
                      Say(said[0].goods, said[0].credits, said[0].charts) + "." + feeText);
    TradeLog(a, b, Say(said[0].goods, said[0].credits, said[0].charts),
             Say(said[1].goods, said[1].credits, said[1].charts));
    // It stays open, emptied: the pilots may trade again. The revision moves on, so a confirm
    // of what was just settled cannot be played again.
    for (ClientSession* who : sides)
    {
        who->trade.offer = Exchange::Bundle{};
        who->trade.confirmed = false;
        who->trade.note.clear();
        who->trade.version++;
    }
    return std::string();
}

void Simulation::TradeLog(const ClientSession& a, const ClientSession& b, const std::string& gave,
                          const std::string& got)
{
    // The ledger an admin reads (documents/player_exchange.md section 4): who gave what to
    // whom, where and when. The host prints it; a test reads it.
    char line[512];
    std::snprintf(line, sizeof line, "trade: %s gave %.200s to %s for %.200s at %s (t=%.0f)",
                  a.accountName.c_str(), gave.c_str(), b.accountName.c_str(), got.c_str(),
                  SystemName(a.systemId).c_str(), time_);
    tradeLog_.emplace_back(line);
    if (tradeLog_.size() > 200)
        tradeLog_.erase(tradeLog_.begin());
}

void Simulation::GrantChart(ClientSession& from, ClientSession& to, const std::string& systemId)
{
    const ChartGift g = ChartGiftFor(from, to, systemId);
    // The seller keeps theirs; it is only counted, so the next buyer is told how common it is.
    from.chartSales[systemId]++;
    if (!OnPublicCharts(systemId))
    {
        // Written the way a visit would write it -- never ChartBy: no first contact is made by a
        // sale -- and marked as a copy unless the buyer already had it from their own eyes.
        if (to.known.insert(ChartKey(systemId)).second)
        {
            ClientSession::Provenance how;
            how.from = from.accountName;
            how.at = time_;
            how.hops = g.hops + 1;
            to.secondHand[systemId] = std::move(how);
        }
        corps_.AddCharts(to.accountName, { systemId });
    }
    for (const std::string& key : g.finds)
        to.known.insert(key);
    // The newer look wins; what the buyer saw more recently is never overwritten.
    if (g.fresher)
    {
        const auto look = from.sightings.find(systemId);
        if (look != from.sightings.end())
        {
            to.sightings[systemId] = look->second;
            to.sightings[systemId].outdated = false;
        }
    }
    TouchKnown(to);
}

void Simulation::ApplyDeal(ClientSession& s, const Proto::Act::Deal& a)
{
    auto refuse = [&](const std::string& why) { s.RecordEvent(Ev::Kind::Notice, "Trade: " + why); };
    // Whatever changes an offer un-confirms both sides and moves the revision.
    auto changed = [&](ClientSession& p)
    {
        s.trade.version++;
        s.trade.confirmed = false;
        s.trade.note.clear();
        p.trade.confirmed = false;
        p.trade.note.clear();
    };
    auto livePartner = [&]() -> ClientSession*
    {
        ClientSession* p = s.trade.with != 0 ? Session(s.trade.with) : nullptr;
        return p != nullptr && p->trade.with == s.id && SameDock(s, *p) ? p : nullptr;
    };

    if (a.action == "cancel")
    {
        if (s.trade.with == 0)
            return;
        CancelTrade(s, "cancelled by " + s.accountName);
        return;
    }
    if (a.action == "open")
    {
        if (!s.IsDocked())
            return refuse("dock at a station first: a trade is between two pilots side by side");
        ClientSession* p = nullptr;
        for (auto& kv : sessions_)
            if (kv.second.id != s.id && SameName(kv.second.accountName, a.with))
                p = &kv.second;
        if (p == nullptr)
            return refuse(a.with.empty()
                              ? "name the pilot to trade with"
                              : a.with + " is not here: no pilot of that name is online");
        if (!SameDock(s, *p))
            return refuse(p->accountName + " is not docked at this station");
        if (s.trade.with == p->id)
            return;  // asked already
        if (p->trade.with != 0 && p->trade.with != s.id && livePartner() == nullptr)
        {
            ClientSession* other = Session(p->trade.with);
            if (other != nullptr && other->trade.with == p->id)
                return refuse(p->accountName + " is already trading with " + other->accountName);
        }
        if (s.trade.with != 0)
            CancelTrade(s, "");
        s.trade = ClientSession::Trade{ p->id };
        s.trade.version = 1;
        if (p->trade.with == s.id)  // they asked first: the trade is open
        {
            p->trade.version = 1;
            p->trade.confirmed = false;
            s.RecordEvent(Ev::Kind::Notice, "Trade with " + p->accountName + " open");
            p->RecordEvent(Ev::Kind::Notice, "Trade with " + s.accountName + " open");
        }
        else
        {
            s.RecordEvent(Ev::Kind::Notice, "Asked " + p->accountName +
                                                " to trade; the trade opens when they accept");
            p->RecordEvent(Ev::Kind::Notice, s.accountName + " asks to trade: open a trade with " +
                                                 s.accountName + " to accept");
        }
        return;
    }

    ClientSession* p = livePartner();
    if (p == nullptr)
        return refuse("no trade is open: open one with a pilot docked at this station");

    if (a.action == "put")
    {
        Exchange::Bundle next = s.trade.offer;
        for (const Exchange::GoodLine& g : a.bundle.goods)
        {
            if (g.type < 0 || g.type >= Goods::Count() || g.amount <= 0)
                return refuse("no such good");
            const std::string id = g.from.empty() ? std::string(HOLD) : g.from;
            if (id.rfind("can:", 0) == 0 || id == "can")
                return refuse("goods in space are not part of a trade");
            Container   c;
            std::string why;
            if (!ContainerOf(s, id, c, why))
                return refuse(why);
            if (!c.gives)
                return refuse("your role may not take from " + c.name);
            const ResourceType type = ResourceType::FromIndex(g.type);
            int                offered = 0;
            for (const Exchange::GoodLine& o : next.goods)
                if (o.type == g.type && Place(o) == id)
                    offered += o.amount;
            if (c.goods->Amount(type) < offered + g.amount)
                return refuse("you have only " + std::to_string(c.goods->Amount(type)) + " " +
                              ResourceName(type) + " in " + c.name +
                              (offered > 0 ? ", " + std::to_string(offered) + " already offered"
                                           : std::string()));
            if (!Merge(next, { g.type, g.amount, id }))
                return refuse("a trade holds at most " + std::to_string(Exchange::MAX_LINES) +
                              " kinds of goods");
        }
        if (a.bundle.credits > 0)
        {
            next.credits += a.bundle.credits;
            if ((double)next.credits > std::floor(s.account.GetMoney()))
                return refuse("you have only " + Cr((long long)std::floor(s.account.GetMoney())));
        }
        for (const std::string& which : a.bundle.charts)
        {
            std::string id = which.rfind("system:", 0) == 0 ? which.substr(7) : which;
            if (!HasSystem(id))
                id = SystemNamed(which);
            if (id.empty() || s.known.count(ChartKey(id)) == 0)
                return refuse("the chart of " + which +
                              " is not on your own charts: public space has nothing to sell");
            if (std::find(next.charts.begin(), next.charts.end(), id) != next.charts.end())
                continue;
            if (next.charts.size() >= Exchange::MAX_LINES)
                return refuse("a trade holds at most " + std::to_string(Exchange::MAX_LINES) +
                              " charts");
            next.charts.push_back(id);
        }
        s.trade.offer = std::move(next);
        changed(*p);
        return;
    }
    if (a.action == "withdraw")
    {
        Exchange::Bundle& o = s.trade.offer;
        for (const Exchange::GoodLine& g : a.bundle.goods)
        {
            const std::string id = g.from.empty() ? std::string(HOLD) : g.from;
            int               left = g.amount;
            for (auto it = o.goods.begin(); it != o.goods.end() && left > 0;)
            {
                if (it->type == g.type && Place(*it) == id)
                {
                    const int n = std::min(left, it->amount);
                    it->amount -= n;
                    left -= n;
                }
                it = it->amount <= 0 ? o.goods.erase(it) : std::next(it);
            }
        }
        o.credits = std::max(0LL, o.credits - a.bundle.credits);
        for (const std::string& which : a.bundle.charts)
        {
            std::string id = which.rfind("system:", 0) == 0 ? which.substr(7) : which;
            if (!HasSystem(id))
                id = SystemNamed(which);
            o.charts.erase(std::remove(o.charts.begin(), o.charts.end(), id), o.charts.end());
        }
        changed(*p);
        return;
    }
    if (a.action == "confirm")
    {
        const int rev = s.trade.version + p->trade.version;
        if (a.rev != rev)
        {
            s.trade.note = "the offer changed: look at it again, then confirm";
            return refuse(s.trade.note);
        }
        const std::string why = SettleTrade(s, *p, false);
        if (!why.empty())
        {
            s.trade.note = why;
            return refuse(why);
        }
        s.trade.confirmed = true;
        s.trade.note.clear();
        if (!p->trade.confirmed)
        {
            p->RecordEvent(Ev::Kind::Notice,
                           s.accountName + " confirmed the trade: confirm to complete it");
            return;
        }
        const std::string failed = SettleTrade(s, *p, true);
        if (!failed.empty())
        {
            s.trade.confirmed = p->trade.confirmed = false;
            s.trade.version++;
            s.trade.note = p->trade.note = failed;
            refuse(failed);
            p->RecordEvent(Ev::Kind::Notice, "Trade: " + failed);
        }
        return;
    }
    refuse("unknown action '" + a.action + "'");
}
