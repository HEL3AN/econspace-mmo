#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "sim/ServerConfig.h"
#include "sim/ServerLog.h"
#include "sim/ServerOps.h"
#include "temp_dir.h"

// Running a friends' server on a box nobody sits at (#400): its settings, its world folder,
// its backups, its log files and the console reached from outside.

namespace
{

namespace fs = std::filesystem;

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

std::string Dir(const TempDir& t)
{
    return t.Path("");
}

}  // namespace

TEST_CASE("server.json is read strictly, with comments (#400)")
{
    Host::ServerConfig c;
    std::string        error;
    REQUIRE(Host::ParseServerConfig("// a comment\n{ \"port\": 51000, \"public\": true,\n"
                                    "  \"registration\": \"open\", // and another\n"
                                    "  \"centres\": 4, \"maxRegions\": 60, \"seed\": 12,\n"
                                    "  \"randomStarts\": true, \"backupsKept\": 3 }",
                                    c, error));
    CHECK(c.port == 51000);
    CHECK(c.isPublic);
    CHECK(c.registration == Host::Registration::Open);
    CHECK(c.centres == 4);
    CHECK(c.maxRegions == 60);
    CHECK(c.maxSystems == 0);  // not given: as it was
    CHECK(c.seed == 12);
    CHECK(c.randomStarts);
    CHECK_FALSE(c.dev);
    CHECK(c.backupsKept == 3);

    // A misspelt key is an error naming it, and the settings are left as they were.
    Host::ServerConfig before = c;
    CHECK_FALSE(Host::ParseServerConfig("{ \"pubilc\": false }", c, error));
    CHECK(error.find("pubilc") != std::string::npos);
    CHECK(c.isPublic == before.isPublic);
    // ...and so is a value of the wrong kind or out of range.
    CHECK_FALSE(Host::ParseServerConfig("{ \"public\": \"yes\" }", c, error));
    CHECK_FALSE(Host::ParseServerConfig("{ \"port\": 70000 }", c, error));
    CHECK_FALSE(Host::ParseServerConfig("{ \"centres\": 9 }", c, error));
    CHECK_FALSE(Host::ParseServerConfig("{ \"registration\": \"closed\" }", c, error));
    CHECK_FALSE(Host::ParseServerConfig("{ \"seed\": -1 }", c, error));
    CHECK_FALSE(Host::ParseServerConfig("[1, 2]", c, error));
    CHECK_FALSE(Host::ParseServerConfig("{ \"port\": ", c, error));
    CHECK(c.port == 51000);
}

TEST_CASE("the file init writes reads back as the settings it was written for (#400)")
{
    Host::ServerConfig c;
    c.port = 50900;
    c.isPublic = true;
    c.registration = Host::Registration::Allowlist;
    c.maxSystems = 1500;
    c.seed = 18446744073709551615ull;  // the largest seed there is
    c.dev = true;
    Host::ServerConfig back;
    std::string        error;
    REQUIRE(Host::ParseServerConfig(Host::ServerConfigText(c), back, error));
    CHECK(back.port == 50900);
    CHECK(back.isPublic);
    CHECK(back.registration == Host::Registration::Allowlist);
    CHECK(back.maxSystems == 1500);
    CHECK(back.seed == c.seed);
    CHECK(back.dev);
}

