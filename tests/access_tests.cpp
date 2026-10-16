#include <doctest/doctest.h>

#include <fstream>
#include <iterator>
#include <string>

#include "sim/AccessList.h"
#include "sim/ServerLog.h"
#include "temp_dir.h"

// Who may play on a friends' server, and what the server says about itself (#400).

namespace
{

void WriteText(const std::string& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary);
    out << text;
}

std::string ReadText(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

}  // namespace

TEST_CASE("a public server checks its list unless told registration is open (#400)")
{
    using Host::AccessList;
    // A server on the internet with no list lets nobody in, not everybody.
    CHECK(AccessList::ListRequired(true, false, false));
    CHECK(AccessList::ListRequired(true, false, true));
    // This machine only: the list is checked only if the owner has written one.
    CHECK_FALSE(AccessList::ListRequired(false, false, false));
    CHECK(AccessList::ListRequired(false, false, true));
    // --open wins over both.
    CHECK_FALSE(AccessList::ListRequired(true, true, true));
}

TEST_CASE("only a listed name comes in, and the list is read as the owner wrote it (#400)")
{
    TempDir           dir;
    const std::string folder = dir.MakeDir("world") + "/";
    WriteText(folder + "allowlist.txt", "# friends\r\nAlice\n  bob   # from work\n\nmal.lory!\n");

    Host::AccessList access;
    access.Open(folder, true);
    CHECK(access.Refusal("alice").empty());  // names are canonical: Alice is alice
    CHECK(access.Refusal("bob").empty());
    const std::string refused = access.Refusal("mallory");
    CHECK(refused.find("mallory") != std::string::npos);
    CHECK(refused.find("not on its list") != std::string::npos);
    // A line that is not a name is reported, not silently dropped.
    REQUIRE(access.Problems().size() == 1);
    CHECK(access.Problems()[0].find(":5:") != std::string::npos);

    SUBCASE("an open server lets anyone in")
    {
        Host::AccessList open;
        open.Open(folder, false);
        CHECK(open.Refusal("mallory").empty());
    }
    SUBCASE("a list that is required and missing lets nobody in")
    {
        Host::AccessList none;
        none.Open(dir.MakeDir("empty") + "/", true);
        CHECK_FALSE(none.Refusal("alice").empty());
    }
}

TEST_CASE("allow and deny edit the file line by line and keep the owner's comments (#400)")
{
    TempDir           dir;
    const std::string folder = dir.MakeDir("world") + "/";
    WriteText(folder + "allowlist.txt", "# friends only\nalice\n");
    Host::AccessList access;
    access.Open(folder, true);
    const int before = access.Generation();

    bool ok = false;
    access.Allow("Carol", ok);
    CHECK(ok);
    CHECK(access.Refusal("carol").empty());
    CHECK(access.Generation() != before);  // the host removes whoever is now refused
    access.Allow("carol", ok);
    CHECK_FALSE(ok);  // already there
    access.Allow("no spaces", ok);
    CHECK_FALSE(ok);

    access.Deny("ALICE", ok);
    CHECK(ok);
    CHECK_FALSE(access.Refusal("alice").empty());
    access.Deny("alice", ok);
    CHECK_FALSE(ok);  // not there any more

    CHECK(ReadText(folder + "allowlist.txt") == "# friends only\ncarol\n");
}

TEST_CASE("a ban refuses with its reason, listed or not, and unban lifts it (#400)")
{
    TempDir           dir;
    const std::string folder = dir.MakeDir("world") + "/";
    Host::AccessList  access;
    access.Open(folder, false);  // bans are checked on an open server too

    bool ok = false;
    access.Ban("Mallory", "griefing # the new players", ok);
    CHECK(ok);
    const std::string why = access.Refusal("mallory");
    CHECK(why.find("banned") != std::string::npos);
    CHECK(why.find("griefing") != std::string::npos);

    // A second ban replaces the reason rather than adding a line.
    access.Ban("mallory", "spam", ok);
    CHECK(ok);
    CHECK(access.Refusal("mallory").find("spam") != std::string::npos);
    CHECK(access.Bans().size() == 1);

    // A ban beats the allowlist.
    access.Allow("mallory", ok);
    CHECK_FALSE(access.Refusal("mallory").empty());

    access.Unban("mallory", ok);
    CHECK(ok);
    CHECK(access.Refusal("mallory").empty());
    access.Unban("mallory", ok);
    CHECK_FALSE(ok);
}

TEST_CASE("a file edited by hand is read again when it changes (#400)")
{
    TempDir           dir;
    const std::string folder = dir.MakeDir("world") + "/";
    Host::AccessList  access;
    access.Open(folder, true);
    CHECK_FALSE(access.ReloadIfChanged());  // nothing changed
    CHECK_FALSE(access.Refusal("dave").empty());

    WriteText(folder + "allowlist.txt", "dave\n");
    CHECK(access.ReloadIfChanged());
    CHECK(access.Refusal("dave").empty());

    WriteText(folder + "bans.txt", "dave cheating\n");
    CHECK(access.ReloadIfChanged());
    CHECK(access.Refusal("dave").find("cheating") != std::string::npos);
}

TEST_CASE("log lines carry an ISO time in UTC, once per line (#400)")
{
    CHECK(Log::IsoTime(0) == "1970-01-01T00:00:00.000Z");
    // 2026-10-16 09:41:07.123 UTC.
    CHECK(Log::IsoTime(1792143667123LL) == "2026-10-16T09:41:07.123Z");
    CHECK(Log::IsoTime(951782400000LL) == "2000-02-29T00:00:00.000Z");  // a leap day
    CHECK(Log::IsoTime(-1) == "1969-12-31T23:59:59.999Z");
    CHECK(Log::Now().size() == 24);

    bool start = true;
    CHECK(Log::StampLines("one\ntwo\n", "T", start) == "T one\nT two\n");
    CHECK(start);
    // A line written in pieces is stamped at its start only.
    CHECK(Log::StampLines("tide:", "T", start) == "T tide:");
    CHECK_FALSE(start);
    CHECK(Log::StampLines(" R3 12", "T", start) == " R3 12");
    CHECK(Log::StampLines("\n", "T", start) == "\n");
    CHECK(start);
}

TEST_CASE("the metrics line reports the minute just gone and starts afresh (#400)")
{
    Log::Metrics m;
    m.Tick(1.0);
    m.Tick(3.0);
    m.SetSent(60.0 * 1024.0);
    const std::string line = m.Line(2, 3, 1, 60.0);
    CHECK(line.find("2 player(s)") != std::string::npos);
    CHECK(line.find("3 connection(s)") != std::string::npos);
    CHECK(line.find("1 hot system(s)") != std::string::npos);
    CHECK(line.find("tick 2.00 ms mean, 3.00 ms max over 2") != std::string::npos);
    CHECK(line.find("out 60.0 KB (1.0 KB/s)") != std::string::npos);

    // Bytes are counted from the last line, not from the start.
    m.SetSent(90.0 * 1024.0);
    const std::string next = m.Line(0, 0, 0, 30.0);
    CHECK(next.find("over 0") != std::string::npos);
    CHECK(next.find("out 30.0 KB (1.0 KB/s)") != std::string::npos);
}
