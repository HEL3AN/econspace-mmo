#include <doctest/doctest.h>

#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "sim/FactionMind.h"
#include "sim/Simulation.h"

// What factions know, and surveying (#295, slice 2): a faction decides on what it saw,
// which goes out of date, and looking is something it chooses to spend its turn on.

namespace
{
const std::string DATA = TEST_DATA_DIR;

// A galaxy as the host makes one: the hand-written systems and a region from `seed`.
std::unique_ptr<Simulation> World(uint64_t seed = 7)
{
    SetRandomSeed(0xC0FFEEu);
    Factions::Load(DATA + "factions.json");
    REQUIRE(Archetypes::Load(DATA + "archetypes.json"));
    REQUIRE(Blueprints::Load(DATA + "blueprints.json"));
    auto sim = std::make_unique<Simulation>();
    sim->LoadUniverse(DATA + "universe.json");
    sim->AttachRegion(seed, DATA + "systems/");
    sim->Seed(1234u);
    sim->InitGalaxy();
    sim->MaterializeAllSystems(DATA + "systems/");
    return sim;
}

// One macro pass, the coarse step every two seconds of world time.
void Pass(Simulation& sim, int passes = 1)
{
    for (int i = 0; i < passes; i++)
        sim.MaintainWorld(2.0f);
}

bool Holds(const Simulation& sim, FactionId f, const std::string& id)
{
    const SystemState* st = sim.SystemById(id);
    return st != nullptr && st->agg.claimed && st->agg.controller == f;
}

// The region system a gate leads to from a system the Traders Guild holds: the first place
// beyond the wormhole anybody can reach.
std::string GuildFrontier(const Simulation& sim)
{
    for (const auto& kv : sim.Systems())
        if (Holds(sim, FactionId::TradersGuild, kv.first))
            for (const std::string& n : sim.Neighbors(kv.first))
                if (!sim.SystemById(n)->agg.claimed)
                    return n;
    return std::string();
}
}  // namespace

TEST_CASE("risk grows with the age of what was seen; the unknown is worth a look (#295)")
{
    Intel seen;
    seen.seenAt = 1000.0;
    const double now = 1000.0;
    CHECK(Intelligence::Staleness(seen, now) == doctest::Approx(0.0f));
    CHECK(Intelligence::Staleness(seen, now + Intelligence::STALE) == doctest::Approx(1.0f));

    // A risk seen today is the risk; seen a while ago it is more, and keeps growing.
    const float fresh = Intelligence::BelievedRisk(1.0f, 0.0f);
    const float old = Intelligence::BelievedRisk(1.0f, 1.0f);
    const float older = Intelligence::BelievedRisk(1.0f, 2.0f);
    CHECK(fresh == doctest::Approx(1.0f));
    CHECK(old > fresh);
    CHECK(older > old);
    // Even a place that looked empty is not trusted to have stayed so.
    CHECK(Intelligence::BelievedRisk(0.0f, 1.0f) > 0.0f);

    // Never seen: worth all of a faction's curiosity. Seen just now: nothing. Old news:
    // worth looking at again.
    CHECK(Intelligence::SurveyValue(0.6f, nullptr, now) == doctest::Approx(0.6f));
    CHECK(Intelligence::SurveyValue(0.6f, &seen, now) == doctest::Approx(0.0f));
    CHECK(Intelligence::SurveyValue(0.6f, &seen, now + 3.0 * Intelligence::STALE) ==
          doctest::Approx(0.6f));
    // A faction with no curiosity never looks.
    CHECK(Intelligence::SurveyValue(0.0f, nullptr, now) == doctest::Approx(0.0f));
}

TEST_CASE("a faction begins knowing what it holds and what is next door, and nothing more (#295)")
{
    auto sim = World();
    Pass(*sim);  // the first pass seeds what everyone knows

    bool someUnknown = false;
    for (int fi = 0; fi < FACTION_COUNT; fi++)
    {
        const FactionId       f = (FactionId)fi;
        std::set<std::string> expected;
        for (const auto& kv : sim->Systems())
            if (Holds(*sim, f, kv.first))
            {
                expected.insert(kv.first);
                for (const std::string& n : sim->Neighbors(kv.first))
                    expected.insert(n);
            }
        for (const auto& kv : sim->Systems())
        {
            CAPTURE(FactionName(f));
            CAPTURE(kv.first);
            const bool known = sim->MindOf(f).intel.count(kv.first) > 0;
            CHECK(known == (expected.count(kv.first) > 0));
            someUnknown = someUnknown || !known;
        }
    }
    // The region beyond the wormhole is, for the most part, nobody's knowledge yet.
    CHECK(someUnknown);
}

