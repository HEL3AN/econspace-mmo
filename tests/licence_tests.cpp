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
#include "sim/FactionMind.h"
#include "sim/Protocol.h"
#include "sim/Simulation.h"
#include "sim/WorldRules.h"
#include "temp_dir.h"

#include "factions_named.h"
#include <nlohmann/json.hpp>

#include <memory>
#include <string>

// Licences (#431 slice 6): building in a system a faction or a corporation holds takes the
// holder's leave -- a standing, a corporation's being its members' together -- and a weekly
// rent, paid from the treasury or the pilot's money in the coarse pass. Unpaid a grace later
// it lapses: what stands there trespasses, is lawless to shoot, and the faction may evict it.

namespace
{
std::string DataFile(const char* name)
{
    return std::string(TEST_DATA_DIR) + name;
}

constexpr double NOW = 1.8e9;  // seconds, as Unix time
constexpr double WEEK = 7.0 * 86400.0;
constexpr double DAY = 86400.0;

void StartServer(Simulation& sim, const std::string& worldPath = std::string())
{
    sim.LoadUniverse(std::string(TEST_WORLD_DIR) + "universe.json");
    sim.Seed(1234u);
    if (worldPath.empty() || sim.LoadWorld(worldPath) != Save::Result::Ok)
        sim.InitGalaxy();
    sim.MaterializeAllSystems(std::string(TEST_WORLD_DIR) + "systems/");
}

// Helios Core, the Traders Guild's: Deep Haul (anna, CEO; boris) and a pilot in none, cara.
struct Core
{
    Simulation        sim;
    const std::string sys = "core";
    float             x = 100000.0f;

    Core()
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
        REQUIRE(St().agg.claimed);
        REQUIRE(St().agg.controller == Fac::TradersGuild());
    }
    SystemState& St() { return *sim.SystemById(sys); }
    std::string  Corp() { return sim.Corporations().IdOf("anna"); }

    int Build(const std::string& blueprint, const std::string& owner)
    {
        const Blueprint* bp = Blueprints::Find(blueprint);
        REQUIRE(bp != nullptr);
        x += 20000.0f;
        auto t = std::make_unique<Structure>(Vector2{ x, -300000.0f }, 0.0f,
                                             blueprint + " " + owner, bp->archetype);
        t->SetBlueprint(blueprint);
        const int id = sim.AddStatic(sys, std::move(t), owner);
        REQUIRE(id != 0);
        return id;
    }
    Structure* ById(int id)
    {
        for (const auto& e : St().entities)
            if (e->GetId() == id)
                return static_cast<Structure*>(e.get());
        return nullptr;
    }
    ClientSession& Pilot(const std::string& account)
    {
        ClientSession& s = sim.CreateSession(sys, Vector2{ 0.0f, 0.0f }, GetShipCatalog()[0].stats);
        s.accountName = account;
        return s;
    }
    // The Guild's station here, where its licences are sold.
    int Hub()
    {
        for (const auto& e : St().entities)
            if (e->GetKind() == EntityKind::Station &&
                static_cast<const Station&>(*e).GetFaction() == Fac::TradersGuild())
                return e->GetId();
        FAIL("no Guild station in core");
        return 0;
    }
};
}  // namespace

