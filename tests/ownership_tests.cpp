#include <doctest/doctest.h>

#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "entities/Ship.h"
#include "entities/ShipType.h"
#include "entities/Station.h"
#include "entities/Structure.h"
#include "sim/ClientSession.h"
#include "sim/Corporation.h"
#include "sim/Protocol.h"
#include "sim/Simulation.h"
#include "sim/WorldRules.h"
#include "temp_dir.h"

#include "factions_named.h"
#include "goods_named.h"
#include <nlohmann/json.hpp>

#include <fstream>
#include <memory>
#include <set>
#include <string>

// Structure ownership the EVE way (#468): what a corporation member builds is the
// corporation's and stays with it when they leave; what a pilot built before joining stays
// theirs until they hand it over. One verb hands a structure over -- pilot or corporation to
// pilot or corporation -- and the licence and what is owed on it go with it, by the leave of
// the system's holder. A disbanded corporation's structures are abandoned: anyone's to take.

namespace
{
std::string DataFile(const char* name)
{
    return std::string(TEST_DATA_DIR) + name;
}

constexpr double NOW = 1.8e9;  // seconds, as Unix time
constexpr double WEEK = 7.0 * 86400.0;

void StartServer(Simulation& sim)
{
    sim.LoadUniverse(std::string(TEST_WORLD_DIR) + "universe.json");
    sim.Seed(1234u);
    sim.InitGalaxy();
    sim.MaterializeAllSystems(std::string(TEST_WORLD_DIR) + "systems/");
}

// Helios Core, the Traders Guild's: Deep Haul (anna, CEO; boris), and pilots in none.
struct World
{
    Simulation        sim;
    const std::string sys = "core";
    float             x = 100000.0f;

    World()
    {
        REQUIRE(Factions::Load(DataFile("factions.json")));
        REQUIRE(Archetypes::Load(DataFile("archetypes.json")));
        REQUIRE(Blueprints::Load(DataFile("blueprints.json")));
        WorldRules::SetInfluenceRules(WorldRules::Influence{});
        WorldRules::SetLicenceRules(WorldRules::Licences{});  // the design's numbers
        StartServer(sim);
        Corps::Registry& r = sim.Corporations();
        REQUIRE(r.Create("anna", "Deep Haul", "DH", NOW).Ok());
        REQUIRE(r.InviteTo("anna", "boris", { "boris" }, NOW).Ok());
        REQUIRE(r.Join("boris", "DH", NOW).Ok());
    }
    SystemState& St(const std::string& id = "core") { return *sim.SystemById(id); }
    std::string  Corp() { return sim.Corporations().IdOf("anna"); }

    int Build(const std::string& owner, const std::string& where = "core")
    {
        const Blueprint* bp = Blueprints::Find("outpost");
        REQUIRE(bp != nullptr);
        x += 20000.0f;
        auto t = std::make_unique<Structure>(Vector2{ x, -300000.0f }, 0.0f, "Depot " + owner,
                                             bp->archetype);
        t->SetBlueprint(bp->id);
        const int id = sim.AddStatic(where, std::move(t), owner);
        REQUIRE(id != 0);
        return id;
    }
    Structure* ById(int id, const std::string& where = "core")
    {
        for (const auto& e : St(where).entities)
            if (e->GetId() == id && e->GetKind() == EntityKind::Structure)
                return static_cast<Structure*>(e.get());
        return nullptr;
    }
    ClientSession& Pilot(const std::string& account, const std::string& where = "core")
    {
        ClientSession& s =
            sim.CreateSession(where, Vector2{ 0.0f, 0.0f }, GetShipCatalog()[0].stats);
        s.accountName = account;
        return s;
    }
    Corps::Outcome Transfer(const std::string& by, int id, const std::string& pilot,
                            const std::string& ticker, const std::set<std::string>& online)
    {
        Corps::Act a;
        a.verb = "transfer";
        a.value = std::to_string(id);
        a.system = sys;
        a.pilot = pilot;
        a.ticker = ticker;
        return sim.ApplyCorpAct(by, a, online, NOW);
    }
};
}  // namespace