TEST_CASE("what a faction saw does not change when the place does (#295)")
{
    auto              sim = World();
    const std::string frontier = GuildFrontier(*sim);
    REQUIRE_FALSE(frontier.empty());
    Pass(*sim);
    const Intel before = sim->MindOf(FactionId::TradersGuild).intel.at(frontier);

    // The place fills with pirates; nobody of the Guild is there to see it.
    SystemState& st = *sim->SystemById(frontier);
    st.agg.presence[(int)FactionId::Pirates] = 20.0f;
    REQUIRE(st.agg.presence[(int)FactionId::TradersGuild] < 1.0f);
    Pass(*sim, 20);

    const Intel& now = sim->MindOf(FactionId::TradersGuild).intel.at(frontier);
    CHECK(now.seenAt == doctest::Approx(before.seenAt));
    CHECK(now.presence[(int)FactionId::Pirates] < 1.0f);
    // The truth is there to be seen, by whoever looks.
    CHECK(sim->Observe(st).presence[(int)FactionId::Pirates] > 10.0f);
}

TEST_CASE("a faction acts on what it believes, not on what is there (#295)")
{
    // The Guild reaches past the wormhole within the first quarter of an hour of a fresh
    // world. Told that the place is swarming with pirates, it stays home until that news is
    // old enough to be doubted.
    auto measure = [](bool frightened)
    {
        auto              sim = World();
        const std::string frontier = GuildFrontier(*sim);
        REQUIRE_FALSE(frontier.empty());
        if (frightened)
        {
            Intel scare = sim->Observe(*sim->SystemById(frontier));
            scare.presence[(int)FactionId::Pirates] = 50.0f;
            sim->MindOf(FactionId::TradersGuild).intel[frontier] = scare;
        }
        float most = 0.0f;
        for (int pass = 0; pass < 30 * 14; pass++)  // fourteen minutes
        {
            Pass(*sim);
            most = std::max(most,
                            sim->SystemById(frontier)->agg.presence[(int)FactionId::TradersGuild]);
        }
        return most;
    };
    CHECK(measure(false) >= 1.0f);
    CHECK(measure(true) < 1.0f);
}

TEST_CASE("factions survey what they do not know, a few at a time, and it is history (#295)")
{
    auto sim = World();
    Pass(*sim);
    std::map<FactionId, size_t> knownAtStart;
    for (int fi = 0; fi < FACTION_COUNT; fi++)
        knownAtStart[(FactionId)fi] = sim->MindOf((FactionId)fi).intel.size();

    for (int pass = 0; pass < 30 * 60; pass++)  // an hour
    {
        Pass(*sim);
        // Never more under way than a faction's holdings can send.
        std::map<FactionId, int> busy;
        for (const auto& kv : sim->Plans())
            busy[kv.second.faction]++;
        for (const auto& kv : busy)
        {
            int holdings = 0;
            for (const auto& s : sim->Systems())
                holdings += Holds(*sim, kv.first, s.first) ? 1 : 0;
            CHECK(kv.second <= 1 + holdings / 8);
        }
    }

    int surveys = 0;
    for (const ChronicleEntry& e : sim->Chronicle())
    {
        if (e.kind != "survey")
            continue;
        surveys++;
        CAPTURE(e.text);
        REQUIRE(e.faction >= 0);
        const FactionId f = (FactionId)e.faction;
        // "Traders Guild surveyed W-2.1: 5 belts, held by nobody, pirates"
        CHECK(e.text.rfind(FactionName(f) + " surveyed ", 0) == 0);
        CHECK(e.text.find(": ") != std::string::npos);
        // ...and it knows now what it found.
        REQUIRE(sim->MindOf(f).intel.count(e.system) == 1);
        CHECK(sim->MindOf(f).intel.at(e.system).seenAt >= e.time - 1e-6);
    }
    MESSAGE(surveys << " surveys in an hour");
    CHECK(surveys > 0);
    // The history is numbered in order.
    for (size_t i = 1; i < sim->Chronicle().size(); i++)
        CHECK(sim->Chronicle()[i].seq > sim->Chronicle()[i - 1].seq);
    // Somebody learned something beyond what they began with.
    bool learned = false;
    for (int fi = 0; fi < FACTION_COUNT; fi++)
        learned = learned || sim->MindOf((FactionId)fi).intel.size() > knownAtStart[(FactionId)fi];
    CHECK(learned);
}

TEST_CASE("the same world tells the same story (#295)")
{
    auto story = []()
    {
        auto sim = World();
        Pass(*sim, 30 * 45);
        std::vector<std::string> lines;
        for (const ChronicleEntry& e : sim->Chronicle())
            lines.push_back(std::to_string((long long)e.time) + " " + e.text);
        return lines;
    };
    const std::vector<std::string> a = story();
    CHECK(a == story());
}

