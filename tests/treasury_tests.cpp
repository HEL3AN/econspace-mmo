#include <doctest/doctest.h>

#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "entities/ShipType.h"
#include "sim/ClientSession.h"
#include "sim/Corporation.h"
#include "sim/Protocol.h"
#include "sim/Simulation.h"
#include "temp_dir.h"

#include <nlohmann/json.hpp>

#include <string>

// A corporation's treasury (#509): any member puts their own money in, the CEO or a director
// takes it out, and a ledger keeps what went in and out -- the pilots' own acts beside the
// automatic flows (fees, rent, police, patrols).

namespace
{
constexpr double NOW = 1.8e9;  // Unix seconds

// Deep Haul: anna (CEO), boris (director), dima (member), with a session for each.
struct Haul
{
    Simulation sim;

    Haul()
    {
        REQUIRE(Factions::Load(std::string(TEST_DATA_DIR) + "factions.json"));
        REQUIRE(Archetypes::Load(std::string(TEST_DATA_DIR) + "archetypes.json"));
        REQUIRE(Blueprints::Load(std::string(TEST_DATA_DIR) + "blueprints.json"));
        sim.LoadUniverse(std::string(TEST_WORLD_DIR) + "universe.json");
        sim.Seed(1234u);
        sim.InitGalaxy();
        sim.MaterializeAllSystems(std::string(TEST_WORLD_DIR) + "systems/");
        Corps::Registry& r = sim.Corporations();
        REQUIRE(r.Create("anna", "Deep Haul", "DH", NOW).Ok());
        for (const char* who : { "boris", "dima" })
        {
            REQUIRE(r.InviteTo("anna", who, { who }, NOW).Ok());
            REQUIRE(r.Join(who, "DH", NOW).Ok());
        }
        REQUIRE(r.SetRole("anna", "boris", Corps::Role::Director).Ok());
        for (const char* who : { "anna", "boris", "dima" })
        {
            ClientSession& s =
                sim.CreateSession("core", Vector2{ 0.0f, 0.0f }, GetShipCatalog()[0].stats);
            s.accountName = who;
            s.account.SetMoney(1000.0);
        }
    }
    ClientSession& Of(const std::string& account)
    {
        for (auto& kv : sim.Sessions())
            if (kv.second.accountName == account)
                return kv.second;
        FAIL("no session");
        return sim.Sessions().begin()->second;
    }
    const Corps::Corp& Corp() { return *sim.Corporations().Of("anna"); }
    Corps::Outcome     Do(const std::string& who, const std::string& verb, const std::string& v,
                          double at = NOW)
    {
        Corps::Act a;
        a.verb = verb;
        a.value = v;
        return sim.ApplyCorpAct(who, a, {}, at);
    }
};
}  // namespace

TEST_CASE("any member deposits their own money; the CEO and a director withdraw (#509)")
{
    Haul h;
    // A plain member puts money in.
    const Corps::Outcome in = h.Do("dima", "deposit", "400");
    REQUIRE(in.Ok());
    CHECK(h.Corp().treasury == doctest::Approx(400.0));
    CHECK(h.Of("dima").account.GetMoney() == doctest::Approx(600.0));

    // ...but not the other way: a member takes nothing out.
    const Corps::Outcome no = h.Do("dima", "withdraw", "100");
    CHECK_FALSE(no.Ok());
    CHECK(no.told.find("CEO or a director") != std::string::npos);
    CHECK(h.Corp().treasury == doctest::Approx(400.0));
    CHECK(h.Of("dima").account.GetMoney() == doctest::Approx(600.0));

    // A director and the CEO do, to themselves, with no cap but the balance.
    REQUIRE(h.Do("boris", "withdraw", "150").Ok());
    CHECK(h.Of("boris").account.GetMoney() == doctest::Approx(1150.0));
    REQUIRE(h.Do("anna", "withdraw", "250").Ok());
    CHECK(h.Of("anna").account.GetMoney() == doctest::Approx(1250.0));
    CHECK(h.Corp().treasury == doctest::Approx(0.0));
}

TEST_CASE("a deposit or a withdrawal the money is not there for is refused, and says why (#509)")
{
    Haul           h;
    Corps::Outcome o = h.Do("dima", "deposit", "5000");
    CHECK_FALSE(o.Ok());
    CHECK(o.told.find("you hold 1000 cr") != std::string::npos);
    CHECK(h.Of("dima").account.GetMoney() == doctest::Approx(1000.0));
    CHECK(h.Corp().treasury == doctest::Approx(0.0));

    o = h.Do("anna", "withdraw", "1");
    CHECK_FALSE(o.Ok());
    CHECK(o.told.find("holds 0 cr") != std::string::npos);
    CHECK(h.Of("anna").account.GetMoney() == doctest::Approx(1000.0));

    // Not a number, nothing, a negative, a fraction below one: all refused, nothing moves.
    for (const char* bad : { "", "abc", "-5", "0", "0.5", "10 cr" })
    {
        CHECK_FALSE(h.Do("anna", "deposit", bad).Ok());
        CHECK_FALSE(h.Do("anna", "withdraw", bad).Ok());
    }
    CHECK(h.Of("anna").account.GetMoney() == doctest::Approx(1000.0));
    CHECK(h.Corp().ledger.empty());

    // A pilot in no corporation has no treasury to pay into.
    ClientSession& e =
        h.sim.CreateSession("core", Vector2{ 0.0f, 0.0f }, GetShipCatalog()[0].stats);
    e.accountName = "erik";
    e.account.SetMoney(100.0);
    CHECK_FALSE(h.Do("erik", "deposit", "10").Ok());
    CHECK(e.account.GetMoney() == doctest::Approx(100.0));
}