TEST_CASE("what a member builds is the corporation's and stays with it; what a pilot built "
          "before joining stays theirs (#468)")
{
    World            w;
    Corps::Registry& r = w.sim.Corporations();
    // The start, where there is room to build (as build_tests find it).
    const std::string start = w.sim.Universe().startId;
    ClientSession&    boris = w.Pilot("boris", start);
    ClientSession&    cara = w.Pilot("cara", start);
    w.sim.GrantLicence("boris", start);
    w.sim.GrantLicence("cara", start);

    // Built through the verb: a member's is the corporation's, a pilot's in none their own.
    const Blueprint* beacon = Blueprints::Find("beacon");
    REQUIRE(beacon != nullptr);
    auto deploy = [&](ClientSession& s)
    {
        s.ship->ClearCargo();
        REQUIRE(s.ship->AddCargo(Good::Iron(), 20));
        REQUIRE(s.ship->AddCargo(Good::Crystal(), 5));
        for (int i = 0; i < 60; i++)
            for (int j = 0; j < 60; j++)
            {
                const Vector2 p = { 120000.0f + 9100.0f * (float)i, -270000.0f + 9300.0f * j };
                s.ship->Teleport(p);
                if (!w.sim.PlacementProblem(s, *beacon, p).empty())
                    continue;
                return w.sim.Deploy(s, "beacon", p, "");
            }
        FAIL("no free spot");
        return 0;
    };
    const int corps = deploy(boris);
    const int own = deploy(cara);
    REQUIRE(corps != 0);
    REQUIRE(own != 0);
    CHECK(w.ById(corps, start)->GetOwner() == Corps::OwnerOf(w.Corp()));
    CHECK(w.ById(own, start)->GetOwner() == "cara");
    CHECK(w.sim.BuildOwner("boris") == Corps::OwnerOf(w.Corp()));
    CHECK(w.sim.OwnerName(w.ById(corps, start)->GetOwner()) == "Deep Haul [DH]");

    // cara joins: what she built before is still hers, and pays as hers.
    REQUIRE(r.InviteTo("anna", "cara", { "cara" }, NOW).Ok());
    REQUIRE(r.Join("cara", "DH", NOW).Ok());
    CHECK(w.ById(own, start)->GetOwner() == "cara");
    CHECK(w.sim.StructureParty("cara") == std::string(InfluenceMap::PILOT) + "cara");
    // ...and her corporation mates are on its owner's side, as before (#347).
    CHECK(w.ById(own, start)
              ->OwnersSide("boris", r.SameCorp(w.ById(own, start)->GetOwner(), "boris")));

    // boris leaves: what he built while a member stays the corporation's.
    REQUIRE(r.Leave("boris").Ok());
    CHECK(w.ById(corps, start)->GetOwner() == Corps::OwnerOf(w.Corp()));
    CHECK(w.sim.StructureParty(w.ById(corps, start)->GetOwner()) == w.Corp());
    CHECK_FALSE(r.Commands("boris", w.ById(corps, start)->GetOwner()));
    CHECK_FALSE(w.ById(corps, start)
                    ->OwnersSide("boris", r.SameCorp(w.ById(corps, start)->GetOwner(), "boris")));
    // The CEO commands it; a plain member does not, but is on its side.
    CHECK(r.Commands("anna", w.ById(corps, start)->GetOwner()));
    CHECK_FALSE(r.Commands("cara", w.ById(corps, start)->GetOwner()));
    CHECK(r.SameCorp("cara", w.ById(corps, start)->GetOwner()));
    CHECK(w.sim.DismantleProblem(boris, corps).find("is not yours") != std::string::npos);
}

TEST_CASE("handing a structure over: who may, and to whom (#468)")
{
    World                       w;
    const std::set<std::string> online{ "anna", "boris", "cara", "dima" };
    w.Pilot("anna");
    w.Pilot("boris");
    w.Pilot("cara");
    w.Pilot("dima");
    const int corps = w.Build(Corps::OwnerOf(w.Corp()));
    const int caras = w.Build("cara");

    // A plain member does not hand over the corporation's; a stranger nothing of anyone's.
    Corps::Outcome o = w.Transfer("boris", corps, "boris", "", online);
    CHECK_FALSE(o.Ok());
    CHECK(o.told.find("CEO or a director") != std::string::npos);
    CHECK_FALSE(w.Transfer("dima", caras, "dima", "", online).Ok());
    CHECK(w.ById(corps)->GetOwner() == Corps::OwnerOf(w.Corp()));
    CHECK(w.ById(caras)->GetOwner() == "cara");
    // To nobody, to both, to somebody away, to a corporation that does not exist: refused.
    CHECK_FALSE(w.Transfer("cara", caras, "", "", online).Ok());
    CHECK_FALSE(w.Transfer("cara", caras, "anna", "DH", online).Ok());
    CHECK_FALSE(w.Transfer("cara", caras, "erik", "", online).Ok());
    CHECK_FALSE(w.Transfer("cara", caras, "", "ZZZ", online).Ok());

    // Its goods and rules go with it. The holder admits whoever the Guild thinks well of.
    w.ById(caras)->AddStored(Good::Iron(), 7);
    Defence d;
    d.engage = Engage::Strangers;
    w.ById(caras)->SetDefence(d);
    for (auto& kv : w.sim.Sessions())
        kv.second.account.AddReputation(Fac::TradersGuild(), 15.0f);

    // Pilot to corporation, by its ticker.
    o = w.Transfer("cara", caras, "", "dh", online);
    REQUIRE(o.Ok());
    CHECK(w.ById(caras)->GetOwner() == Corps::OwnerOf(w.Corp()));
    CHECK(w.ById(caras)->GetStoredAmount(Good::Iron()) == 7);
    CHECK(w.ById(caras)->GetDefence().engage == Engage::Strangers);
    // Corporation to pilot, by its CEO.
    REQUIRE(w.Transfer("anna", caras, "boris", "", online).Ok());
    CHECK(w.ById(caras)->GetOwner() == "boris");
    // Pilot to pilot.
    REQUIRE(w.Transfer("boris", caras, "cara", "", online).Ok());
    CHECK(w.ById(caras)->GetOwner() == "cara");
    // Corporation to corporation: another's ticker.
    REQUIRE(w.sim.Corporations().Create("dima", "Far Reach", "FR", NOW).Ok());
    REQUIRE(w.Transfer("anna", corps, "", "FR", online).Ok());
    CHECK(w.ById(corps)->GetOwner() == Corps::OwnerOf(w.sim.Corporations().IdOf("dima")));
    // The layout says whose it is now, as a player reads it.
    Proto::SystemLayout back;
    REQUIRE(Proto::DecodeLayout(Proto::EncodeLayout(w.sim.BuildLayout("core")), back));
    bool told = false, pilots = false;
    for (const Proto::EntityLayout& el : back.entities)
    {
        if (el.id == corps)
            told = el.owner == Corps::OwnerOf(w.sim.Corporations().IdOf("dima")) &&
                   el.ownerName == "Far Reach [FR]";
        if (el.id == caras)
            pilots = el.owner == "cara" && el.ownerName == "cara";
    }
    CHECK(told);
    CHECK(pilots);
}

