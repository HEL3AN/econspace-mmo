#include <doctest/doctest.h>

#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "entities/NpcShip.h"
#include "entities/Station.h"
#include "entities/Structure.h"
#include "gen/Galaxy.h"
#include "gen/Region.h"
#include "sim/SaveSchema.h"
#include "sim/Simulation.h"
#include "sim/WorldRules.h"

#include "factions_named.h"
#include "temp_dir.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

// Expeditions, stations grown by need, and the builder convoy (#342 S6).

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

void Make(Simulation& sim, uint64_t seed, const std::string& seal = std::string(),
          const std::vector<Simulation::RegionOpening>& log = {})
{
    sim.SetSealDir(seal);
    REQUIRE(sim.OpenGalaxy(Params(seed), DATA, log));
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
    WorldRules::Tide        tide = WorldRules::TideRules();
    WorldRules::Mouths      mouths = WorldRules::MouthRules();
    WorldRules::Expeditions expeditions = WorldRules::ExpeditionRules();
    WorldRules::Growth      growth = WorldRules::GrowthRules();
    WorldRules::Convoys     convoys = WorldRules::ConvoyRules();
    Rules()
    {
        WorldRules::SetTideRules(WorldRules::Tide{});
        WorldRules::SetMouthRules(WorldRules::Mouths{});
        WorldRules::SetExpeditionRules(WorldRules::Expeditions{});
        WorldRules::SetGrowthRules(WorldRules::Growth{});
        WorldRules::SetConvoyRules(WorldRules::Convoys{});
    }
    ~Rules()
    {
        WorldRules::SetTideRules(tide);
        WorldRules::SetMouthRules(mouths);
        WorldRules::SetExpeditionRules(expeditions);
        WorldRules::SetGrowthRules(growth);
        WorldRules::SetConvoyRules(convoys);
    }
};

// The first frontier the world began with.
int FirstFrontier(const Simulation& sim)
{
    for (const Simulation::RegionOpening& o : sim.OpeningLog())
        if (o.parent >= 0)
            return o.slot;
    return -1;
}

// A system of the first frontier that `f` holds now, with a finished outpost of its own in it
// and nothing to trade at: what a faction that settled there would have.
struct Holding
{
    std::string system;
    int         outpost = 0;
};
Holding PlantOutpost(Simulation& sim, FactionId f, int skip = 0)
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
        if (!spot.found || skip-- > 0)
            continue;
        const Blueprint& bp = *Blueprints::Find(Outposts::BLUEPRINT);
        auto             t = std::make_unique<Structure>(spot.at, 0.0f, "Outpost", bp.archetype);
        t->SetBlueprint(bp.id);
        if (spot.orbit)
            t->SetOrbit(*spot.orbit);
        const int oid = sim.AddStatic(id, std::move(t), Outposts::OwnerOf(f));
        REQUIRE(oid != 0);
        st->agg.claimed = true;
        st->agg.controller = f;
        st->agg.presence[f] = Factions::TemperamentOf(f).capacity;
        return { id, oid };
    }
    FAIL("no room for an outpost in the first frontier");
    return {};
}

const Plan* PlanOf(const Simulation& sim, int id)
{
    const auto it = sim.Plans().find(id);
    return it == sim.Plans().end() ? nullptr : &it->second;
}

// The structure or station standing where the outpost was.
const Entity* Grown(const Simulation& sim, const std::string& system, Vector2 at)
{
    for (const auto& e : sim.SystemById(system)->entities)
        if ((e->GetKind() == EntityKind::Structure || e->GetKind() == EntityKind::Station) &&
            std::hypot(e->GetPosition().x - at.x, e->GetPosition().y - at.y) < 1.0f)
            return e.get();
    return nullptr;
}

// Runs a minute at a time, at most `minutes`, until `done` says so.
template <typename F> bool RunUntil(Simulation& sim, int minutes, F done)
{
    for (int m = 0; m < minutes; m++)
    {
        if (done())
            return true;
        RunMinutes(sim, 1);
    }
    return done();
}

bool Said(const Simulation& sim, const std::string& kind, const std::string& words)
{
    for (const ChronicleEntry& e : sim.Chronicle())
        if (e.kind == kind && e.text.find(words) != std::string::npos)
            return true;
    return false;
}
}  // namespace

