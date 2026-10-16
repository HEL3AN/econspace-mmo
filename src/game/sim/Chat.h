#pragma once

#include "sim/Protocol.h"

#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <string_view>
#include <vector>

// Chat (#347): the local channel of each system, a line to one pilot, and the operator's
// word to everyone -- the minimum for a world with other people in it to feel like one.
//
// The rules live here and not in the host loop, so a test can hold them: who hears a line,
// what a line may contain, how fast a pilot may talk and who may not. Like Host::Gate it
// keeps no clock of its own; every call is told the time.
namespace Chat
{

// The longest line, in characters (code points) after cleaning. A client caps what it lets
// be typed at the same number; the server cuts anything longer.
inline constexpr size_t MAX_CHARS = 200;

// The channels a line can be on (Proto::ChatLine::channel).
inline constexpr const char* LOCAL = "local";
inline constexpr const char* PRIVATE = "private";
inline constexpr const char* SERVER = "server";
inline constexpr const char* NOTICE = "notice";
inline constexpr const char* CORP = "corp";  // a corporation's members (#347)

// A line as anyone else may see it: valid UTF-8 (a broken sequence is dropped, not guessed
// at), no control characters (tabs and line breaks become spaces; the C1 set, the
// direction overrides that turn a line round on someone else's screen and the zero-width
// characters that hide one name inside another are taken out), runs of spaces made one,
// trimmed, and cut to `maxChars` code points. Empty if nothing printable was left.
std::string Sanitize(std::string_view raw, size_t maxChars = MAX_CHARS);
// How many code points a valid UTF-8 string holds.
size_t CountChars(std::string_view utf8);

struct Limits
{
    // A pilot may say `burst` lines at once, and one more every `refill` seconds after:
    // enough to answer a question in three lines, not enough to fill everyone's window.
    int    burst = 5;
    double refill = 2.0;
    int    history = 20;       // lines a system keeps for whoever comes in...
    double historyAge = 1800;  // ...said no longer ago than this, seconds
};

// A pilot who can hear: a connected session, its account name (canonical, lower case), the
// system it is in, and its corporation (#347) by id and ticker -- empty for none.
struct Pilot
{
    int         id = 0;
    std::string name;
    std::string system;
    std::string corp;
    std::string ticker;

    Pilot() = default;
    Pilot(int i, std::string n, std::string s, std::string c = std::string(),
          std::string t = std::string())
        : id(i), name(std::move(n)), system(std::move(s)), corp(std::move(c)), ticker(std::move(t))
    {
    }
};

// One line for one session.
struct Delivery
{
    int             to = 0;
    Proto::ChatLine line;
};

// What a pilot is told on coming into a system, or when someone comes into or leaves the
// one it is in: who is there, and -- only on arriving -- what was said there lately.
struct Presence
{
    int                          to = 0;
    Proto::ChatMembers           members;
    std::vector<Proto::ChatLine> history;  // empty unless it has just arrived
};

class Hub
{
public:
    Hub() = default;
    explicit Hub(const Limits& l) : limits_(l) {}
    const Limits& GetLimits() const { return limits_; }

    // `from` says `say` at `now` (seconds; a line is stamped with it as Unix time): the
    // lines that go out and to whom. A local line goes to everyone in `online` in the
    // sender's system, the sender included, and is kept for the system's history; a private
    // one to the pilot named and back to the sender; a corp line to every member of the
    // sender's corporation online, the sender included. Refused -- muted, too fast, nobody of
    // that name online -- it is a notice to the sender alone. A line with nothing left in it
    // after cleaning is no line at all, and costs nothing.
    std::vector<Delivery> Say(const Pilot& from, const Proto::ChatSay& say,
                              const std::vector<Pilot>& online, double now);
    // The operator's line, to everyone online. Not limited, not kept.
    std::vector<Delivery> Broadcast(std::string_view text, const std::vector<Pilot>& online,
                                    double now);
    // The server's line on a corporation's channel (#347) -- who joined, who left -- to its
    // members online. `corp` by id; the line names it by `ticker`.
    std::vector<Delivery> ToCorp(const std::string& corp, const std::string& ticker,
                                 std::string_view text, const std::vector<Pilot>& online,
                                 double now);

    // Who is where now. Each pilot that is new or in another system than last time is sent
    // the members of its system and its history; everyone else in a system whose members
    // changed is sent the members again. Call it whenever the online pilots may have
    // changed -- every frame is fine; nothing changes, nothing is sent.
    std::vector<Presence> Track(const std::vector<Pilot>& online, double now);

    // A muted pilot's lines are refused, until `until` (seconds, the clock Say is told), or
    // until unmuted when `until` is 0. Names as Pilot::name.
    void Mute(const std::string& name, double until);
    bool Unmute(const std::string& name);  // false if it was not muted
    bool Muted(const std::string& name, double now) const;

    // The lines a system keeps, as Track would send them to someone arriving at `now`.
    std::vector<Proto::ChatLine> History(const std::string& system, double now) const;

    // Forgets what is old enough not to matter -- how fast a pilot who left was talking,
    // a mute that has run out, a system's stale history -- so the tables do not grow for
    // ever.
    void Expire(double now);

private:
    struct Bucket
    {
        double tokens = 0.0;
        double last = 0.0;
    };
    // True and a token spent if `name` may say a line now; otherwise `wait` is how long until
    // it may.
    bool            Spend(const std::string& name, double now, double& wait);
    static Delivery Notice(const Pilot& to, std::string text, double now);

    Limits                                              limits_;
    std::map<std::string, Bucket>                       buckets_;  // by pilot
    std::map<std::string, double>                       muted_;    // until; 0 for ever
    std::map<std::string, std::vector<Proto::ChatLine>> history_;  // by system
    std::map<int, std::string>                          where_;    // session -> system
    std::map<std::string, std::vector<std::string>>     members_;  // system -> "pilot ticker"
};

}  // namespace Chat
