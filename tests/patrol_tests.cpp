#include <doctest/doctest.h>

#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "core/Fitting.h"
#include "core/ShipDesign.h"
#include "entities/NpcShip.h"
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

#include "factions_named.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

// A corporation's own AI patrols (#431 C1, slice 12): its officers keep ships of a design in a
// system it holds or has structures in, within the server's limits; the treasury pays for them
// every pass, and unpaid they stand down. They are presence, fought with the same arithmetic as
// any NPC where nobody is, and guards that never fire on a member where somebody is.

namespace
{
constexpr double NOW = 1.8e9;  // seconds, as Unix time
constexpr double DAY = 86400.0;

std::string DataFile(const char* name)
{
    return std::string(TEST_DATA_DIR) + name;
}

void LoadRegistries()
{
    REQUIRE(Factions::Load(DataFile("factions.json")));
    REQUIRE(Archetypes::Load(DataFile("archetypes.json")));
    REQUIRE(Blueprints::Load(DataFile("blueprints.json")));
    REQUIRE(WorldRules::Load(DataFile("world_rules.json")));
}

// The rules as the data file has them, put back after a test changed them.
struct RulesGuard
{
    WorldRules::Patrols  patrols = WorldRules::PatrolRules();
    WorldRules::Licences licences = WorldRules::LicenceRules();
    ~RulesGuard()
    {
        WorldRules::SetPatrolRules(patrols);
        WorldRules::SetLicenceRules(licences);
    }
};

Corps::Outcome SetPatrol(Simulation& sim, const std::string& who, const std::string& value,
                         const std::string& where, const std::string& design = std::string(),
                         const std::string& engage = std::string())
{
    Corps::Act act;
    act.verb = "patrol";
    act.value = value;
    act.system = where;
    act.design = design;
    act.engage = engage;
    return sim.ApplyCorpAct(who, act, {}, NOW);
}

int BuildIn(Simulation& sim, const std::string& sys, const std::string& blueprint,
            const std::string& owner, Vector2 at)
{
    const Blueprint* bp = Blueprints::Find(blueprint);
    REQUIRE(bp != nullptr);
    auto t = std::make_unique<Structure>(at, 0.0f, blueprint + " " + owner, bp->archetype);
    t->SetBlueprint(blueprint);
    const int id = sim.AddStatic(sys, std::move(t), owner);
    REQUIRE(id != 0);
    return id;
}

// Tau Verge, held by Deep Haul [DH] -- anna its CEO, boris a member -- by an outpost of
// anna's, with nobody else's ships, station or claim there.
struct Held
{
    Simulation        sim;
    const std::string sys = "verge";
    Vector2           outpost{ 120000.0f, -300000.0f };