TEST_CASE("the expedition, growth and convoy rules read, and a misspelt one is an error (#342 S6)")
{
    Rules r;
    REQUIRE(WorldRules::Load(DATA + "world_rules.json"));
    CHECK(WorldRules::ExpeditionRules().hours > 0.0);
    CHECK(WorldRules::GrowthRules().stationsPerRegion > 0);
    CHECK(WorldRules::ConvoyRules().hop > 0.0);

    TempDir           dir;
    const std::string path = dir.Path("rules.json");
    {
        std::ofstream f(path);
        f << R"({ "tide": { "threshold": 50 }, "convoys": { "hops": 1 } })";
    }
    CHECK_FALSE(WorldRules::Load(path));
    CHECK(WorldRules::Error().find("hops") != std::string::npos);
    {
        std::ofstream f(path);
        f << R"({ "tide": { "threshold": 50 }, "convoys": { "hop": 0 } })";
    }
    CHECK_FALSE(WorldRules::Load(path));
}

TEST_CASE("the upgrade blueprints are built onto outposts, and a station only so (#342 S6)")
{
    LoadRegistries();
    const Blueprint* market = Blueprints::Find(Outposts::MARKET_BLUEPRINT);
    const Blueprint* trade = Blueprints::Find(Outposts::TRADE_STATION_BLUEPRINT);
    const Blueprint* mining = Blueprints::Find(Outposts::MINING_STATION_BLUEPRINT);
    REQUIRE(market != nullptr);
    REQUIRE(trade != nullptr);
    REQUIRE(mining != nullptr);
    for (const Blueprint* bp : { market, trade, mining })
    {
        CHECK(bp->byFactions);
        CHECK(bp->byPlayers);
        CHECK_FALSE(bp->onto.empty());
    }
    CHECK(Archetypes::Find(market->archetype)->Has(Component::Market));
    CHECK(Archetypes::Find(market->archetype)->Has(Component::Dockable));
    CHECK(Archetypes::Find(trade->archetype)->kind == EntityKind::Station);
    CHECK(Outposts::IsOutpost(market->archetype));

    // A station's archetype is refused for a blueprint that is not an upgrade.
    TempDir           dir;
    const std::string path = dir.Path("blueprints.json");
    {
        std::ofstream f(path);
        f << R"({ "blueprints": [ { "id": "hub", "archetype": "station.trade_hub",
                  "cost": { "Iron": 1 }, "hull": 10, "placement": { "reach": 600 } } ] })";
    }
    CHECK_FALSE(Blueprints::Load(path));
    CHECK(Blueprints::Error().find("upgrade") != std::string::npos);
    {
        std::ofstream f(path);
        f << R"({ "blueprints": [ { "id": "hub", "archetype": "station.trade_hub",
                  "onto": ["nothing"], "cost": { "Iron": 1 }, "hull": 10,
                  "placement": { "reach": 600 } } ] })";
    }
    CHECK_FALSE(Blueprints::Load(path));
    CHECK(Blueprints::Error().find("nothing") != std::string::npos);
    LoadRegistries();
}

TEST_CASE("a mouth opening sends word: intel on the far side and a pull towards it (#342 S6)")
{
    LoadRegistries();
    Rules r;

    // The same world twice, word of the mouth drawing its neighbours in one and not the other.
    auto reach = [](bool word)
    {
        WorldRules::Expeditions e;
        if (!word)
            e.hours = 0.0;
        WorldRules::SetExpeditionRules(e);
        Simulation sim;
        Make(sim, 7);
        RunMinutes(sim, 2);
        std::string why;
        const int   slot = sim.OpenMouthNow(sim.Skeleton().centres.front(), why);
        REQUIRE(slot >= 0);
        const std::string mouth = sim.OpeningOf(slot)->mouth;
        const std::string entry = Simulation::EntryOf(slot);
        const FactionId   holder = sim.SystemById(mouth)->agg.controller;
        REQUIRE(sim.SystemById(mouth)->agg.claimed);
        if (word)
        {
            // The holder of the mouth hears of it at once: what is beyond, and for how long it
            // is drawn there.
            const FactionMind& m = sim.MindOf(holder);
            REQUIRE(m.expeditions.count(slot) == 1);
            CHECK(m.expeditions.at(slot) == doctest::Approx(sim.Time() + e.hours * 3600.0));
            REQUIRE(m.intel.count(entry) == 1);
            CHECK(m.intel.at(entry).seenAt == doctest::Approx(sim.Time()));
            CHECK(Said(sim, "expedition", "Word of the mouth"));
            // A faction that reaches for nothing is told nothing.
            CHECK(sim.MindOf(Fac::Independent()).expeditions.empty());
        }
        else
            CHECK(sim.MindOf(holder).expeditions.empty());
        // ...and an hour on: how much of the space beyond it has looked at or gone into.
        RunMinutes(sim, 60);
        int known = 0;
        for (const std::string& id : sim.RegionSystems(slot))
        {
            const auto it = sim.MindOf(holder).intel.find(id);
            known += it != sim.MindOf(holder).intel.end() ? 1 : 0;
        }
        return known;
    };
    const int drawn = reach(true), alone = reach(false);
    CHECK(drawn > 1);
    CHECK(drawn > alone);
}

