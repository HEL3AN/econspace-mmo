#pragma once

#include "sim/Protocol.h"
#include "sim/SaveSchema.h"
#include "sim/Simulation.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// The server's rules about who may come in and whether the world may start (#341), out of
// the host loop so a test can hold them to account. Each of these was a way to lose or steal
// something: a name that was two accounts on one file, a damaged account that anyone could
// claim, a damaged world that was quietly replaced by a new galaxy.
namespace Host
{

// The account a typed name logs into, or empty if it is not a name at all.
//
// Names are compared in lower case. The account file is named after the account, and NTFS
// does not tell `Pilot` from `pilot`: the server used to, so the two were two sessions on one
// file, and anything carried by one could be sold by the other and saved back.
std::string CanonicalAccount(const std::string& typed);

// The file an account is kept in, under `dir` (which ends in a separator). An account saved
// under a capitalised name before names were canonical is renamed to the canonical one the
// first time it is looked for, on a file system where the two differ -- unless more than one
// spelling exists, which is for a person to settle, not the server.
std::string AccountFile(const std::string& dir, const std::string& canonical);

// What an account file says about the account, before anyone is let in. `salt` and `stored`
// are filled when the file has a secret (#106); a file from before secrets is Ok with both
// empty, and the next login sets them.
//
// Corrupt is the answer that matters: a damaged file is an account that exists, and it must
// not read as a free name -- the caller refuses the login and leaves the file for the owner.
Save::Result ReadAccountAuth(const std::string& path, std::string& salt, std::string& stored);

// Starts (#342 S2). The system an account file says its player was in, or empty: no file,
// a damaged one, one from a newer build (which plays as a fresh account), or one that never
// said.
std::string AccountSystem(const std::string& path);
// The world an account file was last saved in (#392), or empty: not stamped (from before),
// or not readable, as above.
std::string AccountWorld(const std::string& path);
// Whether the player of the account at `accountPath` has to be given a start: a new
// account, one whose system this galaxy does not have -- a world remade under new
// rules keeps its accounts, and they choose again -- or one from another world.
bool NeedsStart(const Simulation& sim, const std::string& accountPath);
// The start a player gets: `wanted` if it is one and the server lets players choose,
// otherwise one of the starts by `roll` -- an asked-for system that is not a start is the
// same as asking for any. Empty only in a galaxy with no starts.
std::string PickStart(const Simulation& sim, const std::string& wanted, bool randomOnly,
                      uint64_t roll);
// What a player is offered.
Proto::Starts StartsMessage(const Simulation& sim, bool randomOnly);

// The world the server starts with. Restored -- read from `path`; Fresh -- there was none,
// and a new galaxy has been made (InitGalaxy); Refused -- the file is there and cannot be
// used, and `why` says what to do. The server stops on Refused: a damaged world.json used to
// be read as "no world", and the first checkpoint a minute later wrote a new galaxy over it.
enum class WorldStart
{
    Restored,
    Fresh,
    Refused
};
WorldStart OpenWorld(Simulation& sim, const std::string& path, std::string& why);

// Where a world's accounts are kept (#392): `accounts/` beside its world.json, ending in a
// separator as AccountFile wants. They were beside the executable, so a world started
// afresh -- or a second world run from the same build -- had another world's pilots.
std::string AccountsDir(const std::string& worldPath);
// Moves the account files in `fromDir` (account_*.json, with the older copies kept beside
// them) into `toDir`, made if need be -- once, when accounts first move beside their world.
// Never over a file: an account already in `toDir` is the live one, and the old file is
// left where it is and named in `kept`, as is one that would not move. `error` is set when
// `toDir` cannot be made, and nothing is moved.
struct AccountMove
{
    int                      moved = 0;
    std::vector<std::string> kept;
    std::string              error;
};
AccountMove MoveAccounts(const std::string& fromDir, const std::string& toDir);
// The corporation members this world has never seen online (#431 slice 12): their standings
// read from their account files in `accountsDir` (NoteUnseenOwner), so a corporation's
// standing -- its members' summed -- counts them from the start. Read only; a file stamped
// for another world (#392), from a newer build or damaged is skipped and named in `skipped`.
// The number noted.
int NoteUnseenMembers(Simulation& sim, const std::string& accountsDir,
                      std::vector<std::string>& skipped);
// A new world's identity (#392): sixteen hex digits, from the clock and random_device.
std::string NewWorldId();

// Where a world's regions are sealed (#342 S3): `regions/` beside its world.json.
std::string RegionsDir(const std::string& worldPath);
// Why a saved world cannot be made by this build, or empty when it can. Its skeleton must be
// of this build's rules -- it is never sealed. Its regions may be of any rules if every one
// in its log is sealed under `sealDir`; a world of other region rules with one that is not,
// or one from before the log, would be remade differently, and is refused.
std::string WorldRulesRefusal(const Simulation::WorldHeader& header, const std::string& sealDir);

// Who may connect, register and try a secret, and how often (#341). A server reachable
// from the internet is reachable by anyone with a loop: without limits one address could
// take every connection, create accounts by the thousand, or guess at a secret as fast as
// the server answers. The numbers are generous next to a person and fatal to a script.
//
// It keeps no clock of its own: every call is told the time, so a test can run an hour of
// it in a line.
class Gate
{
public:
    struct Limits
    {
        int    maxClients = 64;            // connections in all, logged in or not
        int    maxPerAddress = 8;          // ...from one address
        int    registrationsPerHour = 10;  // new accounts from one address
        double maxBackoff = 300.0;         // the longest wait after failed logins, seconds
    };

    Gate() = default;
    explicit Gate(const Limits& l) : limits_(l) {}
    const Limits& GetLimits() const { return limits_; }

    // Why a new connection is turned away, or empty if it may stay. `total` and
    // `fromAddress` are the connections already open, not counting this one.
    std::string RefuseConnection(int total, int fromAddress) const;

    // A new account from `address`: false if the address has made its share this hour.
    bool MayRegister(const std::string& address, double now);
    void Registered(const std::string& address, double now);

    // Seconds `address` must still wait before trying `account` again; 0 if it may now.
    // Each failure doubles the wait, from one second up to maxBackoff, and a login that
    // succeeds clears it. Keyed by the pair: a player who mistypes is not locked out by
    // someone else's guessing, and a guesser does not get a fresh wait by switching names
    // on one account -- each name it tries has its own.
    double LoginWait(const std::string& address, const std::string& account, double now) const;
    void   LoginFailed(const std::string& address, const std::string& account, double now);
    void   LoginSucceeded(const std::string& address, const std::string& account);

    // Forgets what is old enough not to matter, so the tables do not grow for ever.
    void Expire(double now);

private:
    struct Failures
    {
        int    count = 0;
        double until = 0.0;  // no attempt before this
        double last = 0.0;
    };
    Limits                                     limits_;
    std::map<std::string, std::vector<double>> registrations_;  // by address: when
    std::map<std::string, Failures>            failures_;       // by address + account
};

// What a client that speaks another protocol is told before it is let go, naming both
// versions: dropping it in silence looked like a server that was down.
std::string VersionRefusal(int clientVersion);

}  // namespace Host
