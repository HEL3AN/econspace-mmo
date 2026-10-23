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

    // What a member builds is the corporation's (#468).
    const int  depot = c.Build("outpost", Corps::OwnerOf(c.Corp()));
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
        CHECK(siege->evicting == "Deep Haul [DH]");

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

// --- Bought where it is granted (#509) ---

namespace
{
std::string NameOf(Core& c, int id)
{
    for (const auto& e : c.St().entities)
        if (e->GetId() == id)
            return e->GetName();
    return std::string();
}
}  // namespace

TEST_CASE("a licence is bought docked at a station of the holder, and the refusal says where "
          "(#509)")
{
    Core           c;
    ClientSession& cara = c.Pilot("cara");
    cara.account.SetMoney(10000.0);
    cara.account.AddReputation(Fac::TradersGuild(), 15.0f);
    const std::string hub = NameOf(c, c.Hub());
    REQUIRE_FALSE(hub.empty());

    // The refusal for want of a licence names who grants it, the price and where to buy.
    const std::string why = c.sim.LicenceProblem("cara", c.St());
    CHECK(why.find("licence from Traders Guild") != std::string::npos);
    CHECK(why.find("2000 cr a week") != std::string::npos);
    CHECK(why.find("buy one docked at " + hub) != std::string::npos);

    // In flight: no sale, and the offer says where to go.
    Proto::LicenceView v = c.sim.LicenceViewFor(cara);
    CHECK(v.where == hub);
    CHECK_FALSE(v.atHolder);
    CHECK_FALSE(c.sim.BuyLicence(cara));
    CHECK(cara.account.GetMoney() == doctest::Approx(10000.0));

    // Docked somewhere that is not the holder's -- a structure of the pilot's own -- no sale.
    cara.dockedStationId = c.Build("outpost", "cara");
    CHECK_FALSE(c.sim.LicenceViewFor(cara).atHolder);
    CHECK_FALSE(c.sim.BuyLicence(cara));
    CHECK(cara.account.GetMoney() == doctest::Approx(10000.0));

    // At the holder's station: offered, and sold.
    cara.dockedStationId = c.Hub();
    CHECK(c.sim.LicenceViewFor(cara).atHolder);
    REQUIRE(c.sim.BuyLicence(cara));
    CHECK(cara.account.GetMoney() == doctest::Approx(8000.0));
    CHECK(c.sim.LicenceProblem("cara", c.St()).empty());

    // What the party holds, for the corporation window of a pilot in none.
    const std::vector<Proto::LicenceView> held = c.sim.PartyLicences(c.sim.PartyOf("cara"));
    REQUIRE(held.size() == 1);
    CHECK(held[0].system == "core");
    CHECK(held[0].holder == "Traders Guild");
    CHECK(held[0].where == hub);
    CHECK(held[0].state == "licensed");
    CHECK(held[0].party == "you");
}

TEST_CASE("a corporation's system sells its licence at its own structures, to a corporation's "
          "CEO or a director from the treasury (#509)")
{
    Core             c;
    Corps::Registry& r = c.sim.Corporations();
    REQUIRE(r.Create("erik", "Rival", "RV", NOW).Ok());
    const std::string rival = r.IdOf("erik");
    const int         dock = c.Build("outpost", Corps::OwnerOf(rival));
    c.St().agg.holderCorp = rival;  // the Rivals hold core now
    REQUIRE(c.sim.LicenceHolder(c.St()) == rival);

    ClientSession& anna = c.Pilot("anna");    // Deep Haul's CEO
    ClientSession& boris = c.Pilot("boris");  // ...and a member
    REQUIRE(r.Credit(c.Corp(), 5000.0));
    const std::string where = NameOf(c, dock);

    // Not "anywhere in the system" any more: docked at a structure of the holder's.
    CHECK(c.sim.LicenceProblem("anna", c.St()).find("buy one docked at " + where) !=
          std::string::npos);
    CHECK_FALSE(c.sim.BuyLicence(anna));
    anna.dockedStationId = c.Hub();  // the Guild's station is not the holder's
    CHECK_FALSE(c.sim.BuyLicence(anna));
    CHECK(r.Find(c.Corp())->treasury == doctest::Approx(5000.0));

    // A member docked there cannot pay for the corporation; its CEO can.
    boris.dockedStationId = dock;
    CHECK_FALSE(c.sim.LicenceViewFor(boris).canPay);
    CHECK_FALSE(c.sim.BuyLicence(boris));
    anna.dockedStationId = dock;
    const Proto::LicenceView v = c.sim.LicenceViewFor(anna);
    CHECK(v.atHolder);
    CHECK(v.where == where);
    CHECK(v.canPay);
    REQUIRE(c.sim.BuyLicence(anna));
    CHECK(r.Find(c.Corp())->treasury == doctest::Approx(3000.0));
    CHECK(r.Find(rival)->treasury == doctest::Approx(2000.0));
    CHECK(c.sim.LicenceProblem("boris", c.St()).empty());

    // Both treasuries say so in their ledgers (#509): rent paid, and rent received.
    const auto& paid = r.Find(c.Corp())->ledger;
    REQUIRE_FALSE(paid.empty());
    CHECK(paid.back().kind == "rent_paid");
    CHECK(paid.back().note == "Rival [RV]");
    CHECK(paid.back().amount == doctest::Approx(-2000.0));
    const auto& got = r.Find(rival)->ledger;
    REQUIRE_FALSE(got.empty());
    CHECK(got.back().kind == "rent_received");
    CHECK(got.back().note == "Deep Haul [DH]");
    CHECK(got.back().amount == doctest::Approx(2000.0));
}