TEST_CASE("the ledger keeps who, what and when; the automatic flows join into one line (#509)")
{
    Haul             h;
    Corps::Registry& r = h.sim.Corporations();
    r.SetClock(NOW);
    REQUIRE(h.Do("dima", "deposit", "300").Ok());
    // Each automatic flow is a line of its own kind, and a run of them one line.
    const std::string id = h.Corp().id;
    r.SetClock(NOW + 60.0);
    for (int i = 0; i < 5; i++)
        REQUIRE(r.Credit(id, 10.0, "fees"));
    REQUIRE(r.Debit(id, 40.0, "police", "", "Helios Core"));
    REQUIRE(r.Debit(id, 20.0, "police", "", "Helios Core"));
    REQUIRE(r.Debit(id, 30.0, "patrol", "", "Helios Core"));
    r.SetClock(NOW + 120.0);
    REQUIRE(h.Do("boris", "withdraw", "100", NOW + 120.0).Ok());

    const auto& l = h.Corp().ledger;
    REQUIRE(l.size() == 5);
    CHECK(l[0].kind == "deposit");
    CHECK(l[0].who == "dima");
    CHECK(l[0].amount == doctest::Approx(300.0));
    CHECK(l[0].at == (long long)NOW);
    CHECK(l[1].kind == "fees");
    CHECK(l[1].count == 5);
    CHECK(l[1].amount == doctest::Approx(50.0));
    CHECK(l[2].kind == "police");
    CHECK(l[2].note == "Helios Core");
    CHECK(l[2].count == 2);
    CHECK(l[2].amount == doctest::Approx(-60.0));
    CHECK(l[3].kind == "patrol");
    CHECK(l[4].kind == "withdraw");
    CHECK(l[4].who == "boris");
    CHECK(l[4].amount == doctest::Approx(-100.0));
    CHECK(l[4].at == (long long)NOW + 120);

    // A pilot's own act is never folded into another's.
    REQUIRE(h.Do("dima", "deposit", "5").Ok());
    REQUIRE(h.Do("dima", "deposit", "5").Ok());
    CHECK(h.Corp().ledger.size() == 7);

    // Funding with no kind (a test's) leaves no line.
    REQUIRE(r.Credit(id, 1.0));
    CHECK(h.Corp().ledger.size() == 7);

    // ...and it is a bounded list: the oldest go first.
    for (size_t i = 0; i < Corps::LEDGER_CAP + 20; i++)
    {
        r.SetClock(NOW + 1000.0 + (double)i * 7200.0);  // too far apart to join
        REQUIRE(r.Credit(id, 1.0, "fees"));
    }
    CHECK(h.Corp().ledger.size() == Corps::LEDGER_CAP);
    CHECK(h.Corp().ledger.front().kind == "fees");
}

TEST_CASE("the ledger outlives a restart, and an older reader ignores it (#509)")
{
    Haul h;
    REQUIRE(h.Do("dima", "deposit", "300").Ok());
    REQUIRE(h.sim.Corporations().Debit(h.Corp().id, 25.0, "police", "", "Helios Core"));
    TempDir dir;
    REQUIRE(h.sim.Corporations().Save(dir.Path("corps.json")));

    Corps::Registry back;
    REQUIRE(back.Load(dir.Path("corps.json")) == Save::Result::Ok);
    const Corps::Corp* c = back.Of("anna");
    REQUIRE(c != nullptr);
    CHECK(c->treasury == doctest::Approx(275.0));
    REQUIRE(c->ledger.size() == 2);
    CHECK(c->ledger[0].kind == "deposit");
    CHECK(c->ledger[0].who == "dima");
    CHECK(c->ledger[1].note == "Helios Core");
    CHECK(c->ledger[1].amount == doctest::Approx(-25.0));

    // A line this build cannot read is dropped, and the file loads.
    nlohmann::json j = h.sim.Corporations().ToJson();
    j["corps"][0]["ledger"].push_back({ { "kind", "" }, { "amount", 5 } });
    j["corps"][0]["ledger"].push_back("junk");
    Corps::Registry again;
    std::string     why;
    REQUIRE(again.FromJson(j, why));
    CHECK(again.Of("anna")->ledger.size() == 2);

    // The version did not change: a file with a ledger is a file of the same schema.
    CHECK(j["version"] == Save::CORPS_VERSION);
}

TEST_CASE("the ledger rides in the corporation view (#509)")
{
    Proto::CorpView v;
    v.id = "c1";
    v.name = "Deep Haul";
    v.ticker = "DH";
    v.treasury = 275.0;
    v.ledger.push_back({ 1800000000, "deposit", "dima", "", 300.0, 1 });
    v.ledger.push_back({ 1800000100, "fees", "", "", 50.0, 5 });
    Proto::CorpView back;
    REQUIRE(Proto::DecodeCorpView(Proto::EncodeCorpView(v), back));
    REQUIRE(back.ledger.size() == 2);
    CHECK(back.ledger[0].who == "dima");
    CHECK(back.ledger[0].amount == doctest::Approx(300.0));
    CHECK(back.ledger[1].count == 5);
    CHECK(back.ledger[1].at == 1800000100);
}
