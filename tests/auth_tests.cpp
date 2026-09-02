#include <doctest/doctest.h>

#include "sim/Auth.h"

#include <set>
#include <string>

// The properties an account secret rests on (#106). Each of these failing is silent: the
// login still succeeds, and it succeeds for the wrong people too.

TEST_CASE("the hash is the one it claims to be")
{
    // The published SHA-256 of "abc". A dependency that quietly returned something else --
    // a different algorithm, a truncated digest, a build that took the wrong header --
    // would still produce stable-looking hex and nothing would notice.
    CHECK(Auth::Sha256Hex("abc") ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(Auth::Sha256Hex("") ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST_CASE("salts and nonces are actually random")
{
    // MinGW's std::random_device has historically returned a fixed sequence. A constant
    // nonce makes every captured proof replayable, and a constant salt makes one table of
    // guesses cover the whole server -- while everything still appears to work.
    std::set<std::string> salts, nonces;
    for (int i = 0; i < 32; i++)
    {
        salts.insert(Auth::MakeSalt());
        nonces.insert(Auth::MakeNonce());
    }
    CHECK(salts.size() == 32);
    CHECK(nonces.size() == 32);

    CHECK(Auth::MakeSalt().size() == 32);  // 16 bytes as hex
    CHECK(Auth::MakeSalt().find_first_not_of("0123456789abcdef") == std::string::npos);
}

TEST_CASE("what the server stores does not give the secret back")
{
    const std::string salt = "0123456789abcdef0123456789abcdef";
    const std::string stored = Auth::Stored(salt, "correct horse");

    CHECK(stored != "correct horse");
    CHECK(stored.find("correct") == std::string::npos);
    CHECK(Auth::Stored(salt, "correct horse") == stored);  // same inputs, same answer

    SUBCASE("a different secret stores differently")
    {
        CHECK(Auth::Stored(salt, "correct horsf") != stored);
    }

    SUBCASE("the same secret under a different salt stores differently")
    {
        // This is what the salt is for: two players who choose the same secret must not
        // share a stored value, or one table of guesses breaks both.
        CHECK(Auth::Stored("fedcba9876543210fedcba9876543210", "correct horse") != stored);
    }
}

TEST_CASE("a proof is good for one nonce and no other")
{
    const std::string stored = Auth::Stored(Auth::MakeSalt(), "correct horse");
    const std::string nonce = Auth::MakeNonce();

    // The server recomputes it from what it stored, and gets the same answer.
    CHECK(Auth::Proof(nonce, stored) == Auth::Proof(nonce, stored));

    SUBCASE("captured and replayed on the next connection, it is worthless")
    {
        const std::string captured = Auth::Proof(nonce, stored);
        CHECK(Auth::Proof(Auth::MakeNonce(), stored) != captured);
    }

    SUBCASE("knowing the wrong secret proves nothing")
    {
        const std::string wrong = Auth::Stored("0123456789abcdef0123456789abcdef", "hunter2");
        CHECK(Auth::Proof(nonce, wrong) != Auth::Proof(nonce, stored));
    }
}