TEST_CASE("a licence: the holder's leave first, then the rent; the frontier needs none "
          "(#431 slice 6)")
{
    Core           c;
    ClientSession& cara = c.Pilot("cara");
    cara.account.SetMoney(10000.0);

    // What the world's unaffiliated keep is nobody's to rent: build freely.
    const SystemState& verge = *c.sim.SystemById("verge");
    CHECK(c.sim.LicenceHolder(verge).empty());
    CHECK(c.sim.LicenceProblem("cara", verge).empty());

    // In the Guild's space, without standing: refused, with the reason.
    CHECK(c.sim.LicenceHolder(c.St()) == "f:TradersGuild");
    const std::string why = c.sim.LicenceProblem("cara", c.St());
    CHECK(why.find("licence from Traders Guild") != std::string::npos);
    CHECK(why.find("standing of 10") != std::string::npos);
    cara.dockedStationId = c.Hub();
    CHECK_FALSE(c.sim.BuyLicence(cara));
    CHECK(cara.account.GetMoney() == doctest::Approx(10000.0));
    Proto::LicenceView v = c.sim.LicenceViewFor(cara);
    CHECK(v.holder == "Traders Guild");
    CHECK_FALSE(v.admitted);
    CHECK(v.state == "none");

    // Thought well of, but not at the Guild's station: refused too.
    cara.account.AddReputation(Fac::TradersGuild(), 15.0f);
    cara.dockedStationId = 0;
    CHECK_FALSE(c.sim.BuyLicence(cara));
    CHECK(c.sim.LicenceProblem("cara", c.St()).find("buy one docked") != std::string::npos);

    // Docked there: a week's rent to the Guild, and building is allowed.
    cara.dockedStationId = c.Hub();
    const double treasury = c.sim.MindOf(Fac::TradersGuild()).treasury;
    REQUIRE(c.sim.BuyLicence(cara));
    CHECK(cara.account.GetMoney() == doctest::Approx(8000.0));
    CHECK(c.sim.MindOf(Fac::TradersGuild()).treasury == doctest::Approx(treasury + 2000.0));
    CHECK(c.sim.LicenceProblem("cara", c.St()).empty());
    v = c.sim.LicenceViewFor(cara);
    CHECK(v.state == "licensed");
    CHECK(v.until == doctest::Approx(c.sim.Time() + WEEK));
    CHECK(v.canPay);
    // ...and the placement rule says the same, at a spot that is otherwise free.
    cara.dockedStationId = 0;
    const Blueprint* beacon = Blueprints::Find("beacon");
    REQUIRE(beacon != nullptr);
    Vector2 spot{ 0.0f, 0.0f };
    bool    found = false;
    for (int i = 1; i < 200 && !found; i++)
    {
        spot = Vector2{ 4000.0f * (float)i, -9000.0f * (float)i };
        found = c.sim.SpotProblem(c.St(), *beacon, spot).empty();
    }
    REQUIRE(found);
    cara.ship->Teleport(spot);
    CHECK(c.sim.PlacementProblem(cara, *beacon, spot, false).find("licence") == std::string::npos);
    ClientSession& dima = c.Pilot("dima");
    dima.ship->Teleport(spot);
    dima.account.AddReputation(Fac::TradersGuild(), 15.0f);
    CHECK(c.sim.PlacementProblem(dima, *beacon, spot, false).find("licence") != std::string::npos);

    // A second week ahead is too far ahead.
    cara.dockedStationId = c.Hub();
    CHECK_FALSE(c.sim.BuyLicence(cara));
}

