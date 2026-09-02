#include "sim/Auth.h"

#include "sim/Protocol.h"

#include <picosha2.h>

#include <chrono>
#include <cstdint>
#include <random>
#include <thread>

namespace Auth
{
namespace
{

// How many times the secret is re-hashed before it is stored. It costs a client about a
// hundredth of a second per login and multiplies the work of guessing offline by the same
// factor. The number is part of the stored format: changing it invalidates every existing
// account, so it moves only with a migration.
constexpr int STRETCH_ROUNDS = 100000;

// Random bytes for a salt or a nonce.
//
// std::random_device is asked first and then mixed with the clock and with the address of
// a stack object, because on MinGW it has historically been a fixed sequence -- the exact
// kind of failure that leaves every nonce identical while everything still appears to
// work.
std::string RandomHex(int bytes)
{
    std::random_device rd;
    uint64_t           seed = ((uint64_t)rd() << 32) ^ (uint64_t)rd();
    seed ^= (uint64_t)std::chrono::high_resolution_clock::now().time_since_epoch().count();
    int local = 0;
    seed ^= (uint64_t)reinterpret_cast<uintptr_t>(&local);

    std::mt19937_64                         gen(seed);
    std::uniform_int_distribution<unsigned> byte(0, 255);
    static const char*                      hex = "0123456789abcdef";

    std::string out;
    out.reserve((size_t)bytes * 2);
    for (int i = 0; i < bytes; i++)
    {
        const unsigned b = byte(gen);
        out.push_back(hex[(b >> 4) & 0xF]);
        out.push_back(hex[b & 0xF]);
    }
    return out;
}

}  // namespace

std::string Sha256Hex(const std::string& data)
{
    return picosha2::hash256_hex_string(data);
}

std::string MakeSalt()
{
    return RandomHex(16);
}

std::string MakeNonce()
{
    return RandomHex(16);
}

std::string Stored(const std::string& salt, const std::string& secret)
{
    std::string h = Sha256Hex(salt + ":" + secret);
    for (int i = 0; i < STRETCH_ROUNDS; i++)
        h = Sha256Hex(h);
    return h;
}

std::string Proof(const std::string& nonce, const std::string& stored)
{
    // Cheap on purpose: the stretching has already happened, and this runs once per
    // connection on both sides.
    return Sha256Hex(nonce + ":" + stored);
}

bool ClientHandshake(ITransport& conn, const std::string& account, const std::string& secret,
                     double timeoutSeconds, std::string& error)
{
    Proto::Hello hello;
    hello.account = account;
    conn.Send(Proto::EncodeHello(hello));

    const auto       deadline = std::chrono::steady_clock::now() +
                                std::chrono::milliseconds((long long)(timeoutSeconds * 1000.0));
    Proto::Challenge chal;
    bool             got = false;
    std::string      msg;
    while (!got && std::chrono::steady_clock::now() < deadline)
    {
        while (conn.Poll(msg))
        {
            if (Proto::DecodeChallenge(msg, chal))
            {
                got = true;
                break;
            }
            Proto::Bye bye;
            if (Proto::DecodeBye(msg, bye))
            {
                error = bye.reason;
                return false;
            }
            // Anything else this early is a server that does not speak this handshake --
            // a version gap, most likely, which its own check will have reported.
        }
        if (!got)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!got)
    {
        error = "the server did not answer the login";
        return false;
    }

    const std::string stored = Stored(chal.salt, secret);
    Proto::Auth       answer;
    answer.proof = Proof(chal.nonce, stored);
    // Only when the account is being created: this is the value the server will keep, and
    // it is the one message that carries it.
    if (chal.isNew)
        answer.stored = stored;
    conn.Send(Proto::EncodeAuth(answer));
    return true;
}

}  // namespace Auth
