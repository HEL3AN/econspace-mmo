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

namespace
{
// A pilot docked at a station of a system other than `not`: somewhere a delivery is not.
ClientSession& DockedElsewhere(Simulation& sim, const std::string& notIn)
{
    for (auto& kv : sim.Systems())
    {
        if (kv.first == notIn)
            continue;
        for (const auto& e : kv.second.entities)
        {
            if (e->GetKind() != EntityKind::Station)
                continue;
            const Vector2  at = e->GetPosition();
            ClientSession& s = sim.CreateSession(kv.first, { at.x + e->GetSize() + 20.0f, at.y },
                                                 GetShipCatalog()[0].stats);
            if (sim.StepPlayerDock(s, kv.second) != 0)
                return s;
        }
    }
    FAIL("no station to dock at elsewhere");
    return sim.CreateSession(notIn, { 0.0f, 0.0f }, GetShipCatalog()[0].stats);
}

bool Told(const ClientSession& s, const std::string& what)
{
    for (const Ev::Event& e : s.journal)
        if (e.text.find(what) != std::string::npos)
            return true;
    return false;
}
}  // namespace

TEST_CASE("supply goods are delivered on arrival: beside the site or docked in its system, not "
          "elsewhere (#429)")
{
    LoadRegistries();
    Rules           r;
    const FactionId f = Fac::TradersGuild();
    Simulation      sim;
    Make(sim, 7);
    const Upgrade u = MarketGoingUp(sim, f);
    for (int minute = 0; minute < 30 && !PlanOf(sim, u.plan)->arrived; minute++)
        RunMinutes(sim, 1);
    REQUIRE(PlanOf(sim, u.plan) != nullptr);
    REQUIRE(PlanOf(sim, u.plan)->arrived);
    const Structure* site = sim.JobSite(*PlanOf(sim, u.plan));
    REQUIRE(site != nullptr);

    // Two pilots take the job at the depot in the site's system.
    ClientSession& a = DockedAt(sim, u.h);
    const int      offer = OfferOf(a, MissionType::Supply, u.plan);
    REQUIRE(offer >= 0);
    const Mission job = a.missions.Offers()[(size_t)offer];
    REQUIRE(sim.AcceptMission(a, offer));
    ClientSession& b = DockedAt(sim, u.h);
    const int      offerB = OfferOf(b, MissionType::Supply, u.plan);
    REQUIRE(offerB >= 0);
    REQUIRE(sim.AcceptMission(b, offerB));

    // The snapshot names the site wherever the pilot is.
    CHECK(sim.JobSiteName(job) == site->GetName());

    // Docked in the site's system without the goods: nothing happens.
    sim.MaintainWorld(2.0f);
    REQUIRE(ActiveOf(a, MissionType::Supply) >= 0);

    // In flight with the goods but far from the site: nothing yet.
    sim.StepPlayerUndock(a);
    REQUIRE(a.ship->AddCargo(job.resource, job.targetCount));
    a.ship->Teleport({ site->GetPosition().x + site->GetSize() + 9000.0f, site->GetPosition().y });
    for (int pass = 0; pass < 3; pass++)
        sim.MaintainWorld(2.0f);
    REQUIRE(ActiveOf(a, MissionType::Supply) >= 0);
    CHECK(a.ship->GetCargoAmount(job.resource) == job.targetCount);

    // Docked at a station in another system with the goods: not delivered either.
    ClientSession& c = DockedElsewhere(sim, job.systemId);
    c.missions.Active().push_back(a.missions.Active()[(size_t)ActiveOf(a, MissionType::Supply)]);
    REQUIRE(c.ship->AddCargo(job.resource, job.targetCount));
    CHECK_FALSE(sim.MissionCompletableNow(c, c.missions.Active().back()));
    for (int pass = 0; pass < 3; pass++)
        sim.MaintainWorld(2.0f);
    CHECK(ActiveOf(c, MissionType::Supply) >= 0);
    CHECK(c.ship->GetCargoAmount(job.resource) == job.targetCount);
    CHECK(PlanOf(sim, u.plan)->supplied == 0);

    // Beside the site, undocked: delivered on the next pass, with no Hand in -- paid, the goods
    // gone, the site sped, and the pilot told.
    const double money = a.account.GetMoney();
    const double completes = site->GetCompletesAt();
    a.ship->Teleport({ site->GetPosition().x + site->GetSize() + 200.0f, site->GetPosition().y });
    sim.MaintainWorld(2.0f);
    CHECK(ActiveOf(a, MissionType::Supply) < 0);
    CHECK(a.ship->GetCargoAmount(job.resource) == 0);
    CHECK(a.account.GetMoney() == doctest::Approx(money + job.rewardMoney));
    CHECK(PlanOf(sim, u.plan)->supplied == 1);
    CHECK(site->GetCompletesAt() < completes);
    CHECK(Told(a, "Delivered -- " + std::to_string(job.targetCount) + " " +
                      ResourceName(job.resource) + " to the " + site->GetName()));

    // Docked at the depot in the site's system: delivered there too.
    REQUIRE(b.IsDocked());
    REQUIRE(b.ship->AddCargo(job.resource, job.targetCount));
    const Mission& mb = b.missions.Active()[(size_t)ActiveOf(b, MissionType::Supply)];
    CHECK(sim.MissionCompletableNow(b, mb));
    const double moneyB = b.account.GetMoney();
    const double completesB = site->GetCompletesAt();
    sim.MaintainWorld(2.0f);
    CHECK(ActiveOf(b, MissionType::Supply) < 0);
    CHECK(b.ship->GetCargoAmount(job.resource) == 0);
    CHECK(b.account.GetMoney() > moneyB);
    CHECK(PlanOf(sim, u.plan)->supplied == 2);
    CHECK(site->GetCompletesAt() < completesB);
    CHECK(Told(b, "Delivered -- "));
}

