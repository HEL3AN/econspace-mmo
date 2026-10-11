#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "entities/NpcShip.h"
#include "sim/Simulation.h"
#include "temp_dir.h"
#include "factions_named.h"

// Ships that leave (#295): a system holding more ships of a role than it keeps sends them,
// one at a time, to a neighbour that lacks them -- a number where nobody is, a real ship
// through the gate where somebody is -- and they count where they arrive.

namespace
{
const std::string DATA = TEST_DATA_DIR;

std::unique_ptr<Simulation> World(bool allHot = false)
{
    SetRandomSeed(0xC0FFEEu);
    REQUIRE(Factions::Load(DATA + "factions.json"));
    REQUIRE(Archetypes::Load(DATA + "archetypes.json"));
    REQUIRE(Blueprints::Load(DATA + "blueprints.json"));
    auto sim = std::make_unique<Simulation>();
    sim->LoadUniverse(DATA + "universe.json");
    sim->AttachRegion(7, DATA + "systems/");
    sim->Seed(1234u);
    sim->SetAllHot(allHot);
    sim->InitGalaxy();
    sim->MaterializeAllSystems(DATA + "systems/");
    return sim;
}

// The host's tick with nobody's input, for a whole coarse pass.
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

// A system with traders to keep, and its neighbours with room for them too.
struct Scene
{
    std::string              source;
    std::vector<std::string> next;  // its neighbours with room for traders
};

Scene FindScene(Simulation& sim)
{
    Scene s;
    for (const auto& kv : sim.Systems())
    {
        if (kv.first == sim.Universe().startId || sim.KeepOf(kv.first, NpcRole::Trader) == 0)
            continue;
        std::vector<std::string> next;
        for (const std::string& n : sim.Neighbors(kv.first))
            if (sim.KeepOf(n, NpcRole::Trader) > 0 && n != sim.Universe().startId)
                next.push_back(n);
        if (!next.empty())
        {
            s.source = kv.first;
            s.next = next;
            break;
        }
    }
    REQUIRE_FALSE(s.source.empty());
    return s;
}

// No pirates in a system: none to count, none to raid its traders, as numbers or as ships --
// so that what is watched here is ships coming and going, not ships lost.
void Calm(Simulation& sim, const std::string& id)
{
    SystemState& st = *sim.SystemById(id);
    st.agg.presence[Fac::Pirates()] = 0.0f;
    st.agg.pirates = 0.0f;
    st.agg.supTraders = 0.0f;
    for (const auto& e : st.entities)
        if (e->GetKind() == EntityKind::Npc &&
            static_cast<const NpcShip&>(*e).GetRole() == NpcRole::Pirate)
            static_cast<NpcShip&>(*e).TakeDamage(1.0e6f);
}

void CalmAll(Simulation& sim, const Scene& s)
{
    Calm(sim, s.source);
    for (const std::string& n : s.next)
        Calm(sim, n);
}

// Prosperous and safe: the source keeps its most traders, and has them.
void Boom(Simulation& sim, const Scene& s, int passes)
{
    for (int i = 0; i < passes; i++)
    {
        CalmAll(sim, s);
        sim.SystemById(s.source)->agg.security = 1.0f;
        Pass(sim);
    }
}

// Where nobody is, simply six of them: as many as it keeps at its safest.
void Crowd(Simulation& sim, const Scene& s)
{
    Boom(sim, s, 5);
    sim.SystemById(s.source)->agg.traders = 6.0f;
}

// Next door has lost its traders -- as numbers, or as ships somebody shot down.
void Empty(Simulation& sim, const Scene& s)
{
    for (const std::string& n : s.next)
    {
        SystemState& st = *sim.SystemById(n);
        st.agg.traders = 0.0f;
        for (const auto& e : st.entities)
            if (e->GetKind() == EntityKind::Npc &&
                static_cast<const NpcShip&>(*e).GetRole() == NpcRole::Trader)
                static_cast<NpcShip&>(*e).TakeDamage(1.0e6f);
    }
}

// Then it goes quiet -- it keeps two -- while next door is safe and keeps six.
void Bust(Simulation& sim, const Scene& s)
{
    CalmAll(sim, s);
    sim.SystemById(s.source)->agg.security = 0.0f;
    for (const std::string& n : s.next)
        sim.SystemById(n)->agg.security = 1.0f;
}

float Traders(const Simulation& sim, const std::string& id)
{
    return sim.SystemById(id)->agg.traders;
}

std::set<long long> Seen(const Simulation& sim, const std::string& from,
                         std::map<long long, Simulation::Migrant>& log)
{
    std::set<long long> ids;
    for (const auto& kv : sim.Migrants())
        if (kv.second.from == from)
        {
            ids.insert(kv.first);
            if (log.count(kv.first) == 0)
                log[kv.first] = kv.second;
        }
    return ids;
}
}  // namespace

