#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "goods_named.h"
#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "core/ShipDesign.h"
#include "core/World.h"
#include "economy/Resource.h"
#include "entities/NpcShip.h"
#include "entities/Ship.h"
#include "entities/ShipType.h"
#include "sim/ClientSession.h"
#include "sim/FactionMind.h"
#include "sim/Simulation.h"
#include "factions_named.h"
#include "temp_dir.h"

// Cold systems (#295, slice 4): NPC ships exist only where a player is, and for a minute
// after; everywhere else the population, its losses, the market and the bodies are kept as
// numbers once a coarse pass, and the economy and the factions go on reading the aggregates.

namespace
{
const std::string DATA = TEST_DATA_DIR;

std::unique_ptr<Simulation> ColdWorld(uint64_t seed = 7, bool allHot = false)
{
    SetRandomSeed(0xC0FFEEu);
    REQUIRE(Factions::Load(DATA + "factions.json"));
    REQUIRE(Archetypes::Load(DATA + "archetypes.json"));
    REQUIRE(Blueprints::Load(DATA + "blueprints.json"));
    auto sim = std::make_unique<Simulation>();
    sim->LoadUniverse(std::string(TEST_WORLD_DIR) + "universe.json");
    sim->AttachRegion(seed, std::string(TEST_WORLD_DIR) + "systems/");
    sim->Seed(1234u);
    sim->SetAllHot(allHot);
    sim->InitGalaxy();
    sim->MaterializeAllSystems(std::string(TEST_WORLD_DIR) + "systems/");
    return sim;
}

// The host's tick with nobody's input: every hot system's ships, then maintenance.
void Tick(Simulation& sim, int ticks = 1)
{
    const float dt = 1.0f / 60.0f;
    for (int i = 0; i < ticks; i++)
    {
        for (auto& kv : sim.Systems())
            sim.StepSystemAgents(kv.second, {}, nullptr, dt);
        sim.MaintainWorld(dt);
    }
}

std::vector<const NpcShip*> ShipsIn(const Simulation& sim, const std::string& id)
{
    std::vector<const NpcShip*> out;
    for (const auto& e : sim.SystemById(id)->entities)
        if (e->GetKind() == EntityKind::Npc)
            out.push_back(static_cast<const NpcShip*>(e.get()));
    return out;
}

int HotCount(const Simulation& sim)
{
    int n = 0;
    for (const auto& kv : sim.Systems())
        n += Simulation::IsHot(kv.second) ? 1 : 0;
    return n;
}

int Expected(const SystemAggregate& a)
{
    return (int)std::lround(a.traders) + (int)std::lround(a.miners) + (int)std::lround(a.police) +
           (int)std::lround(a.pirates);
}

// A player in the start system, and the system a gate from it leads to.
ClientSession& Arrive(Simulation& sim)
{
    const std::string& start = sim.Universe().startId;
    return sim.CreateSession(start, sim.SafeArrival(start), GetShipCatalog()[0].stats);
}
}  // namespace

TEST_CASE("with nobody connected every system is cold and has no ships (#295)")
{
    auto sim = ColdWorld();
    CHECK(HotCount(*sim) == 0);
    long ships = 0, population = 0;
    for (const auto& kv : sim->Systems())
    {
        ships += (long)ShipsIn(*sim, kv.first).size();
        population += Expected(kv.second.agg);
    }
    CHECK(ships == 0);
    CHECK(population > 0);  // the numbers are there; the ships are not

    // A minute of the world: still nothing to step, and still nobody made.
    Tick(*sim, 3600);
    CHECK(HotCount(*sim) == 0);
    for (const auto& kv : sim->Systems())
        CHECK(ShipsIn(*sim, kv.first).empty());

    // As before cold systems, on request: every system hot, ships everywhere.
    auto hot = ColdWorld(7, true);
    CHECK(HotCount(*hot) == (int)hot->Systems().size());
    Tick(*hot, 600);
    CHECK(HotCount(*hot) == (int)hot->Systems().size());
}