TEST_CASE("the terms of building are in the galaxy statistics, and where it is sold on the wire "
          "(#509)")
{
    Core                     c;
    const Proto::GalaxyState gs = c.sim.GalaxyStats();
    bool                     core = false, verge = false;
    for (const Proto::GalaxySystemStat& g : gs.systems)
    {
        if (g.id == "core")
        {
            core = true;
            CHECK(g.licenceBy == "Traders Guild");
            CHECK(g.licenceRent == doctest::Approx(2000.0));
        }
        if (g.id == "verge")
        {
            verge = true;
            CHECK(g.licenceBy.empty());
        }
    }
    CHECK(core);
    CHECK(verge);
    Proto::GalaxyState back;
    REQUIRE(Proto::DecodeGalaxy(Proto::EncodeGalaxy(gs), back));
    for (const Proto::GalaxySystemStat& g : back.systems)
        if (g.id == "core")
        {
            CHECK(g.licenceBy == "Traders Guild");
            CHECK(g.licenceRent == doctest::Approx(2000.0));
        }

    // A pilot in no corporation has licences of their own in the view that carries them.
    Proto::CorpView    v;
    Proto::LicenceView l;
    l.system = "core";
    l.holder = "Traders Guild";
    l.where = "Helios Hub";
    l.atHolder = true;
    v.licences.push_back(l);
    Proto::CorpView vb;
    REQUIRE(Proto::DecodeCorpView(Proto::EncodeCorpView(v), vb));
    REQUIRE(vb.licences.size() == 1);
    CHECK(vb.id.empty());
    CHECK(vb.licences[0].where == "Helios Hub");
    CHECK(vb.licences[0].atHolder);
}

// --- The rent follows the world (#509) ---

TEST_CASE("the rent is a share of what the system's economy makes, by its security, between a "
          "floor and a ceiling (#509)")
{
    Core               c;
    const SystemState& core = c.St();
    const SystemState& reach = *c.sim.SystemById("reach");

    // The default rules are the flat price, whatever the system.
    CHECK(c.sim.LicenceRent(core) == doctest::Approx(2000.0));
    CHECK(c.sim.LicenceRent(reach) == doctest::Approx(2000.0));

    WorldRules::Licences rules;
    rules.rent = 3000.0;  // the floor
    rules.share = 0.01;
    rules.ceiling = 30000.0;
    rules.securityBase = 0.5;
    rules.securitySlope = 1.0;
    WorldRules::SetLicenceRules(rules);

    // The formula, on the two licensed systems of the fixture world.
    for (const SystemState* st : { &core, &reach })
    {
        const double output = c.sim.SystemOutput(*st, 7.0 * 24.0);
        const double want =
            std::min(30000.0, std::max(3000.0, 0.01 * output * (0.5 + (double)st->agg.security)));
        CHECK(c.sim.LicenceRent(*st) == doctest::Approx(std::round(want / 50.0) * 50.0));
    }
    // ...a richer, safer system pays more than a poorer one.
    CHECK(c.sim.SystemOutput(core, 168.0) > c.sim.SystemOutput(reach, 168.0));
    CHECK(c.sim.LicenceRent(core) > c.sim.LicenceRent(reach));
    // ...and a poor one pays the floor.
    CHECK(c.sim.LicenceRent(reach) == doctest::Approx(3000.0));

    // The ceiling caps the rent.
    rules.ceiling = 3500.0;
    WorldRules::SetLicenceRules(rules);
    CHECK(c.sim.LicenceRent(core) == doctest::Approx(3500.0));

    // What a pilot pays, sees and the galaxy says is that one number.
    rules.ceiling = 30000.0;
    WorldRules::SetLicenceRules(rules);
    const double   rent = c.sim.LicenceRent(core);
    ClientSession& cara = c.Pilot("cara");
    cara.account.SetMoney(100000.0);
    cara.account.AddReputation(Fac::TradersGuild(), 15.0f);
    cara.dockedStationId = c.Hub();
    CHECK(c.sim.LicenceViewFor(cara).rent == doctest::Approx(rent));
    REQUIRE(c.sim.BuyLicence(cara));
    CHECK(cara.account.GetMoney() == doctest::Approx(100000.0 - rent));
    for (const Proto::GalaxySystemStat& g : c.sim.GalaxyStats().systems)
        if (g.id == "core")
            CHECK(g.licenceRent == doctest::Approx(rent));

    WorldRules::SetLicenceRules(WorldRules::Licences{});
}