TEST_CASE("a supply job reads where it is delivered, and is not done until the goods are there "
          "(#429)")
{
    LoadRegistries();
    Proto::Snapshot snap;
    snap.time = 1000.0;
    Proto::MissionView m;
    m.type = (int)MissionType::Supply;
    m.faction = Fac::TradersGuild();
    m.title = "Supply: Watch Post";
    m.resource = ResourceType::FromIndex(0).Index();
    m.targetCount = 20;
    m.rewardMoney = 500.0;
    m.planId = 7;
    m.systemId = "r1-2-3";
    m.systemName = "Vela";
    m.deadline = 1600.0;
    m.siteName = "Watch Post";
    snap.missionActive.push_back(m);
    snap.player.cargoByType.assign(64, 0);

    Proto::Snapshot back;
    REQUIRE(Proto::DecodeSnapshot(Proto::EncodeSnapshot(snap), back));
    REQUIRE(back.missionActive.size() == 1);
    CHECK(back.missionActive[0].siteName == "Watch Post");

    Obs::View view;
    view.snapshot = &back;
    const std::string good = ResourceName(ResourceType::FromIndex(0));
    CHECK(Obs::MissionNeeds(view, back.missionActive[0]) ==
          "carry 20 " + good + " (the hold has 0) to the Watch Post in Vela (system r1-2-3)");
    const std::string text = Obs::DescribeMissions(view);
    CHECK(text.find("delivered at the Watch Post or docked at any station in its system") !=
          std::string::npos);
    CHECK(text.find("hand in anywhere") == std::string::npos);

    // All aboard, elsewhere: told where it goes; not ready.
    back.player.cargoByType[0] = 20;
    CHECK(Obs::MissionNeeds(view, back.missionActive[0]) ==
          "take the " + good +
              " to the Watch Post in Vela (system r1-2-3): it is delivered beside the site, or "
              "docked at any station there");
    // ...and here, beside it.
    back.missionActive[0].siteId = 55;
    CHECK(Obs::MissionNeeds(view, back.missionActive[0]) ==
          "fly beside the Watch Post (#55), or dock at any station in this system: the " + good +
              " is delivered on arrival");
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

TEST_CASE("an escort job's view carries its convoy: where, where next, its ships here (#430)")
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
    const int job = ActiveOf(s, MissionType::Escort);
    REQUIRE(job >= 0);

    // Taken: the view says where the convoy is now, and that the pilot has not been with it.
    {
        const Plan* p = PlanOf(sim, u.plan);
        REQUIRE(p != nullptr);
        const Proto::Snapshot     snap = sim.BuildSnapshot(s, s.systemId);
        const Proto::MissionView& v = snap.missionActive[(size_t)job];
        CHECK(v.convoySystemId == p->ConvoyAt());
        CHECK_FALSE(v.convoySystemName.empty());
        CHECK(v.convoyMoving == (p->leg >= 0));
        CHECK_FALSE(v.rode);
        CHECK_FALSE(Obs::ConvoyStatus(v, "->").empty());
    }
    sim.StepPlayerUndock(s);

    // Into the system it is crossing: its hauler and escorts are named, and only there.
    bool with = false;
    for (int pass = 0; pass < 60 * 30 && !with; pass++)
    {
        const Plan* p = PlanOf(sim, u.plan);
        REQUIRE(p != nullptr);
        REQUIRE_FALSE(p->arrived);
        if (p->leg >= 0 && s.systemId != p->route[(size_t)p->leg])
            sim.ServerEnterSystem(s, p->route[(size_t)p->leg], s.systemId);
        sim.MaintainWorld(2.0f);
        p = PlanOf(sim, u.plan);
        with = p != nullptr && p->hauler != 0 && p->fieldedIn == s.systemId;
    }
    REQUIRE(with);
    const Plan&           p = *PlanOf(sim, u.plan);
    const Proto::Snapshot snap = sim.BuildSnapshot(s, s.systemId);
    REQUIRE(snap.missionActive.size() > (size_t)job);
    const Proto::MissionView& v = snap.missionActive[(size_t)job];
    CHECK(v.convoySystemId == s.systemId);
    CHECK(v.convoyMoving);
    CHECK(v.haulerId == p.hauler);
    CHECK((int)v.convoyEscortIds.size() == p.fielded);
    CHECK(v.rode == s.missions.Active()[(size_t)job].rode);
    if ((size_t)p.leg + 1 < p.route.size())
        CHECK_FALSE(v.convoyNextName.empty());
    else
        CHECK(v.convoyNextName.empty());
    CHECK(Obs::ConvoyStatus(v, "->") == "with convoy");
    CHECK(Obs::ConvoyPartOf(snap, p.hauler) == Obs::ConvoyPart::Hauler);
    for (int id : v.convoyEscortIds)
        CHECK(Obs::ConvoyPartOf(snap, id) == Obs::ConvoyPart::Escort);
    // The hauler is a ship the pilot can see.
    bool seen = false;
    for (const Proto::EntitySnapshot& e : snap.entities)
        seen = seen || e.id == p.hauler;
    CHECK(seen);

    // Across the wire as it went, and in what an agent reads.
    Proto::Snapshot back;
    REQUIRE(Proto::DecodeSnapshot(Proto::EncodeSnapshot(snap), back));
    const Proto::MissionView& got = back.missionActive[(size_t)job];
    CHECK(got.convoySystemId == v.convoySystemId);
    CHECK(got.convoySystemName == v.convoySystemName);
    CHECK(got.convoyNextName == v.convoyNextName);
    CHECK(got.convoyMoving);
    CHECK(got.haulerId == v.haulerId);
    CHECK(got.convoyEscortIds == v.convoyEscortIds);
    CHECK(got.rode == v.rode);
    Obs::View view;
    view.snapshot = &back;
    const std::string text = Obs::DescribeMissions(view);
    CHECK(text.find("convoy: with convoy") != std::string::npos);
    CHECK(text.find("hauler #" + std::to_string(p.hauler)) != std::string::npos);
    const std::string look = Obs::Describe(view, Obs::Detail::Brief);
    CHECK(look.find("YOUR CONVOY'S HAULER") != std::string::npos);
}