TEST_CASE("entering a cold system finds it populated from its aggregate (#295)")
{
    auto              sim = ColdWorld();
    ClientSession&    s = Arrive(*sim);
    const std::string start = s.systemId;
    CHECK(Simulation::IsHot(*sim->SystemById(start)));
    CHECK(HotCount(*sim) == 1);

    // Let the world run a while with the player at home, then go next door.
    Tick(*sim, 600);
    std::string dest;
    for (const std::string& n : sim->Neighbors(start))
        if (Expected(sim->SystemById(n)->agg) > 0)
            dest = n;
    REQUIRE_FALSE(dest.empty());
    REQUIRE_FALSE(Simulation::IsHot(*sim->SystemById(dest)));
    const int expected = Expected(sim->SystemById(dest)->agg);
    sim->ServerEnterSystem(s, dest, s.systemId);

    // Hot the moment the player is there, with the aggregate's ships in it -- before any
    // snapshot is built.
    SystemState& st = *sim->SystemById(dest);
    CHECK(Simulation::IsHot(st));
    const std::vector<const NpcShip*> ships = ShipsIn(*sim, dest);
    CHECK((int)ships.size() == expected);
    CHECK(Expected(st.agg) == expected);  // counted as the whole ships it now holds
    const Vector2 me = s.ship->GetPosition();
    for (const NpcShip* n : ships)
    {
        const Vector2 p = n->GetPosition();
        CHECK(std::sqrt(p.x * p.x + p.y * p.y) < ::World::SYSTEM_RADIUS * 1.1f);
        if (n->GetRole() == NpcRole::Pirate)
        {
            // Nobody is ambushed by the act of arriving.
            const float dx = p.x - me.x, dy = p.y - me.y;
            CHECK(dx * dx + dy * dy >= 2600.0f * 2600.0f);
        }
    }
    // The snapshot the player is sent shows them.
    int seen = 0;
    for (const Proto::EntitySnapshot& e : sim->BuildSnapshot(s, dest).entities)
        seen += e.kind == Proto::EntityKind::Npc ? 1 : 0;
    CHECK(seen == expected);
}

TEST_CASE("a system's ships are drawn from the system and the time, not the order (#295)")
{
    // The same system warmed at the same second gives the same ships, whichever other
    // systems were warmed first.
    auto sample = [](bool detour)
    {
        auto              sim = ColdWorld();
        const std::string start = sim->Universe().startId;
        const std::string dest = sim->Neighbors(start).front();
        if (detour)
            for (const auto& kv : sim->Systems())
                if (kv.first != dest && kv.first != start)
                {
                    sim->Warm(*sim->SystemById(kv.first));
                    break;
                }
        sim->Warm(*sim->SystemById(dest));
        std::vector<std::tuple<int, int, float, float, std::string>> out;
        for (const NpcShip* n : ShipsIn(*sim, dest))
            out.emplace_back((int)n->GetRole(), n->GetFaction().Index(), n->GetPosition().x,
                             n->GetPosition().y, n->GetDesign());  // and what it flies (#279)
        return out;
    };
    const auto a = sample(false);
    CHECK_FALSE(a.empty());
    CHECK(a == sample(false));
    CHECK(a == sample(true));
}

TEST_CASE("a system cools a minute after the last player leaves, and keeps its numbers (#295)")
{
    auto              sim = ColdWorld();
    ClientSession&    s = Arrive(*sim);
    const std::string home = s.systemId;
    Tick(*sim, 600);
    const int before = (int)ShipsIn(*sim, home).size();
    REQUIRE(before > 0);

    sim->ServerEnterSystem(s, sim->Neighbors(home).front(), s.systemId);
    // Back within the minute: the same ships, the same ids.
    Tick(*sim, (int)(Simulation::COOL_AFTER * 60.0f) - 300);
    CHECK(Simulation::IsHot(*sim->SystemById(home)));
    CHECK((int)ShipsIn(*sim, home).size() > 0);

    // Past the minute and the next coarse pass: let go, counted into the aggregate.
    Tick(*sim, 300 + 5 * 60);
    const SystemState& st = *sim->SystemById(home);
    CHECK_FALSE(Simulation::IsHot(st));
    CHECK(ShipsIn(*sim, home).empty());
    CHECK(Expected(st.agg) > 0);
    // Where the player is now stays hot as long as they do.
    CHECK(Simulation::IsHot(*sim->SystemById(s.systemId)));
    CHECK(HotCount(*sim) == 1);
}