TEST_CASE("what factions know and what they have under way survive a restart (#295)")
{
    const std::string path = "intel_world_tmp.json";
    auto              a = World();
    Pass(*a);
    // Run until a survey is under way.
    for (int pass = 0; pass < 30 * 60 && a->Plans().empty(); pass++)
        Pass(*a);
    REQUIRE_FALSE(a->Plans().empty());
    const Plan pending = a->Plans().begin()->second;
    a->SaveWorld(path);

    Simulation b;
    b.LoadUniverse(DATA + "universe.json");
    b.AttachRegion(7, DATA + "systems/");
    REQUIRE(b.LoadWorld(path) == Save::Result::Ok);
    b.MaterializeAllSystems(DATA + "systems/");
    std::remove(path.c_str());

    // The plan, as it was.
    REQUIRE(b.Plans().count(pending.id) == 1);
    const Plan& back = b.Plans().at(pending.id);
    CHECK(back.faction == pending.faction);
    CHECK(back.from == pending.from);
    CHECK(back.target == pending.target);
    CHECK(back.startedAt == doctest::Approx(pending.startedAt));
    CHECK(back.dueAt == doctest::Approx(pending.dueAt));
    // What each faction knew, as of when it saw it.
    for (int fi = 0; fi < FACTION_COUNT; fi++)
    {
        const auto& was = a->MindOf((FactionId)fi).intel;
        const auto& is = b.MindOf((FactionId)fi).intel;
        // All but what it sees for itself, which it looks at again before it decides.
        for (const auto& kv : is)
            CHECK(was.count(kv.first) == 1);
        for (const auto& kv : was)
        {
            CAPTURE(kv.first);
            const SystemAggregate& here = a->SystemById(kv.first)->agg;
            if ((here.claimed && here.controller == (FactionId)fi) || here.presence[fi] >= 1.0f)
                continue;
            REQUIRE(is.count(kv.first) == 1);
            const Intel& i = is.at(kv.first);
            CHECK(i.seenAt == doctest::Approx(kv.second.seenAt));
            CHECK(i.belts == kv.second.belts);
            CHECK(i.claimed == kv.second.claimed);
            CHECK(i.controller == kv.second.controller);
            CHECK(i.defenders == kv.second.defenders);
            CHECK(i.presence[(int)FactionId::Pirates] ==
                  doctest::Approx(kv.second.presence[(int)FactionId::Pirates]));
        }
    }
    // The history so far, and it goes on from where it was.
    REQUIRE(b.Chronicle().size() == a->Chronicle().size());
    for (size_t i = 0; i < a->Chronicle().size(); i++)
    {
        CHECK(b.Chronicle()[i].seq == a->Chronicle()[i].seq);
        CHECK(b.Chronicle()[i].text == a->Chronicle()[i].text);
    }

    // And the survey comes back when it was due, not before.
    const std::string line = FactionName(pending.faction) + " surveyed ";
    bool              returned = false;
    for (int pass = 0; pass < 120 && !returned; pass++)
    {
        Pass(b);
        for (const ChronicleEntry& e : b.Chronicle())
            if (e.kind == "survey" && e.system == pending.target && e.time > pending.startedAt)
            {
                CHECK(e.text.rfind(line, 0) == 0);
                CHECK(e.time >= pending.dueAt);
                CHECK(e.time < pending.dueAt + 2.5);
                returned = true;
            }
    }
    CHECK(returned);
    if (!b.Chronicle().empty() && !a->Chronicle().empty())
        CHECK(b.Chronicle().back().seq > a->Chronicle().back().seq);
}

TEST_CASE("a world saved before factions knew anything loads, and they start from next door (#295)")
{
    const std::string path = "intel_old_world_tmp.json";
    {
        std::ofstream out(path);
        out << R"({ "version": 4, "simTime": 600.0, "galaxy": {} })";
    }
    Factions::Load(DATA + "factions.json");
    REQUIRE(Archetypes::Load(DATA + "archetypes.json"));
    Simulation sim;
    sim.LoadUniverse(DATA + "universe.json");
    REQUIRE(sim.LoadWorld(path) == Save::Result::Ok);
    sim.MaterializeAllSystems(DATA + "systems/");
    CHECK(sim.Plans().empty());
    CHECK(sim.Chronicle().empty());

    Pass(sim);
    bool knowsHome = false;
    for (const auto& kv : sim.Systems())
        if (Holds(sim, FactionId::TradersGuild, kv.first))
        {
            CAPTURE(kv.first);
            CHECK(sim.MindOf(FactionId::TradersGuild).intel.count(kv.first) == 1);
            for (const std::string& n : sim.Neighbors(kv.first))
                CHECK(sim.MindOf(FactionId::TradersGuild).intel.count(n) == 1);
            knowsHome = true;
        }
    CHECK(knowsHome);

    // Written back, it is the current version and carries what they know.
    sim.SaveWorld(path);
    std::string text;
    {
        std::ifstream in(path);
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    std::remove(path.c_str());
    CHECK(text.find("\"version\": " + std::to_string(Save::WORLD_VERSION)) != std::string::npos);
    CHECK(text.find("\"factions\"") != std::string::npos);
    CHECK(Save::WORLD_VERSION >= 5);
}
