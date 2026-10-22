#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "entities/Nebula.h"
#include "entities/NpcShip.h"
#include "entities/Ship.h"
#include "entities/ShipType.h"
#include "sim/ClientSession.h"
#include "sim/HostRules.h"
#include "sim/SaveFile.h"
#include "sim/Simulation.h"
#include "temp_dir.h"

// Hardening before a public playtest (#341): the exploits and the ways to lose a save that a
// review found, each held to the rule that closed it.

namespace
{
const std::string DATA = TEST_DATA_DIR;

// A server's world up to the point where it would read world.json: the registries, the
// universe and the region, but no galaxy yet.
std::unique_ptr<Simulation> Unopened(bool allHot = false)
{
    SetRandomSeed(0xC0FFEEu);
    REQUIRE(Factions::Load(DATA + "factions.json"));
    REQUIRE(Archetypes::Load(DATA + "archetypes.json"));
    REQUIRE(Blueprints::Load(DATA + "blueprints.json"));
    auto sim = std::make_unique<Simulation>();
    sim->LoadUniverse(std::string(TEST_WORLD_DIR) + "universe.json");
    sim->AttachRegion(7, std::string(TEST_WORLD_DIR) + "systems/");
    sim->Seed(1234u);
    sim->SetAllHot(allHot);
    return sim;
}

std::unique_ptr<Simulation> World(bool allHot = false)
{
    auto sim = Unopened(allHot);
    sim->InitGalaxy();
    sim->MaterializeAllSystems(std::string(TEST_WORLD_DIR) + "systems/");
    return sim;
}

ClientSession& Arrive(Simulation& sim)
{
    const std::string& start = sim.Universe().startId;
    return sim.CreateSession(start, sim.SafeArrival(start), GetShipCatalog()[0].stats);
}

std::string Slurp(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void Spit(const std::string& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary);
    out << text;
}

void Tick(Simulation& sim, int ticks)
{
    const float dt = 1.0f / 60.0f;
    for (int i = 0; i < ticks; i++)
    {
        for (auto& kv : sim.Systems())
            sim.StepSystemAgents(kv.second, {}, nullptr, dt);
        sim.MaintainWorld(dt);
    }
}
}  // namespace

TEST_CASE("debug money is refused unless the server was started with --dev (#341)")
{
    auto           sim = World();
    ClientSession& s = Arrive(*sim);
    const double   before = s.account.GetMoney();

    // Anyone who could open a socket used to print sixty thousand credits a second.
    CHECK_FALSE(sim->DevCommands());
    CHECK_FALSE(sim->GrantDebugMoney(s));
    CHECK(s.account.GetMoney() == before);

    sim->SetDevCommands(true);
    CHECK(sim->GrantDebugMoney(s));
    CHECK(s.account.GetMoney() > before);
}

TEST_CASE("an interrupted save leaves the old file as it was (#341)")
{
    const TempDir     tmp;
    const std::string path = tmp.Path("world.json");
    auto              sim = World();
    REQUIRE(sim->SaveWorld(path));
    const std::string good = Slurp(path);
    REQUIRE(good.size() > 100);

    SUBCASE("a crash half way through the write is a truncated file beside it, not in it")
    {
        // What a server that died mid-write leaves behind now: the new save, half written,
        // under the temporary name. The world it would have replaced is untouched and loads.
        Spit(path + ".tmp", good.substr(0, good.size() / 2));
        CHECK(Slurp(path) == good);
        auto again = Unopened();
        CHECK(again->LoadWorld(path) == Save::Result::Ok);
        // ...and the next save goes through over the leftover.
        CHECK(sim->SaveWorld(path));
        CHECK_FALSE(std::filesystem::exists(path + ".tmp"));
        auto after = Unopened();
        CHECK(after->LoadWorld(path) == Save::Result::Ok);
    }
    SUBCASE("a write that cannot finish changes nothing")
    {
        // A folder where the temporary file would go: the write fails before anything is
        // renamed, which is what a full disk or a pulled plug looks like from here.
        std::filesystem::create_directory(path + ".tmp");
        CHECK_FALSE(Save::WriteFileAtomic(path, "{\"half\":", 3));
        CHECK(Slurp(path) == good);
    }
    SUBCASE("the saves it replaces are kept, newest first")
    {
        REQUIRE(Save::WriteFileAtomic(path, "second", Simulation::WORLD_BACKUPS));
        REQUIRE(Save::WriteFileAtomic(path, "third", Simulation::WORLD_BACKUPS));
        CHECK(Slurp(path) == "third");
        CHECK(Slurp(Save::BackupPath(path, 1)) == "second");
        CHECK(Slurp(Save::BackupPath(path, 2)) == good);
        for (int i = 0; i < 5; i++)
            REQUIRE(Save::WriteFileAtomic(path, "more", Simulation::WORLD_BACKUPS));
        CHECK_FALSE(std::filesystem::exists(Save::BackupPath(path, Simulation::WORLD_BACKUPS + 1)));
    }
}

