#pragma once

#include "core/WorldLoader.h"
#include "entities/Defence.h"
#include "sim/Events.h"
#include "sim/SaveSchema.h"

#include <nlohmann/json_fwd.hpp>

#include <map>
#include <set>
#include <string>
#include <vector>

// Player corporations (#347): pilots who fly together, with a name, a ticker, a CEO and the
// trust the corporation's structures extend to every member.
//
// A corporation is not a faction. The faction registry (#355) is the NPC powers: every entry
// joins the relations, the macro pass, the plans and every PerFaction<T> in the save. A
// corporation does none of that, there may be hundreds of them, and they come and go. So it
// is a registry of its own, named everywhere -- in the save and on the wire -- by a stable
// string id ("c<N>"), never an index. Standings stay each pilot's own: they are earned by one
// pilot's missions and lost by one pilot's crimes, and pooling them would let a clean member
// launder docking rights for a wanted one.
//
// The rules live here and not in the host loop, so a test can hold them; like Chat::Hub,
// nothing here keeps a clock -- every call that needs the time is told it (Unix seconds).
namespace Corps
{

enum class Role
{
    Member,
    Director,  // invites and kicks members; manages members' structures' rules of engagement
    Ceo,       // all of that, and roles; one per corporation
};
const char* RoleName(Role r);   // "member", "director", "ceo"
const char* RoleLabel(Role r);  // what a player reads: "Member", "Director", "CEO"
bool        RoleFrom(const std::string& name, Role& out);

// Who docks at the corporation's structures in a system it holds (#431 T3): its members only,
// its members and whoever each structure's rules of engagement hold fire for (its allow-list),
// or anyone -- each with the limits a dock already has (a private dock is the members' only).
enum class Docking
{
    Own,
    Allies,
    All,
};
const char* DockingName(Docking d);  // "own", "allies", "all"
bool        DockingFrom(const std::string& name, Docking& out);

inline constexpr int    MEMBERS_MAX = 200;
inline constexpr int    INVITES_MAX = 50;  // pending, per corporation
inline constexpr double INVITE_SECONDS = 7.0 * 86400.0;

struct Member
{
    std::string account;  // canonical, lower case
    Role        role = Role::Member;
    long long   joined = 0;  // Unix seconds
};

struct Invite
{
    std::string account;
    std::string by;
    long long   at = 0;
};

// A licence to build in a system somebody holds (#431 slice 6): a weekly rent to the holder,
// kept by the party that pays it -- on a corporation's record here, on a pilot's in the
// world's book of owners. `until` is world time (Simulation::Time), not Unix seconds: the
// rent runs on the world's clock, which stops while the server does.
struct Licence
{
    std::string holder;       // who granted it: a faction's "f:<id>" or a corporation's id
    double      until = 0.0;  // paid to; unpaid after it, lapsed a grace later
    int         told = 0;     // what its payer has been told: 1 unpaid, 2 lapsed
};

// A patrol a corporation keeps in a system (#431 C1, slice 12): AI ships of one design that
// guard its members' structures and ships there while they are away. Set by its CEO or a
// director, within the server's limits (world_rules.json -> patrols), and paid for from the
// treasury every macro pass; what stands there now is the world's number (the system's
// aggregate), which fights and is lost like any NPC presence.
struct Patrol
{
    int ships = 0;  // how many it keeps there
    // The design they fly: a stock design's id or a filed one's content id (#279); empty, the
    // default doctrine's `guard` (data/ships.json).
    std::string design;
    // Whom they fire on first, from the rules of engagement structures use (#340): never a
    // member, always back at whoever shot a member, a member's structure or the patrol.
    Defence rules = DefaultRules();

