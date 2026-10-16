#include <doctest/doctest.h>

#include "goods_named.h"
#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "economy/Market.h"
#include "entities/Station.h"
#include "gen/Galaxy.h"
#include "gen/Region.h"
#include "sim/Protocol.h"
#include "sim/SaveSchema.h"
#include "sim/Simulation.h"
#include "sim/WorldRules.h"

#include "temp_dir.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

// Natives and frontier prices (#342 S5): every frontier region has a minor faction of its own,
// with stations that trade, and a market's prices follow its distance from a centre.

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

// A frontier slot's region, opened from nowhere in particular, as the editor shows one.
Gen::Region Frontier(const Gen::GalaxySkeleton& g, int slot)
{
    Gen::RegionParams p = Gen::SlotParams(g, slot);
    p.homeId = "home";
    return Gen::GenerateRegion(p);
}

// The first frontier the world began with.
int FirstFrontier(const Simulation& sim)
{
    for (const Simulation::RegionOpening& o : sim.OpeningLog())
        if (o.parent >= 0)
            return o.slot;
    return -1;
}

// The natives' stations in an open region, as built.
std::vector<const Station*> NativeStations(const Simulation& sim, int slot, FactionId natives)
{
    std::vector<const Station*> out;
    for (const std::string& id : sim.RegionSystems(slot))
        if (const SystemState* st = sim.SystemById(id))
            for (const auto& e : st->entities)
                if (e->GetKind() == EntityKind::Station &&
                    static_cast<const Station&>(*e).GetFaction() == natives)
                    out.push_back(static_cast<const Station*>(e.get()));
    return out;
}

std::string ReadText(const std::string& path)
{
    std::ifstream in(path);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
}  // namespace

TEST_CASE("every frontier region has natives of its own, made from the seed and slot (#342 S5)")
{
    LoadRegistries();
    const Gen::GalaxySkeleton g = Gen::GenerateGalaxySkeleton(Params(4));
    std::set<std::string>     ids, themes;
    int                       frontiers = 0;
    for (const Gen::Slot& slot : g.slots)
    {
        if (frontiers >= 40)
            break;
        const Gen::Region r = Frontier(g, slot.index);
        if (slot.kind == Gen::SlotKind::Centre)
        {
            CHECK(r.natives.empty());  // a centre's people are the majors
            continue;
        }
        frontiers++;
        CAPTURE(slot.index);
        REQUIRE_FALSE(r.natives.empty());
        // The same slot makes the same natives, whenever it is made.
        CHECK(Frontier(g, slot.index).natives == r.natives);
        const std::string id = r.natives.value("id", std::string());
        CHECK(id.rfind("native-", 0) == 0);
        ids.insert(id);
        themes.insert(r.natives.value("theme", std::string()));
        CHECK_FALSE(r.natives.value("name", std::string()).empty());

        // One to three stations, each in a system of theirs that the index says they hold.
        int stations = 0;
        for (const auto& kv : r.documents)
            for (const nlohmann::json& st : kv.second.value("stations", nlohmann::json::array()))
            {
                CHECK(st.value("faction", std::string()) == id);
                StationRole role;
                CHECK(ParseStationRole(st.value("role", std::string()), role));
                CHECK(st.contains("pos"));
                stations++;
                bool held = false;
                for (const nlohmann::json& s : r.systems)
                    held = held || (s.value("id", std::string()) == kv.first &&
                                    s.value("owner", std::string()) == id);
                CHECK(held);
            }
        CHECK(stations >= 1);
        CHECK(stations <= 3);
        CHECK(stations == (int)r.natives["systems"].size());

        // Sealed as they are: the region's document carries them.
        Gen::Region back;
        REQUIRE(Gen::RegionFromJson(Gen::RegionToJson(r), back));
        CHECK(back.natives == r.natives);
    }
    CHECK(frontiers == 40);
    CHECK(ids.size() == 40);    // an id per region: a hash of (seed, slot)
    CHECK(themes.size() >= 3);  // and not one kind of people everywhere
    // Another world's slot is another people.
    CHECK(Frontier(Gen::GenerateGalaxySkeleton(Params(5)), 7).natives.value("id", "") !=
          Frontier(g, 7).natives.value("id", ""));
}

