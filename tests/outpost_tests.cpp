#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "core/Orbits.h"
#include "core/World.h"
#include "entities/AsteroidField.h"
#include "entities/Planet.h"
#include "entities/Structure.h"
#include "gen/Region.h"
#include "sim/FactionMind.h"
#include "sim/Simulation.h"

// Outposts placed by purpose (#318): what a faction valued in a system decides what it
// builds there, the purpose decides where, and each purpose looks like what it is for.

namespace
{
const std::string DATA = TEST_DATA_DIR;
using Outposts::Purpose;

void LoadRegistries()
{
    Factions::Load(DATA + "factions.json");
    REQUIRE(Archetypes::Load(DATA + "archetypes.json"));
    REQUIRE(Blueprints::Load(DATA + "blueprints.json"));
}

std::unique_ptr<Simulation> MakeWorld(uint64_t seed)
{
    SetRandomSeed(0xC0FFEEu);
    LoadRegistries();
    auto sim = std::make_unique<Simulation>();
    sim->LoadUniverse(DATA + "universe.json");
    sim->AttachRegion(seed, DATA + "systems/");
    sim->Seed(1234u);
    sim->InitGalaxy();
    sim->MaterializeAllSystems(DATA + "systems/");
    return sim;
}

float Distance(Vector2 a, Vector2 b)
{
    return std::hypot(a.x - b.x, a.y - b.y);
}

float ToSegment(Vector2 p, Vector2 a, Vector2 b)
{
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float len2 = dx * dx + dy * dy;
    const float t =
        len2 > 0.0f ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / len2, 0.0f, 1.0f) : 0.0f;
    return Distance(p, Vector2{ a.x + dx * t, a.y + dy * t });
}

std::vector<const Entity*> Of(const SystemState& st, EntityKind kind)
{
    std::vector<const Entity*> out;
    for (const auto& e : st.entities)
        if (e->GetKind() == kind)
            out.push_back(e.get());
    return out;
}

const Structure* OutpostIn(const Simulation& sim, const std::string& system)
{
    for (const auto& e : sim.SystemById(system)->entities)
        if (e->GetKind() == EntityKind::Structure &&
            Outposts::IsOutpost(static_cast<const Structure&>(*e).GetBuilds()))
            return static_cast<const Structure*>(e.get());
    return nullptr;
}
}  // namespace

TEST_CASE("every purpose has a faction's blueprint, a look of its own and the same cost (#318)")
{
    LoadRegistries();
    std::set<std::string> looks;
    for (int i = 0; i < Outposts::PURPOSE_COUNT; i++)
    {
        const Blueprint* bp = Blueprints::Find(Outposts::BlueprintOf((Purpose)i));
        REQUIRE(bp != nullptr);
        CHECK(bp->byFactions);
        CHECK_FALSE(bp->byPlayers);
        float cost = 0.0f;
        for (const auto& c : bp->cost)
            cost += (float)c.second;
        CHECK(cost == doctest::Approx(Simulation::OutpostCost()));
        const Archetype* a = Archetypes::Find(bp->archetype);
        REQUIRE(a != nullptr);
        CHECK(a->kind == EntityKind::Structure);
        looks.insert(bp->archetype);
        Purpose back = Purpose::Open;
        CHECK(Outposts::PurposeOf(bp->archetype, back));
        CHECK(back == (Purpose)i);
        for (const Blueprint& b : Blueprints::All())
            CHECK(b.id != bp->id);  // no player is offered one
    }
    CHECK(looks.size() == (size_t)Outposts::PURPOSE_COUNT);
    CHECK_FALSE(Outposts::IsOutpost("structure.beacon"));
}