TEST_CASE("the host's flags go on top of server.json, and a typo is an error (#400)")
{
    Host::ServerConfig c;
    c.isPublic = true;
    c.port = 50800;
    std::string world, packs, error;
    REQUIRE(Host::ApplyHostFlags({ "51234", "--world", "w", "--private", "--open", "--centres", "2",
                                   "--max-regions", "60", "--seed", "99", "--dev" },
                                 c, world, packs, error));
    CHECK(c.port == 51234);  // `host <port>`, as it always was
    CHECK(world == "w");
    CHECK_FALSE(c.isPublic);
    CHECK(c.registration == Host::Registration::Open);
    CHECK(c.centres == 2);
    CHECK(c.maxRegions == 60);
    CHECK(c.seed == 99);
    CHECK(c.dev);

    REQUIRE(
        Host::ApplyHostFlags({ "--port", "0", "--allowlist", "--public" }, c, world, packs, error));
    CHECK(c.port == 0);
    CHECK(c.isPublic);
    CHECK(c.registration == Host::Registration::Allowlist);

    Host::ServerConfig kept = c;
    CHECK_FALSE(Host::ApplyHostFlags({ "--pubilc" }, c, world, packs, error));
    CHECK(error.find("--pubilc") != std::string::npos);
    CHECK_FALSE(Host::ApplyHostFlags({ "--centres" }, c, world, packs, error));
    CHECK_FALSE(Host::ApplyHostFlags({ "--centres", "seven" }, c, world, packs, error));
    CHECK_FALSE(Host::ApplyHostFlags({ "--port", "99999" }, c, world, packs, error));
    // A port after the first word is not one.
    CHECK_FALSE(Host::ApplyHostFlags({ "--dev", "50800" }, c, world, packs, error));
    CHECK(c.port == kept.port);
}

TEST_CASE("init makes a world directory and never writes over one (#400)")
{
    TempDir            t;
    const std::string  dir = Dir(t) + "friends";
    Host::ServerConfig c;
    c.isPublic = true;
    std::string error;
    REQUIRE(Host::InitWorldDir(dir, c, error));
    CHECK(fs::exists(dir + "/server.json"));
    CHECK(fs::exists(dir + "/allowlist.txt"));

    Host::ServerConfig back;
    bool               exists = false;
    REQUIRE(Host::LoadServerConfig(dir + "/server.json", back, exists, error));
    CHECK(exists);
    CHECK(back.isPublic);
    // Written as an allowlist when nothing chose: the open path is the one that leaks (#351).
    CHECK(back.registration == Host::Registration::Allowlist);

    // The owner's edits are kept: a second init changes nothing.
    WriteText(dir + "/server.json", "{ \"port\": 1 }");
    CHECK_FALSE(Host::InitWorldDir(dir, c, error));
    CHECK(ReadText(dir + "/server.json") == "{ \"port\": 1 }");

    // No file is no settings, not an error.
    Host::ServerConfig none;
    REQUIRE(Host::LoadServerConfig(Dir(t) + "nothing/server.json", none, exists, error));
    CHECK_FALSE(exists);
    CHECK(none.port == 50800);
}

TEST_CASE("a backup copies the world and nothing else, and keeps the newest (#400)")
{
    TempDir           t;
    const std::string dir = Dir(t);
    WriteText(dir + "world.json", "{\"w\":1}");
    WriteText(dir + "world.json.tmp", "half");
    WriteText(dir + "server.json", "{}");
    WriteText(dir + "bans.txt", "eve cheating\n");
    fs::create_directories(dir + "accounts");
    WriteText(dir + "accounts/account_bob.json", "{\"bob\":1}");
    WriteText(dir + "accounts/account_bob.json.tmp", "half");
    fs::create_directories(dir + "regions");
    WriteText(dir + "regions/7.json", "{}");
    // What shares the folder and is not the world: a log, the executable, data/.
    fs::create_directories(dir + "logs");
    WriteText(dir + "logs/server-2026-10-17.log", "lines");
    WriteText(dir + "econserver", "binary");
    fs::create_directories(dir + "data");

    CHECK(Host::BackupName("2026-10-17T09:41:07.123Z") == "2026-10-17T09-41-07Z");

    std::string made, error;
    REQUIRE(Host::BackupWorld(dir, "2026-10-17T09-41-07Z", 2, made, error));
    const std::string b = made + "/";
    CHECK(ReadText(b + "world.json") == "{\"w\":1}");
    CHECK(ReadText(b + "accounts/account_bob.json") == "{\"bob\":1}");
    CHECK(fs::exists(b + "regions/7.json"));
    CHECK(fs::exists(b + "bans.txt"));
    CHECK(fs::exists(b + "server.json"));
    CHECK_FALSE(fs::exists(b + "world.json.tmp"));
    CHECK_FALSE(fs::exists(b + "accounts/account_bob.json.tmp"));
    CHECK_FALSE(fs::exists(b + "logs"));
    CHECK_FALSE(fs::exists(b + "econserver"));
    CHECK_FALSE(fs::exists(b + "data"));
    CHECK_FALSE(fs::exists(b + "backups"));

    // A second in the same second is numbered, not written over.
    REQUIRE(Host::BackupWorld(dir, "2026-10-17T09-41-07Z", 2, made, error));
    CHECK(made.find("2026-10-17T09-41-07Z-2") != std::string::npos);
    // Two are kept: the oldest goes. Something of the owner's in backups/ is not touched.
    fs::create_directories(dir + "backups/before-the-update");
    REQUIRE(Host::BackupWorld(dir, "2026-10-18T00-00-00Z", 2, made, error));
    CHECK_FALSE(fs::exists(dir + "backups/2026-10-17T09-41-07Z"));
    CHECK(fs::exists(dir + "backups/2026-10-17T09-41-07Z-2"));
    CHECK(fs::exists(dir + "backups/2026-10-18T00-00-00Z"));
    CHECK(fs::exists(dir + "backups/before-the-update"));

    // An empty folder is not a world to back up.
    TempDir empty;
    CHECK_FALSE(Host::BackupWorld(Dir(empty), "2026-10-18T00-00-00Z", 0, made, error));
}