    Held()
    {
        LoadRegistries();
        sim.LoadUniverse(std::string(TEST_WORLD_DIR) + "universe.json");
        sim.Seed(1234u);
        sim.InitGalaxy();
        sim.MaterializeAllSystems(std::string(TEST_WORLD_DIR) + "systems/");
        Corps::Registry& r = sim.Corporations();
        REQUIRE(r.Create("anna", "Deep Haul", "DH", NOW).Ok());
        REQUIRE(r.InviteTo("anna", "boris", { "boris" }, NOW).Ok());
        REQUIRE(r.Join("boris", "DH", NOW).Ok());
        for (const char* id : { "core", "verge" })
        {
            SystemAggregate& a = sim.SystemById(id)->agg;
            a.claimed = false;
            a.controller = Factions::Unaffiliated();
            for (FactionId g : Factions::All())
                a.presence[g] = 0.0f;
            a.police = a.pirates = a.traders = a.miners = 0.0f;
        }
        auto& e = St().entities;
        e.erase(std::remove_if(e.begin(), e.end(), [](const std::unique_ptr<Entity>& en)
                               { return en->GetKind() == EntityKind::Station; }),
                e.end());
        St().profile = Simulation::ProfileOf(St());
        BuildIn(sim, sys, "outpost", "anna", outpost);
        for (int i = 0; i < Simulation::ContestPasses() + 1; i++)
            sim.StepControl(false);
        REQUIRE(Agg().holderCorp == Corp());
    }
    SystemState&     St() { return *sim.SystemById(sys); }
    SystemAggregate& Agg() { return St().agg; }
    std::string      Corp() { return sim.Corporations().IdOf("anna"); }
    double           Treasury() { return sim.Corporations().Find(Corp())->treasury; }
    float            There()
    {
        const auto it = Agg().patrols.find(Corp());
        return it != Agg().patrols.end() ? it->second : 0.0f;
    }
    ClientSession& Pilot(const std::string& account, Vector2 at)
    {
        ClientSession& s = sim.CreateSession(sys, at, GetShipCatalog()[0].stats);
        s.accountName = account;
        return s;
    }
    std::vector<const NpcShip*> Guards()
    {
        std::vector<const NpcShip*> out;
        for (const auto& e : St().entities)
            if (e->GetKind() == EntityKind::Npc &&
                static_cast<const NpcShip&>(*e).GetRole() == NpcRole::Guard &&
                static_cast<const NpcShip&>(*e).IsAlive())
                out.push_back(static_cast<const NpcShip*>(e.get()));
        return out;
    }
};
}  // namespace

TEST_CASE("a patrol is an officer's to keep where the corporation has something, within the "
          "server's limits (#431 slice 12)")
{
    RulesGuard          guard;
    Held                h;
    WorldRules::Patrols r;
    r.systemMax = 4;
    r.shipsMax = 6;
    r.shipHour = 100.0;
    WorldRules::SetPatrolRules(r);

    CHECK_FALSE(SetPatrol(h.sim, "boris", "2", "verge").Ok());  // a member sets nothing
    CHECK_FALSE(SetPatrol(h.sim, "anna", "2", "core").Ok());    // nothing of DH's there
    const Corps::Outcome tooMany = SetPatrol(h.sim, "anna", "5", "verge");
    CHECK_FALSE(tooMany.Ok());
    CHECK(tooMany.error.find("at most 4") != std::string::npos);
    CHECK(SetPatrol(h.sim, "anna", "4", "verge").Ok());

    // A structure of a member's in core lets it keep one there too -- within what is left.
    BuildIn(h.sim, "core", "outpost", "boris", { 90000.0f, 250000.0f });
    const Corps::Outcome over = SetPatrol(h.sim, "anna", "3", "core");
    CHECK_FALSE(over.Ok());
    CHECK(over.error.find("at most 6") != std::string::npos);
    CHECK(SetPatrol(h.sim, "anna", "2", "core").Ok());
    CHECK(h.sim.PatrolOf(h.Corp(), "core") != nullptr);

    // A design: armed, and one there is.
    CHECK(SetPatrol(h.sim, "anna", "2", "verge", "hauler").error.find("no guns") !=
          std::string::npos);
    CHECK_FALSE(SetPatrol(h.sim, "anna", "2", "verge", "dreadnought").Ok());
    CHECK_FALSE(SetPatrol(h.sim, "anna", "2", "verge", "", "bogus").Ok());
    REQUIRE(SetPatrol(h.sim, "anna", "2", "verge", "cruiser", "strangers").Ok());
    const Corps::Patrol* p = h.sim.PatrolOf(h.Corp(), "verge");
    REQUIRE(p != nullptr);
    CHECK(p->ships == 2);
    CHECK(p->design == "cruiser");
    CHECK(p->rules.engage == Engage::Strangers);
    // The design and the mode stay as they were when only the number changes.
    REQUIRE(SetPatrol(h.sim, "anna", "3", "verge").Ok());
    p = h.sim.PatrolOf(h.Corp(), "verge");
    CHECK(p->design == "cruiser");
    CHECK(p->rules.engage == Engage::Strangers);

    // What it costs: the default guard's rate a ship, more for a heavier design.
    Corps::Patrol plain;
    plain.ships = 3;
    CHECK(Simulation::PatrolUpkeep(plain) == doctest::Approx(300.0));
    CHECK(Simulation::PatrolUpkeep(*p) > Simulation::PatrolUpkeep(plain));
    CHECK(Simulation::PatrolDesign(plain) ==
          Archetypes::ShipCatalogue().Pick("default", "guard", 0));

    // It is the corporation's, kept in corps.json.
    Corps::Registry back;
    std::string     why;
    REQUIRE(back.FromJson(h.sim.Corporations().ToJson(), why));
    const Corps::Patrol& kept = back.Find(h.Corp())->patrols.at("verge");
    CHECK(kept.ships == 3);
    CHECK(kept.design == "cruiser");
    CHECK(kept.rules.engage == Engage::Strangers);

    // Standing down is always allowed.
    CHECK(SetPatrol(h.sim, "anna", "none", "verge").Ok());
    CHECK(h.sim.PatrolOf(h.Corp(), "verge") == nullptr);
}