TEST_CASE("natives are in the registry, hold their systems and trade from the first minute "
          "(#342 S5)")
{
    LoadRegistries();
    Simulation sim;
    Make(sim, 9);
    const int frontier = FirstFrontier(sim);
    REQUIRE(frontier >= 0);
    const FactionId natives = sim.NativesOf(frontier);
    REQUIRE(natives != Factions::Unaffiliated());
    CHECK(Factions::Kind(natives) == "Native");
    CHECK(Factions::Id(natives).rfind("native-", 0) == 0);
    CHECK(FactionName(natives).find("native-") == std::string::npos);  // a name, not the id
    CHECK(sim.NativesOf(sim.Skeleton().centres.front()) == Factions::Unaffiliated());

    const std::vector<const Station*> stations = NativeStations(sim, frontier, natives);
    REQUIRE_FALSE(stations.empty());
    for (const Station* st : stations)
    {
        CHECK(st->Has(Component::Dockable));
        CHECK(st->Has(Component::Market));
    }
    // Theirs, held, with a garrison and traffic in the cold aggregate before anyone comes.
    for (const std::string& id : sim.RegionSystems(frontier))
    {
        const SystemState& st = sim.Systems().at(id);
        if (st.agg.controller != natives)
            continue;
        CHECK(st.agg.claimed);
        CHECK(st.agg.presence[natives] > 0.0f);
        CHECK(st.agg.traders > 0.0f);
        CHECK(st.profile.stations >= 1);
    }
}

TEST_CASE("natives are sealed with their region, restored on load and back from the archive "
          "(#342 S5)")
{
    LoadRegistries();
    TempDir           dir;
    const std::string seal = dir.MakeDir("regions");
    const std::string path = dir.Path("world.json");
    Simulation        a;
    Make(a, 9, seal);
    const int frontier = FirstFrontier(a);
    REQUIRE(frontier >= 0);
    const std::string id = Factions::Id(a.NativesOf(frontier));
    const std::string name = FactionName(a.NativesOf(frontier));
    REQUIRE(id.rfind("native-", 0) == 0);
    CHECK(ReadText(Simulation::SealedPath(seal, frontier)).find(id) != std::string::npos);
    REQUIRE(a.SaveWorld(path));

    // A new process: the registry is factions.json alone until the regions are read back.
    REQUIRE(Factions::Load(DATA + "factions.json"));
    FactionId none;
    REQUIRE_FALSE(Factions::Find(id, none));
    Simulation::WorldHeader h;
    REQUIRE(Simulation::ReadWorldHeader(path, h));
    Simulation b;
    b.SetSealDir(seal);
    REQUIRE(b.OpenGalaxy(Params(9), DATA, h.log));
    REQUIRE(b.LoadWorld(path) == Save::Result::Ok);
    b.MaterializeAllSystems(DATA + "systems/");
    REQUIRE(Factions::Id(b.NativesOf(frontier)) == id);
    CHECK(FactionName(b.NativesOf(frontier)) == name);
    CHECK_FALSE(NativeStations(b, frontier, b.NativesOf(frontier)).empty());

    // Drifted away into the archive, then taken back by the next slot its parent opens: the
    // same people, under the same id, where their stations stood as hulks.
    WorldRules::Archive rules = WorldRules::ArchiveRules(), keep = rules;
    rules.returnChance = 1.0;
    WorldRules::SetArchiveRules(rules);
    const int   parent = b.OpeningOf(frontier)->parent;
    std::string why;
    REQUIRE(b.ArchiveRegionNow(frontier, why) >= 0);
    CHECK(b.NativesOf(frontier) == Factions::Unaffiliated());
    REQUIRE(Factions::Load(DATA + "factions.json"));  // ...and a restart in between
    const int slot = b.OpenMouthNow(parent, why);
    WorldRules::SetArchiveRules(keep);
    REQUIRE(slot >= 0);
    REQUIRE(b.OpeningOf(slot)->archive >= 0);
    CHECK(Factions::Id(b.NativesOf(slot)) == id);
    CHECK(FactionName(b.NativesOf(slot)) == name);
}

