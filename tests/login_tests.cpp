#include <doctest/doctest.h>

#include "sim/Auth.h"
#include "sim/HostRules.h"
#include "sim/Login.h"
#include "sim/Protocol.h"

#include <string>

// The two decisions of a login that read bytes a stranger sent, as the host loop makes them
// (#518). Both used to be inline in the host loop and out of reach of a test or a fuzzer.

namespace
{
std::string HelloFor(const std::string& account)
{
    Proto::Hello h;
    h.account = account;
    h.build = "0.1";
    return Proto::EncodeHello(h);
}
}  // namespace

TEST_CASE("a hello is read to a canonical name")
{
    Proto::Hello h;
    int          version = 0;
    CHECK(Login::ReadHello(HelloFor("Pilot_1"), true, h, version) == Login::HelloVerdict::Ok);
    CHECK(h.account == "pilot_1");
    CHECK(version == Proto::PROTO_VERSION);
}

TEST_CASE("a hello that is not a name is rejected")
{
    Proto::Hello h;
    int          version = 0;
    for (const char* bad : { "", "a b", "../x", "pilot\n", "ünï" })
        CHECK(Login::ReadHello(HelloFor(bad), true, h, version) == Login::HelloVerdict::Rejected);
    CHECK(Login::ReadHello(HelloFor(std::string(33, 'a')), true, h, version) ==
          Login::HelloVerdict::Rejected);
    // The right version, but no name that is text.
    const std::string v = std::to_string(Proto::PROTO_VERSION);
    CHECK(Login::ReadHello(R"({"t":"hello","v":)" + v + R"(,"acct":5})", true, h, version) ==
          Login::HelloVerdict::Rejected);
    // Not JSON at all is no version at all, which is told apart from a wrong one below.
    CHECK(Login::ReadHello("not json", true, h, version) == Login::HelloVerdict::WrongVersion);
    CHECK(version == 0);
}

TEST_CASE("a second hello on one connection is rejected, whatever it says")
{
    Proto::Hello h;
    int          version = 0;
    CHECK(Login::ReadHello(HelloFor("pilot"), false, h, version) == Login::HelloVerdict::Rejected);
}

TEST_CASE("a hello from another protocol version is told apart from junk")
{
    Proto::Hello h;
    int          version = 0;
    std::string  msg = HelloFor("pilot");
    const auto   at = msg.find("\"v\":" + std::to_string(Proto::PROTO_VERSION));
    REQUIRE(at != std::string::npos);
    msg.replace(at, 4 + std::to_string(Proto::PROTO_VERSION).size(), "\"v\":1");
    CHECK(Login::ReadHello(msg, true, h, version) == Login::HelloVerdict::WrongVersion);
    CHECK(version == 1);
    // Junk has no version at all, which is not the same as a wrong one.
    CHECK(Login::ReadHello("{}", true, h, version) == Login::HelloVerdict::WrongVersion);
    CHECK(version == 0);
}

TEST_CASE("an answer is accepted only for this nonce and this stored value")
{
    const std::string stored = Auth::Stored("salt", "secret");
    const std::string nonce = "00112233445566778899aabbccddeeff";
    Proto::Auth       a;
    a.stored = stored;
    a.proof = Auth::Proof(nonce, stored);

    CHECK(Login::AnswerAccepted(false, stored, a, nonce));
    CHECK(Login::AnswerAccepted(true, "", a, nonce));  // a new account registers its own

    CHECK_FALSE(Login::AnswerAccepted(false, stored, a, "ffeeddccbbaa99887766554433221100"));
    CHECK_FALSE(Login::AnswerAccepted(false, Auth::Stored("salt", "other"), a, nonce));
    CHECK_FALSE(Login::AnswerAccepted(false, "", a, nonce));  // no stored value, nothing to prove
    CHECK_FALSE(Login::AnswerAccepted(false, "short", a, nonce));
    Proto::Auth bad = a;
    bad.proof = "";
    CHECK_FALSE(Login::AnswerAccepted(false, stored, bad, nonce));
    // A registration is held to the value it brings: a proof for another one is refused.
    bad = a;
    bad.stored = Auth::Stored("salt", "other");
    CHECK_FALSE(Login::AnswerAccepted(true, "", bad, nonce));
}