TEST_CASE("a patrol is paid for from the treasury, makes good its losses and stands down "
          "unpaid (#431 slice 12)")
{
    RulesGuard          guard;
    Held                h;
    WorldRules::Patrols r;
    r.shipHour = 100.0;
    r.regrowHours = 0.5;
    WorldRules::SetPatrolRules(r);
    REQUIRE(h.sim.Corporations().Credit(h.Corp(), 450.0));
    REQUIRE(SetPatrol(h.sim, "anna", "3", "verge").Ok());
    const float before = h.sim.InfluenceIn(h.St()).corp.at(h.Corp());

    h.sim.StepPatrols(1800.0);  // half an hour: 150 cr, and the first ship
    CHECK(h.Treasury() == doctest::Approx(300.0));
    CHECK(h.There() == doctest::Approx(1.0f));
    h.sim.StepPatrols(3600.0);  // an hour: 300 cr, and the other two
    CHECK(h.Treasury() == doctest::Approx(0.0));
    CHECK(h.There() == doctest::Approx(3.0f));
    // They are presence: influence, at a faction ship's weight.
    CHECK(h.sim.InfluenceIn(h.St()).corp.at(h.Corp()) ==
          doctest::Approx(before + 3.0f * (float)WorldRules::InfluenceRules().perShip));

    // The treasury is empty: they stand down, and the corporation is told once.
    h.sim.StepPatrols(1800.0);
    CHECK(h.Agg().patrols.count(h.Corp()) == 0);
    int told = 0;
    for (const Ev::Event& e : h.sim.Corporations().Find(h.Corp())->journal)
        told += e.text.find("can no longer pay for its patrol") != std::string::npos ? 1 : 0;
    CHECK(told == 1);
    h.sim.StepPatrols(1800.0);
    CHECK(told == 1);

    // Paid again, it comes back a ship at a time; the setting was kept.
    REQUIRE(h.sim.Corporations().Credit(h.Corp(), 1000.0));
    h.sim.StepPatrols(1800.0);
    CHECK(h.There() == doctest::Approx(1.0f));

    // Losing what it guarded stands it down, unpaid: nothing left to guard there.
    auto& e = h.St().entities;
    e.erase(std::remove_if(e.begin(), e.end(), [](const std::unique_ptr<Entity>& en)
                           { return en->GetKind() == EntityKind::Structure; }),
            e.end());
    h.Agg().holderCorp.clear();
    const double money = h.Treasury();
    h.sim.StepPatrols(1800.0);
    CHECK(h.Agg().patrols.count(h.Corp()) == 0);
    CHECK(h.Treasury() == doctest::Approx(money));
}