TEST_CASE("a faction builds for what it values in the system (#318)")
{
    LoadRegistries();
    Intel rich;
    rich.belts = 3;
    rich.wrecks = 4;
    rich.planets = 2;
    rich.gates = 3;

    // Whatever a lawless faction came for, it hides.
    CHECK(Outposts::ChoosePurpose(Factions::TemperamentOf(FactionId::Pirates), false, rich) ==
          Purpose::Hidden);

    Temperament t;  // values nothing
    CHECK(Outposts::ChoosePurpose(t, true, rich) == Purpose::Open);
    t.ore = 1.0f;
    CHECK(Outposts::ChoosePurpose(t, true, rich) == Purpose::Mining);
    Intel barren = rich;
    barren.belts = 0;
    CHECK(Outposts::ChoosePurpose(t, true, barren) == Purpose::Open);  // no ore, nothing to do

    t = Temperament{};
    t.traffic = 1.0f;
    CHECK(Outposts::ChoosePurpose(t, true, rich) == Purpose::Watch);  // a junction
    Intel deadEnd = rich;
    deadEnd.gates = 1;
    CHECK(Outposts::ChoosePurpose(t, true, deadEnd) == Purpose::Open);  // nobody passes through

    t = Temperament{};
    t.unclaimed = 1.0f;
    CHECK(Outposts::ChoosePurpose(t, true, rich) == Purpose::Orbital);
    t = Temperament{};
    t.salvage = 1.0f;
    CHECK(Outposts::ChoosePurpose(t, true, rich) == Purpose::Salvage);

    // The strongest pull wins: the Syndicate digs where there is ore, and holds a planet
    // where there is none.
    const Temperament& syndicate = Factions::TemperamentOf(FactionId::Syndicate);
    CHECK(Outposts::ChoosePurpose(syndicate, true, rich) == Purpose::Mining);
    CHECK(Outposts::ChoosePurpose(syndicate, true, barren) == Purpose::Orbital);
}

TEST_CASE("each purpose leads to its place, every place is legal, and the same world gives the "
          "same one (#318)")
{
    std::array<int, Outposts::PURPOSE_COUNT> found{};
    for (uint64_t seed = 1; seed <= 4; seed++)
    {
        auto sim = MakeWorld(seed);
        for (const auto& kv : sim->Systems())
        {
            const std::string& id = kv.first;
            const SystemState& st = kv.second;
            const auto&        near = sim->Neighbors(id);
            const std::string  from = near.empty() ? std::string() : near.front();
            const auto         gates = Of(st, EntityKind::Gate);
            for (int i = 0; i < Outposts::PURPOSE_COUNT; i++)
            {
                const Purpose    p = (Purpose)i;
                const Blueprint* bp = Blueprints::Find(Outposts::BlueprintOf(p));
                REQUIRE(bp != nullptr);
                const auto spot = sim->FindOutpostSpot(FactionId::Syndicate, p, from, id);
                const auto again = sim->FindOutpostSpot(FactionId::Syndicate, p, from, id);
                CHECK(spot.found == again.found);
                CHECK(spot.at.x == again.at.x);
                CHECK(spot.at.y == again.at.y);
                if (!spot.found)
                    continue;
                found[(size_t)i]++;
                INFO("seed " << seed << ", " << id << ", " << Outposts::Called(p));

                if (spot.orbit)
                {
                    // A satellite: a free orbit of a planet, within its reach.
                    const auto planets = Of(st, EntityKind::Planet);
                    REQUIRE(spot.orbit->planet >= 0);
                    REQUIRE(spot.orbit->planet < (int)planets.size());
                    const Entity& planet = *planets[(size_t)spot.orbit->planet];
                    CHECK(spot.orbit->radius > planet.GetSize());
                    CHECK(spot.orbit->radius <= Gen::MoonZone(planet.GetSize()));
                    for (const auto& e : st.entities)
                        if (e->GetOrbit() && e->GetOrbit()->planet == spot.orbit->planet)
                            CHECK(std::fabs(e->GetOrbit()->radius - spot.orbit->radius) >
                                  e->GetSize());
                }
                else
                    CHECK(sim->SpotProblem(st, *bp, spot.at) == "");

                switch (p)
                {
                    case Purpose::Mining:
                    {
                        const AsteroidField* best = nullptr;
                        for (const Entity* e : Of(st, EntityKind::Field))
                        {
                            const auto* b = static_cast<const AsteroidField*>(e);
                            if (best == nullptr || b->GetOreMax() > best->GetOreMax())
                                best = b;
                        }
                        REQUIRE(best != nullptr);
                        if (best->GetOrbit())
                            CHECK((spot.orbit && spot.orbit->planet == best->GetOrbit()->planet));
                        else
                            CHECK(Distance(spot.at, best->GetPosition()) <
                                  best->GetSize() + 70000.0f);
                        break;
                    }
                    case Purpose::Salvage:
                    {
                        float nearest = 1.0e9f;
                        for (const Entity* w : Of(st, EntityKind::Derelict))
                            nearest = std::min(nearest,
                                               Distance(spot.at, w->GetPosition()) - w->GetSize());
                        CHECK((spot.orbit.has_value() || nearest < 80000.0f));
                        break;
                    }
                    case Purpose::Orbital: CHECK(spot.orbit.has_value()); break;
                    case Purpose::Watch:
                    {
                        // Touching no gate, and on the way between them or at their middle.
                        float   way = 1.0e9f;
                        Vector2 middle{ 0.0f, 0.0f };
                        for (const Entity* g : gates)
                        {
                            CHECK(Distance(spot.at, g->GetPosition()) >= g->GetSize() + 40000.0f);
                            middle.x += g->GetPosition().x / (float)gates.size();
                            middle.y += g->GetPosition().y / (float)gates.size();
                            for (const Entity* h : gates)
                                if (h != g)
                                    way = std::min(way, ToSegment(spot.at, g->GetPosition(),
                                                                  h->GetPosition()));
                        }
                        if (gates.size() >= 2)
                            CHECK((way <= 25000.0f || Distance(spot.at, middle) <= 75000.0f));
                        break;
                    }
                    case Purpose::Hidden:
                    {
                        bool inCloud = false;
                        for (const Entity* n : Of(st, EntityKind::Nebula))
                            inCloud = inCloud || Distance(spot.at, n->GetPosition()) < n->GetSize();
                        CHECK((inCloud || Distance(spot.at, Vector2{ 0.0f, 0.0f }) >=
                                              0.8f * World::SYSTEM_RADIUS - 1.0f));
                        break;
                    }
                    case Purpose::Open:
                        for (const Entity* g : gates)
                            CHECK(Distance(spot.at, g->GetPosition()) >= g->GetSize() + 60000.0f);
                        break;
                }
            }
        }
    }
    // Over four regions every purpose found its place somewhere.
    for (int i = 0; i < Outposts::PURPOSE_COUNT; i++)
    {
        INFO(Outposts::Called((Purpose)i));
        CHECK(found[(size_t)i] > 3);
    }
}