TEST_CASE("the admin channel carries a command and its answer, and nothing stale (#400)")
{
    TempDir           t;
    const std::string dir = Dir(t);
    std::string       answer;
    // No server has ever run here.
    CHECK_FALSE(Host::AdminRequest(dir, "status", answer, 0.2, 0.2));

    // A command left from a server that is gone is not run by the next one.
    fs::create_directories(dir + "admin");
    WriteText(dir + "admin/000000000000001-00000000.cmd", "shutdown\n");
    Host::AdminChannel server;
    std::string        error;
    REQUIRE(server.Open(dir, error));
    CHECK(server.Take().empty());

    // A command given while the server runs: taken once, answered, the answer read back.
    std::thread admin([&]() { Host::AdminRequest(dir, "kick bob  being rude", answer, 5.0, 5.0); });
    std::vector<Host::AdminChannel::Request> got;
    for (int i = 0; i < 200 && got.empty(); i++)
    {
        got = server.Take();
        if (got.empty())
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(got.size() == 1);
    CHECK(got[0].command == "kick bob  being rude");
    CHECK(server.Take().empty());
    server.Answer(got[0].id, "bob kicked.\n");
    admin.join();
    CHECK(answer == "bob kicked.\n");

    // Nobody takes it: it is withdrawn, so a server started later does not run it.
    CHECK_FALSE(Host::AdminRequest(dir, "shutdown", answer, 0.2, 0.2));
    CHECK(server.Take().empty());
}

TEST_CASE("the log goes into a file a day, and a command's lines can be kept (#400)")
{
    TempDir t;
    CHECK(Log::FileName(0) == "server-1970-01-01.log");
    CHECK(Log::FileName(86400000LL * 365) == "server-1971-01-01.log");

    REQUIRE(Log::SetFileDir(Dir(t) + "logs"));
    Log::BeginCapture();
    Log::Printf("first line\nsecond ");
    Log::Printf("half\n");
    const std::string captured = Log::EndCapture();
    Log::Printf("not captured\n");
    Log::SetFileDir(std::string());
    CHECK(captured == "first line\nsecond half\n");

    std::string text;
    for (const auto& e : fs::directory_iterator(Dir(t) + "logs"))
    {
        CHECK(e.path().filename().string().rfind("server-", 0) == 0);
        text += ReadText(e.path().string());
    }
    // Stamped at each line's start, in the file whatever stderr is.
    CHECK(text.find("Z first line\n") != std::string::npos);
    CHECK(text.find("Z second half\n") != std::string::npos);
    CHECK(text.find("Z not captured\n") != std::string::npos);

    // A metrics peek leaves the line to come whole.
    Log::Metrics m;
    m.Tick(1.0);
    m.Tick(3.0);
    CHECK(m.Ticks() == 2);
    CHECK(m.MeanMs() == doctest::Approx(2.0));
    CHECK(m.MaxMs() == doctest::Approx(3.0));
    CHECK(m.Line(0, 0, 0, 60.0).find("over 2") != std::string::npos);
}