TEST_CASE("cold systems still progress: stock, presence, prices and outposts (#295)")
{
    // The goods with their industries and haulers (#343), for this case only.
    struct GoodsFile
    {
        GoodsFile() { REQUIRE(Goods::Load(DATA + "goods.json")); }
        ~GoodsFile() { Goods::Load(DATA + "goods.json"); }
    } goods;
    auto sim = ColdWorld();
    REQUIRE(HotCount(*sim) == 0);

    // A market somebody sold into, left to recover in a system nobody is in.
    SystemState& home = *sim->SystemById(sim->Universe().startId);
    const double base = home.market.GetPrice(Good::Iron());
    home.market.Sell(Good::Iron(), 200);
    const double sold = home.market.GetPrice(Good::Iron());
    REQUIRE(sold < base * 0.9);

    // An hour of world time in coarse passes, every system cold throughout.
    for (int i = 0; i < 1800; i++)
        sim->MaintainWorld(2.0f);
    CHECK(HotCount(*sim) == 0);

    // Prices are coming back: what was sold is used up, or carried off by haulers (#343).
    CHECK(home.market.GetPrice(Good::Iron()) > sold + (base - sold) * 0.05);
    // Holdings yielded, and somebody spent it on an outpost that now stands -- the claim.
    bool founded = false, surveyed = false;
    for (const ChronicleEntry& e : sim->Chronicle())
    {
        // Whatever the outpost is for (#318): a watch post, a hidden base...
        founded = founded || (e.kind == "settle" && e.text.find(" founded ") != std::string::npos);
        surveyed = surveyed || e.text.find("surveyed") != std::string::npos;
    }
    CHECK(surveyed);
    CHECK(founded);
    bool claimedBeyond = false;
    for (const auto& kv : sim->Systems())
        claimedBeyond = claimedBeyond || (kv.second.agg.claimed && !kv.second.agg.publicChart);
    CHECK(claimedBeyond);
    // Populations are kept: every lawful holding has police, and pirates hold somewhere.
    for (const auto& kv : sim->Systems())
    {
        const SystemAggregate& a = kv.second.agg;
        if (a.claimed && Factions::IsLawful(a.controller))
        {
            CHECK(a.police >= 1.0f);
            CHECK(a.traders >= 1.0f);
        }
        CHECK(a.prosperity >= 0.0f);
        CHECK(a.prosperity <= 1.0f);
    }
}

TEST_CASE("a cold system's fights are losses, counted as a hot system counts them (#295)")
{
    auto         sim = ColdWorld();
    SystemState* st = nullptr;
    for (auto& kv : sim->Systems())
        if (kv.second.agg.claimed && Factions::IsLawful(kv.second.agg.controller) &&
            kv.second.profile.belts > 0)
            st = &kv.second;
    REQUIRE(st != nullptr);
    SystemAggregate& a = st->agg;
    const FactionId  law = a.controller;
    a.presence[Fac::Pirates()] = 8.0f;
    a.presence[law] = 8.0f;
    a.pirates = 8.0f;
    a.police = 8.0f;
    a.traders = 6.0f;
    const float pirates = a.presence[Fac::Pirates()];

    // One pass: a little of each side is gone, the director feels it as pressure, and the
    // faction step takes it out of the presence the pirates had committed.
    sim->MaintainWorld(2.0f);
    CHECK(a.supPirates > 0.0f);
    CHECK(a.supTraders > 0.0f);
    CHECK(a.presence[Fac::Pirates()] < pirates);
    CHECK_FALSE(Simulation::IsHot(*st));
}

TEST_CASE("a warmed system's ships fly their doctrine's designs, the same on every warm (#279)")
{
    // A system that warms makes its ships afresh from the aggregate (#295), so a design picked
    // by a ship's id would follow whatever was made before it. The n-th ship of a role flies
    // the n-th pick of its faction's doctrine instead: the same ships look the same every time
    // the system warms, and a ship keeps its design while it lives.
    const Ships::Catalogue& c = Archetypes::ShipCatalogue();
    auto                    designs = [&](const Simulation& sim, const std::string& id)
    {
        std::vector<std::tuple<int, int, std::string>> out;
        int                                            nth[5] = { 0, 0, 0, 0, 0 };
        for (const NpcShip* n : ShipsIn(sim, id))
        {
            const int role = (int)n->GetRole();
            CHECK(n->GetDesign() == c.Pick(Factions::Id(n->GetFaction()), NpcRoleId(n->GetRole()),
                                           (unsigned)nth[role]++));
            REQUIRE(n->GetArchetype() != nullptr);
            CHECK(n->GetArchetype()->design == n->GetDesign());
            out.emplace_back(role, n->GetFaction().Index(), n->GetDesign());
        }
        return out;
    };

    auto              sim = ColdWorld();
    ClientSession&    s = Arrive(*sim);
    const std::string home = s.systemId;
    const auto        first = designs(*sim, home);
    REQUIRE_FALSE(first.empty());

    // A ship keeps its design while it lives.
    std::vector<std::pair<int, std::string>> alive;
    for (const NpcShip* n : ShipsIn(*sim, home))
        alive.emplace_back(n->GetId(), n->GetDesign());
    Tick(*sim, 300);
    for (const NpcShip* n : ShipsIn(*sim, home))
        for (const auto& [id, design] : alive)
            if (id == n->GetId())
                CHECK(n->GetDesign() == design);

    // Away until it cools, back again: new ships, made afresh from the aggregate, and the k-th
    // ship of a role flies what the k-th did before whenever it is of the same faction.
    const std::string away = sim->Neighbors(home).front();
    sim->ServerEnterSystem(s, away, s.systemId);
    Tick(*sim, (int)(Simulation::COOL_AFTER * 60.0f) + 5 * 60);
    REQUIRE_FALSE(Simulation::IsHot(*sim->SystemById(home)));
    sim->ServerEnterSystem(s, home, s.systemId);
    const auto again = designs(*sim, home);
    REQUIRE_FALSE(again.empty());
    int compared = 0;
    for (int role = 0; role < 5; role++)
    {
        std::vector<std::tuple<int, int, std::string>> a, b;
        for (const auto& t : first)
            if (std::get<0>(t) == role)
                a.push_back(t);
        for (const auto& t : again)
            if (std::get<0>(t) == role)
                b.push_back(t);
        for (size_t k = 0; k < a.size() && k < b.size(); k++)
            if (std::get<1>(a[k]) == std::get<1>(b[k]))
            {
                CHECK(std::get<2>(a[k]) == std::get<2>(b[k]));
                compared++;
            }
    }
    CHECK(compared > 0);
}