TEST_CASE("a damaged world stops the server instead of being replaced (#341)")
{
    const TempDir     tmp;
    const std::string path = tmp.Path("world.json");
    {
        auto sim = World();
        REQUIRE(sim->SaveWorld(path));
    }
    const std::string good = Slurp(path);
    std::string       why;

    SUBCASE("a whole file is restored")
    {
        auto sim = Unopened();
        CHECK(Host::OpenWorld(*sim, path, why) == Host::WorldStart::Restored);
    }
    SUBCASE("no file is a new galaxy")
    {
        auto sim = Unopened();
        CHECK(Host::OpenWorld(*sim, tmp.Path("none.json"), why) == Host::WorldStart::Fresh);
        CHECK_FALSE(sim->Systems().empty());
    }
    SUBCASE("a truncated file is refused, says where the backup is, and is not touched")
    {
        // It used to read as "no world": a new galaxy started, and the checkpoint a minute
        // later wrote it over the old one.
        const std::string cut = good.substr(0, good.size() / 3);
        Spit(path, cut);
        auto sim = Unopened();
        CHECK(Host::OpenWorld(*sim, path, why) == Host::WorldStart::Refused);
        CHECK(why.find(".bak1") != std::string::npos);
        CHECK(Slurp(path) == cut);
    }
}

TEST_CASE("a damaged account file refuses the login instead of reading as a free name (#341)")
{
    const TempDir     tmp;
    const std::string path = tmp.Path("account_pilot.json");
    auto              sim = World();
    ClientSession&    s = Arrive(*sim);
    s.authSalt = std::string(32, 'a');
    s.authStored = std::string(64, 'b');
    REQUIRE(sim->SaveAccount(s, path));

    std::string salt, stored;
    CHECK(Host::ReadAccountAuth(tmp.Path("account_nobody.json"), salt, stored) ==
          Save::Result::Missing);
    REQUIRE(Host::ReadAccountAuth(path, salt, stored) == Save::Result::Ok);
    CHECK(salt == s.authSalt);
    CHECK(stored == s.authStored);

    // Cut off half way, as a crash during a write in place used to leave it. Before, the
    // server saw no secret, offered a new account, and the first person to log in had it.
    const std::string good = Slurp(path);
    Spit(path, good.substr(0, good.size() / 2));
    CHECK(Host::ReadAccountAuth(path, salt, stored) == Save::Result::Corrupt);
    CHECK(salt.empty());
    CHECK(stored.empty());
    Spit(path, "{\"auth\": 7}");
    CHECK(Host::ReadAccountAuth(path, salt, stored) == Save::Result::Corrupt);
}

TEST_CASE("Pilot and pilot are one account (#341)")
{
    // NTFS does not tell them apart, so they were two sessions on one file -- items carried
    // by one could be sold by the other and saved back.
    CHECK(Host::CanonicalAccount("Pilot") == "pilot");
    CHECK(Host::CanonicalAccount("PILOT") == Host::CanonicalAccount("pilot"));
    CHECK(Host::CanonicalAccount("Ann_B-2") == "ann_b-2");
    // ...and what was never a name still is not one.
    CHECK(Host::CanonicalAccount("").empty());
    CHECK(Host::CanonicalAccount("a b").empty());
    CHECK(Host::CanonicalAccount("../etc").empty());
    CHECK(Host::CanonicalAccount(std::string(33, 'x')).empty());

    // An account saved under its capitalised name before this is still found, whatever the
    // file system thinks of case.
    const TempDir     tmp;
    const std::string dir = tmp.Path("");
    Spit(tmp.Path("account_Pilot.json"), "{}");
    const std::string found = Host::AccountFile(dir, "pilot");
    CHECK(std::filesystem::exists(found));
    CHECK(Slurp(found) == "{}");
    // A name with no file is the canonical name, ready to be written.
    CHECK(Host::AccountFile(dir, "ann") == dir + "account_ann.json");
}

