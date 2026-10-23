#include "sim/Login.h"

#include "sim/Auth.h"
#include "sim/HostRules.h"

#include <cctype>

// The account a typed name logs into. Defined here, with the other code that reads a name
// off the wire and needs no world to do it, so the fuzz targets can link it; HostRules.h
// still declares it.
std::string Host::CanonicalAccount(const std::string& typed)
{
    // Rejecting is better than mangling: two names differing only in punctuation would
    // otherwise share one account file, which is one player spending another's money.
    if (typed.empty() || typed.size() > 32)
        return std::string();
    std::string out;
    out.reserve(typed.size());
    for (char c : typed)
    {
        const unsigned char u = (unsigned char)c;
        if (!std::isalnum(u) && c != '_' && c != '-')
            return std::string();
        out.push_back((char)std::tolower(u));
    }
    return out;
}

namespace Login
{

HelloVerdict ReadHello(const std::string& msg, bool fresh, Proto::Hello& h, int& version)
{
    version = Proto::MessageVersion(msg);
    // Told, with both numbers (#341): dropped in silence, a client one version behind looked
    // exactly like a server that was down.
    if (fresh && version != Proto::PROTO_VERSION)
        return HelloVerdict::WrongVersion;
    if (!fresh || !Proto::DecodeHello(msg, h) ||
        (h.account = Host::CanonicalAccount(h.account)).empty())
        return HelloVerdict::Rejected;
    return HelloVerdict::Ok;
}

bool AnswerAccepted(bool isNewAccount, const std::string& fileStored, const Proto::Auth& a,
                    const std::string& nonce)
{
    const std::string& against = isNewAccount ? a.stored : fileStored;
    return against.size() == 64 && a.proof == ::Auth::Proof(nonce, against);
}

}  // namespace Login