TEST_CASE("guards never fire on the corporation's members, and engage as its rules say "
          "(#431 slice 12)")
{
    RulesGuard guard;
    Held       h;
    REQUIRE(SetPatrol(h.sim, "anna", "2", "verge", "", "strangers").Ok());
    h.Agg().patrols[h.Corp()] = 2.0f;
    const Vector2  near{ h.outpost.x + 700.0f, h.outpost.y };
    ClientSession& anna = h.Pilot("anna", near);
    ClientSession& boris = h.Pilot("boris", { near.x, near.y + 150.0f });
    ClientSession& dima = h.Pilot("dima", { near.x, near.y - 150.0f });
    h.sim.Warm(h.St());
    const std::vector<const NpcShip*> guards = h.Guards();
    REQUIRE(guards.size() == 2);
    const NpcShip& g = *guards.front();
    CHECK(g.GetCorp() == h.Corp());
    CHECK(g.GetName() == "[DH] patrol");

    // Strangers: anyone but a member.
    CHECK_FALSE(h.sim.NpcHunts(h.St(), anna, g));
    CHECK_FALSE(h.sim.NpcHunts(h.St(), boris, g));
    CHECK(h.sim.NpcHunts(h.St(), dima, g));

    std::vector<Simulation::PlayerPresence> players;
    for (ClientSession* s : { &anna, &boris, &dima })
    {
        Simulation::PlayerPresence p;
        p.session = s;
        p.ship = s->ship.get();
        players.push_back(p);
    }
    std::vector<FireEvent> fires;
    for (int i = 0; i < 60 * 20; i++)
        h.sim.StepSystemAgents(h.St(), players, &fires, 1.0f / 60.0f);
    bool atDima = false, atMember = false;
    for (const FireEvent& f : fires)
    {
        atDima = atDima || f.targetSessionId == dima.id;
        atMember = atMember || f.targetSessionId == anna.id || f.targetSessionId == boris.id;
    }
    CHECK(atDima);
    CHECK_FALSE(atMember);

    // Enemies (the default): an innocent stranger is let be, a flagged criminal is not; a
    // member never, flagged or not.
    REQUIRE(SetPatrol(h.sim, "anna", "2", "verge", "", "enemies").Ok());
    CHECK_FALSE(h.sim.NpcHunts(h.St(), dima, g));
    dima.account.SetCriminalLeft(600.0);
    boris.account.SetCriminalLeft(600.0);
    CHECK(h.sim.NpcHunts(h.St(), dima, g));
    CHECK_FALSE(h.sim.NpcHunts(h.St(), boris, g));

    // Hold fire: nobody, not even a criminal.
    REQUIRE(SetPatrol(h.sim, "anna", "2", "verge", "", "hold").Ok());
    CHECK_FALSE(h.sim.NpcHunts(h.St(), dima, g));

    // A guard shot down is one ship fewer, and the patrol makes it good from what it has.
    const int before = (int)h.Guards().size();
    for (auto& e : h.St().entities)
        if (e->GetKind() == EntityKind::Npc &&
            static_cast<NpcShip&>(*e).GetRole() == NpcRole::Guard)
        {
            static_cast<NpcShip&>(*e).TakeDamage(1.0e9f);
            break;
        }
    REQUIRE(h.sim.Corporations().Credit(h.Corp(), 1.0e6));
    h.sim.MaintainWorld(2.0f);  // a coarse pass: counted, and paid for
    CHECK(h.There() == doctest::Approx(1.0f).epsilon(0.01));
    CHECK((int)h.Guards().size() == before - 1);
}