TEST_CASE("an outpost grows by need: a market where none is near, none where one is (#342 S6)")
{
    LoadRegistries();
    Rules r;
    // Any system without a market of its own is as far from one as matters.
    WorldRules::Growth g;
    g.marketReach = 1;
    WorldRules::SetGrowthRules(g);
    const FactionId f = Fac::TradersGuild();

    auto grows = [&](bool marketThere)
    {
        Simulation sim;
        Make(sim, 7);
        const Holding h = PlantOutpost(sim, f);
        if (marketThere)
        {
            // Somebody trades there already.
            SystemState* st = sim.SystemById(h.system);
            Vector2      at{ 0.0f, 0.0f };
            for (const auto& e : st->entities)
                if (e->GetId() == h.outpost)
                    at = { e->GetPosition().x + 8000.0f, e->GetPosition().y };
            REQUIRE(sim.AddStatic(h.system,
                                  std::make_unique<Station>(at, 400.0f, "Old Post",
                                                            Factions::Unaffiliated(),
                                                            StationRole::TradeHub),
                                  std::string()) != 0);
            REQUIRE(st->profile.markets == 1);
        }
        for (int minute = 0; minute < 20; minute++)
        {
            sim.MindOf(f).stock = 4.0f * Simulation::OutpostCost();
            RunMinutes(sim, 1);
            for (const auto& kv : sim.Plans())
                if (kv.second.kind == Plan::Kind::Upgrade && kv.second.target == h.system)
                {
                    CHECK(kv.second.blueprint == Outposts::MARKET_BLUEPRINT);
                    return true;
                }
        }
        return false;
    };
    CHECK(grows(false));
    CHECK_FALSE(grows(true));
}