TEST_CASE("accounts move in beside their world, and none is lost or written over (#392)")
{
    const TempDir     tmp;
    const std::string world = tmp.Path("world.json");
    const std::string to = Host::AccountsDir(world);
    CHECK(to == tmp.Path("accounts/"));
    CHECK(Host::AccountsDir("world.json") == "accounts/");
    CHECK(Host::AccountsDir("C:\\saves\\world.json") == "C:\\saves\\accounts/");

    // Beside the executable, where they were: two accounts with an older copy, a write that
    // never finished, and files that are not accounts at all.
    Spit(tmp.Path("account_ann.json"), "{\"money\": 1}");
    Spit(tmp.Path("account_ann.json.bak1"), "{\"money\": 0}");
    Spit(tmp.Path("account_bob.json"), "{\"money\": 2}");
    Spit(tmp.Path("account_cid.json.tmp"), "{\"mo");
    Spit(tmp.Path("world.json"), "{}");
    Spit(tmp.Path("account_notes.txt"), "x");
    // ...and bob is already in accounts/: that one is in use, and the old one stays put.
    tmp.MakeDir("accounts");
    Spit(tmp.Path("accounts/account_bob.json"), "{\"money\": 99}");

    const Host::AccountMove m = Host::MoveAccounts(tmp.Path(""), to);
    CHECK(m.error.empty());
    CHECK(m.moved == 2);
    REQUIRE(m.kept.size() == 1);
    CHECK(m.kept[0].find("account_bob.json") != std::string::npos);
    CHECK(Slurp(to + "account_ann.json") == "{\"money\": 1}");
    CHECK(Slurp(to + "account_ann.json.bak1") == "{\"money\": 0}");
    CHECK_FALSE(std::filesystem::exists(tmp.Path("account_ann.json")));
    CHECK(Slurp(to + "account_bob.json") == "{\"money\": 99}");
    CHECK(Slurp(tmp.Path("account_bob.json")) == "{\"money\": 2}");
    CHECK(std::filesystem::exists(tmp.Path("account_cid.json.tmp")));
    CHECK_FALSE(std::filesystem::exists(to + "account_cid.json.tmp"));
    CHECK(std::filesystem::exists(tmp.Path("world.json")));
    CHECK(std::filesystem::exists(tmp.Path("account_notes.txt")));
    CHECK(Host::AccountFile(to, "ann") == to + "account_ann.json");

    // Again, as every start does: nothing more to move, and bob is still left alone.
    const Host::AccountMove again = Host::MoveAccounts(tmp.Path(""), to);
    CHECK(again.moved == 0);
    CHECK(again.kept.size() == 1);
    CHECK(Slurp(to + "account_bob.json") == "{\"money\": 99}");

    // A folder that cannot be made -- a file is in the way -- moves nothing.
    Spit(tmp.Path("blocked"), "x");
    tmp.MakeDir("src");
    Spit(tmp.Path("src/account_dee.json"), "{}");
    const Host::AccountMove blocked = Host::MoveAccounts(tmp.Path("src/"), tmp.Path("blocked/"));
    CHECK_FALSE(blocked.error.empty());
    CHECK(std::filesystem::exists(tmp.Path("src/account_dee.json")));
}

TEST_CASE("sending the galaxy to clients does not change how the galaxy counts its losses (#341)")
{
    // BuildGalaxyState used to recount every system's population once a second while anyone
    // was connected. The maintenance pass reads a fall in that count as ships lost, so the
    // recount ate the losses, and a watched galaxy behaved differently from one nobody saw.
    auto run = [](bool watched)
    {
        auto sim = World(true);
        Tick(*sim, 150);
        // One pirate gone, the same one in both runs, as if shot down.
        std::string where;
        for (auto& kv : sim->Systems())
        {
            auto& ents = kv.second.entities;
            for (auto it = ents.begin(); it != ents.end() && where.empty(); ++it)
                if ((*it)->GetKind() == EntityKind::Npc &&
                    static_cast<NpcShip*>(it->get())->GetRole() == NpcRole::Pirate)
                {
                    ents.erase(it);
                    where = kv.first;
                    break;
                }
            if (!where.empty())
                break;
        }
        REQUIRE_FALSE(where.empty());
        if (watched)
            sim->GalaxyStats();
        Tick(*sim, 150);
        const SystemAggregate& a = sim->SystemById(where)->agg;
        return std::vector<float>{ a.supPirates, a.lostPirates, a.pirates, a.security };
    };
    const std::vector<float> unwatched = run(false);
    const std::vector<float> watched = run(true);
    CHECK(unwatched[0] > 0.0f);  // the loss was counted at all
    CHECK(watched == unwatched);
}

TEST_CASE("a disconnected ship stays in space for about thirty seconds (#341)")
{
    auto           sim = World();
    ClientSession& s = Arrive(*sim);
    const int      id = s.id;
    CHECK_FALSE(s.Lingering());

    // Pulling the cable used to take the ship out of a fight at once.
    REQUIRE(sim->BeginLinger(s));
    CHECK(sim->StepLinger(29.0f).empty());
    REQUIRE(sim->Session(id) != nullptr);
    CHECK(sim->Session(id)->Lingering());
    const std::vector<int> over = sim->StepLinger(2.0f);
    REQUIRE(over.size() == 1);
    CHECK(over[0] == id);

    // A docked ship goes at once: the station is a refuge either way.
    ClientSession& d = Arrive(*sim);
    d.dockedStationId = 1;
    CHECK_FALSE(sim->BeginLinger(d));
    CHECK_FALSE(d.Lingering());
}