TEST_CASE("the licence and what is owed on it move with a structure, and the holder is asked "
          "again (#468)")
{
    World          w;
    ClientSession& cara = w.Pilot("cara");
    ClientSession& anna = w.Pilot("anna");
    w.Pilot("dima");
    const std::set<std::string> online{ "anna", "cara", "dima" };
    cara.account.SetMoney(5000.0);
    cara.account.AddReputation(Fac::TradersGuild(), 15.0f);
    anna.account.AddReputation(Fac::TradersGuild(), 15.0f);
    const std::string pilot = std::string(InfluenceMap::PILOT) + "cara";

    // cara's, licensed and then left unpaid: the rent is owed.
    for (const auto& e : w.St().entities)
        if (e->GetKind() == EntityKind::Station &&
            static_cast<const Station&>(*e).GetFaction() == Fac::TradersGuild())
            cara.dockedStationId = e->GetId();
    REQUIRE(w.sim.BuyLicence(cara));
    cara.dockedStationId = 0;
    const int id = w.Build("cara");
    w.sim.SetTime(w.sim.Time() + WEEK + 60.0);
    cara.account.SetMoney(0.0);
    w.sim.StepLicences();
    REQUIRE(w.ById(id)->GetLicence() == LicenceState::Unpaid);
    const double until = w.sim.LicenceOf(pilot, "core")->until;

    // To dima, whom the Guild does not think well of: refused, and nothing moves.
    Corps::Outcome o = w.Transfer("cara", id, "dima", "", online);
    CHECK_FALSE(o.Ok());
    CHECK(o.told.find("would not license") != std::string::npos);
    CHECK(w.ById(id)->GetOwner() == "cara");
    CHECK(w.sim.LicenceOf(pilot, "core") != nullptr);

    // To Deep Haul, which it admits: the licence goes over as it stood, owed and all.
    REQUIRE(w.Transfer("cara", id, "", "DH", online).Ok());
    const Corps::Licence* corp = w.sim.LicenceOf(w.Corp(), "core");
    REQUIRE(corp != nullptr);
    CHECK(corp->holder == "f:TradersGuild");
    CHECK(corp->until == doctest::Approx(until));
    CHECK(w.sim.LicenceOf(pilot, "core") == nullptr);  // nothing else of hers stands here
    CHECK(w.ById(id)->GetLicence() == LicenceState::Unpaid);
    // ...and the treasury pays what is owed.
    REQUIRE(w.sim.Corporations().Credit(w.Corp(), 5000.0));
    w.sim.StepLicences();
    CHECK(w.ById(id)->GetLicence() == LicenceState::Licensed);
    CHECK(w.sim.Corporations().Find(w.Corp())->treasury == doctest::Approx(3000.0));
}

