#include <doctest/doctest.h>

#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "entities/NpcShip.h"
#include "entities/Ship.h"
#include "entities/ShipType.h"
#include "sim/ClientSession.h"
#include "sim/Simulation.h"
#include "sim/SpatialGrid.h"
#include "factions_named.h"
#include "goods_named.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// A crowd in one system (#350): NPC targeting reads a spatial grid, and what it picks is what
// walking every ship and pilot picks -- ties and all.

namespace
{
struct Lcg  // a generator of our own: the same crowd in two simulations
{
    uint32_t s;
    uint32_t Next()
    {
        s = s * 1664525u + 1013904223u;
        return s >> 8;
    }
    float Unit() { return (float)(Next() & 0xFFFF) / 65536.0f; }
};

struct Crowd
{
    Simulation                              sim;
    std::string                             sys;
    std::vector<Simulation::PlayerPresence> players;
    std::vector<FireEvent>                  fires;
    std::vector<int>                        npcIds;

    // The registries are loaded once, before either of two worlds is built: a reload would
    // leave the first one's entities pointing at archetypes that are gone.
    static void Load()
    {
        const std::string data = TEST_DATA_DIR;
        REQUIRE(Factions::Load(data + "factions.json"));
        REQUIRE(Archetypes::Load(data + "archetypes.json"));
        REQUIRE(Blueprints::Load(data + "blueprints.json"));
        REQUIRE(Goods::Load(data + "goods.json"));
    }

    Crowd(bool grid, int pilots, int npcs, float spread)
    {
        SetRandomSeed(0xC0FFEEu);
        sim.LoadUniverse(std::string(TEST_WORLD_DIR) + "universe.json");
        sim.Seed(1234u);
        sim.InitGalaxy();
        sim.MaterializeAllSystems(std::string(TEST_WORLD_DIR) + "systems/");
        sim.SetAllHot(true);
        sim.UseTargetGrid(grid);

        Lcg               rng{ 99u };
        const std::string at = sim.Universe().startId;
        SystemState&      st = *sim.SystemById(at);
        sys = at;
        auto& ents = st.entities;
        ents.erase(std::remove_if(ents.begin(), ents.end(), [](const std::unique_ptr<Entity>& e)
                                  { return e->GetKind() == EntityKind::Npc; }),
                   ents.end());
        const Vector2 c{ 50000.0f, 50000.0f };
        for (int i = 0; i < pilots; i++)
        {
            ClientSession& s = sim.CreateSession(at, c, GetShipCatalog()[0].stats);
            s.accountName = "pilot" + std::to_string(i);
            s.ship->Teleport(
                { c.x + (rng.Unit() - 0.5f) * spread, c.y + (rng.Unit() - 0.5f) * spread });
            if (i % 3 == 0)
                s.account.SetReputation(Fac::Syndicate(), -30.0f);  // somebody's enemy
            Simulation::PlayerPresence p;
            p.session = &s;
            p.ship = s.ship.get();
            p.hidden = i % 7 == 3;
            players.push_back(p);
        }
        const FactionId factions[] = { Fac::Independent(), Fac::TradersGuild(), Fac::Syndicate() };
        const NpcRole   roles[] = { NpcRole::Pirate, NpcRole::Police, NpcRole::Trader,
                                    NpcRole::Miner };
        for (int i = 0; i < npcs; i++)
        {
            const NpcRole r = roles[i % 4];
            // Some on a grid of exact spots, so distances tie.
            Vector2 p = (i % 5 == 0)
                            ? Vector2{ c.x + 40.0f * (float)(i % 11), c.y + 40.0f * (float)(i % 7) }
                            : Vector2{ c.x + (rng.Unit() - 0.5f) * spread,
                                       c.y + (rng.Unit() - 0.5f) * spread };
            const FactionId f = r == NpcRole::Pirate ? Factions::Pirates() : factions[i % 3];
            auto            n = std::make_unique<NpcShip>(p, f, r, std::vector<Vector2>{ c });
            n->SetId(800000 + i);
            npcIds.push_back(800000 + i);
            ents.push_back(std::move(n));
        }
    }

    void Run(int ticks)
    {
        SystemState& st = *sim.SystemById(sys);
        for (int i = 0; i < ticks; i++)
            sim.StepSystemAgents(st, players, &fires, 1.0f / 60.0f);
    }

    std::vector<std::tuple<int, float, float, float, int>> Ships() const
    {
        std::vector<std::tuple<int, float, float, float, int>> out;
        for (const auto& e : sim.SystemById(sys)->entities)
            if (e->GetKind() == EntityKind::Npc)
            {
                const NpcShip& n = static_cast<const NpcShip&>(*e);
                out.emplace_back(n.GetId(), n.GetPosition().x, n.GetPosition().y, n.GetHull(),
                                 (int)n.GetState());
            }
        return out;
    }
};
}  // namespace

TEST_CASE("the grid returns every point within the radius, in index order")
{
    Lcg                  rng{ 5u };
    std::vector<Vector2> pts;
    for (int i = 0; i < 600; i++)
        pts.push_back({ (rng.Unit() - 0.5f) * 30000.0f, (rng.Unit() - 0.5f) * 30000.0f });
    pts.push_back({ 1.0e9f, -1.0e9f });  // far away, and a NaN, must not break the cell maths
    pts.push_back({ std::nanf(""), 0.0f });
    SpatialGrid g;
    g.Build(pts, 700.0f);
    std::vector<int> hit;
    for (int q = 0; q < 200; q++)
    {
        const Vector2 at{ (rng.Unit() - 0.5f) * 30000.0f, (rng.Unit() - 0.5f) * 30000.0f };
        const float   r = 50.0f + 650.0f * rng.Unit();
        g.Query(at, r, hit);
        CHECK(std::is_sorted(hit.begin(), hit.end()));
        std::vector<int> truth;
        for (size_t i = 0; i < pts.size(); i++)
            if (std::hypot(pts[i].x - at.x, pts[i].y - at.y) <= r)
                truth.push_back((int)i);
        CHECK(std::includes(hit.begin(), hit.end(), truth.begin(), truth.end()));
    }
}

TEST_CASE("a crowd fights the same with the grid as with every ship and pilot (#350)")
{
    // Packed enough that ships are in each other's reach and in the pilots', and sparse enough
    // that some are not.
    Crowd::Load();
    for (float spread : { 3000.0f, 12000.0f })
    {
        Crowd grid(true, 40, 160, spread);
        Crowd full(false, 40, 160, spread);
        grid.Run(240);
        full.Run(240);
        CHECK(grid.Ships() == full.Ships());
        REQUIRE(grid.fires.size() == full.fires.size());
        CHECK(grid.fires.size() > 0);  // the case has a fight in it
        for (size_t i = 0; i < grid.fires.size(); i++)
        {
            CHECK(grid.fires[i].from.x == full.fires[i].from.x);
            CHECK(grid.fires[i].to.x == full.fires[i].to.x);
            CHECK(grid.fires[i].to.y == full.fires[i].to.y);
            CHECK(grid.fires[i].targetSessionId == full.fires[i].targetSessionId);
        }
        for (size_t i = 0; i < grid.players.size(); i++)
            CHECK(grid.players[i].ship->GetHull() == full.players[i].ship->GetHull());
    }
}