TEST_CASE("a nebula hides a player from other players, not only from NPCs (#341)")
{
    auto           sim = World();
    ClientSession& hider = Arrive(*sim);
    ClientSession& seeker = Arrive(*sim);
    REQUIRE(hider.systemId == seeker.systemId);
    const int hiderShip = hider.ship->GetId();

    auto sees = [&](const ClientSession& who)
    {
        for (const Proto::EntitySnapshot& e : sim->BuildSnapshot(who, who.systemId).entities)
            if (e.id == hiderShip)
                return true;
        return false;
    };
    CHECK(sees(seeker));
    CHECK_FALSE(sim->InCover(hider));

    const Vector2 at = { hider.ship->GetPosition().x + 50000.0f, hider.ship->GetPosition().y };
    REQUIRE(sim->AddStatic(hider.systemId, std::make_unique<Nebula>(at, 4000.0f, "Shroud"), "") !=
            0);
    hider.ship->Teleport(at);
    CHECK(sim->InCover(hider));
    CHECK_FALSE(sees(seeker));

    // Out of it, seen again.
    hider.ship->Teleport({ at.x + 20000.0f, at.y });
    CHECK(sees(seeker));
}

TEST_CASE("connections are limited in all and per address (#341)")
{
    Host::Gate::Limits l;
    l.maxClients = 5;
    l.maxPerAddress = 2;
    const Host::Gate gate(l);
    CHECK(gate.RefuseConnection(0, 0).empty());
    CHECK(gate.RefuseConnection(4, 1).empty());
    CHECK_FALSE(gate.RefuseConnection(4, 2).empty());                   // this address has its two
    CHECK_FALSE(gate.RefuseConnection(5, 0).empty());                   // the server is full
    CHECK(gate.RefuseConnection(5, 0) != gate.RefuseConnection(4, 2));  // and says which
}

TEST_CASE("new accounts from one address are limited per hour (#341)")
{
    Host::Gate::Limits l;
    l.registrationsPerHour = 3;
    Host::Gate gate(l);
    for (int i = 0; i < 3; i++)
    {
        REQUIRE(gate.MayRegister("10.0.0.1", 100.0 + i));
        gate.Registered("10.0.0.1", 100.0 + i);
    }
    CHECK_FALSE(gate.MayRegister("10.0.0.1", 200.0));
    CHECK(gate.MayRegister("10.0.0.2", 200.0));                 // another address is not held to it
    CHECK(gate.MayRegister("10.0.0.1", 100.0 + 3600.0 + 1.0));  // an hour on, it may again
}

TEST_CASE("each failed login doubles the wait before the next, and a success clears it (#341)")
{
    Host::Gate        gate;
    const std::string a = "10.0.0.1";
    CHECK(gate.LoginWait(a, "pilot", 0.0) == 0.0);
    gate.LoginFailed(a, "pilot", 0.0);
    CHECK(gate.LoginWait(a, "pilot", 0.0) == doctest::Approx(1.0));
    CHECK(gate.LoginWait(a, "pilot", 1.0) == 0.0);
    gate.LoginFailed(a, "pilot", 1.0);
    CHECK(gate.LoginWait(a, "pilot", 1.0) == doctest::Approx(2.0));
    gate.LoginFailed(a, "pilot", 3.0);
    CHECK(gate.LoginWait(a, "pilot", 3.0) == doctest::Approx(4.0));
    for (int i = 0; i < 20; i++)
        gate.LoginFailed(a, "pilot", 10.0);
    CHECK(gate.LoginWait(a, "pilot", 10.0) == doctest::Approx(gate.GetLimits().maxBackoff));

    // Only that account from that address: the owner elsewhere, and other names, are free.
    CHECK(gate.LoginWait("10.0.0.2", "pilot", 10.0) == 0.0);
    CHECK(gate.LoginWait(a, "ann", 10.0) == 0.0);

    gate.LoginSucceeded(a, "pilot");
    CHECK(gate.LoginWait(a, "pilot", 10.0) == 0.0);

    // An hour of quiet forgets a run of failures.
    gate.LoginFailed(a, "ann", 0.0);
    gate.LoginFailed(a, "ann", 0.0);
    gate.Expire(4000.0);
    gate.LoginFailed(a, "ann", 4000.0);
    CHECK(gate.LoginWait(a, "ann", 4000.0) == doctest::Approx(1.0));
}