TEST_CASE("how fast ships leave, and how long they take, are rules in the data (#295)")
{
    REQUIRE(Factions::Load(DATA + "factions.json"));
    const Migration& m = Factions::MigrationRules();
    CHECK(m.trader > 0.0f);
    CHECK(m.patrol > 0.0f);
    CHECK(m.pirate > 0.0f);
    CHECK(m.travel > 0.0f);
    CHECK(m.align < m.travel);  // through the gate before it is due on the other side
}

TEST_CASE("surplus ships leave a cold system one at a time for a neighbour that lacks them (#295)")
{
    auto         sim = World();
    const Scene  s = FindScene(*sim);
    const double every = Factions::MigrationRules().trader;
    Crowd(*sim, s);
    Empty(*sim, s);
    const float before = Traders(*sim, s.source);
    REQUIRE(before >= 4.0f);

    std::map<long long, Simulation::Migrant> log;
    const double                             until = sim->Time() + 5 * every;
    while (sim->Time() < until)
    {
        Bust(*sim, s);
        Pass(*sim);
        Seen(*sim, s.source, log);
    }
    REQUIRE(log.size() >= 2);
    std::set<long long> slots;
    for (const auto& kv : log)
    {
        const Simulation::Migrant& m = kv.second;
        CHECK(m.role == NpcRole::Trader);
        CHECK(std::find(s.next.begin(), s.next.end(), m.to) != s.next.end());
        CHECK(m.arrivesAt - m.leftAt == doctest::Approx(Factions::MigrationRules().travel));
        // At most one a role's interval: every stretch of the clock has its one moment.
        CHECK(slots.insert((long long)std::floor(m.leftAt / every)).second);
    }
    // What it keeps is two; it is no longer far above that, and nothing was made up there.
    CHECK(Traders(*sim, s.source) < before - 1.5f);
    CHECK(Traders(*sim, s.source) <= sim->KeepOf(s.source, NpcRole::Trader) + 1.0f);
    CHECK_FALSE(Simulation::IsHot(*sim->SystemById(s.source)));
}

TEST_CASE("a system next to one with ships to spare waits for them rather than have more "
          "made, and they count there when they arrive (#295)")
{
    auto        sim = World();
    const Scene s = FindScene(*sim);
    Crowd(*sim, s);
    Empty(*sim, s);
    const float                              empty = 0.0f;  // next door, emptied
    std::map<long long, Simulation::Migrant> log;
    int                                      arrived = 0;
    std::set<long long>                      live;
    for (int pass = 0; pass < 300; pass++)
    {
        Bust(*sim, s);
        Pass(*sim);
        const std::set<long long> now = Seen(*sim, s.source, log);
        for (long long id : live)
            arrived += now.count(id) == 0 ? 1 : 0;
        live = now;
    }
    REQUIRE(arrived >= 1);
    float there = 0.0f;
    for (const std::string& n : s.next)
        there += Traders(*sim, n);
    // Whatever pirates took on the way, what arrived is there, and the director made up only
    // what the source could not spare.
    CHECK(there > empty);
    CHECK(there >= (float)arrived - 1.0f);
}

TEST_CASE("surplus spreads out: populations converge towards what systems keep (#295)")
{
    auto sim = World();
    auto excess = [&]()
    {
        float total = 0.0f;
        for (const auto& kv : sim->Systems())
            for (NpcRole r : { NpcRole::Trader, NpcRole::Miner, NpcRole::Police, NpcRole::Pirate })
            {
                const SystemAggregate& a = kv.second.agg;
                const float            have = r == NpcRole::Trader   ? a.traders
                                              : r == NpcRole::Miner  ? a.miners
                                              : r == NpcRole::Police ? a.police
                                                                     : a.pirates;
                total += std::max(0.0f, have - (float)sim->KeepOf(kv.first, r) - 1.0f);
            }
        return total;
    };
    for (int pass = 0; pass < 60; pass++)
        Pass(*sim);
    const float settled = excess();
    // Everywhere with room for them gets four traders more than it keeps.
    int crowded = 0;
    for (auto& kv : sim->Systems())
        if (sim->KeepOf(kv.first, NpcRole::Trader) > 0)
        {
            kv.second.agg.traders = (float)sim->KeepOf(kv.first, NpcRole::Trader) + 4.0f;
            crowded++;
        }
    REQUIRE(crowded >= 4);
    const float crowd = excess();
    REQUIRE(crowd > settled + 2.0f * crowded);
    for (int pass = 0; pass < 30 * 60; pass++)  // an hour
        Pass(*sim);
    MESSAGE("excess over what systems keep: settled " << settled << ", crowded " << crowd
                                                      << ", an hour later " << excess());
    CHECK(excess() < settled + 0.5f * (crowd - settled));
}