// One combat model for NPCs and players (owner, 2026-10-22, #186): an NPC fires its design's
// volley at a player's cooldown, a cold system's fights are reckoned from the same numbers,
// and what makes a raider weaker or stronger than a pilot is the fit in data/ships.json.
namespace
{
// Hull `target` loses over `ticks` calls of `shoot`.
template <typename Shoot> float HullLost(const Combatant& target, int ticks, Shoot shoot)
{
    const float before = target.GetHull();
    for (int i = 0; i < ticks; i++)
        shoot();
    return before - target.GetHull();
}

NpcShip& PutNpc(SystemState& st, Vector2 at, FactionId f, NpcRole role, const std::string& design,
                int id)
{
    st.entities.push_back(
        std::make_unique<NpcShip>(at, f, role, std::vector<Vector2>{ at }, design));
    st.entities.back()->SetId(id);
    return static_cast<NpcShip&>(*st.entities.back());
}
}  // namespace

TEST_CASE("an NPC of a design hits as hard as a player's ship of that design (#186)")
{
    auto              sim = ColdWorld();
    const std::string sys = sim->Universe().startId;
    SystemState&      st = *sim->SystemById(sys);
    const float       dt = 1.0f / 60.0f;
    const int         ticks = 100;  // 1.67 s: four volleys at a half-second cooldown
    const Vector2     at = { 300000.0f, -300000.0f };

    for (const char* design : { "raider", "cutter", "cruiser" })
    {
        INFO(design);
        // A lawful patrol flying the design, on a pirate beside it.
        NpcShip& npc = PutNpc(st, at, Fac::TradersGuild(), NpcRole::Police, design, 900001);
        NpcShip& npcTarget =
            PutNpc(st, { at.x + 100.0f, at.y }, Fac::Pirates(), NpcRole::Pirate, "cruiser", 900002);
        const float byNpc = HullLost(npcTarget, ticks,
                                     [&]()
                                     {
                                         npc.Update(dt);
                                         npcTarget.Update(dt);
                                         sim->StepNpcCombat(st, {}, nullptr);
                                     });

        // A pilot in a ship of the same design, on another pirate like it.
        ShipStats stats;
        REQUIRE(StatsOfDesign(design, stats));
        ClientSession& pilot = sim->CreateSession(sys, { at.x, at.y + 20000.0f }, stats);
        pilot.weaponOn = true;
        NpcShip&    pilotTarget = PutNpc(st, { at.x + 100.0f, at.y + 20000.0f }, Fac::Pirates(),
                                         NpcRole::Pirate, "cruiser", 900003);
        const float byPilot = HullLost(pilotTarget, ticks, [&]()
                                       { sim->StepPlayerFire(pilot, st, 900003, dt, nullptr); });

        Ships::Stats d;
        std::string  why;
        REQUIRE(
            Ships::Derive(Archetypes::ShipCatalogue(), *Archetypes::FindDesign(design), d, why));
        CHECK(npc.GetDamage() == doctest::Approx(d.volley));
        CHECK(npc.GetMaxHull() == doctest::Approx(stats.hull));  // the same structure, too
        CHECK(byPilot == doctest::Approx(4.0f * d.volley));
        CHECK(byNpc == doctest::Approx(byPilot));

        sim->DestroySession(pilot.id);
        st.entities.erase(std::remove_if(st.entities.begin(), st.entities.end(),
                                         [](const std::unique_ptr<Entity>& e)
                                         { return e->GetId() >= 900001 && e->GetId() <= 900003; }),
                          st.entities.end());
    }
}