TEST_CASE("a corporation's standing is its members' together, and its rent comes from the "
          "treasury each week until it lapses into trespass (#431 slice 6)")
{
    Core             c;
    ClientSession&   anna = c.Pilot("anna");
    ClientSession&   boris = c.Pilot("boris");
    Corps::Registry& r = c.sim.Corporations();
    // Neither is enough alone; together they are.
    anna.account.AddReputation(Fac::TradersGuild(), 6.0f);
    boris.account.AddReputation(Fac::TradersGuild(), 6.0f);
    CHECK(c.sim.PartyStanding(c.Corp(), Fac::TradersGuild()) == doctest::Approx(12.0f));
    CHECK(c.sim.LicenceProblem("boris", c.St()).find("standing") == std::string::npos);

    // A member pays nothing for the corporation; its CEO pays from the treasury.
    boris.dockedStationId = c.Hub();
    anna.dockedStationId = c.Hub();
    CHECK_FALSE(c.sim.BuyLicence(boris));
    CHECK_FALSE(c.sim.BuyLicence(anna));  // the treasury is empty
    REQUIRE(r.Credit(c.Corp(), 5000.0));
    REQUIRE(c.sim.BuyLicence(anna));
    CHECK(r.Find(c.Corp())->treasury == doctest::Approx(3000.0));
    CHECK(c.sim.LicenceProblem("boris", c.St()).empty());

    const int  depot = c.Build("outpost", "boris");
    Structure* t = c.ById(depot);
    REQUIRE(t != nullptr);
    c.sim.StepLicences();
    CHECK(t->GetLicence() == LicenceState::Licensed);

    // Licensed, it is guarded: shooting it is a crime under the Guild's law.
    ClientSession& dima = c.Pilot("dima");
    FactionId      law;
    CHECK(c.sim.IsCrime(c.St(), dima, *t, law));
    CHECK(law == Fac::TradersGuild());

    // A week on, the rent is taken again, cold or hot alike.
    c.sim.SetTime(c.sim.Time() + WEEK);
    c.sim.StepLicences();
    CHECK(r.Find(c.Corp())->treasury == doctest::Approx(1000.0));
    CHECK(t->GetLicence() == LicenceState::Licensed);

    // Another week, and the treasury cannot pay: unpaid, with a day's grace.
    c.sim.SetTime(c.sim.Time() + WEEK);
    c.sim.StepLicences();
    CHECK(r.Find(c.Corp())->treasury == doctest::Approx(1000.0));
    CHECK(t->GetLicence() == LicenceState::Unpaid);
    CHECK(c.sim.IsCrime(c.St(), dima, *t, law));  // still guarded through the grace
    CHECK(c.St().trespass.empty());

    SUBCASE("paid within the grace, it stands licensed")
    {
        REQUIRE(r.Credit(c.Corp(), 1500.0));
        c.sim.SetTime(c.sim.Time() + DAY / 2.0);
        c.sim.StepLicences();
        CHECK(t->GetLicence() == LicenceState::Licensed);
        CHECK(r.Find(c.Corp())->treasury == doctest::Approx(500.0));
    }
    SUBCASE("unpaid past the grace, it trespasses -- and the Guild weighs evicting it")
    {
        c.sim.SetTime(c.sim.Time() + DAY + 1.0);
        c.sim.StepLicences();
        CHECK(t->GetLicence() == LicenceState::Trespass);
        CHECK_FALSE(c.sim.IsCrime(c.St(), dima, *t, law));  // lawless to shoot
        REQUIRE(c.St().trespass.size() == 1);
        CHECK(c.St().trespass[0] == depot);
        // ...and the rent is not taken any more, whatever comes into the treasury.
        REQUIRE(r.Credit(c.Corp(), 5000.0));
        c.sim.StepLicences();
        CHECK(t->GetLicence() == LicenceState::Trespass);
        CHECK(r.Find(c.Corp())->treasury == doctest::Approx(6000.0));
        // The layout says so to everyone.
        bool told = false;
        for (const Proto::EntityLayout& el : c.sim.BuildLayout("core").entities)
            if (el.id == depot)
                told = el.licence == (int)LicenceState::Trespass;
        CHECK(told);

        // A decision, not a rule: worth it, with the strength and the stock, the Guild sends
        // its garrison against it.
        WorldRules::Licences rules;
        rules.evict = 50.0;  // decisive, so nothing else it could do this period wins
        WorldRules::SetLicenceRules(rules);
        const float cap = Factions::TemperamentOf(Fac::TradersGuild()).capacity;
        const Plan* siege = nullptr;
        for (int pass = 0; pass < 200 && siege == nullptr; pass++)
        {
            c.St().agg.presence[Fac::TradersGuild()] = cap * 2.0f;
            c.sim.MindOf(Fac::TradersGuild()).stock = Simulation::OutpostCost() * 20.0f;
            c.sim.StepFactions();
            for (const auto& kv : c.sim.Plans())
                if (kv.second.kind == Plan::Kind::Siege && kv.second.faction == Fac::TradersGuild())
                    siege = &kv.second;
        }
        REQUIRE(siege != nullptr);
        CHECK(siege->target == "core");
        CHECK(siege->evicting == "boris");

        // Buying a licence again makes it lawful.
        REQUIRE(c.sim.BuyLicence(anna));
        CHECK(t->GetLicence() == LicenceState::Licensed);
        CHECK(c.sim.IsCrime(c.St(), dima, *t, law));
        WorldRules::SetLicenceRules(WorldRules::Licences{});
    }
}

TEST_CASE("a trespasser worth nothing to evict is left standing (#431 slice 6)")
{
    Core                 c;
    WorldRules::Licences rules;
    rules.evict = 0.0;
    WorldRules::SetLicenceRules(rules);
    const int depot = c.Build("outpost", "cara");  // a pilot in none, who never paid
    c.sim.StepLicences();
    REQUIRE(c.ById(depot)->GetLicence() == LicenceState::Unpaid);
    c.sim.SetTime(c.sim.Time() + DAY + 1.0);
    c.sim.StepLicences();
    REQUIRE(c.ById(depot)->GetLicence() == LicenceState::Trespass);
    const float cap = Factions::TemperamentOf(Fac::TradersGuild()).capacity;
    for (int pass = 0; pass < 60; pass++)
    {
        c.St().agg.presence[Fac::TradersGuild()] = cap * 2.0f;
        c.sim.MindOf(Fac::TradersGuild()).stock = Simulation::OutpostCost() * 20.0f;
        c.sim.StepFactions();
    }
    for (const auto& kv : c.sim.Plans())
        CHECK(kv.second.evicting.empty());
    WorldRules::SetLicenceRules(WorldRules::Licences{});
}