TEST_CASE("where somebody is, a ship turns for the gate and goes, and comes in through the gate "
          "at the other end (#295)")
{
    auto        sim = World(true);
    const Scene s = FindScene(*sim);
    Boom(*sim, s, 30);
    Empty(*sim, s);
    std::map<long long, Simulation::Migrant> log;
    const Simulation::Migrant*               first = nullptr;
    for (int pass = 0; pass < 300 && first == nullptr; pass++)
    {
        Bust(*sim, s);
        Pass(*sim);
        for (const auto& kv : sim->Migrants())
            if (kv.second.from == s.source && kv.second.shipId != 0)
                first = &kv.second;
    }
    REQUIRE(first != nullptr);
    const Simulation::Migrant m = *first;
    auto                      find = [&](int id) -> const NpcShip*
    {
        for (const auto& e : sim->SystemById(s.source)->entities)
            if (e->GetId() == id && e->GetKind() == EntityKind::Npc)
                return static_cast<const NpcShip*>(e.get());
        return nullptr;
    };
    REQUIRE(find(m.shipId) != nullptr);
    CHECK(find(m.shipId)->IsLeaving());
    CHECK(find(m.shipId)->GetRole() == NpcRole::Trader);

    // It warps to the gate and is through it as soon as it gets there -- well before the
    // longest it may take.
    while (sim->Time() < m.jumpAt + 2.5 && sim->Migrants().count(m.id) == 1 &&
           sim->Migrants().at(m.id).shipId != 0)
    {
        Bust(*sim, s);
        Pass(*sim);
    }
    CHECK(sim->Time() < m.jumpAt);
    CHECK(find(m.shipId) == nullptr);
    REQUIRE(sim->Migrants().count(m.id) == 1);
    CHECK(sim->Migrants().at(m.id).shipId == 0);

    // ...and in through the gate on the other side when it is due.
    std::set<int> before;
    for (const auto& e : sim->SystemById(m.to)->entities)
        before.insert(e->GetId());
    while (sim->Time() < m.arrivesAt + 0.1)
    {
        Bust(*sim, s);
        Pass(*sim);
    }
    CHECK(sim->Migrants().count(m.id) == 0);
    const Vector2 gate = sim->ArrivalFrom(m.to, m.from);
    bool          came = false;
    for (const auto& e : sim->SystemById(m.to)->entities)
        if (before.count(e->GetId()) == 0 && e->GetKind() == EntityKind::Npc &&
            static_cast<const NpcShip&>(*e).GetRole() == NpcRole::Trader)
        {
            // Where it came in, or as far from there as it has flown since -- at most a pass at
            // warp, towards the first stop of its round; nowhere near the far side of the system.
            const float dx = e->GetPosition().x - gate.x, dy = e->GetPosition().y - gate.y;
            const float reach = 60000.0f * 2.5f;
            came = came || dx * dx + dy * dy <= reach * reach;
        }
    CHECK(came);
}

TEST_CASE("ships leave the same way whether anybody watches or not (#295)")
{
    std::vector<double> left[2];
    float               after[2] = { 0.0f, 0.0f };
    for (int hot = 0; hot < 2; hot++)
    {
        auto        sim = World(hot == 1);
        const Scene s = FindScene(*sim);
        Empty(*sim, s);
        std::map<long long, Simulation::Migrant> log;
        for (int pass = 0; pass < 900; pass++)  // half an hour
        {
            Bust(*sim, s);
            Pass(*sim);
            Seen(*sim, s.source, log);
        }
        for (const auto& kv : log)
            left[hot].push_back(kv.second.leftAt);
        sim->RecountAgg(*sim->SystemById(s.source));
        after[hot] = Traders(*sim, s.source);
    }
    MESSAGE("cold left " << left[0].size() << ", hot " << left[1].size() << "; traders after "
                         << after[0] << " / " << after[1]);
    REQUIRE_FALSE(left[0].empty());
    CHECK(std::abs((int)left[0].size() - (int)left[1].size()) <= 1);
    for (size_t i = 0; i < std::min(left[0].size(), left[1].size()); i++)
        CHECK(std::fabs(left[0][i] - left[1][i]) <= 60.0);
    CHECK(std::fabs(after[0] - after[1]) <= 1.5f);
}

TEST_CASE("ships on their way survive a restart (#295)")
{
    const TempDir     tmp;
    const std::string path = tmp.Path("migration_world.json");
    auto              a = World();
    const Scene       s = FindScene(*a);
    Crowd(*a, s);
    Empty(*a, s);
    for (int pass = 0; pass < 600 && a->Migrants().empty(); pass++)
    {
        Bust(*a, s);
        Pass(*a);
    }
    REQUIRE_FALSE(a->Migrants().empty());
    const Simulation::Migrant m = a->Migrants().begin()->second;
    a->SaveWorld(path);

    Simulation b;
    b.LoadUniverse(DATA + "universe.json");
    b.AttachRegion(7, DATA + "systems/");
    REQUIRE(b.LoadWorld(path) == Save::Result::Ok);
    b.MaterializeAllSystems(DATA + "systems/");
    REQUIRE(b.Migrants().size() == a->Migrants().size());
    const Simulation::Migrant back = b.Migrants().begin()->second;
    CHECK(back.from == m.from);
    CHECK(back.to == m.to);
    CHECK(back.role == m.role);
    CHECK(back.faction == m.faction);
    CHECK(back.arrivesAt == doctest::Approx(m.arrivesAt));
    CHECK(back.shipId == 0);
}