TEST_CASE("an outpost becomes a market, then a station, each by convoy and build (#342 S6)")
{
    LoadRegistries();
    Rules           r;
    const FactionId f = Fac::TradersGuild();
    Simulation      sim;
    Make(sim, 7);
    const Holding h = PlantOutpost(sim, f);
    SystemState*  st = sim.SystemById(h.system);
    Vector2       at{ 0.0f, 0.0f };
    for (const auto& e : st->entities)
        if (e->GetId() == h.outpost)
            at = e->GetPosition();

    // A station before the market is refused; so is anything without the stock.
    sim.MindOf(f).stock = 0.0f;
    CHECK(sim.BeginUpgrade(f, h.system, Outposts::MARKET_BLUEPRINT) == 0);
    sim.MindOf(f).stock = 4.0f * Simulation::OutpostCost();
    CHECK(sim.BeginUpgrade(f, h.system, Outposts::TRADE_STATION_BLUEPRINT) == 0);
    const float before = sim.MindOf(f).stock;
    const int   id = sim.BeginUpgrade(f, h.system, Outposts::MARKET_BLUEPRINT);
    REQUIRE(id != 0);
    CHECK(sim.MindOf(f).stock ==
          doctest::Approx(before - Simulation::BlueprintCost(Outposts::MARKET_BLUEPRINT)));
    // One at a time at a site.
    CHECK(sim.BeginUpgrade(f, h.system, Outposts::MARKET_BLUEPRINT) == 0);
    const Plan* p = PlanOf(sim, id);
    REQUIRE(p != nullptr);
    CHECK(p->OnTheWay());
    REQUIRE_FALSE(p->route.empty());
    CHECK(p->route.back() == h.system);
    CHECK(Said(sim, "convoy", "sent a convoy"));

    // The convoy comes, the outpost is the market's site -- still the claim -- and then a market.
    REQUIRE(
        RunUntil(sim, 30, [&] { return PlanOf(sim, id) == nullptr || PlanOf(sim, id)->arrived; }));
    const Structure* site = static_cast<const Structure*>(Grown(sim, h.system, at));
    REQUIRE(site != nullptr);
    CHECK(site->IsBuilding());
    CHECK(site->GetBuilds() == Blueprints::Find(Outposts::MARKET_BLUEPRINT)->archetype);
    CHECK(st->profile.outposts.size() == 1);
    CHECK(st->profile.markets == 0);
    REQUIRE(RunUntil(sim, 40, [&] { return PlanOf(sim, id) == nullptr; }));
    CHECK(st->profile.markets == 1);
    CHECK(Said(sim, "upgrade", "opened a market"));
    const Entity* market = Grown(sim, h.system, at);
    REQUIRE(market != nullptr);
    CHECK(market->Has(Component::Dockable));
    CHECK(market->Has(Component::Market));
    CHECK(st->profile.outposts.size() == 1);  // still an outpost: the claim, upkeep, a siege

    // ...and the market a station, while the region has room for one.
    WorldRules::Growth full;
    full.stationsPerRegion = sim.StationsIn(Gen::SlotOf(h.system));
    WorldRules::SetGrowthRules(full);
    sim.MindOf(f).stock = 4.0f * Simulation::OutpostCost();
    CHECK(sim.BeginUpgrade(f, h.system, Outposts::TRADE_STATION_BLUEPRINT) == 0);
    WorldRules::Growth room;
    room.stationsPerRegion = full.stationsPerRegion + 1;
    WorldRules::SetGrowthRules(room);
    const int second = sim.BeginUpgrade(f, h.system, Outposts::TRADE_STATION_BLUEPRINT);
    REQUIRE(second != 0);
    REQUIRE(RunUntil(sim, 120, [&] { return st->profile.stations == 1; }));
    const Entity* station = Grown(sim, h.system, at);
    REQUIRE(station != nullptr);
    REQUIRE(station->GetKind() == EntityKind::Station);
    CHECK(static_cast<const Station*>(station)->GetFaction() == f);
    CHECK(static_cast<const Station*>(station)->GetRole() == StationRole::TradeHub);
    CHECK(st->profile.outposts.empty());
    CHECK(st->agg.claimed);
    CHECK(st->agg.controller == f);
    CHECK(Said(sim, "station", "opened a trade station"));
    CHECK(PlanOf(sim, second) == nullptr);
}

TEST_CASE("a convoy goes the same cold as hot, and shows its ships where it is hot (#342 S6)")
{
    LoadRegistries();
    Rules           r;
    const FactionId f = Fac::TradersGuild();
    Simulation      cold, hot;
    hot.SetAllHot(true);
    int  ids[2] = { 0, 0 };
    bool seen = false;
    for (int i = 0; i < 2; i++)
    {
        Simulation& sim = i == 0 ? cold : hot;
        Make(sim, 7);
        const Holding h = PlantOutpost(sim, f);
        sim.MindOf(f).stock = 4.0f * Simulation::OutpostCost();
        ids[i] = sim.BeginUpgrade(f, h.system, Outposts::MARKET_BLUEPRINT);
        REQUIRE(ids[i] != 0);
    }
    double done[2] = { -1.0, -1.0 };
    int    setbacks[2] = { 0, 0 };
    for (int minute = 0; minute < 60 && (done[0] < 0.0 || done[1] < 0.0); minute++)
    {
        RunMinutes(cold, 1);
        RunMinutes(hot, 1);
        for (int i = 0; i < 2; i++)
        {
            Simulation& sim = i == 0 ? cold : hot;
            if (const Plan* q = PlanOf(sim, ids[i]))
                setbacks[i] = q->setbacks;
            if (done[i] < 0.0 && Said(sim, "upgrade", "opened a market"))
                done[i] = sim.Time();
        }
        const Plan* p = PlanOf(hot, ids[1]);
        if (p == nullptr || !p->OnTheWay() || p->leg < 0)
            continue;
        // Where it is, its hauler and escort, tasked to it and nobody else's.
        for (const auto& e : hot.SystemById(p->route[(size_t)p->leg])->entities)
            if (e->GetKind() == EntityKind::Npc &&
                static_cast<const NpcShip&>(*e).GetTask() == ids[1])
                seen = true;
    }
    CHECK(seen);
    CHECK(done[0] > 0.0);
    CHECK(done[0] == doctest::Approx(done[1]));
    CHECK(setbacks[0] == setbacks[1]);
}

