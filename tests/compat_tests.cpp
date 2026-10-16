#include "doctest/doctest.h"
#include "core/Version.h"
#include "sim/Compat.h"
#include "sim/Protocol.h"
#include "temp_dir.h"

#include <cstdio>
#include <fstream>
#include <map>
#include <string>

// A client and a server let each other in only on the same build reading the same data (#400).

namespace
{
void WriteFile(const std::string& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary);
    out << text;
}

bool Contains(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}
}  // namespace

TEST_CASE("the version is one semver with a codename for its minor release")
{
    CHECK(std::string(Version::NUMBER) == std::to_string(Version::MAJOR) + "." +
                                              std::to_string(Version::MINOR) + "." +
                                              std::to_string(Version::PATCH));
    CHECK_FALSE(std::string(Version::CODENAME).empty());
    // "EconSpace 0.1 Anchor": the name a player reads carries the codename.
    CHECK(Contains(Version::NAME, "EconSpace "));
    CHECK(Contains(Version::NAME, Version::CODENAME));
}

TEST_CASE("a hello carries the client's build and its data hashes")
{
    Proto::Hello h;
    h.account = "alice";
    h.build = "0.1.0";
    h.data = { { "goods.json", "0123456789abcdef" }, { "ships.json", "" } };
    Proto::Hello back;
    REQUIRE(Proto::DecodeHello(Proto::EncodeHello(h), back));
    CHECK(back.account == "alice");
    CHECK(back.build == "0.1.0");
    CHECK(back.data == h.data);

    // A file hashed as something other than a string reads as missing, never as a match.
    const std::string odd = "{\"t\":\"hello\",\"v\":" + std::to_string(Proto::PROTO_VERSION) +
                            ",\"acct\":\"bob\",\"build\":7,\"data\":{\"goods.json\":5}}";
    REQUIRE(Proto::DecodeHello(odd, back));
    CHECK(back.build.empty());
    CHECK(back.data.at("goods.json").empty());
}

TEST_CASE("the shared data is hashed per file, line endings aside")
{
    const TempDir a, b;
    for (const std::string& name : Compat::SharedFiles())
    {
        WriteFile(a.Path(name), "{\n  \"name\": \"" + name + "\"\n}\n");
        WriteFile(b.Path(name), "{\r\n  \"name\": \"" + name + "\"\r\n}\r\n");
    }
    const auto ha = Compat::DataHashes(a.Path(""));
    const auto hb = Compat::DataHashes(b.Path(""));
    CHECK(ha.size() == Compat::SharedFiles().size());
    // A checkout with CRLF is the same data as one with LF.
    CHECK(ha == hb);
    CHECK(ha.at("goods.json") != ha.at("ships.json"));
    CHECK_FALSE(ha.at("goods.json").empty());

    // A changed file changes its own hash and no other; a missing one hashes as empty.
    WriteFile(b.Path("ships.json"), "{}\n");
    std::remove(b.Path("goods.json").c_str());
    const auto hc = Compat::DataHashes(b.Path(""));
    CHECK(hc.at("ships.json") != ha.at("ships.json"));
    CHECK(hc.at("goods.json").empty());
    CHECK(hc.at("archetypes.json") == ha.at("archetypes.json"));

    // Art the client alone reads is not part of it: a retheme is not a forced update.
    for (const std::string& name : Compat::SharedFiles())
    {
        CHECK(name != "materials.json");
        CHECK(name != "modules.json");
        CHECK(name != "look.json");
    }
}

TEST_CASE("a client of another build or other data is told what differs and what to get")
{
    const std::map<std::string, std::string> server = { { "archetypes.json", "aa" },
                                                        { "blueprints.json", "bb" },
                                                        { "factions.json", "ff" },
                                                        { "goods.json", "gg" },
                                                        { "ships.json", "ss" } };
    const std::string                        build = Version::NUMBER;

    // The same build, the same data: let in.
    CHECK(Compat::Refusal(build, server, build, server).empty());

    // Another build: both numbers and where to get the server's, whatever the data says.
    const std::string other = Compat::Refusal(build, server, "0.0.9", server);
    CHECK(Contains(other, build));
    CHECK(Contains(other, "0.0.9"));
    CHECK(Contains(other, Version::CODENAME));
    CHECK(Contains(other, Version::RELEASES_URL));
    // An empty build is a client that did not say: refused, not waved through.
    CHECK_FALSE(Compat::Refusal(build, server, "", server).empty());

    // Other data: exactly the files that differ are named, and no others.
    auto client = server;
    client["ships.json"] = "s2";
    client.erase("goods.json");
    const std::string data = Compat::Refusal(build, server, build, client);
    CHECK(Contains(data, "ships.json"));
    CHECK(Contains(data, "goods.json (missing on your side)"));
    CHECK_FALSE(Contains(data, "archetypes.json"));
    CHECK_FALSE(Contains(data, "factions.json"));
    CHECK(Contains(data, Version::RELEASES_URL));

    // A file the client has and nobody hashed on the server is not a reason: only the
    // shared files are compared.
    auto extra = server;
    extra["look.json"] = "ll";
    CHECK(Compat::Refusal(build, server, build, extra).empty());
}

TEST_CASE("the refusal reaches a client as a Bye")
{
    auto       client = std::map<std::string, std::string>{ { "goods.json", "x" } };
    Proto::Bye bye;
    bye.reason = Compat::Refusal(Version::NUMBER, {}, Version::NUMBER, client);
    REQUIRE_FALSE(bye.reason.empty());
    Proto::Bye back;
    REQUIRE(Proto::DecodeBye(Proto::EncodeBye(bye), back));
    CHECK(back.reason == bye.reason);
    CHECK(Contains(back.reason, "goods.json (missing on the server)"));
}