    static Defence DefaultRules()
    {
        Defence d;
        d.engage = Engage::Enemies;
        return d;
    }
};

struct Corp
{
    std::string         id;  // "c<N>", stable for the corporation's life
    std::string         name;
    std::string         ticker;  // 2 to 5 of A-Z and 0-9, upper case
    long long           founded = 0;
    std::vector<Member> members;  // the CEO first, then by account
    std::vector<Invite> invites;
    // What happened to the corporation (#431 C2): the pilot's journal entry, kept for the
    // corporation, which every member reads -- at once while on, and at login what came
    // since they last read it (ClientSession::corpJournalSeen). Capped, oldest dropped.
    std::vector<Ev::Event> journal;
    int                    nextJournalSeq = 1;
    // The corporation's charts (#431 K1): every system a member has charted while in it, by
    // id, shown on every member's map. They are the corporation's: a member who leaves takes
    // their own charts and leaves these. `chartsRev` changes with them (not saved).
    std::set<std::string> charts;
    long long             chartsRev = 0;
    // ...and what its members and structures last saw standing in each system, and when
    // (#431 slice 14): the observed layer of its charts, which an offline member's
    // corporation mates keep current for them. A change of what stood there moves `chartsRev`.
    std::map<std::string, WorldLoader::Sighting> sightings;
    // Its money (#431 T4): what the markets of the systems it holds take, and what its
    // members' markets take anywhere else. It pays for police (slice 11) and its patrols'
    // upkeep (slice 12).
    double treasury = 0.0;
    // What it pays for police in each system it holds (#431 L5, slice 11), in credits an hour,
    // by system id. The coarse pass takes it from the treasury while the corporation holds the
    // system and can pay; a budget for a system it no longer holds waits, unpaid.
    std::map<std::string, double> police;
    // Its patrols (#431 C1, slice 12), by system id.
    std::map<std::string, Patrol> patrols;
    // The policies of a holder (T3), set by its CEO or a director: the fee on trades at markets
    // in the systems it holds, as a share (below zero: the standard fee), and who docks at its
    // structures there.
    double  tax = -1.0;
    Docking docking = Docking::All;
    // Its licences to build in other holders' systems (#431 slice 6), by system id, the rent
    // paid from the treasury.
    std::map<std::string, Licence> licences;

    const Member* Find(const std::string& account) const;
    const Member* Ceo() const;
    // The fee its markets take: its tax, or `standard` when it set none, never above `cap`.
    double TaxRate(double standard, double cap) const;
};

// Whether a name and a ticker have the shape the rules allow; `why` says what is wrong.
// Uniqueness is the registry's.
bool ValidName(const std::string& name, std::string& why);
bool ValidTicker(const std::string& ticker, std::string& why);

// A verb, as a client sends it (Proto::CorpAct): "create" (name, ticker), "invite" (pilot),
// "join" and "decline" (ticker), "leave", "kick" (pilot), "role" (pilot, role); a holder's
// policies (#431 T3), by the CEO or a director: "tax" (value: a percentage, or "standard") and
// "docking" (value: own, allies or all); and "police" (system, value: credits an hour, 0 for
// none), the police it pays for in a system it holds (slice 11) -- which only the world can
// answer, so Simulation::ApplyCorpAct is the way in for every verb; and "patrol" (system,
// value: ships, 0 or "none" to stand them down; design; engage), the patrol it keeps in a
// system it holds or has structures in (slice 12), which the world answers too.
struct Act
{
    std::string verb;
    std::string name;
    std::string ticker;
    std::string pilot;
    std::string role;
    std::string value{};   // braced: the verbs without one leave it out
    std::string system{};  // "police", "patrol": the system, by id
    std::string design{};  // "patrol": the design, a name in the setter's library or a stock id
    std::string engage{};  // "patrol": hold, return, enemies or strangers; empty -- as it was
};

// What came of an act. `error` empty: done. `told` is the actor's answer either way;
// `announce` is a line for the corporation's channel (`corp`, the corporation the act was
// about -- for a leave that disbanded it, the members who are left are nobody); `subject` is
// the pilot the act was done to -- invited, removed, given a role -- and `subjectText` what
// they are told.
struct Outcome
{
    std::string error;
    std::string told;
    std::string corp;
    std::string announce;
    std::string subject;
    std::string subjectText;
    bool        Ok() const { return error.empty(); }
};

class Registry
{
public:
    // `account` does `act` at `now`. `online` is who may be invited: an invite is to a pilot
    // who is there to read it.
    Outcome Apply(const std::string& account, const Act& act, const std::set<std::string>& online,
                  double now);

    // The verbs, each answering as Apply does.
    Outcome Create(const std::string& account, const std::string& name, const std::string& ticker,
                   double now);
    Outcome InviteTo(const std::string& by, const std::string& pilot,
                     const std::set<std::string>& online, double now);
    Outcome Join(const std::string& account, const std::string& ticker, double now);
    Outcome Decline(const std::string& account, const std::string& ticker);
    Outcome Leave(const std::string& account);
    Outcome Kick(const std::string& by, const std::string& pilot);
    Outcome SetRole(const std::string& by, const std::string& pilot, Role role);
    // A holder's policy (#431 T3): `verb` "tax" or "docking". A tax above `taxCap` is refused.
    Outcome SetPolicy(const std::string& by, const std::string& verb, const std::string& value,
                      double taxCap);
    // The police `by`'s corporation pays for in `system` (#431 slice 11), called `where`:
    // `value` credits an hour, at most `cap`; 0 or "none" stops it. `held` is whether the
    // corporation holds the system now -- the world's to say; a budget is set only there.
    Outcome SetPolice(const std::string& by, const std::string& system, const std::string& where,
                      const std::string& value, double cap, bool held);
    // The patrol `by`'s corporation keeps in `system` (#431 slice 12), called `where`: `value`
    // ships (0 or "none" stands it down) of `design` (resolved by the world; empty -- the
    // default doctrine's), `designName` what the officer called it, engaging as `engage` says
    // (empty: as it was). `allowed` is whether the corporation holds the system or has a
    // structure there -- the world's to say; a patrol is set only there. At most `systemMax`
    // ships in a system and `corpMax` in all.
    Outcome SetPatrol(const std::string& by, const std::string& system, const std::string& where,
                      const std::string& value, const std::string& design,
                      const std::string& designName, const std::string& engage, int systemMax,
                      int corpMax, bool allowed);