TEST_CASE(
    "an escort's convoy crosses the wire waiting, on its way elsewhere, and not at all (#430)")
{
    LoadRegistries();
    Proto::Snapshot    snap;
    Proto::MissionView m;
    m.type = (int)MissionType::Escort;
    m.faction = Fac::TradersGuild();
    m.title = "Escort: convoy to Kor";
    m.planId = 9;
    m.systemId = "r1-2-3";
    m.systemName = "Kor";
    m.convoySystemId = "r1-1-0";
    m.convoySystemName = "Vela";
    m.convoyNextName = "Ost";
    m.convoyMoving = true;
    m.rode = true;
    snap.missionActive.push_back(m);
    Proto::MissionView bounty;
    bounty.type = (int)MissionType::Bounty;
    snap.missionActive.push_back(bounty);

    Proto::Snapshot back;
    REQUIRE(Proto::DecodeSnapshot(Proto::EncodeSnapshot(snap), back));
    REQUIRE(back.missionActive.size() == 2);
    const Proto::MissionView& got = back.missionActive[0];
    CHECK(got.convoySystemId == "r1-1-0");
    CHECK(got.convoySystemName == "Vela");
    CHECK(got.convoyNextName == "Ost");
    CHECK(got.convoyMoving);
    CHECK(got.haulerId == 0);
    CHECK(got.convoyEscortIds.empty());
    CHECK(got.rode);
    CHECK(back.missionActive[1].convoySystemId.empty());
    CHECK_FALSE(back.missionActive[1].rode);

    Obs::View view;
    view.snapshot = &back;
    CHECK(Obs::ConvoyStatus(got, "->") == "en route in Vela -> Ost");
    CHECK(Obs::MissionNeeds(view, got) ==
          "catch up with the convoy in Vela (system r1-1-0), bound for Kor");
    CHECK(Obs::DescribeMissions(view).find("convoy: en route in Vela -> Ost (system r1-1-0); "
                                           "you have been with it") != std::string::npos);
    back.missionActive[0].convoyMoving = false;
    back.missionActive[0].rode = false;
    CHECK(Obs::ConvoyStatus(back.missionActive[0], "->") == "waiting at Vela");
    CHECK(Obs::MissionNeeds(view, back.missionActive[0]) ==
          "meet the convoy waiting to set out from Vela (system r1-1-0), bound for Kor");
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