TEST_CASE("a warmed system's armed ships fight with the numbers its cold pass reckons with (#186)")
{
    auto              sim = ColdWorld();
    ClientSession&    p = Arrive(*sim);
    const std::string sys = p.systemId;
    REQUIRE(Simulation::IsHot(*sim->SystemById(sys)));
    int armed = 0;
    for (const NpcShip* n : ShipsIn(*sim, sys))
    {
        if (!n->IsCombatant())
            continue;
        const std::vector<std::string>* list = Archetypes::ShipCatalogue().DesignsFor(
            Factions::Id(n->GetFaction()), NpcRoleId(n->GetRole()));
        REQUIRE(list != nullptr);
        if (list->size() != 1)
            continue;  // a mix is reckoned by its mean, which no one ship of it flies
        const Ships::Fighter f = sim->FighterOf(n->GetFaction(), n->GetRole());
        CHECK(n->GetDamage() / Ships::PLAYER_FIRE_INTERVAL == doctest::Approx(f.dps));
        CHECK(n->GetMaxHull() == doctest::Approx(f.hull));
        armed++;
    }
    CHECK(armed > 0);
}

TEST_CASE("a lighter gun on the pirates' raider costs a cold system fewer traders (#186)")
{
    namespace fs = std::filesystem;
    // The pirates' dps, and the suppression one cold pass leaves on a system's traders, with
    // the archetypes (and the ship catalogue beside them) at `archetypes`.
    auto onePass = [](const std::string& archetypes)
    {
        SetRandomSeed(0xC0FFEEu);
        REQUIRE(Factions::Load(DATA + "factions.json"));
        REQUIRE(Archetypes::Load(archetypes));
        REQUIRE(Blueprints::Load(DATA + "blueprints.json"));
        auto sim = std::make_unique<Simulation>();
        sim->LoadUniverse(std::string(TEST_WORLD_DIR) + "universe.json");
        sim->AttachRegion(7, std::string(TEST_WORLD_DIR) + "systems/");
        sim->Seed(1234u);
        sim->InitGalaxy();
        sim->MaterializeAllSystems(std::string(TEST_WORLD_DIR) + "systems/");
        SystemState& st = *sim->SystemById(sim->Universe().startId);
        REQUIRE_FALSE(Simulation::IsHot(st));
        st.agg.pirates = 4.0f;
        st.agg.traders = 6.0f;
        st.agg.supTraders = 0.0f;
        sim->MaintainWorld(2.0f);
        return std::make_pair(sim->FighterOf(Fac::Pirates(), NpcRole::Pirate).dps,
                              st.agg.supTraders);
    };
    const auto stock = onePass(DATA + "archetypes.json");

    // The same data, but the raider's railgun swapped for a laser.
    const TempDir  tmp;
    const fs::path dir = tmp.Path("data");
    fs::create_directories(dir);
    for (const char* f : { "archetypes.json", "modules.json" })
        fs::copy_file(fs::path(DATA) / f, dir / f);
    fs::copy(fs::path(DATA) / "modules", dir / "modules", fs::copy_options::recursive);
    std::string ships;
    {
        std::ifstream in(DATA + "ships.json");
        ships.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const std::string railgun = "\"module\": \"weapons.railgun\", \"in\": \"bow\", \"on\": "
                                "\"top\", \"count\": 1, \"scale\": 0.18";
    const size_t      at = ships.find(railgun);
    REQUIRE(at != std::string::npos);  // the raider's, the one railgun fitted at that scale
    ships.replace(at, railgun.size(),
                  "\"module\": \"weapons.laser\", \"in\": \"bow\", \"on\": \"top\", "
                  "\"count\": 1, \"scale\": 0.18");
    {
        std::ofstream out(dir / "ships.json");
        out << ships;
    }
    const auto lighter = onePass((dir / "archetypes.json").string());
    REQUIRE(Archetypes::Load(DATA + "archetypes.json"));  // the stock data again

    REQUIRE(stock.second > 0.0f);
    CHECK(lighter.first < stock.first);  // a laser takes off less than a railgun
    // ...and the traders the pass costs fall in the same proportion as the raider's guns.
    CHECK(lighter.second / stock.second == doctest::Approx(lighter.first / stock.first));
}