TEST_CASE("a lost convoy delays the plan and does not cancel it (#342 S6)")
{
    LoadRegistries();
    Rules           r;
    const FactionId f = Fac::TradersGuild();

    // How long the market takes with nothing in the way, and with the pirates waiting at it.
    auto build = [&](bool ambush, bool shoot)
    {
        Simulation sim;
        sim.SetAllHot(shoot);
        Make(sim, 7);
        const Holding h = PlantOutpost(sim, f);
        sim.MindOf(f).stock = 4.0f * Simulation::OutpostCost();
        const int id = sim.BeginUpgrade(f, h.system, Outposts::MARKET_BLUEPRINT);
        REQUIRE(id != 0);
        SystemState* st = sim.SystemById(h.system);
        if (ambush)
            st->agg.presence[Factions::Pirates()] = 40.0f;
        if (shoot)
        {
            // A pilot shoots the hauler down wherever it is.
            const Plan* p = PlanOf(sim, id);
            REQUIRE(p != nullptr);
            bool shot = false;
            for (auto& e : sim.SystemById(p->route[(size_t)p->leg])->entities)
                if (e->GetKind() == EntityKind::Npc && static_cast<NpcShip&>(*e).GetTask() == id &&
                    static_cast<NpcShip&>(*e).GetRole() == NpcRole::Trader)
                {
                    static_cast<NpcShip&>(*e).TakeDamage(1.0e9f);
                    shot = true;
                }
            REQUIRE(shot);
        }
        bool lost = false;
        REQUIRE(RunUntil(sim, 40,
                         [&]
                         {
                             const Plan* p = PlanOf(sim, id);
                             lost = lost || (p != nullptr && p->setbacks > 0);
                             return lost || (p != nullptr && p->arrived);
                         }));
        if (lost)
        {
            const Plan* p = PlanOf(sim, id);
            REQUIRE(p != nullptr);  // delayed, not cancelled
            CHECK(p->leg == -1);
            CHECK_FALSE(p->arrived);
            CHECK(Said(sim, "convoy", shoot ? "shot down" : "lost its convoy"));
            st->agg.presence[Factions::Pirates()] = 0.0f;  // the way is clear again
        }
        double done = -1.0;
        RunUntil(sim, 90,
                 [&]
                 {
                     if (st->profile.markets > 0 && done < 0.0)
                         done = sim.Time();
                     return done >= 0.0;
                 });
        return std::make_pair(lost, done);
    };
    const auto clear = build(false, false);
    const auto ambushed = build(true, false);
    const auto shot = build(false, true);
    CHECK_FALSE(clear.first);
    CHECK(ambushed.first);
    CHECK(shot.first);
    REQUIRE(clear.second > 0.0);
    REQUIRE(ambushed.second > 0.0);
    REQUIRE(shot.second > 0.0);
    CHECK(ambushed.second > clear.second);
    CHECK(shot.second > clear.second);
}