TEST_CASE("an orbital outpost moves with its planet and is still its satellite after a restart "
          "(#318)")
{
    const std::string path = "outpost_world_tmp.json";
    auto              a = MakeWorld(7);
    // A system with a planet that has room for one more moon.
    std::string             where;
    Simulation::OutpostSpot spot;
    for (const auto& kv : a->Systems())
    {
        if (kv.second.agg.claimed)
            continue;
        spot = a->FindOutpostSpot(FactionId::Syndicate, Purpose::Orbital, "", kv.first);
        if (spot.found)
        {
            where = kv.first;
            break;
        }
    }
    REQUIRE_FALSE(where.empty());
    REQUIRE(spot.orbit.has_value());
    const Blueprint* bp = Blueprints::Find(Outposts::BlueprintOf(Purpose::Orbital));
    auto t = std::make_unique<Structure>(spot.at, 0.0f, "Syndicate Orbital Outpost", bp->archetype);
    t->SetOrbit(*spot.orbit);
    const int id = a->AddStatic(where, std::move(t), Outposts::OwnerOf(FactionId::Syndicate));
    REQUIRE(id != 0);

    auto planetAt = [&](const Simulation& sim, double time)
    {
        int n = 0;
        for (const auto& e : sim.SystemById(where)->entities)
            if (e->GetKind() == EntityKind::Planet && n++ == spot.orbit->planet)
                return static_cast<const Planet&>(*e).PositionAt(time);
        return Vector2{ 0.0f, 0.0f };
    };
    a->SetTime(a->Time() + 3600.0);
    // The clock places it: every tick in a hot system, every coarse pass in a cold one (#295).
    a->MaintainWorld(2.0f);
    const Structure* moved = OutpostIn(*a, where);
    REQUIRE(moved != nullptr);
    const Vector2 expect =
        Orbits::SatellitePosition(*spot.orbit, planetAt(*a, a->Time()), a->Time());
    CHECK(moved->GetPosition().x == doctest::Approx(expect.x).epsilon(1e-4));
    CHECK(moved->GetPosition().y == doctest::Approx(expect.y).epsilon(1e-4));
    CHECK(Distance(moved->GetPosition(), spot.at) > 1000.0f);  // an hour on, it has moved
    // Shot at (#39): the damage is state, and the orbit stays description.
    CHECK_FALSE(a->DamageStructure(*a->SystemById(where), id, 100.0f, nullptr));
    a->SaveWorld(path);

    Simulation b;
    b.LoadUniverse(DATA + "universe.json");
    b.AttachRegion(7, DATA + "systems/");
    REQUIRE(b.LoadWorld(path) == Save::Result::Ok);
    b.MaterializeAllSystems(DATA + "systems/");
    std::remove(path.c_str());
    const Structure* back = OutpostIn(b, where);
    REQUIRE(back != nullptr);
    CHECK(back->GetBuilds() == bp->archetype);
    REQUIRE(back->GetOrbit().has_value());
    CHECK(back->GetOrbit()->planet == spot.orbit->planet);
    CHECK(back->GetOrbit()->radius == doctest::Approx(spot.orbit->radius));
    CHECK(back->GetOrbit()->phase == doctest::Approx(spot.orbit->phase));
    CHECK(back->GetOrbit()->speed == doctest::Approx(spot.orbit->speed));
    CHECK(back->GetOwner() == Outposts::OwnerOf(FactionId::Syndicate));
    CHECK(back->GetDamage() == doctest::Approx(100.0f));

    // Destroyed, it leaves its wreck on the same orbit (#39).
    REQUIRE(b.DamageStructure(*b.SystemById(where), back->GetId(), 1.0e6f, nullptr));
    CHECK(OutpostIn(b, where) == nullptr);
    const Entity* wreck = nullptr;
    for (const auto& e : b.SystemById(where)->entities)
        if (e->GetKind() == EntityKind::Derelict && e->GetOrbit() &&
            e->GetOrbit()->planet == spot.orbit->planet &&
            e->GetOrbit()->radius == doctest::Approx(spot.orbit->radius))
            wreck = e.get();
    REQUIRE(wreck != nullptr);
    CHECK(wreck->GetArchetype()->id == bp->wreck);
}