TEST_CASE("a pilot's rent is taken while they are away, within what they had (#431 slice 6)")
{
    Core              c;
    const std::string party = std::string(InfluenceMap::PILOT) + "cara";
    {
        ClientSession& cara = c.Pilot("cara");
        cara.account.SetMoney(5000.0);
        cara.account.AddReputation(Fac::TradersGuild(), 15.0f);
        cara.dockedStationId = c.Hub();
        REQUIRE(c.sim.BuyLicence(cara));
        c.Build("outpost", "cara");
        c.sim.StepOwners();  // what they have is noted: 3000
        c.sim.DestroySession(cara.id);
    }
    // Away a week: the rent is taken from what they had...
    const double first = c.sim.LicenceOf(party, "core")->until;
    c.sim.SetTime(c.sim.Time() + WEEK);
    c.sim.StepLicences();
    CHECK(c.sim.LicenceOf(party, "core")->until == doctest::Approx(first + WEEK));
    // ...and a second week it is not there: unpaid.
    c.sim.SetTime(c.sim.Time() + WEEK);
    c.sim.StepLicences();
    CHECK(c.sim.LicenceOf(party, "core")->until == doctest::Approx(first + WEEK));
    // Back, the account they log in with is short of the rent that went.
    ClientSession& back = c.Pilot("cara");
    back.account.SetMoney(3000.0);
    c.sim.StepOwners();
    CHECK(back.account.GetMoney() == doctest::Approx(1000.0));
}

TEST_CASE("licences outlive a restart: a corporation's in corps.json, a pilot's with the world "
          "(#431 slice 6)")
{
    TempDir     dir;
    std::string id;
    double      corpUntil = 0.0, pilotUntil = 0.0;
    {
        Core           c;
        ClientSession& anna = c.Pilot("anna");
        ClientSession& cara = c.Pilot("cara");
        anna.account.AddReputation(Fac::TradersGuild(), 15.0f);
        cara.account.AddReputation(Fac::TradersGuild(), 15.0f);
        cara.account.SetMoney(5000.0);
        REQUIRE(c.sim.Corporations().Credit(c.Corp(), 5000.0));
        anna.dockedStationId = c.Hub();
        cara.dockedStationId = c.Hub();
        REQUIRE(c.sim.BuyLicence(anna));
        REQUIRE(c.sim.BuyLicence(cara));
        c.sim.StepOwners();
        id = c.Corp();
        corpUntil = c.sim.LicenceOf(id, "core")->until;
        pilotUntil = c.sim.LicenceOf(std::string(InfluenceMap::PILOT) + "cara", "core")->until;
        REQUIRE(c.sim.Corporations().Save(dir.Path("corps.json")));
        c.sim.SaveWorld(dir.Path("world.json"));
    }
    Simulation back;
    StartServer(back, dir.Path("world.json"));
    REQUIRE(back.Corporations().Load(dir.Path("corps.json")) == Save::Result::Ok);
    const Corps::Licence* corp = back.LicenceOf(id, "core");
    const Corps::Licence* pilot = back.LicenceOf(std::string(InfluenceMap::PILOT) + "cara", "core");
    REQUIRE(corp != nullptr);
    REQUIRE(pilot != nullptr);
    CHECK(corp->holder == "f:TradersGuild");
    CHECK(corp->until == doctest::Approx(corpUntil));
    CHECK(pilot->until == doctest::Approx(pilotUntil));
}

TEST_CASE("a licence on the wire: the command, the snapshot's view, a structure's state, the "
          "corporation's list (#431 slice 6)")
{
    Proto::Command c;
    c.buyLicence = true;
    Proto::Command back;
    REQUIRE(Proto::DecodeCommand(Proto::EncodeCommand(c), back));
    CHECK(back.buyLicence);

    Proto::CorpView v;
    v.id = "c1";
    v.name = "Deep Haul";
    v.ticker = "DH";
    Proto::LicenceView l;
    l.system = "core";
    l.holder = "Traders Guild";
    l.state = "unpaid";
    l.until = 1234.5;
    l.rent = 2000.0;
    v.licences.push_back(l);
    Proto::CorpView vb;
    REQUIRE(Proto::DecodeCorpView(Proto::EncodeCorpView(v), vb));
    REQUIRE(vb.licences.size() == 1);
    CHECK(vb.licences[0].system == "core");
    CHECK(vb.licences[0].state == "unpaid");
    CHECK(vb.licences[0].until == doctest::Approx(1234.5));

    Proto::SystemLayout lay;
    lay.systemId = "core";
    Proto::EntityLayout el;
    el.id = 7;
    el.kind = Proto::EntityKind::Structure;
    el.licence = (int)LicenceState::Trespass;
    el.licenceUntil = 0.0;
    lay.entities.push_back(el);
    Proto::SystemLayout lb;
    REQUIRE(Proto::DecodeLayout(Proto::EncodeLayout(lay), lb));
    REQUIRE(lb.entities.size() == 1);
    CHECK(lb.entities[0].licence == (int)LicenceState::Trespass);
}
