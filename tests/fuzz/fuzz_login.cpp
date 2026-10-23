// Fuzz target 3 (#518): the login, from the hello to the answer check, without a socket.
//
// The same two functions the host loop calls (sim/Login.h), plus the build-and-data
// comparison it applies between them. The input is a two-byte length, then the hello, then the
// rest as the answer; the hello is read on a fresh connection and on one that has already
// said hello, since a hostile client may send them in any order and twice.
//
// The target fails if an answer is accepted that is not the proof for this nonce and this
// stored value, or a name is accepted that is not canonical -- the properties that keep a
// stranger from flying someone's account (#106) or two spellings from sharing one file.
#include "sim/Auth.h"
#include "sim/Compat.h"
#include "sim/HostRules.h"
#include "sim/Login.h"
#include "sim/Protocol.h"

#include <cstdint>
#include <cstdlib>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size < 2)
        return 0;
    const size_t      want = ((size_t)data[0] << 8) | data[1];
    const size_t      helloLen = want < size - 2 ? want : size - 2;
    const std::string hello((const char*)data + 2, helloLen);
    const std::string answer((const char*)data + 2 + helloLen, size - 2 - helloLen);

    for (const bool fresh : { true, false })
    {
        Proto::Hello h;
        int          version = 0;
        const auto   verdict = Login::ReadHello(hello, fresh, h, version);
        if (verdict == Login::HelloVerdict::Ok)
        {
            if (!fresh || h.account.empty() || h.account.size() > 32 ||
                Host::CanonicalAccount(h.account) != h.account)
                abort();
            (void)Compat::Refusal("0.1", { { "archetypes.json", "abc" } }, h.build, h.data);
        }
    }

    // The answer, against what an account file would store, and against a new account's own
    // claim.
    static const std::string kStored = Auth::Stored("salt", "secret");
    const std::string        nonce = "00112233445566778899aabbccddeeff";
    Proto::Auth              a;
    if (Proto::DecodeAuth(answer, a))
    {
        if (Login::AnswerAccepted(false, kStored, a, nonce) &&
            a.proof != Auth::Proof(nonce, kStored))
            abort();
        if (Login::AnswerAccepted(true, kStored, a, nonce) &&
            (a.stored.size() != 64 || a.proof != Auth::Proof(nonce, a.stored)))
            abort();
    }
    return 0;
}