TEST_CASE("a convoy sets out from where its faction makes things: the heart behind the frontier "
          "(#407)")
{
    LoadRegistries();
    Rules      r;
    Simulation sim;
    Make(sim, 7);
    // Whose centre the first frontier opened from.
    const Simulation::RegionOpening* o = sim.OpeningOf(FirstFrontier(sim));
    REQUIRE(o != nullptr);
    const SystemState* mouth = sim.SystemById(o->mouth);
    REQUIRE(mouth != nullptr);
    REQUIRE(mouth->agg.claimed);
    const FactionId f = mouth->agg.controller;

    const Holding h = PlantOutpost(sim, f);
    sim.MindOf(f).stock = 4.0f * Simulation::OutpostCost();
    const int id = sim.BeginUpgrade(f, h.system, Outposts::MARKET_BLUEPRINT);
    REQUIRE(id != 0);
    const Plan* p = PlanOf(sim, id);
    REQUIRE(p != nullptr);
    REQUIRE(p->route.size() >= 2);
    CHECK(p->route.back() == h.system);
    for (size_t i = 0; i + 1 < p->route.size(); i++)
    {
        const std::vector<std::string>& links = sim.Neighbors(p->route[i]);
        CHECK(std::find(links.begin(), links.end(), p->route[i + 1]) != links.end());
    }

    // Of the faction's holdings within the convoy's reach of the site, it set out from one with
    // the most stations of its own -- in the centre, not the outpost's frontier.
    auto stations = [&](const std::string& id)
    {
        int n = 0;
        for (const auto& e : sim.SystemById(id)->entities)
            n += e->GetKind() == EntityKind::Station &&
                         static_cast<const Station&>(*e).GetFaction() == f
                     ? 1
                     : 0;
        return n;
    };
    std::map<std::string, int> near{ { h.system, 0 } };
    std::vector<std::string>   ring{ h.system };
    for (int d = 1; d <= WorldRules::ConvoyRules().reach; d++)
    {
        std::vector<std::string> next;
        for (const std::string& id : ring)
            for (const std::string& n : sim.Neighbors(id))
                if (sim.HasSystem(n) && near.emplace(n, d).second)
                    next.push_back(n);
        ring = next;
    }
    int most = 0;
    for (const auto& kv : near)
    {
        const SystemState* st = sim.SystemById(kv.first);
        if (kv.second > 0 && st->agg.claimed && st->agg.controller == f)
            most = std::max(most, stations(kv.first));
    }
    const std::string& home = p->route.front();
    CHECK(most > 0);
    CHECK(stations(home) == most);
    CHECK(sim.SystemById(home)->agg.controller == f);
    CHECK(Gen::SlotOf(home) == o->parent);
}

TEST_CASE("a civilisation builds in more than one place at once (#407)")
{
    LoadRegistries();
    Rules      r;
    Simulation sim;
    Make(sim, 7);
    const Simulation::RegionOpening* o = sim.OpeningOf(FirstFrontier(sim));
    REQUIRE(o != nullptr);
    const FactionId f = sim.SystemById(o->mouth)->agg.controller;
    int             holdings = 0;
    for (const auto& kv : sim.Systems())
        holdings += kv.second.agg.claimed && kv.second.agg.controller == f ? 1 : 0;
    REQUIRE(holdings >= 8);  // a centre's worth

    // Two outposts in the frontier, and markets worth more than anything else it could do.
    WorldRules::Growth worth;
    worth.marketWorth = 50.0;
    WorldRules::SetGrowthRules(worth);
    PlantOutpost(sim, f, 0);
    PlantOutpost(sim, f, 1);
    int most = 0;
    for (int minute = 0; minute < 20 && most < 2; minute++)
    {
        sim.MindOf(f).stock = std::max(sim.MindOf(f).stock, 4.0f * Simulation::OutpostCost());
        RunMinutes(sim, 1);
        int growing = 0;
        for (const auto& kv : sim.Plans())
            growing += kv.second.faction == f && kv.second.kind == Plan::Kind::Upgrade ? 1 : 0;
        most = std::max(most, growing);
    }
    CHECK(most >= 2);
}

TEST_CASE("the law presses on the lawless system at its door rather than sit on its stock (#407)")
{
    LoadRegistries();
    Rules      r;
    Simulation sim;
    Make(sim, 7);
    // The system beyond a centre's mouth, made a lawless one: nobody's, no law, pirates about
    // -- and what the centre's faction saw of it, long ago. For what the place itself is worth
    // to it, that was not worth a second look, let alone its ships.
    const Simulation::RegionOpening* o = sim.OpeningOf(FirstFrontier(sim));
    REQUIRE(o != nullptr);
    const FactionId f = sim.SystemById(o->mouth)->agg.controller;
    REQUIRE(Factions::IsLawful(f));
    const std::string door = sim.EntryOf(o->slot);
    SystemState*      st = sim.SystemById(door);
    REQUIRE(st != nullptr);
    REQUIRE_FALSE(st->agg.claimed);
    st->agg.security = st->agg.baseSecurity = 0.0f;
    st->agg.presence[Factions::Pirates()] = 4.0f;
    Intel old = sim.Observe(*st);
    old.seenAt = -4.0 * 3600.0;
    sim.MindOf(f).intel[door] = old;

    // Worth looking at again, and then worth reaching into.
    bool looked = false, reached = false;
    for (int minute = 0; minute < 45 && !reached; minute++)
    {
        RunMinutes(sim, 1);
        for (const auto& kv : sim.Plans())
            looked = looked || (kv.second.faction == f && kv.second.target == door);
        looked = looked || sim.MindOf(f).intel[door].seenAt > 0.0;
        reached = st->agg.presence[f] >= 1.0f || (st->agg.claimed && st->agg.controller == f);
    }
    CHECK(looked);
    CHECK(reached);
}