TEST_CASE("a disbanded corporation's structures are abandoned: no rent, no law, anyone's to "
          "take (#468)")
{
    World            w;
    Corps::Registry& r = w.sim.Corporations();
    REQUIRE(r.Create("erik", "Last Light", "LL", NOW).Ok());
    const std::string ll = r.IdOf("erik");
    const int         held = w.Build(Corps::OwnerOf(ll));
    const int         far = w.Build(Corps::OwnerOf(ll), "verge");
    w.sim.RefreshLicences(w.St());
    REQUIRE(w.ById(held)->GetLicence() == LicenceState::Unpaid);

    // The last one out disbands it.
    REQUIRE(r.Leave("erik").Ok());
    CHECK(w.sim.Abandoned(w.ById(held)->GetOwner()));
    CHECK(w.sim.OwnerName(w.ById(held)->GetOwner()) == "abandoned");
    CHECK(w.sim.StructureParty(w.ById(held)->GetOwner()).empty());
    // No rent, and nobody's law: shooting it is no crime even under the Guild's.
    w.sim.RefreshLicences(w.St());
    CHECK(w.ById(held)->GetLicence() == LicenceState::None);
    ClientSession& dima = w.Pilot("dima");
    FactionId      law;
    CHECK_FALSE(w.sim.IsCrime(w.St(), dima, *w.ById(held), law));
    // Nobody commands it; nor does handing it over work -- it is taken.
    CHECK_FALSE(r.Commands("erik", w.ById(held)->GetOwner()));
    CHECK_FALSE(w.Transfer("erik", held, "dima", "", { "erik", "dima" }).Ok());

    // Taken from beside it, by the rules of building there.
    ClientSession& fay = w.Pilot("fay", "verge");
    Corps::Act     take;
    take.verb = "take";
    take.value = std::to_string(far);
    fay.ship->Teleport(Vector2{ 0.0f, 0.0f });
    CHECK_FALSE(w.sim.ApplyCorpAct("fay", take, { "fay" }, NOW).Ok());  // too far
    fay.ship->Teleport(w.ById(far, "verge")->GetPosition());
    const Corps::Outcome o = w.sim.ApplyCorpAct("fay", take, { "fay" }, NOW);
    CHECK(o.Ok());
    CHECK(w.ById(far, "verge")->GetOwner() == "fay");
    // A member takes it for the corporation, as a member builds for it.
    const int      other = w.Build(Corps::OwnerOf(ll), "verge");
    ClientSession& boris = w.Pilot("boris", "verge");
    boris.ship->Teleport(w.ById(other, "verge")->GetPosition());
    take.value = std::to_string(other);
    REQUIRE(w.sim.ApplyCorpAct("boris", take, { "boris" }, NOW).Ok());
    CHECK(w.ById(other, "verge")->GetOwner() == Corps::OwnerOf(w.Corp()));
    // ...and only an abandoned one is free.
    CHECK_FALSE(w.sim.ApplyCorpAct("fay", take, { "fay" }, NOW).Ok());
}

TEST_CASE("an older world's member structures are the corporation's (#468)")
{
    TempDir     dir;
    int         member = 0, pilot = 0;
    std::string corp;
    {
        World w;
        member = w.Build("boris");  // a member's, as an older build kept it
        pilot = w.Build("cara");    // a pilot in none
        corp = w.Corp();
        REQUIRE(w.sim.Corporations().Save(dir.Path("corps.json")));
        w.sim.SaveWorld(dir.Path("world.json"));
    }
    auto load = [&](int version, Simulation& sim)
    {
        nlohmann::json j;
        {
            std::ifstream in(dir.Path("world.json"));
            j = nlohmann::json::parse(in);
        }
        j["version"] = version;
        {
            std::ofstream out(dir.Path("world_v.json"));
            out << j.dump();
        }
        sim.LoadUniverse(std::string(TEST_WORLD_DIR) + "universe.json");
        sim.Seed(1234u);
        REQUIRE(sim.LoadWorld(dir.Path("world_v.json")) == Save::Result::Ok);
        // As the host does: the corporations before the systems.
        REQUIRE(sim.Corporations().Load(dir.Path("corps.json")) == Save::Result::Ok);
        sim.MaterializeAllSystems(std::string(TEST_WORLD_DIR) + "systems/");
    };
    auto ownerOf = [](Simulation& sim, const std::string& name)
    {
        for (const auto& e : sim.SystemById("core")->entities)
            if (e->GetName() == name)
                return e->GetOwner();
        return std::string("(gone)");
    };
    {
        Simulation old;
        load(17, old);
        CHECK(ownerOf(old, "Depot boris") == Corps::OwnerOf(corp));
        CHECK(ownerOf(old, "Depot cara") == "cara");
    }
    {
        // A current file says what it means: a member's own, built before they joined.
        Simulation now;
        load(Save::WORLD_VERSION, now);
        CHECK(ownerOf(now, "Depot boris") == "boris");
    }
    (void)member;
    (void)pilot;
}
