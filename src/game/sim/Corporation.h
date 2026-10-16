#pragma once

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

struct Corp
{
    std::string         id;  // "c<N>", stable for the corporation's life
    std::string         name;
    std::string         ticker;  // 2 to 5 of A-Z and 0-9, upper case
    long long           founded = 0;
    std::vector<Member> members;  // the CEO first, then by account
    std::vector<Invite> invites;

    const Member* Find(const std::string& account) const;
    const Member* Ceo() const;
};

// Whether a name and a ticker have the shape the rules allow; `why` says what is wrong.
// Uniqueness is the registry's.
bool ValidName(const std::string& name, std::string& why);
bool ValidTicker(const std::string& ticker, std::string& why);

// A verb, as a client sends it (Proto::CorpAct): "create" (name, ticker), "invite" (pilot),
// "join" and "decline" (ticker), "leave", "kick" (pilot), "role" (pilot, role).
struct Act
{
    std::string verb;
    std::string name;
    std::string ticker;
    std::string pilot;
    std::string role;
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

    // Drops invites older than INVITE_SECONDS.
    void Expire(double now);

    // Counts every change, so a host can tell whether what it sent is still true.
    int Rev() const { return rev_; }

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
};

}  // namespace Corps
