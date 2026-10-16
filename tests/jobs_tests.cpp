#include <doctest/doctest.h>

#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "entities/NpcShip.h"
#include "entities/Ship.h"
#include "entities/ShipType.h"
#include "entities/Station.h"
#include "entities/Structure.h"
#include "gen/Galaxy.h"
#include "gen/Region.h"
#include "missions/Mission.h"
#include "sim/ClientSession.h"
#include "sim/Observation.h"
#include "sim/Protocol.h"
#include "sim/Simulation.h"
#include "sim/WorldRules.h"

#include "factions_named.h"
#include "temp_dir.h"

#include <cmath>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

// Jobs a faction's plans post (#345): the board fed by live plans, what a pilot does feeding
// the plan back, and the job in the save, on the wire and in what an agent reads.

namespace
{
const std::string DATA = TEST_DATA_DIR;

void LoadRegistries()
{
    REQUIRE(Factions::Load(DATA + "factions.json"));
    REQUIRE(Archetypes::Load(DATA + "archetypes.json"));
    REQUIRE(Blueprints::Load(DATA + "blueprints.json"));
}

Gen::WorldParams Params(uint64_t seed)
{
    Gen::WorldParams p;
    p.seed = seed;
    p.centres = 3;
    p.owners = Factions::MajorIds();
    p.pirates = Factions::Id(Factions::Pirates());
    p.unaffiliated = Factions::Id(Factions::Unaffiliated());
    return p;
}

void Make(Simulation& sim, uint64_t seed)
{
    REQUIRE(sim.OpenGalaxy(Params(seed), DATA, {}));
    sim.InitGalaxy();
    sim.MaterializeAllSystems(DATA + "systems/");
}

void RunMinutes(Simulation& sim, double minutes)
{
    for (int i = 0; i < (int)(minutes * 30.0); i++)
        sim.MaintainWorld(2.0f);
}

// The design's rules for every test, whatever the last test set; restored when it ends.
struct Rules
{
    WorldRules::Growth  growth = WorldRules::GrowthRules();
    WorldRules::Convoys convoys = WorldRules::ConvoyRules();
    WorldRules::Jobs    jobs = WorldRules::JobRules();
    Rules()
    {
        WorldRules::SetGrowthRules(WorldRules::Growth{});
        WorldRules::SetConvoyRules(WorldRules::Convoys{});
        WorldRules::SetJobRules(WorldRules::Jobs{});
    }
    ~Rules()
    {
        WorldRules::SetGrowthRules(growth);
        WorldRules::SetConvoyRules(convoys);
        WorldRules::SetJobRules(jobs);
    }
};

int FirstFrontier(const Simulation& sim)
{
    for (const Simulation::RegionOpening& o : sim.OpeningLog())
        if (o.parent >= 0)
            return o.slot;
    return -1;
}

// A system of the first frontier `f` holds, with a finished outpost of its own and a station of
// its own a long way from it: somewhere to read the faction's board.
struct Holding
{
    std::string system;
    int         outpost = 0;
    int         station = 0;
    Vector2     at{ 0.0f, 0.0f };  // the outpost
};
Holding PlantOutpost(Simulation& sim, FactionId f)
{
    const int slot = FirstFrontier(sim);
    REQUIRE(slot >= 0);
    for (const std::string& id : sim.RegionSystems(slot))
    {
        SystemState* st = sim.SystemById(id);
        if (st == nullptr || st->profile.stations > 0 || st->profile.markets > 0 ||
            st->profile.outpostSite || (st->agg.claimed && st->agg.controller != f))
            continue;
        const Simulation::OutpostSpot spot =
            sim.FindOutpostSpot(f, Outposts::Purpose::Open, id, id);
        if (!spot.found)
            continue;
        const Blueprint& bp = *Blueprints::Find(Outposts::BLUEPRINT);
        auto             t = std::make_unique<Structure>(spot.at, 0.0f, "Outpost", bp.archetype);
        t->SetBlueprint(bp.id);
        if (spot.orbit)
            t->SetOrbit(*spot.orbit);
        Holding h;
        h.system = id;
        h.at = spot.at;
        h.outpost = sim.AddStatic(id, std::move(t), Outposts::OwnerOf(f));
        REQUIRE(h.outpost != 0);
        const Vector2 far{ spot.at.x + 9000.0f, spot.at.y + 9000.0f };
        h.station = sim.AddStatic(
            id, std::make_unique<Station>(far, 300.0f, "Guild Depot", f, StationRole::TradeHub),
            std::string());
        REQUIRE(h.station != 0);
        st->agg.claimed = true;
        st->agg.controller = f;
        st->agg.presence[f] = Factions::TemperamentOf(f).capacity;
        return h;
    }
    FAIL("no room for an outpost in the first frontier");
    return {};
}

const Plan* PlanOf(const Simulation& sim, int id)
{
    const auto it = sim.Plans().find(id);
    return it == sim.Plans().end() ? nullptr : &it->second;
}

Vector2 PosOf(const Simulation& sim, const std::string& system, int id)
{
    for (const auto& e : sim.SystemById(system)->entities)
        if (e->GetId() == id)
            return e->GetPosition();
    FAIL("no such entity");
    return { 0.0f, 0.0f };
}

// A pilot docked at the holding's station: the board is its faction's.
ClientSession& DockedAt(Simulation& sim, const Holding& h)
{
    const Vector2  at = PosOf(sim, h.system, h.station);
    ClientSession& s =
        sim.CreateSession(h.system, { at.x + 350.0f, at.y }, GetShipCatalog()[0].stats);
    REQUIRE(sim.StepPlayerDock(s, *sim.SystemById(h.system)) == h.station);
    return s;
}

// The offer of `type` on the board for plan `planId`; -1 when there is none.
int OfferOf(const ClientSession& s, MissionType type, int planId)
{
    const std::vector<Mission>& offers = s.missions.Offers();
    for (size_t i = 0; i < offers.size(); i++)
        if (offers[i].type == type && offers[i].planId == planId)
            return (int)i;
    return -1;
}

int ActiveOf(const ClientSession& s, MissionType type)
{
    const std::vector<Mission>& active = s.missions.Active();
    for (size_t i = 0; i < active.size(); i++)
        if (active[i].type == type)
            return (int)i;
    return -1;
}

// A Guild holding with a market on its way to it, and the stock to pay for jobs.
struct Upgrade
{
    Holding h;
    int     plan = 0;
};
Upgrade MarketGoingUp(Simulation& sim, FactionId f)
{
    Upgrade u;
    u.h = PlantOutpost(sim, f);
    sim.MindOf(f).stock = 4.0f * Simulation::OutpostCost();
    u.plan = sim.BeginUpgrade(f, u.h.system, Outposts::MARKET_BLUEPRINT);
    REQUIRE(u.plan != 0);
    return u;
}
}  // namespace