TEST_CASE("a market's prices follow its distance from a centre (#342 S5)")
{
    // The rule: what is dug up where it is sold is cheaper the further out, what is carried
    // there is dearer, and nothing changes at a centre.
    CHECK(RemotenessFactor(Supply::Local, 0) == 1.0);
    CHECK(RemotenessFactor(Supply::Imported, 0) == 1.0);
    CHECK(RemotenessFactor(Supply::Local, -1) == 1.0);  // no centre reaches it
    for (int j = 1; j < 40; j++)
    {
        CHECK(RemotenessFactor(Supply::Local, j) <= RemotenessFactor(Supply::Local, j - 1));
        CHECK(RemotenessFactor(Supply::Imported, j) >= RemotenessFactor(Supply::Imported, j - 1));
    }
    CHECK(RemotenessFactor(Supply::Local, 5) < 1.0);
    CHECK(RemotenessFactor(Supply::Imported, 5) > 1.0);
    CHECK(RemotenessFactor(Supply::Local, 1000) >= 0.55);
    CHECK(RemotenessFactor(Supply::Imported, 1000) <= 2.0);
    CHECK(SupplyOf(Good::Iron()) == Supply::Local);

    // A market keeps how far a sale pushed it when its base moves.
    Market m;
    m.Sell(Good::Iron(), 10);
    const double sagged = m.GetPrice(Good::Iron()) / m.GetBasePrice(Good::Iron());
    m.SetBasePrice(Good::Iron(), 6.0);
    CHECK(m.GetBasePrice(Good::Iron()) == 6.0);
    CHECK(m.GetPrice(Good::Iron()) / 6.0 == doctest::Approx(sagged));

    // In a world: a centre pays the centre's price, the natives' frontier less for ore, and
    // the further along the route, the less.
    LoadRegistries();
    Simulation sim;
    Make(sim, 9);
    const std::string heart = sim.Starts().front().systemId;
    CHECK(sim.Remoteness(heart) == 0);
    CHECK(sim.Systems().at(heart).market.GetBasePrice(Good::Iron()) ==
          Market::DefaultBasePrice(Good::Iron()));
    const int frontier = FirstFrontier(sim);
    REQUIRE(frontier >= 0);
    std::string nearest, furthest;
    for (const std::string& id : sim.RegionSystems(frontier))
    {
        const int j = sim.Remoteness(id);
        CAPTURE(id);
        CHECK(j > 0);
        CHECK(sim.Systems().at(id).market.GetBasePrice(Good::Iron()) ==
              doctest::Approx(Market::DefaultBasePrice(Good::Iron()) *
                              RemotenessFactor(Supply::Local, j)));
        if (nearest.empty() || j < sim.Remoteness(nearest))
            nearest = id;
        if (furthest.empty() || j > sim.Remoteness(furthest))
            furthest = id;
    }
    REQUIRE(sim.Remoteness(furthest) > sim.Remoteness(nearest));
    CHECK(sim.Systems().at(furthest).market.GetPrice(Good::Crystal()) <
          sim.Systems().at(nearest).market.GetPrice(Good::Crystal()));
    CHECK(sim.Systems().at(nearest).market.GetPrice(Good::Crystal()) <
          sim.Systems().at(heart).market.GetPrice(Good::Crystal()));
}

TEST_CASE("the server's registry reaches a client, natives by name (#342 S5)")
{
    LoadRegistries();
    Simulation sim;
    Make(sim, 9);
    const FactionId natives = sim.NativesOf(FirstFrontier(sim));
    REQUIRE(natives != Factions::Unaffiliated());
    const std::string id = Factions::Id(natives), name = FactionName(natives);
    const Stance      toPirates = Factions::Relation(natives, Factions::Pirates());
    const std::string wire = Proto::EncodeFactions(Proto::RegistryMessage());

    // What a station of theirs looks like on the wire, to a client that has not been told.
    Proto::SystemLayout lay;
    lay.systemId = "r1-1-1";
    Proto::EntityLayout station;
    station.id = 7;
    station.kind = EntityKind::Station;
    station.name = "Their Post";
    station.faction = natives;
    lay.entities.push_back(station);
    const std::string layout = Proto::EncodeLayout(lay);

    // A client process: its own factions.json, and nothing else.
    REQUIRE(Factions::Load(DATA + "factions.json"));
    Proto::SystemLayout before;
    REQUIRE(Proto::DecodeLayout(layout, before));
    REQUIRE(before.entities.size() == 1);
    CHECK(before.entities[0].faction == Factions::Unaffiliated());  // what #355 left

    Proto::FactionList list;
    REQUIRE(Proto::DecodeFactions(wire, list));
    Proto::ApplyFactions(list);
    FactionId known;
    REQUIRE(Factions::Find(id, known));
    CHECK(FactionName(known) == name);
    CHECK(Factions::Kind(known) == "Native");
    CHECK(Factions::Relation(known, Factions::Pirates()) == toPirates);
    Proto::SystemLayout after;
    REQUIRE(Proto::DecodeLayout(layout, after));
    CHECK(after.entities[0].faction == known);
    // Told twice, nothing doubles.
    const int count = Factions::Count();
    Proto::ApplyFactions(list);
    CHECK(Factions::Count() == count);
    // An older peer's message is refused, not misread.
    nlohmann::json old = nlohmann::json::parse(wire);
    old["v"] = Proto::PROTO_VERSION - 1;
    CHECK_FALSE(Proto::DecodeFactions(old.dump(), list));
}
