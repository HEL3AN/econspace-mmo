#include "sim/Corporation.h"

#include "core/Faction.h"
#include "sim/Names.h"
#include "sim/SaveFile.h"
#include "sim/WorldRules.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <set>

using nlohmann::json;

namespace Corps
{

namespace
{
std::string Lower(std::string s)
{
    for (char& c : s)
        c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string Upper(std::string s)
{
    for (char& c : s)
        c = (char)std::toupper((unsigned char)c);
    return s;
}

Outcome Refuse(std::string why)
{
    Outcome o;
    o.error = std::move(why);
    o.told = o.error;
    return o;
}

std::string Tag(const Corp& c)
{
    return c.name + " [" + c.ticker + "]";
}
}  // namespace

const char* RoleName(Role r)
{
    switch (r)
    {
        case Role::Member: return "member";
        case Role::Director: return "director";
        case Role::Ceo: return "ceo";
    }
    return "member";
}

const char* RoleLabel(Role r)
{
    switch (r)
    {
        case Role::Member: return "Member";
        case Role::Director: return "Director";
        case Role::Ceo: return "CEO";
    }
    return "Member";
}

bool RoleFrom(const std::string& name, Role& out)
{
    for (Role r : { Role::Member, Role::Director, Role::Ceo })
        if (Lower(name) == RoleName(r))
        {
            out = r;
            return true;
        }
    return false;
}

const char* DockingName(Docking d)
{
    switch (d)
    {
        case Docking::Own: return "own";
        case Docking::Allies: return "allies";
        case Docking::All: return "all";
    }
    return "all";
}

bool DockingFrom(const std::string& name, Docking& out)
{
    for (Docking d : { Docking::Own, Docking::Allies, Docking::All })
        if (Lower(name) == DockingName(d))
        {
            out = d;
            return true;
        }
    return false;
}

double Corp::TaxRate(double standard, double cap) const
{
    return std::clamp(tax < 0.0 ? standard : tax, 0.0, std::max(0.0, cap));
}

const Member* Corp::Find(const std::string& account) const
{
    for (const Member& m : members)
        if (m.account == account)
            return &m;
    return nullptr;
}

const Member* Corp::Ceo() const
{
    for (const Member& m : members)
        if (m.role == Role::Ceo)
            return &m;
    return nullptr;
}

bool ValidName(const std::string& name, std::string& why)
{
    if (!Names::ValidSystemName(name, why))
        return false;
    // Nobody passes as the Traders' Guild: a faction's name or id is not a corporation's.
    for (FactionId f : Factions::All())
        if (Names::SameName(name, FactionName(f)) || Names::SameName(name, Factions::Id(f)))
        {
            why = "that is a faction's name";
            return false;
        }
    return true;
}

bool ValidTicker(const std::string& ticker, std::string& why)
{
    if (ticker.size() < 2 || ticker.size() > 5)
    {
        why = "a ticker is 2 to 5 characters";
        return false;
    }
    for (char c : ticker)
        if (!std::isalnum((unsigned char)c) || (unsigned char)c > 127)
        {
            why = "a ticker is letters and digits only";
            return false;
        }
    return true;
}

Corp* Registry::Mutable(const std::string& id)
{
    const auto it = corps_.find(id);
    return it == corps_.end() ? nullptr : &it->second;
}

void Registry::Index()
{
    byAccount_.clear();
    for (const auto& kv : corps_)
        for (const Member& m : kv.second.members)
            byAccount_[m.account] = kv.first;
}

void Registry::SortMembers(Corp& c)
{
    std::sort(c.members.begin(), c.members.end(),
              [](const Member& a, const Member& b)
              {
                  if ((a.role == Role::Ceo) != (b.role == Role::Ceo))
                      return a.role == Role::Ceo;
                  return a.account < b.account;
              });
}

const Corp* Registry::Of(const std::string& account) const
{
    const std::string id = IdOf(account);
    return id.empty() ? nullptr : Find(id);
}

const Corp* Registry::Find(const std::string& id) const
{
    const auto it = corps_.find(id);
    return it == corps_.end() ? nullptr : &it->second;
}

const Corp* Registry::FindTicker(const std::string& ticker) const
{
    const std::string t = Upper(ticker);
    for (const auto& kv : corps_)
        if (kv.second.ticker == t)
            return &kv.second;
    return nullptr;
}

bool Registry::SameCorp(const std::string& a, const std::string& b) const
{
    if (a.empty() || b.empty())
        return false;
    const std::string ia = IdOf(a);
    return !ia.empty() && ia == IdOf(b);
}

std::string Registry::TickerOf(const std::string& account) const
{
    const Corp* c = Of(account);
    return c != nullptr ? c->ticker : std::string();
}

std::string Registry::IdOf(const std::string& account) const
{
    std::string id;
    if (CorpOwner(account, id))
        return corps_.count(id) != 0 ? id : std::string();  // gone: abandoned (#468)
    const auto it = byAccount_.find(account);
    return it == byAccount_.end() ? std::string() : it->second;
}

bool Registry::Manages(const std::string& account, const std::string& owner) const
{
    if (account.empty() || owner.empty())
        return false;
    if (account == owner)
        return true;
    if (!SameCorp(account, owner))
        return false;
    const Member* m = Of(account)->Find(account);
    return m != nullptr && m->role != Role::Member;
}

bool Registry::Commands(const std::string& account, const std::string& owner) const
{
    if (account.empty() || owner.empty())
        return false;
    std::string id;
    if (!CorpOwner(owner, id))
        return account == owner;
    const Corp*   c = Find(id);
    const Member* m = c != nullptr ? c->Find(account) : nullptr;
    return m != nullptr && m->role != Role::Member;
}

std::vector<const Corp*> Registry::InvitesFor(const std::string& account, double now) const
{
    std::vector<const Corp*> out;
    for (const auto& kv : corps_)
        for (const Invite& i : kv.second.invites)
            if (i.account == account && (double)i.at + INVITE_SECONDS > now)
                out.push_back(&kv.second);
    return out;
}

int Registry::Record(const std::string& account, Ev::Kind kind, const std::string& text)
{
    const std::string id = IdOf(account);
    Corp*             c = id.empty() ? nullptr : Mutable(id);
    if (c == nullptr)
        return 0;
    Ev::Event e;
    e.seq = c->nextJournalSeq++;
    e.kind = kind;
    e.text = text;
    c->journal.push_back(e);
    if (c->journal.size() > JOURNAL_CAP)
        c->journal.erase(c->journal.begin());
    rev_++;  // corps.json keeps it
    return e.seq;
}

void Registry::Expire(double now)
{
    for (auto& kv : corps_)
    {
        std::vector<Invite>& v = kv.second.invites;
        const size_t         before = v.size();
        v.erase(std::remove_if(v.begin(), v.end(), [&](const Invite& i)
                               { return (double)i.at + INVITE_SECONDS <= now; }),
                v.end());
        if (v.size() != before)
            rev_++;
    }
}

Outcome Registry::Apply(const std::string& account, const Act& act,
                        const std::set<std::string>& online, double now)
{
    if (account.empty())
        return Refuse("only an account can be in a corporation");
    const std::string verb = Lower(act.verb);
    if (verb == "create")
        return Create(account, act.name, act.ticker, now);
    if (verb == "invite")
        return InviteTo(account, act.pilot, online, now);
    if (verb == "join")
        return Join(account, act.ticker, now);
    if (verb == "decline")
        return Decline(account, act.ticker);
    if (verb == "leave")
        return Leave(account);
    if (verb == "kick")
        return Kick(account, act.pilot);
    if (verb == "role")
    {
        Role r;
        if (!RoleFrom(act.role, r))
            return Refuse("a role is ceo, director or member");
        return SetRole(account, act.pilot, r);
    }
    if (verb == "tax" || verb == "docking")
        return SetPolicy(account, verb, act.value, WorldRules::InfluenceRules().taxMax);
    if (verb == "police")  // whether it holds the system is the world's to say
        return Refuse("a police budget is set through the world, not the registry alone");
    if (verb == "patrol")  // ...and where it may keep a patrol, and what the design is
        return Refuse("a patrol is set through the world, not the registry alone");
    if (verb == "transfer" || verb == "take")  // ...and whose a structure is (#468)
        return Refuse("a structure is handed over through the world, not the registry alone");
    return Refuse("there is no corporation verb '" + act.verb.substr(0, 16) + "'");
}

Outcome Registry::Create(const std::string& account, const std::string& name,
                         const std::string& ticker, double now)
{
    if (const Corp* c = Of(account))
        return Refuse("you are in " + Tag(*c) + " already; leave it first");
    std::string why;
    if (!ValidName(name, why))
        return Refuse(why);
    if (!ValidTicker(ticker, why))
        return Refuse(why);
    for (const auto& kv : corps_)
    {
        if (Names::SameName(kv.second.name, name))
            return Refuse("there is a corporation called " + kv.second.name + " already");
        if (kv.second.ticker == Upper(ticker))
            return Refuse("the ticker " + kv.second.ticker + " is taken");
    }
    Corp c;
    c.id = "c" + std::to_string(nextId_++);
    c.name = name;
    c.ticker = Upper(ticker);
    c.founded = (long long)now;
    c.members.push_back({ account, Role::Ceo, (long long)now });
    const std::string id = c.id;
    corps_[id] = std::move(c);
    // An invite elsewhere is moot once a pilot is in a corporation of their own.
    for (auto& kv : corps_)
    {
        std::vector<Invite>& v = kv.second.invites;
        v.erase(std::remove_if(v.begin(), v.end(),
                               [&](const Invite& i) { return i.account == account; }),
                v.end());
    }
    Index();
    rev_++;
    Outcome o;
    o.corp = id;
    o.told = "You founded " + Tag(corps_[id]) + ", and you are its CEO";
    o.announce = account + " founded the corporation";
    return o;
}

Outcome Registry::InviteTo(const std::string& by, const std::string& pilot,
                           const std::set<std::string>& online, double now)
{
    const Corp* in = Of(by);
    if (in == nullptr)
        return Refuse("you are in no corporation");
    if (in->Find(by)->role == Role::Member)
        return Refuse("only the CEO or a director invites");
    const std::string who = Lower(pilot);
    if (who.empty())
        return Refuse("invite whom?");
    if (who == by)
        return Refuse("you are a member already");
    if (online.count(who) == 0)
        return Refuse("nobody called " + pilot.substr(0, 32) + " is online");
    if (const Corp* theirs = Of(who))
        return Refuse(who + " is in " + Tag(*theirs) +
                      (theirs == in ? "" : " and has to leave it"));
    Corp& c = *Mutable(in->id);
    if ((int)c.members.size() >= MEMBERS_MAX)
        return Refuse("a corporation holds at most " + std::to_string(MEMBERS_MAX) + " pilots");
    auto it = std::find_if(c.invites.begin(), c.invites.end(),
                           [&](const Invite& i) { return i.account == who; });
    if (it == c.invites.end())
    {
        if ((int)c.invites.size() >= INVITES_MAX)
            return Refuse("too many invites are waiting for an answer");
        c.invites.push_back({ who, by, (long long)now });
    }
    else
        *it = { who, by, (long long)now };  // asked again: the week starts again
    rev_++;
    Outcome o;
    o.corp = c.id;
    o.told = "Invited " + who + " to " + Tag(c);
    o.announce = by + " invited " + who;
    o.subject = who;
    o.subjectText = by + " invites you to " + Tag(c) +
                    ". Open the Corporation window to join, or /corp join " + c.ticker;
    return o;
}

Outcome Registry::Join(const std::string& account, const std::string& ticker, double now)
{
    if (const Corp* c = Of(account))
        return Refuse("you are in " + Tag(*c) + " already; leave it first");
    const Corp* found = FindTicker(ticker);
    if (found == nullptr)
        return Refuse("there is no corporation [" + Upper(ticker).substr(0, 8) + "]");
    Corp&      c = *Mutable(found->id);
    const auto it = std::find_if(c.invites.begin(), c.invites.end(),
                                 [&](const Invite& i) { return i.account == account; });
    if (it == c.invites.end() || (double)it->at + INVITE_SECONDS <= now)
        return Refuse(Tag(c) + " has not invited you");
    if ((int)c.members.size() >= MEMBERS_MAX)
        return Refuse(Tag(c) + " is full");
    c.invites.erase(it);
    c.members.push_back({ account, Role::Member, (long long)now });
    SortMembers(c);
    for (auto& kv : corps_)
    {
        std::vector<Invite>& v = kv.second.invites;
        v.erase(std::remove_if(v.begin(), v.end(),
                               [&](const Invite& i) { return i.account == account; }),
                v.end());
    }
    Index();
    rev_++;
    Outcome o;
    o.corp = c.id;
    o.told = "You joined " + Tag(c);
    o.announce = account + " joined the corporation";
    return o;
}

Outcome Registry::Decline(const std::string& account, const std::string& ticker)
{
    const Corp* found = FindTicker(ticker);
    if (found == nullptr)
        return Refuse("there is no corporation [" + Upper(ticker).substr(0, 8) + "]");
    Corp&      c = *Mutable(found->id);
    const auto it = std::find_if(c.invites.begin(), c.invites.end(),
                                 [&](const Invite& i) { return i.account == account; });
    if (it == c.invites.end())
        return Refuse(Tag(c) + " has not invited you");
    c.invites.erase(it);
    rev_++;
    Outcome o;
    o.corp = c.id;
    o.told = "You declined " + Tag(c) + "'s invite";
    o.announce = account + " declined the invite";
    return o;
}

Outcome Registry::Leave(const std::string& account)
{
    const Corp* in = Of(account);
    if (in == nullptr)
        return Refuse("you are in no corporation");
    Corp&             c = *Mutable(in->id);
    const std::string id = c.id;
    Outcome           o;
    o.corp = id;
    if (c.members.size() == 1)
    {
        // The last one out closes it: its name and ticker are free again.
        o.told = "You left " + Tag(c) + ", and with nobody left in it, it is disbanded";
        corps_.erase(id);
        Index();
        rev_++;
        return o;
    }
    if (c.Find(account)->role == Role::Ceo)
        return Refuse("the CEO hands the corporation over before leaving: make another member "
                      "CEO first");
    c.members.erase(std::remove_if(c.members.begin(), c.members.end(),
                                   [&](const Member& m) { return m.account == account; }),
                    c.members.end());
    Index();
    rev_++;
    o.told = "You left " + Tag(c);
    o.announce = account + " left the corporation";
    return o;
}

Outcome Registry::Kick(const std::string& by, const std::string& pilot)
{
    const Corp* in = Of(by);
    if (in == nullptr)
        return Refuse("you are in no corporation");
    const std::string who = Lower(pilot);
    const Member*     me = in->Find(by);
    const Member*     them = in->Find(who);
    if (them == nullptr)
        return Refuse(pilot.substr(0, 32) + " is not in " + Tag(*in));
    if (who == by)
        return Refuse("to go, leave");
    // The CEO removes anyone; a director, only members -- not another director, nor the CEO.
    if (me->role == Role::Member || (me->role == Role::Director && them->role != Role::Member))
        return Refuse(me->role == Role::Member ? "only the CEO or a director removes pilots"
                                               : "a director removes members only");
    Corp& c = *Mutable(in->id);
    c.members.erase(std::remove_if(c.members.begin(), c.members.end(),
                                   [&](const Member& m) { return m.account == who; }),
                    c.members.end());
    Index();
    rev_++;
    Outcome o;
    o.corp = c.id;
    o.told = "Removed " + who + " from " + Tag(c);
    o.announce = by + " removed " + who + " from the corporation";
    o.subject = who;  // not on the channel any more, so told on their own
    o.subjectText = by + " removed you from " + Tag(c);
    return o;
}

Outcome Registry::SetRole(const std::string& by, const std::string& pilot, Role role)
{
    const Corp* in = Of(by);
    if (in == nullptr)
        return Refuse("you are in no corporation");
    if (in->Find(by)->role != Role::Ceo)
        return Refuse("only the CEO gives roles");
    const std::string who = Lower(pilot);
    if (in->Find(who) == nullptr)
        return Refuse(pilot.substr(0, 32) + " is not in " + Tag(*in));
    if (who == by)
        return Refuse(role == Role::Ceo ? "you are the CEO already"
                                        : "make another member CEO, and you are a director");
    Corp& c = *Mutable(in->id);
    for (Member& m : c.members)
        if (m.account == who)
            m.role = role;
        else if (role == Role::Ceo && m.account == by)
            m.role = Role::Director;  // one CEO: handing it over makes the old one a director
    SortMembers(c);
    rev_++;
    Outcome o;
    o.corp = c.id;
    o.told =
        who + " is " + (role == Role::Ceo ? "the CEO" : RoleLabel(role)) + " of " + Tag(c) + " now";
    o.announce = role == Role::Ceo ? by + " handed the corporation to " + who + ", its CEO now"
                                   : by + " made " + who + " a " + Lower(RoleLabel(role));
    return o;
}

Outcome Registry::SetPolicy(const std::string& by, const std::string& verb,
                            const std::string& value, double taxCap)
{
    const Corp* in = Of(by);
    if (in == nullptr)
        return Refuse("you are in no corporation");
    if (in->Find(by)->role == Role::Member)
        return Refuse("only the CEO or a director sets the corporation's policies");
    Corp&       c = *Mutable(in->id);
    std::string now;
    if (Lower(verb) == "tax")
    {
        std::string v = value;
        while (!v.empty() && (v.back() == '%' || v.back() == ' '))
            v.pop_back();
        if (Lower(v) == "standard" || Lower(v) == "default")
        {
            c.tax = -1.0;
            now = "the standard fee";
        }
        else
        {
            char*        end = nullptr;
            const double pct = v.empty() ? -1.0 : std::strtod(v.c_str(), &end);
            if (v.empty() || end == nullptr || *end != '\0' || !(pct >= 0.0))
                return Refuse("a tax is a percentage of a trade, such as 5, or 'standard'");
            if (pct / 100.0 > taxCap + 1e-9)
            {
                char buf[96];
                std::snprintf(buf, sizeof(buf), "this server allows a tax of at most %.1f%%",
                              taxCap * 100.0);
                return Refuse(buf);
            }
            c.tax = pct / 100.0;
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.1f%% of every trade", pct);
            now = buf;
        }
        now = "The tax at markets in systems " + Tag(c) + " holds is " + now;
    }
    else
    {
        Docking d;
        if (!DockingFrom(value, d))
            return Refuse("docking is own, allies or all");
        c.docking = d;
        now = Tag(c) + "'s structures in the systems it holds dock " +
              (d == Docking::Own      ? std::string("its members only")
               : d == Docking::Allies ? std::string("its members and whoever each one's rules of "
                                                    "engagement hold fire for")
                                      : std::string("anyone"));
    }
    rev_++;
    Outcome o;
    o.corp = c.id;
    o.told = now;
    o.announce = by + ": " + now;
    return o;
}

Outcome Registry::SetPolice(const std::string& by, const std::string& system,
                            const std::string& where, const std::string& value, double cap,
                            bool held)
{
    const Corp* in = Of(by);
    if (in == nullptr)
        return Refuse("you are in no corporation");
    if (in->Find(by)->role == Role::Member)
        return Refuse("only the CEO or a director sets the corporation's policies");
    if (system.empty())
        return Refuse("name a system the corporation holds");
    std::string v = value;
    while (!v.empty() && v.back() == ' ')
        v.pop_back();
    double perHour = 0.0;
    if (Lower(v) != "none" && Lower(v) != "off")
    {
        char* end = nullptr;
        perHour = v.empty() ? -1.0 : std::strtod(v.c_str(), &end);
        if (v.empty() || end == nullptr || *end != '\0' || !(perHour >= 0.0))
            return Refuse("a police budget is credits an hour, such as 1200, or none");
    }
    // Stopping is always allowed -- the system may have been lost since -- but paying only
    // where the corporation holds it: police serve the holder's law.
    if (perHour > 0.0 && !held)
        return Refuse(Tag(*in) + " does not hold " + where);
    if (perHour > cap + 1e-9)
    {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "this server allows at most %.0f cr an hour for police",
                      cap);
        return Refuse(buf);
    }
    Corp& c = *Mutable(in->id);
    if (perHour > 0.0)
        c.police[system] = perHour;
    else
        c.police.erase(system);
    rev_++;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.0f cr an hour", perHour);
    Outcome o;
    o.corp = c.id;
    o.told = perHour > 0.0 ? Tag(c) + " pays " + buf + " for police in " + where
                           : Tag(c) + " pays for no police in " + where;
    o.announce = by + ": " + o.told;
    return o;
}

Outcome Registry::SetPatrol(const std::string& by, const std::string& system,
                            const std::string& where, const std::string& value,
                            const std::string& design, const std::string& designName,
                            const std::string& engage, const std::string& threshold,
                            const std::string& allow, int systemMax, int corpMax, bool allowed)
{
    const Corp* in = Of(by);
    if (in == nullptr)
        return Refuse("you are in no corporation");
    if (in->Find(by)->role == Role::Member)
        return Refuse("only the CEO or a director sets the corporation's patrols");
    if (system.empty())
        return Refuse("name a system the corporation holds or has a structure in");
    std::string v = value;
    while (!v.empty() && v.back() == ' ')
        v.pop_back();
    long ships = 0;
    if (Lower(v) != "none" && Lower(v) != "off")
    {
        char* end = nullptr;
        ships = v.empty() ? -1 : std::strtol(v.c_str(), &end, 10);
        if (v.empty() || end == nullptr || *end != '\0' || ships < 0)
            return Refuse("a patrol is a number of ships, such as 3, or none");
    }
    const auto was = in->patrols.find(system);
    Defence    rules = was != in->patrols.end() ? was->second.rules : Patrol::DefaultRules();
    if (!engage.empty() && !EngageFrom(Lower(engage), rules.engage))
        return Refuse("a patrol engages as hold, return, enemies or strangers");
    // The rest of the rules a structure keeps (#340): the standing below which a faction is
    // an enemy, and whom a patrol on strangers lets be.
    if (!threshold.empty())
    {
        char*       end = nullptr;
        const float th = std::strtof(threshold.c_str(), &end);
        if (end == nullptr || *end != '\0' || !(th >= Defence::THRESHOLD_MIN) ||
            !(th <= Defence::THRESHOLD_MAX))
            return Refuse("a patrol's threshold is a standing from -100 to 100, such as -10");
        rules.threshold = th;
    }
    if (!allow.empty())
        ReadAllowList(Lower(allow) == "none" ? std::string() : allow, rules);
    const std::string problem = DefenceProblem(rules);
    if (!problem.empty())
        return Refuse(problem);
    // Standing one down is always allowed -- the system may have been lost since -- but
    // keeping one only where the corporation has something to guard.
    if (ships > 0 && !allowed)
        return Refuse(Tag(*in) + " neither holds " + where + " nor has a structure there");
    if (ships > systemMax)
        return Refuse("this server allows at most " + std::to_string(systemMax) +
                      " patrol ships in a system");
    long elsewhere = 0;
    for (const auto& p : in->patrols)
        if (p.first != system)
            elsewhere += p.second.ships;
    if (ships > 0 && elsewhere + ships > corpMax)
        return Refuse("this server allows a corporation at most " + std::to_string(corpMax) +
                      " patrol ships, and " + Tag(*in) + " keeps " + std::to_string(elsewhere) +
                      " elsewhere");
    Corp&   c = *Mutable(in->id);
    Outcome o;
    o.corp = c.id;
    if (ships > 0)
    {
        Patrol& p = c.patrols[system];
        p.ships = (int)ships;
        p.design = design;
        p.rules = rules;
        const std::string flies = designName.empty() ? std::string() : " of " + designName;
        o.told = Tag(c) + " keeps " + std::to_string(ships) + " patrol ship" +
                 (ships == 1 ? "" : "s") + flies + " in " + where + ", engaging " +
                 EngageLabel(rules.engage);
        if (rules.engage == Engage::Enemies)
            o.told += " (below " + std::to_string((int)std::lround(rules.threshold)) + ")";
        const std::string allows = AllowListText(rules);
        if (!allows.empty())
            o.told += ", letting be " + allows;
    }
    else
    {
        c.patrols.erase(system);
        o.told = Tag(c) + " stands its patrol in " + where + " down";
    }
    rev_++;
    o.announce = by + ": " + o.told;
    return o;
}

bool Registry::Credit(const std::string& id, double amount)
{
    Corp* c = Mutable(id);
    if (c == nullptr || !(amount > 0.0))
        return c != nullptr;
    c->treasury += amount;
    moneyRev_++;
    return true;
}

bool Registry::Debit(const std::string& id, double amount)
{
    Corp* c = Mutable(id);
    if (c == nullptr || !(amount >= 0.0) || c->treasury < amount)
        return false;
    c->treasury -= amount;
    moneyRev_++;
    return true;
}

void Registry::SetLicence(const std::string& id, const std::string& system, const Licence& l)
{
    if (Corp* c = Mutable(id))
    {
        c->licences[system] = l;
        moneyRev_++;
    }
}

void Registry::DropLicence(const std::string& id, const std::string& system)
{
    Corp* c = Mutable(id);
    if (c != nullptr && c->licences.erase(system) > 0)
        moneyRev_++;
}

json Registry::ToJson() const
{
    json list = json::array();
    for (const auto& kv : corps_)
    {
        const Corp& c = kv.second;
        json        members = json::array(), invites = json::array();
        for (const Member& m : c.members)
            members.push_back(
                { { "account", m.account }, { "role", RoleName(m.role) }, { "joined", m.joined } });
        for (const Invite& i : c.invites)
            invites.push_back({ { "account", i.account }, { "by", i.by }, { "at", i.at } });
        json cj = { { "id", c.id },
                    { "name", c.name },
                    { "ticker", c.ticker },
                    { "founded", c.founded },
                    { "members", members } };
        if (!c.invites.empty())
            cj["invites"] = invites;
        // An addition an older build ignores (#431 C2): no CORPS_VERSION bump.
        if (!c.journal.empty())
        {
            json journal = json::array();
            for (const Ev::Event& e : c.journal)
                journal.push_back(
                    { { "seq", e.seq }, { "kind", Ev::KindName(e.kind) }, { "text", e.text } });
            cj["journal"] = journal;
            cj["nextJournalSeq"] = c.nextJournalSeq;
        }
        // Its charts (#431). A field an older build ignores, so no version bump (#20).
        if (!c.charts.empty())
            cj["charts"] = c.charts;
        // ...and what it saw there (#431 slice 14), ignored by an older build too.
        if (!c.sightings.empty())
        {
            json seen = json::object();
            for (const auto& [system, sighting] : c.sightings)
                seen[system] = WorldLoader::ToJson(sighting);
            cj["observed"] = seen;
        }
        // Additions an older build ignores too (#431 slice 5): its money and its policies.
        if (c.treasury != 0.0)
            cj["treasury"] = c.treasury;
        if (c.tax >= 0.0)
            cj["tax"] = c.tax;
        if (c.docking != Docking::All)
            cj["docking"] = DockingName(c.docking);
        // Its licences (#431 slice 6): an addition an older build ignores.
        if (!c.licences.empty())
        {
            json licences = json::object();
            for (const auto& l : c.licences)
                licences[l.first] = { { "holder", l.second.holder },
                                      { "until", l.second.until },
                                      { "told", l.second.told } };
            cj["licences"] = licences;
        }
        // Slice 11's addition, which an older build ignores: no CORPS_VERSION bump.
        if (!c.police.empty())
            cj["police"] = c.police;
        // ...and slice 12's patrols, the same.
        if (!c.patrols.empty())
        {
            json patrols = json::object();
            for (const auto& p : c.patrols)
            {
                json pj = { { "ships", p.second.ships } };
                if (!p.second.design.empty())
                    pj["design"] = p.second.design;
                pj["rules"] = DefenceJson(p.second.rules);
                patrols[p.first] = std::move(pj);
            }
            cj["patrols"] = std::move(patrols);
        }
        list.push_back(std::move(cj));
    }
    return { { "version", Save::CORPS_VERSION }, { "next", nextId_ }, { "corps", list } };
}

bool Registry::FromJson(const json& j, std::string& why)
{
    auto text = [](const json& o, const char* key) -> std::string
    {
        const auto it = o.find(key);
        return it != o.end() && it->is_string() ? it->get<std::string>() : std::string();
    };
    auto whole = [](const json& o, const char* key) -> long long
    {
        const auto it = o.find(key);
        return it != o.end() && it->is_number_integer() ? it->get<long long>() : 0;
    };
    if (!j.is_object())
    {
        why = "not an object";
        return false;
    }
    std::map<std::string, Corp> corps;
    std::set<std::string>       accounts, names, tickers;
    int                         next = (int)std::max(1LL, whole(j, "next"));
    const auto                  list = j.find("corps");
    if (list != j.end() && !list->is_array())
    {
        why = "corps is not a list";
        return false;
    }
    if (list != j.end())
        for (const json& cj : *list)
        {
            if (!cj.is_object())
            {
                why = "a corporation is not an object";
                return false;
            }
            Corp c;
            c.id = text(cj, "id");
            c.name = text(cj, "name");
            c.ticker = text(cj, "ticker");
            c.founded = whole(cj, "founded");
            if (c.id.size() < 2 || c.id[0] != 'c' || corps.count(c.id) != 0)
            {
                why = "a corporation's id '" + c.id + "' is missing or not unique";
                return false;
            }
            if (c.name.empty() || c.ticker.empty() || !names.insert(Lower(c.name)).second ||
                !tickers.insert(Upper(c.ticker)).second)
            {
                why = c.id + "'s name or ticker is missing or not unique";
                return false;
            }
            c.ticker = Upper(c.ticker);
            const auto members = cj.find("members");
            if (members != cj.end() && members->is_array())
                for (const json& mj : *members)
                {
                    Member m;
                    m.account = mj.is_object() ? text(mj, "account") : std::string();
                    if (m.account.empty() || !RoleFrom(text(mj, "role"), m.role) ||
                        !accounts.insert(m.account).second)
                    {
                        why = c.id + " has a member who is unnamed, has no role, or is in "
                                     "another corporation too";
                        return false;
                    }
                    m.joined = whole(mj, "joined");
                    c.members.push_back(m);
                }
            int ceos = 0;
            for (const Member& m : c.members)
                ceos += m.role == Role::Ceo ? 1 : 0;
            if (ceos != 1)
            {
                why = c.id + " has " + std::to_string(ceos) + " CEOs";
                return false;
            }
            const auto invites = cj.find("invites");
            if (invites != cj.end() && invites->is_array())
                for (const json& ij : *invites)
                    if (ij.is_object() && !text(ij, "account").empty())
                        c.invites.push_back(
                            { text(ij, "account"), text(ij, "by"), whole(ij, "at") });
            const auto journal = cj.find("journal");
            if (journal != cj.end() && journal->is_array())
                for (const json& ej : *journal)
                {
                    Ev::Event e;
                    if (!ej.is_object() || !Ev::KindFrom(text(ej, "kind"), e.kind))
                        continue;  // a kind a later build added: not this build's to tell
                    e.seq = (int)whole(ej, "seq");
                    e.text = text(ej, "text");
                    if (e.seq > 0)
                        c.journal.push_back(e);
                }
            c.nextJournalSeq = (int)std::max<long long>(1, whole(cj, "nextJournalSeq"));
            for (const Ev::Event& e : c.journal)
                c.nextJournalSeq = std::max(c.nextJournalSeq, e.seq + 1);
            const auto charts = cj.find("charts");
            if (charts != cj.end() && charts->is_array())
                for (const json& sj : *charts)
                    if (sj.is_string())
                        c.charts.insert(sj.get<std::string>());
            const auto observed = cj.find("observed");
            if (observed != cj.end() && observed->is_object())
                for (auto o = observed->begin(); o != observed->end(); ++o)
                {
                    WorldLoader::Sighting seen = WorldLoader::SightingFrom(o.value());
                    if (seen.Observed())
                        c.sightings[o.key()] = std::move(seen);
                }
            c.chartsRev = ++chartsSerial_;
            const auto treasury = cj.find("treasury");
            const auto tax = cj.find("tax");
            const auto docking = cj.find("docking");
            if ((treasury != cj.end() &&
                 (!treasury->is_number() || treasury->get<double>() < 0.0)) ||
                (tax != cj.end() &&
                 (!tax->is_number() || tax->get<double>() < 0.0 || tax->get<double>() > 1.0)) ||
                (docking != cj.end() &&
                 (!docking->is_string() || !DockingFrom(docking->get<std::string>(), c.docking))))
            {
                why = c.id + "'s treasury, tax or docking is not one it could have";
                return false;
            }
            if (treasury != cj.end())
                c.treasury = treasury->get<double>();
            if (tax != cj.end())
                c.tax = tax->get<double>();
            const auto licences = cj.find("licences");
            if (licences != cj.end() && licences->is_object())
                for (auto l = licences->begin(); l != licences->end(); ++l)
                {
                    Licence lc;
                    lc.holder = l->is_object() ? text(*l, "holder") : std::string();
                    const auto until = l->is_object() ? l->find("until") : l->end();
                    if (lc.holder.empty() || until == l->end() || !until->is_number())
                        continue;  // not a licence this build can read: none
                    lc.until = until->get<double>();
                    lc.told = (int)whole(*l, "told");
                    c.licences[l.key()] = lc;
                }
            const auto police = cj.find("police");
            if (police != cj.end())
            {
                if (!police->is_object())
                {
                    why = c.id + "'s police budgets are not a list of systems";
                    return false;
                }
                for (auto pit = police->begin(); pit != police->end(); ++pit)
                {
                    if (!pit.value().is_number() || !(pit.value().get<double>() > 0.0))
                    {
                        why =
                            c.id + "'s police budget in " + pit.key() + " is not one it could have";
                        return false;
                    }
                    c.police[pit.key()] = pit.value().get<double>();
                }
            }
            const auto patrols = cj.find("patrols");
            if (patrols != cj.end())
            {
                if (!patrols->is_object())
                {
                    why = c.id + "'s patrols are not a list of systems";
                    return false;
                }
                for (auto pit = patrols->begin(); pit != patrols->end(); ++pit)
                {
                    const json& pj = pit.value();
                    Patrol      p;
                    const auto  ships = pj.is_object() ? pj.find("ships") : pj.end();
                    const auto  rules = pj.is_object() ? pj.find("rules") : pj.end();
                    if (ships == pj.end() || !ships->is_number_integer() ||
                        ships->get<int>() <= 0 ||
                        (rules != pj.end() && !DefenceFrom(*rules, p.rules)))
                    {
                        why = c.id + "'s patrol in " + pit.key() + " is not one it could have";
                        return false;
                    }
                    p.ships = ships->get<int>();
                    p.design = text(pj, "design");
                    c.patrols[pit.key()] = p;
                }
            }
            // An id is never handed out twice, even after its corporation is gone.
            next = std::max(next, std::atoi(c.id.c_str() + 1) + 1);
            SortMembers(c);
            corps[c.id] = std::move(c);
        }
    corps_ = std::move(corps);
    nextId_ = next;
    Index();
    rev_++;
    return true;
}

void Registry::AddCharts(const std::string& account, const std::vector<std::string>& systems)
{
    auto it = byAccount_.find(account);
    if (it == byAccount_.end())
        return;
    Corp& c = corps_[it->second];
    bool  added = false;
    for (const std::string& id : systems)
        added = c.charts.insert(id).second || added;
    if (added)
        c.chartsRev = ++chartsSerial_;
}

void Registry::Sight(const std::string& id, const std::string& systemId,
                     const WorldLoader::Sighting& now)
{
    auto it = corps_.find(id);
    if (it == corps_.end())
        return;
    WorldLoader::Sighting& kept = it->second.sightings[systemId];
    const bool             news = !kept.Observed() || kept.outdated || !kept.SameAs(now);
    kept = now;
    if (news)
        it->second.chartsRev = ++chartsSerial_;
}

void Registry::RenameCharts(const std::map<std::string, std::string>& ids, bool outdate)
{
    for (auto& [id, c] : corps_)
    {
        bool renamed = false;
        for (const auto& [from, to] : ids)
            if (c.charts.erase(from) != 0)
            {
                c.charts.insert(to);
                renamed = true;
            }
        for (const auto& kv : c.sightings)
            renamed = renamed || ids.count(kv.first) != 0;
        WorldLoader::RenameSightings(c.sightings, ids, outdate);
        if (renamed)
            c.chartsRev = ++chartsSerial_;  // its members' maps change, and corps.json with them
    }
}

bool Registry::Save(const std::string& path) const
{
    return Save::WriteFileAtomic(path, ToJson().dump(2) + "\n", 1);
}

Save::Result Registry::Load(const std::string& path)
{
    std::ifstream in(path);
    if (!in.is_open())
        return Save::Result::Missing;
    const json j = json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.is_object())
        return Save::Result::Corrupt;
    if (j.value("version", Save::UNVERSIONED) > Save::CORPS_VERSION)
        return Save::Result::TooNew;
    std::string why;
    if (!FromJson(j, why))
    {
        fprintf(stderr, "corps: %s: %s\n", path.c_str(), why.c_str());
        return Save::Result::Corrupt;
    }
    return Save::Result::Ok;
}

}  // namespace Corps
