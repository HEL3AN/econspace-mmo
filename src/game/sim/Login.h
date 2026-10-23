#pragma once

#include "sim/Protocol.h"

#include <string>

// The two decisions of a login that read bytes a stranger sent (#106), out of the host loop
// so a fuzzer (tests/fuzz, #518) and a test can hold them to account without a socket, a
// world or an account folder. The host loop does the rest: the access list, the backoff, the
// files. It calls these for exactly what it used to do inline.
namespace Login
{

enum class HelloVerdict
{
    Ok,            // a name; `h.account` is its canonical form
    WrongVersion,  // a first hello from a client speaking another protocol; `version` says which
    Rejected       // not a name, or a second hello on the same connection
};

// What a hello message is. `fresh` -- no session and no pending account on the connection --
// is the only state in which one is welcome. `version` is the protocol the message was
// written with (0 for junk), filled whatever the verdict, for the log.
HelloVerdict ReadHello(const std::string& msg, bool fresh, Proto::Hello& h, int& version);

// Whether an answer to the challenge proves the account. A new account is taken at its word
// once -- there is nothing to check it against yet -- but the proof must match the value
// being registered; an existing one is held to what its file stores, which must be a
// 64-character hex digest.
bool AnswerAccepted(bool isNewAccount, const std::string& fileStored, const Proto::Auth& a,
                    const std::string& nonce);

}  // namespace Login