TEST_CASE("convoys, upgrades and word of a mouth round-trip through the save (#342 S6)")
{
    LoadRegistries();
    Rules             r;
    TempDir           dir;
    const std::string seal = dir.MakeDir("regions");
    const std::string path = dir.Path("world.json");
    const FactionId   f = Fac::TradersGuild();
    Simulation        a;
    Make(a, 9, seal);
    RunMinutes(a, 2);
    std::string why;
    const int   slot = a.OpenMouthNow(a.Skeleton().centres.front(), why);
    REQUIRE(slot >= 0);
    const Holding h = PlantOutpost(a, f);
    a.MindOf(f).stock = 4.0f * Simulation::OutpostCost();
    const int id = a.BeginUpgrade(f, h.system, Outposts::MARKET_BLUEPRINT);
    REQUIRE(id != 0);
    // ...and a site growing into a station, which only an upgrade may stand as.
    const Holding    other = PlantOutpost(a, f, 1);
    const Blueprint& hub = *Blueprints::Find(Outposts::TRADE_STATION_BLUEPRINT);
    Structure*       grows = nullptr;
    for (const auto& e : a.SystemById(other.system)->entities)
        if (e->GetId() == other.outpost)
            grows = static_cast<Structure*>(e.get());
    REQUIRE(grows != nullptr);
    grows->Retask(hub.archetype, hub.id, "Growing Hub");
    grows->StartBuilding(a.Time(), a.Time() + hub.buildSeconds);
    a.MarkStaticChanged(*a.SystemById(other.system), other.outpost);
    RunMinutes(a, 1);
    REQUIRE(PlanOf(a, id) != nullptr);
    REQUIRE(PlanOf(a, id)->OnTheWay());
    REQUIRE(a.SaveWorld(path));

    Simulation::WorldHeader head;
    REQUIRE(Simulation::ReadWorldHeader(path, head));
    Simulation b;
    b.SetSealDir(seal);
    REQUIRE(b.OpenGalaxy(Params(9), DATA, head.log));
    REQUIRE(b.LoadWorld(path) == Save::Result::Ok);
    b.MaterializeAllSystems(DATA + "systems/");

    const Plan* pa = PlanOf(a, id);
    const Plan* pb = PlanOf(b, id);
    REQUIRE(pb != nullptr);
    CHECK(pb->kind == Plan::Kind::Upgrade);
    CHECK(pb->blueprint == pa->blueprint);
    CHECK(pb->route == pa->route);
    CHECK(pb->leg == pa->leg);
    CHECK(pb->legAt == doctest::Approx(pa->legAt));
    CHECK(pb->force == doctest::Approx(pa->force));
    CHECK(pb->site == pa->site);
    CHECK(b.MindOf(f).expeditions == a.MindOf(f).expeditions);
    for (FactionId g : Factions::All())
        CHECK(b.MindOf(g).expeditions == a.MindOf(g).expeditions);
    bool kept = false;
    for (const auto& e : b.SystemById(other.system)->entities)
        if (e->GetKind() == EntityKind::Structure && e->GetName() == "Growing Hub")
            kept = static_cast<const Structure&>(*e).IsBuilding();
    CHECK(kept);

    // ...and both worlds go on the same: the market goes up at the same instant.
    double done[2] = { -1.0, -1.0 };
    for (int minute = 0; minute < 60 && (done[0] < 0.0 || done[1] < 0.0); minute++)
    {
        RunMinutes(a, 1);
        RunMinutes(b, 1);
        if (done[0] < 0.0 && a.SystemById(h.system)->profile.markets > 0)
            done[0] = a.Time();
        if (done[1] < 0.0 && b.SystemById(h.system)->profile.markets > 0)
            done[1] = b.Time();
    }
    CHECK(done[0] > 0.0);
    CHECK(done[0] == doctest::Approx(done[1]));
}