TEST_CASE("a settlement builds for what the faction saw there, and the same world settles the "
          "same place (#318)")
{
    std::vector<std::pair<std::string, Vector2>> runs;
    for (int run = 0; run < 2; run++)
    {
        auto sim = MakeWorld(7);
        // The Guild's frontier, where it has come in strength with the stock to build.
        std::string frontier, home;
        for (const auto& kv : sim->Systems())
            if (kv.second.agg.claimed && kv.second.agg.controller == FactionId::TradersGuild)
                for (const std::string& n : sim->Neighbors(kv.first))
                    if (frontier.empty() && !sim->SystemById(n)->agg.claimed)
                    {
                        frontier = n;
                        home = kv.first;
                    }
        REQUIRE_FALSE(frontier.empty());
        const float cap = Factions::TemperamentOf(FactionId::TradersGuild).capacity;
        for (int pass = 0; pass < 30 && OutpostIn(*sim, frontier) == nullptr; pass++)
        {
            sim->SystemById(frontier)->agg.presence[(int)FactionId::TradersGuild] = cap;
            sim->MindOf(FactionId::TradersGuild).stock = Simulation::OutpostCost();
            sim->MaintainWorld(2.0f);
        }
        const Structure* site = OutpostIn(*sim, frontier);
        REQUIRE(site != nullptr);

        // What it built is what it valued there -- or an open point, when that had no room.
        const Purpose wanted =
            Outposts::ChoosePurpose(Factions::TemperamentOf(FactionId::TradersGuild), true,
                                    sim->MindOf(FactionId::TradersGuild).intel.at(frontier));
        Purpose built = Purpose::Open;
        REQUIRE(Outposts::PurposeOf(site->GetBuilds(), built));
        CHECK((built == wanted || built == Purpose::Open));
        bool said = false;
        for (const ChronicleEntry& e : sim->Chronicle())
            said = said || (e.system == frontier &&
                            e.text.rfind("Traders Guild began " +
                                             std::string(Outposts::Called(built)) + " in ",
                                         0) == 0);
        CHECK(said);
        runs.push_back({ site->GetBuilds(), site->GetPosition() });
    }
    CHECK(runs[0].first == runs[1].first);
    CHECK(runs[0].second.x == runs[1].second.x);
    CHECK(runs[0].second.y == runs[1].second.y);
}