namespace
{
// Boris's outpost in Helios Core, the Traders Guild's, standing there without a licence: the
// Guild sends its warships to evict it (#431 slice 6), and nobody is on to see. With `patrol`
// ships of DH's standing by it. What the siege has left after `passes` coarse passes once it
// is there, and what is left of the patrol.
struct Eviction
{
    float force = 0.0f;
    float patrol = 0.0f;
    bool  over = false;
};

Eviction Evict(bool hot, int patrol, int passes)
{
    WorldRules::Licences lr = WorldRules::LicenceRules();
    lr.evict = 50.0;  // decisive: the Guild does it this period
    WorldRules::SetLicenceRules(lr);
    Simulation sim;
    sim.LoadUniverse(std::string(TEST_WORLD_DIR) + "universe.json");
    sim.Seed(1234u);
    sim.SetAllHot(hot);
    sim.InitGalaxy();
    sim.MaterializeAllSystems(std::string(TEST_WORLD_DIR) + "systems/");
    Corps::Registry& r = sim.Corporations();
    REQUIRE(r.Create("anna", "Deep Haul", "DH", NOW).Ok());
    REQUIRE(r.InviteTo("anna", "boris", { "boris" }, NOW).Ok());
    REQUIRE(r.Join("boris", "DH", NOW).Ok());
    const std::string corp = r.IdOf("anna");
    REQUIRE(r.Credit(corp, 1.0e7));
    SystemState& core = *sim.SystemById("core");
    REQUIRE(core.agg.controller == Fac::TradersGuild());
    BuildIn(sim, "core", "outpost", "boris", { 90000.0f, 250000.0f });
    sim.StepLicences();  // found without one: asked from now, a grace day
    sim.SetTime(sim.Time() + DAY + 1.0);
    sim.StepLicences();
    REQUIRE(core.trespass.size() == 1);
    if (patrol > 0)
    {
        REQUIRE(SetPatrol(sim, "anna", std::to_string(patrol), "core").Ok());
        core.agg.patrols[corp] = (float)patrol;
    }

    const float cap = Factions::TemperamentOf(Fac::TradersGuild()).capacity;
    int         siege = 0;
    for (int pass = 0; pass < 200 && siege == 0; pass++)
    {
        core.agg.presence[Fac::TradersGuild()] = cap * 2.0f;
        sim.MindOf(Fac::TradersGuild()).stock = Simulation::OutpostCost() * 20.0f;
        sim.StepFactions();
        for (const auto& kv : sim.Plans())
            if (kv.second.kind == Plan::Kind::Siege && kv.second.evicting == "boris")
                siege = kv.first;
    }
    REQUIRE(siege != 0);
    // The host's tick, for whole coarse passes, until the siege is there and then `passes`.
    auto pass = [&]()
    {
        const float dt = 1.0f / 60.0f;
        for (int i = 0; i < 120; i++)
        {
            if (hot)
                for (auto& kv : sim.Systems())
                    sim.StepSystemAgents(kv.second, {}, nullptr, dt);
            sim.MaintainWorld(dt);
        }
    };
    for (int i = 0; i < 400; i++)
    {
        const auto it = sim.Plans().find(siege);
        if (it == sim.Plans().end() || it->second.arrived)
            break;
        pass();
    }
    for (int i = 0; i < passes; i++)
        pass();
    Eviction   out;
    const auto it = sim.Plans().find(siege);
    out.over = it == sim.Plans().end();
    out.force = out.over ? 0.0f : it->second.force;
    const auto left = core.agg.patrols.find(corp);
    out.patrol = left != core.agg.patrols.end() ? left->second : 0.0f;
    return out;
}
}  // namespace

TEST_CASE("a patrol guards its members' structure while they are offline, hot and cold alike "
          "(#431 slice 12)")
{
    RulesGuard guard;
    LoadRegistries();
    // Nobody stands by a trespasser's structure: the besiegers lose nothing.
    const Eviction alone = Evict(false, 0, 10);
    // A patrol stands by it: both sides bleed, by their designs' guns and hulls.
    const Eviction guarded = Evict(false, 4, 10);
    REQUIRE_FALSE(alone.over);
    CHECK((guarded.over || guarded.force < alone.force - 0.01f));
    CHECK(guarded.patrol < 4.0f);
    // ...and the same where somebody watches: the guards are its face, the arithmetic its
    // course.
    const Eviction watched = Evict(true, 4, 10);
    CHECK(watched.over == guarded.over);
    CHECK(watched.force == doctest::Approx(guarded.force).epsilon(0.05));
    CHECK(watched.patrol == doctest::Approx(guarded.patrol).epsilon(0.05));
}