    // Money into a corporation's treasury (#431 T4); false for one that does not exist. Not a
    // change Rev() counts -- a fee comes with every trade, and Rev() is what writes corps.json
    // and retells allow-lists -- but MoneyRev() does: the host sends members their view on
    // it, and the treasury is written with the world.
    bool Credit(const std::string& id, double amount);
    // ...and out of it (#431 slice 6): false, and nothing taken, when it holds less.
    bool Debit(const std::string& id, double amount);
    // A corporation's licence in `system` set or dropped (#431 slice 6). Counted as MoneyRev:
    // written with the world, and retold to the members.
    void SetLicence(const std::string& id, const std::string& system, const Licence& l);
    void DropLicence(const std::string& id, const std::string& system);
    int  MoneyRev() const { return moneyRev_; }

    // The corporation `account` is in; null for none.
    const Corp* Of(const std::string& account) const;
    const Corp* Find(const std::string& id) const;
    const Corp* FindTicker(const std::string& ticker) const;  // in any case
    // Whether two accounts are in the same corporation. Never for an empty name.
    bool        SameCorp(const std::string& a, const std::string& b) const;
    std::string TickerOf(const std::string& account) const;  // empty: in none
    std::string IdOf(const std::string& account) const;      // empty: in none
    // Whether `account` may manage `owner`'s structures: the owner, or the CEO or a director
    // of the owner's corporation.
    bool Manages(const std::string& account, const std::string& owner) const;
    // The corporations that have invited `account` and whose invite has not run out.
    std::vector<const Corp*>           InvitesFor(const std::string& account, double now) const;
    const std::map<std::string, Corp>& All() const { return corps_; }

    // Adds an entry to the journal of `account`'s corporation; its seq, or 0 when they are
    // in none.
    static constexpr size_t JOURNAL_CAP = 50;
    int Record(const std::string& account, Ev::Kind kind, const std::string& text);

    // Drops invites older than INVITE_SECONDS.
    void Expire(double now);

    // Counts every change, so a host can tell whether what it sent is still true.
    int Rev() const { return rev_; }

    // Systems onto the charts of `account`'s corporation (#431); nothing for a pilot in none.
    // Counted apart from Rev, which is who is in what and is retold to everyone: charts grow
    // each time a member goes somewhere new.
    void      AddCharts(const std::string& account, const std::vector<std::string>& systems);
    long long ChartsSerial() const { return chartsSerial_; }
    // What corporation `id` sees standing in `systemId` now (#431 slice 14). The date is
    // always taken; only a change of what stood there is news to its members' maps.
    void Sight(const std::string& id, const std::string& systemId,
               const WorldLoader::Sighting& now);
    // A region's systems renamed (#431 slice 15): into the archive when it drifted away, out
    // of it when it came back into another slot. Every corporation's charts and sightings of
    // a system of `ids` move to its new id; what they saw is outdated when `outdate`.
    void RenameCharts(const std::map<std::string, std::string>& ids, bool outdate);

    // corps.json beside the world (Save::CORPS_VERSION). A missing file is no corporations;
    // one from a later build is refused rather than read and written back without what it
    // held (#20). Read strictly: a member in two corporations, two of the same name or ticker,
    // or one with no CEO is a damaged file, not something to guess about.
    nlohmann::json ToJson() const;
    bool           FromJson(const nlohmann::json& j, std::string& why);
    bool           Save(const std::string& path) const;  // false: the file is as it was
    Save::Result   Load(const std::string& path);

private:
    Corp* Mutable(const std::string& id);
    void  Index();  // rebuilds byAccount_
    void  SortMembers(Corp& c);

    std::map<std::string, Corp>        corps_;      // by id
    std::map<std::string, std::string> byAccount_;  // account -> id
    int                                nextId_ = 1;
    int                                rev_ = 0;
    long long                          chartsSerial_ = 0;
    int                                moneyRev_ = 0;
};

}  // namespace Corps
