#pragma once

#include "sim/SaveSchema.h"

#include <string>

class Simulation;

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

// What a client that speaks another protocol is told before it is let go, naming both
// versions: dropping it in silence looked like a server that was down.
std::string VersionRefusal(int clientVersion);

}  // namespace Host