TEST_CASE("the jobs rules read, and a misspelt or impossible one is an error (#345)")
{
    Rules r;
    REQUIRE(WorldRules::Load(DATA + "world_rules.json"));
    CHECK(WorldRules::JobRules().reach > 0);
    CHECK(WorldRules::JobRules().supplyCut > 0.0);
    CHECK(WorldRules::JobRules().supplyCut <= 1.0);

    TempDir     dir;
    std::string text;
    {
        std::ifstream in(DATA + "world_rules.json");
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    auto loads = [&](const std::string& from, const std::string& to)
    {
        std::string  changed = text;
        const size_t at = changed.find(from);
        REQUIRE(at != std::string::npos);
        changed.replace(at, from.size(), to);
        const std::string path = dir.Path("rules.json");
        std::ofstream(path) << changed;
        return WorldRules::Load(path);
    };
    CHECK_FALSE(loads("\"supplyCut\"", "\"supplyCutt\""));
    CHECK(WorldRules::Error().find("supplyCutt") != std::string::npos);
    CHECK_FALSE(loads("\"supplyCut\": 0.35", "\"supplyCut\": 1.5"));
    CHECK_FALSE(loads("\"creditsPerStock\": 50", "\"creditsPerStock\": 0"));
}

TEST_CASE("a convoy on its way posts an escort job; a pilot with it is paid when it arrives (#345)")
{
    LoadRegistries();
    Rules           r;
    const FactionId f = Fac::TradersGuild();
    Simulation      sim;
    Make(sim, 7);
    const Upgrade  u = MarketGoingUp(sim, f);
    ClientSession& s = DockedAt(sim, u.h);

    const int offer = OfferOf(s, MissionType::Escort, u.plan);
    REQUIRE(offer >= 0);
    const Mission job = s.missions.Offers()[(size_t)offer];
    CHECK(job.faction == f);
    CHECK(job.systemId == u.h.system);
    CHECK(job.deadline > sim.Time());
    CHECK(job.rewardMoney > 0.0);
    // Jobs go before the station's own work.
    CHECK(offer == 0);
    // ...and the station's own work is still there.
    CHECK(s.missions.Offers().size() > 1);

    REQUIRE(sim.AcceptMission(s, offer));
    CHECK(OfferOf(s, MissionType::Escort, u.plan) < 0);
    sim.StepPlayerUndock(s);
    // Not yet with it, until a pass finds the pilot where the convoy is.
    REQUIRE(ActiveOf(s, MissionType::Escort) == 0);
    CHECK_FALSE(sim.MissionCompletableNow(s, s.missions.Active()[0]));

    const double money = s.account.GetMoney();
    const float  standing = s.account.GetReputation(f);
    bool         arrived = false;
    for (int pass = 0; pass < 60 * 30 && !arrived; pass++)
    {
        const Plan* p = PlanOf(sim, u.plan);
        REQUIRE(p != nullptr);
        REQUIRE(p->setbacks == 0);
        if (p->leg >= 0 && s.systemId != p->route[(size_t)p->leg])
            sim.ServerEnterSystem(s, p->route[(size_t)p->leg], s.systemId);
        sim.MaintainWorld(2.0f);
        arrived = PlanOf(sim, u.plan) != nullptr && PlanOf(sim, u.plan)->arrived;
    }
    REQUIRE(arrived);
    sim.MaintainWorld(2.0f);
    REQUIRE(ActiveOf(s, MissionType::Escort) == 0);
    const Mission& done = s.missions.Active()[0];
    CHECK(done.rode);
    CHECK(done.progress == done.targetCount);
    CHECK(sim.MissionCompletableNow(s, done));

    // Handed in anywhere -- in flight, nowhere near the station -- and paid out of the stock.
    const float  stock = sim.MindOf(f).stock;
    const double reward = done.rewardMoney;
    REQUIRE(sim.CompleteMission(s, 0));
    CHECK(s.missions.Active().empty());
    CHECK(s.account.GetMoney() == doctest::Approx(money + reward));
    CHECK(s.account.GetReputation(f) > standing);
    CHECK(sim.MindOf(f).stock ==
          doctest::Approx(std::max(0.0, stock - reward / WorldRules::JobRules().creditsPerStock)));
}

TEST_CASE("an escort is wanted where the convoy is, not only where it is bound (#407)")
{
    LoadRegistries();
    Rules      r;
    Simulation sim;
    Make(sim, 7);
    // The faction whose centre the frontier opened from: its convoys set out from that centre.
    const Simulation::RegionOpening* o = sim.OpeningOf(FirstFrontier(sim));
    REQUIRE(o != nullptr);
    const FactionId f = sim.SystemById(o->mouth)->agg.controller;
    const Upgrade   u = MarketGoingUp(sim, f);
    const Plan*     p = PlanOf(sim, u.plan);
    REQUIRE(p != nullptr);
    REQUIRE(p->leg == 0);
    REQUIRE(p->route.size() >= 2);
    const std::string from = p->route.front();
    REQUIRE(from != u.h.system);

    // Only a board in the system itself counts: where the convoy sets out, a long way from its
    // site, the faction's station still has the job.
    WorldRules::Jobs here = WorldRules::JobRules();
    here.reach = 0;
    WorldRules::SetJobRules(here);
    int station = 0;
    for (const auto& e : sim.SystemById(from)->entities)
        if (station == 0 && e->GetKind() == EntityKind::Station &&
            static_cast<const Station&>(*e).GetFaction() == f)
            station = e->GetId();
    REQUIRE(station != 0);
    const Vector2  at = PosOf(sim, from, station);
    ClientSession& s = sim.CreateSession(from, { at.x + 350.0f, at.y }, GetShipCatalog()[0].stats);
    REQUIRE(sim.StepPlayerDock(s, *sim.SystemById(from)) == station);
    const int offer = OfferOf(s, MissionType::Escort, u.plan);
    REQUIRE(offer >= 0);
    CHECK(s.missions.Offers()[(size_t)offer].systemId == u.h.system);  // bound for the site

    // A pilot docked at the site's system sees it too, as before.
    ClientSession& t = DockedAt(sim, u.h);
    CHECK(OfferOf(t, MissionType::Escort, u.plan) >= 0);
}

TEST_CASE("an escort the convoy never saw fails when it arrives, and costs standing (#345)")
{
    LoadRegistries();
    Rules           r;
    const FactionId f = Fac::TradersGuild();
    Simulation      sim;
    Make(sim, 7);
    const Upgrade  u = MarketGoingUp(sim, f);
    ClientSession& s = DockedAt(sim, u.h);
    const int      offer = OfferOf(s, MissionType::Escort, u.plan);
    REQUIRE(offer >= 0);
    REQUIRE(sim.AcceptMission(s, offer));
    // Docked the whole way: never with it.
    const float standing = s.account.GetReputation(f);
    for (int minute = 0; minute < 60 && !s.missions.Active().empty(); minute++)
        RunMinutes(sim, 1);
    CHECK(s.missions.Active().empty());
    CHECK(s.account.GetReputation(f) < standing);
}

TEST_CASE("pilots with a convoy stand between it and an ambush (#345)")
{
    LoadRegistries();
    Rules           r;
    const FactionId f = Fac::TradersGuild();
    // Attacked for certain wherever anyone hostile is, whatever the security: the fight, not
    // the chance of one, is what this is about.
    WorldRules::Convoys sure = WorldRules::ConvoyRules();
    sure.calm = 1.0e-4;
    sure.policed = 0.0;
    WorldRules::SetConvoyRules(sure);
    // The same convoy, ambushed where it is, with a pilot on its escort job and with a pilot on
    // none. The fight goes the same way in both -- its draws are the convoy's and the place's
    // -- so whatever differs is the pilot's doing.
    auto freight = [&](bool escorted)
    {
        Simulation sim;
        Make(sim, 7);
        const Upgrade u = MarketGoingUp(sim, f);
        const Plan*   p = PlanOf(sim, u.plan);
        REQUIRE(p != nullptr);
        REQUIRE(p->leg == 0);
        const std::string where = p->route[0];
        ClientSession&    s = DockedAt(sim, u.h);
        if (escorted)
        {
            const int offer = OfferOf(s, MissionType::Escort, u.plan);
            REQUIRE(offer >= 0);
            REQUIRE(sim.AcceptMission(s, offer));
        }
        sim.StepPlayerUndock(s);
        if (s.systemId != where)
            sim.ServerEnterSystem(s, where, s.systemId);
        // Until it has crossed the system it set out from.
        for (int pass = 0;
             pass < 120 && PlanOf(sim, u.plan)->leg == 0 && PlanOf(sim, u.plan)->setbacks == 0;
             pass++)
        {
            sim.SystemById(where)->agg.presence[Factions::Pirates()] = 3.0f;
            s.ship->Repair();  // the pilot is there to be counted, not to be shot down
            sim.MaintainWorld(2.0f);
        }
        const Plan* after = PlanOf(sim, u.plan);
        REQUIRE(after != nullptr);
        CHECK(after->escorted == escorted);
        REQUIRE(after->leg != 0);  // across, arrived, or turned back
        return after->setbacks > 0 ? 0.0f : after->freight;
    };
    const float alone = freight(false);
    const float guarded = freight(true);
    CHECK(alone < 1.0f);  // it was hit
    CHECK(guarded > alone);
}

TEST_CASE("a site going up posts a supply job, and a delivery brings it closer to done (#345)")
{
    LoadRegistries();
    Rules           r;
    const FactionId f = Fac::TradersGuild();
    Simulation      sim;
    Make(sim, 7);
    const Upgrade u = MarketGoingUp(sim, f);
    for (int minute = 0; minute < 30 && !PlanOf(sim, u.plan)->arrived; minute++)
        RunMinutes(sim, 1);
    const Plan* p = PlanOf(sim, u.plan);
    REQUIRE(p != nullptr);
    REQUIRE(p->arrived);
    const Structure* site = sim.JobSite(*p);
    REQUIRE(site != nullptr);

    ClientSession& s = DockedAt(sim, u.h);
    const int      offer = OfferOf(s, MissionType::Supply, u.plan);
    REQUIRE(offer >= 0);
    const Mission job = s.missions.Offers()[(size_t)offer];
    // Half the largest line of the market's cost (48 Iron), due when the site would be done.
    CHECK(ResourceName(job.resource) == "Iron");
    CHECK(job.targetCount == 24);
    CHECK(job.deadline == doctest::Approx(site->GetCompletesAt()));
    REQUIRE(sim.AcceptMission(s, offer));
    sim.StepPlayerUndock(s);

    // Not without the goods, and not away from the site.
    const Mission& m = s.missions.Active()[(size_t)ActiveOf(s, MissionType::Supply)];
    CHECK_FALSE(sim.MissionCompletableNow(s, m));
    REQUIRE(s.ship->AddCargo(job.resource, job.targetCount));
    CHECK_FALSE(sim.MissionCompletableNow(s, m));
    s.ship->Teleport({ site->GetPosition().x + site->GetSize() + 200.0f, site->GetPosition().y });
    REQUIRE(sim.MissionCompletableNow(s, m));
    CHECK(sim.JobSiteId(s, m) == site->GetId());

    const double completes = site->GetCompletesAt();
    const double left = completes - sim.Time();
    const double due = p->dueAt;
    REQUIRE(sim.CompleteMission(s, ActiveOf(s, MissionType::Supply)));
    CHECK(s.ship->GetCargoAmount(job.resource) == 0);
    CHECK(PlanOf(sim, u.plan)->supplied == 1);
    CHECK(site->GetCompletesAt() ==
          doctest::Approx(sim.Time() + left * (1.0 - WorldRules::JobRules().supplyCut)));
    CHECK(PlanOf(sim, u.plan)->dueAt < due);

    // The site finishes on its new time line, and the plan with it.
    const double finish = site->GetCompletesAt();
    const int    markets = sim.SystemById(u.h.system)->profile.markets;  // the depot's
    for (int pass = 0; pass < 60 * 30 && PlanOf(sim, u.plan) != nullptr; pass++)
        sim.MaintainWorld(2.0f);
    CHECK(PlanOf(sim, u.plan) == nullptr);
    CHECK(sim.SystemById(u.h.system)->profile.markets == markets + 1);
    CHECK(sim.Time() <= finish + 4.0);
    CHECK(sim.Time() < completes);
}

TEST_CASE("a plan's job survives the save, and one whose plan is over is dropped (#345)")
{
    LoadRegistries();
    Rules           r;
    const FactionId f = Fac::TradersGuild();
    Simulation      sim;
    Make(sim, 7);
    const Upgrade  u = MarketGoingUp(sim, f);
    ClientSession& s = DockedAt(sim, u.h);
    const int      offer = OfferOf(s, MissionType::Escort, u.plan);
    REQUIRE(offer >= 0);
    REQUIRE(sim.AcceptMission(s, offer));
    s.missions.Active()[0].rode = true;
    const Mission was = s.missions.Active()[0];

    TempDir           dir;
    const std::string path = dir.Path("pilot.json");
    REQUIRE(sim.SaveAccount(s, path));
    ClientSession& back = sim.CreateSession(u.h.system, { 0.0f, 0.0f }, GetShipCatalog()[0].stats);
    REQUIRE(sim.LoadAccount(back, path) == Save::Result::Ok);
    REQUIRE(back.missions.Active().size() == 1);
    const Mission& m = back.missions.Active()[0];
    CHECK(m.type == MissionType::Escort);
    CHECK(m.planId == was.planId);
    CHECK(m.systemId == was.systemId);
    CHECK(m.systemName == was.systemName);
    CHECK(m.deadline == doctest::Approx(was.deadline));
    CHECK(m.stage == was.stage);
    CHECK(m.rode);

    // A world where the plan is over: the job goes, and the pilot is told.
    {
        std::ifstream  in(path);
        nlohmann::json j = nlohmann::json::parse(in);
        j["missions"][0]["plan"] = 999999;
        std::ofstream(path) << j.dump();
    }
    ClientSession& gone = sim.CreateSession(u.h.system, { 0.0f, 0.0f }, GetShipCatalog()[0].stats);
    REQUIRE(sim.LoadAccount(gone, path) == Save::Result::Ok);
    CHECK(gone.missions.Active().empty());
}

TEST_CASE("a plan's job crosses the wire, and an agent reads where, when and what it needs (#345)")
{
    LoadRegistries();
    Proto::Snapshot snap;
    snap.time = 1000.0;
    Proto::MissionView m;
    m.type = (int)MissionType::Siege;
    m.faction = Fac::TradersGuild();
    m.title = "Break the siege of Vela";
    m.targetCount = 3;
    m.progress = 1;
    m.rewardMoney = 750.0;
    m.planId = 42;
    m.systemId = "r1-2-3";
    m.systemName = "Vela";
    m.deadline = 1600.0;
    m.siteId = 0;
    m.against = Fac::Pirates();
    snap.missionActive.push_back(m);
    // ...and the other side's job of the same siege (#407).
    Proto::MissionView join = m;
    join.type = (int)MissionType::Besiege;
    join.faction = Fac::Pirates();
    join.against = Fac::TradersGuild();
    join.title = "Join the siege of Vela";
    snap.missionActive.push_back(join);

    Proto::Snapshot back;
    REQUIRE(Proto::DecodeSnapshot(Proto::EncodeSnapshot(snap), back));
    REQUIRE(back.missionActive.size() == 2);
    const Proto::MissionView& got = back.missionActive[0];
    CHECK(got.against == Fac::Pirates());
    CHECK(back.missionActive[1].type == (int)MissionType::Besiege);
    CHECK(back.missionActive[1].against == Fac::TradersGuild());
    CHECK(got.planId == 42);
    CHECK(got.systemId == "r1-2-3");
    CHECK(got.systemName == "Vela");
    CHECK(got.deadline == doctest::Approx(1600.0));

    Obs::View view;
    view.snapshot = &back;
    CHECK(Obs::JobDue(view, got) == "due in 10 min");
    CHECK(Obs::MissionNeeds(view, got) == "shoot down 2 more besiegers in Vela (system r1-2-3)");
    const std::string text = Obs::DescribeMissions(view);
    CHECK(text.find("siege: Break the siege of Vela") != std::string::npos);
    CHECK(text.find("hand in anywhere") != std::string::npos);
    CHECK(text.find("progress 1/3") != std::string::npos);
    // Which side each is for, and whom against.
    CHECK(text.find("for " + FactionName(Fac::TradersGuild()) + " (besieged), against " +
                    FactionName(Fac::Pirates())) != std::string::npos);
    CHECK(text.find("besiege: Join the siege of Vela") != std::string::npos);
    CHECK(text.find("for " + FactionName(Fac::Pirates()) + " (besieging), against " +
                    FactionName(Fac::TradersGuild())) != std::string::npos);
    CHECK(Obs::MissionNeeds(view, back.missionActive[1]) ==
          "shoot down 2 more of the " + FactionName(Fac::TradersGuild()) +
              " defenders in Vela (system r1-2-3)");
}

namespace
{
// The scene the siege tests play on (siege_tests.cpp): a Guild outpost next to a pirate den,
// with a Guild station beside it to post the job.
std::unique_ptr<Simulation> FixtureWorld()
{
    SetRandomSeed(0xC0FFEEu);
    LoadRegistries();
    auto sim = std::make_unique<Simulation>();
    sim->LoadUniverse(std::string(TEST_WORLD_DIR) + "universe.json");
    sim->AttachRegion(7, std::string(TEST_WORLD_DIR) + "systems/");
    sim->Seed(1234u);
    sim->InitGalaxy();
    sim->MaterializeAllSystems(std::string(TEST_WORLD_DIR) + "systems/");
    return sim;
}

void Pass(Simulation& sim)
{
    const float dt = 1.0f / 60.0f;
    for (int i = 0; i < 120; i++)
    {
        for (auto& kv : sim.Systems())
            sim.StepSystemAgents(kv.second, {}, nullptr, dt);
        sim.MaintainWorld(dt);
    }
}

const Plan* SiegeBy(const Simulation& sim, FactionId f)
{
    for (const auto& kv : sim.Plans())
        if (kv.second.faction == f && kv.second.kind == Plan::Kind::Siege)
            return &kv.second;
    return nullptr;
}
}  // namespace

TEST_CASE("a siege posts a break-the-siege job on the besieged's board; besiegers shot down pay "
          "(#345)")
{
    auto sim = FixtureWorld();
    // A pirate den and the unclaimed system next door, made the Guild's.
    std::string den, frontier;
    for (const auto& kv : sim->Systems())
    {
        if (!den.empty())
            break;
        if (!kv.second.agg.claimed || kv.second.agg.controller != Fac::Pirates())
            continue;
        for (const std::string& n : sim->Neighbors(kv.first))
            if (n != sim->Universe().startId && !sim->SystemById(n)->agg.claimed)
            {
                den = kv.first;
                frontier = n;
                break;
            }
    }
    REQUIRE_FALSE(den.empty());
    const Simulation::OutpostSpot spot =
        sim->FindOutpostSpot(Fac::TradersGuild(), Outposts::Purpose::Open, den, frontier);
    REQUIRE(spot.found);
    const Blueprint* bp = Blueprints::Find(Outposts::BLUEPRINT);
    auto t = std::make_unique<Structure>(spot.at, 0.0f, "Traders Guild Outpost", bp->archetype);
    t->SetBlueprint(bp->id);
    REQUIRE(sim->AddStatic(frontier, std::move(t), Outposts::OwnerOf(Fac::TradersGuild())) != 0);
    const Vector2 depot{ spot.at.x + 9000.0f, spot.at.y + 9000.0f };
    const int     station =
        sim->AddStatic(frontier,
                       std::make_unique<Station>(depot, 300.0f, "Guild Depot", Fac::TradersGuild(),
                                                 StationRole::TradeHub),
                       std::string());
    REQUIRE(station != 0);
    SystemAggregate& a = sim->SystemById(frontier)->agg;
    a.claimed = true;
    a.controller = Fac::TradersGuild();

    // The pirates, strong and knowing the garrison is thin, lay siege.
    const float cap = Factions::TemperamentOf(Fac::Pirates()).capacity;
    for (int pass = 0; pass < 90 && SiegeBy(*sim, Fac::Pirates()) == nullptr; pass++)
    {
        for (auto& kv : sim->Systems())
        {
            const bool held = kv.second.agg.claimed && kv.second.agg.controller == Fac::Pirates();
            kv.second.agg.presence[Fac::Pirates()] = kv.first == den ? cap
                                                     : held          ? cap * 0.6f
                                                                     : 0.0f;
            if (!held && kv.first != frontier)
                sim->MindOf(Fac::Pirates()).intel.erase(kv.first);
        }
        a.presence[Fac::TradersGuild()] = 1.0f;
        a.presence[Fac::Pirates()] = 0.0f;
        sim->MindOf(Fac::Pirates()).stock = Simulation::OutpostCost() * 2.0f;
        sim->MindOf(Fac::Pirates()).intel[frontier] = sim->Observe(*sim->SystemById(frontier));
        Pass(*sim);
    }
    const Plan* siege = SiegeBy(*sim, Fac::Pirates());
    REQUIRE(siege != nullptr);
    const int plan = siege->id;

    // A Guild pilot docked at the depot sees the job.
    sim->MindOf(Fac::TradersGuild()).stock = 4.0f * Simulation::OutpostCost();
    ClientSession& s =
        sim->CreateSession(frontier, { depot.x + 350.0f, depot.y }, GetShipCatalog()[0].stats);
    REQUIRE(sim->StepPlayerDock(s, *sim->SystemById(frontier)) == station);
    const int offer = OfferOf(s, MissionType::Siege, plan);
    REQUIRE(offer >= 0);
    REQUIRE(sim->AcceptMission(s, offer));
    const int need = s.missions.Active()[0].targetCount;
    REQUIRE(need >= 1);
    sim->StepPlayerUndock(s);
    for (int pass = 0; pass < 60 && SiegeBy(*sim, Fac::Pirates()) != nullptr &&
                       !SiegeBy(*sim, Fac::Pirates())->arrived;
         pass++)
        Pass(*sim);
    REQUIRE(SiegeBy(*sim, Fac::Pirates()) != nullptr);
    REQUIRE(SiegeBy(*sim, Fac::Pirates())->arrived);

    // A ship that is not a besieger counts for nothing; each besieger shot down counts one.
    NpcShip trader({ 0.0f, 0.0f }, Fac::Pirates(), NpcRole::Trader, {});
    sim->CreditJobKill(s, trader);
    CHECK(s.missions.Active()[0].progress == 0);
    int shot = 0;
    for (const auto& e : sim->SystemById(frontier)->entities)
    {
        if (shot >= need || e->GetKind() != EntityKind::Npc)
            continue;
        NpcShip& n = static_cast<NpcShip&>(*e);
        if (n.GetRole() != NpcRole::Warship || n.GetFaction() != Fac::Pirates() || n.GetTask() != 0)
            continue;
        n.TakeDamage(1.0e9f);
        sim->CreditJobKill(s, n);
        shot++;
    }
    REQUIRE(shot == need);
    CHECK(s.missions.Active()[0].progress == need);
    REQUIRE(sim->MissionCompletableNow(s, s.missions.Active()[0]));
    const double money = s.account.GetMoney();
    const double reward = s.missions.Active()[0].rewardMoney;
    REQUIRE(sim->CompleteMission(s, 0));
    CHECK(s.account.GetMoney() == doctest::Approx(money + reward));
}
