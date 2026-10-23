#include <doctest/doctest.h>

#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "entities/Ship.h"
#include "entities/ShipType.h"
#include "gen/Galaxy.h"
#include "gen/Names.h"
#include "gen/Region.h"
#include "sim/Names.h"
#include "sim/NamingRule.h"
#include "sim/Observation.h"
#include "sim/SaveSchema.h"
#include "sim/Simulation.h"
#include "temp_dir.h"
#include "factions_named.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <fstream>
#include <memory>
#include <set>
#include <string>

// Factions chart what they reach, and first contact (#431 slice 2;
// documents/world_knowledge_and_territory.md K2-K4, N1, decided after the rounds 1): whoever
// comes first from outside -- a pilot or a faction -- makes first contact; a faction's charts
// go public, "charted by" it, and a pilot is the discoverer only of what no faction reached.

namespace
{
const std::string DATA = TEST_DATA_DIR;
const std::string SYSTEMS = std::string(TEST_WORLD_DIR) + "systems/";

void LoadRegistries()
{
    REQUIRE(Factions::Load(DATA + "factions.json"));
    REQUIRE(Archetypes::Load(DATA + "archetypes.json"));
    REQUIRE(Blueprints::Load(DATA + "blueprints.json"));
}

// The hand-written systems and the region of seed 7, as the settling tests have them.
std::unique_ptr<Simulation> World(bool hot = false)
{
    SetRandomSeed(0xC0FFEEu);
    LoadRegistries();
    auto sim = std::make_unique<Simulation>();
    sim->LoadUniverse(std::string(TEST_WORLD_DIR) + "universe.json");
    sim->AttachRegion(7, SYSTEMS);
    sim->Seed(1234u);
    sim->InitGalaxy();
    sim->MaterializeAllSystems(SYSTEMS);
    sim->SetAllHot(hot);
    return sim;
}

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

// A system nobody holds that a gate leads to from one the Traders Guild holds.
std::string GuildFrontier(const Simulation& sim)
{
    for (const auto& kv : sim.Systems())
        if (Holds(sim, Fac::TradersGuild(), kv.first))
            for (const std::string& n : sim.Neighbors(kv.first))
                if (!sim.SystemById(n)->agg.claimed)
                    return n;
    return std::string();
}

bool Settling(const Simulation& sim, FactionId f)
{
    for (const auto& kv : sim.Plans())
        if (kv.second.faction == f && kv.second.kind == Plan::Kind::Settle)
            return true;
    return false;
}

// The Guild comes to its frontier in strength with the stock for an outpost, and lays one.
std::string GuildSettles(Simulation& sim)
{
    const std::string frontier = GuildFrontier(sim);
    REQUIRE_FALSE(frontier.empty());
    REQUIRE_FALSE(sim.OnPublicCharts(frontier));
    const float cap = Factions::TemperamentOf(Fac::TradersGuild()).capacity;
    for (int pass = 0; pass < 30 && !Settling(sim, Fac::TradersGuild()); pass++)
    {
        sim.SystemById(frontier)->agg.presence[Fac::TradersGuild()] = cap;
        sim.MindOf(Fac::TradersGuild()).stock = Simulation::OutpostCost();
        Pass(sim);
    }
    REQUIRE(Settling(sim, Fac::TradersGuild()));
    return frontier;
}

ClientSession& Pilot(Simulation& sim, const std::string& name, const std::string& at)
{
    ClientSession& s = sim.CreateSession(at, Vector2{ 0.0f, 0.0f }, GetShipCatalog()[0].stats);
    s.accountName = name;
    s.ship->SetPilotName(name);
    return s;
}

const WorldLoader::SystemInfo* In(const WorldLoader::Universe& u, const std::string& id)
{
    for (const auto& si : u.systems)
        if (si.id == id)
            return &si;
    return nullptr;
}

// What a system is called: the name it was given, else what the index calls it.
std::string NameOf(const Simulation& sim, const std::string& id)
{
    const SystemState* st = sim.SystemById(id);
    if (st != nullptr && !st->agg.givenName.empty())
        return st->agg.givenName;
    for (const WorldLoader::SystemInfo& info : sim.Universe().systems)
        if (info.id == id)
            return info.name;
    return id;
}

int Contacts(const Simulation& sim, const std::string& system)
{
    int n = 0;
    for (const ChronicleEntry& e : sim.Chronicle())
        n += e.kind == "contact" && e.system == system ? 1 : 0;
    return n;
}

// A generated galaxy with natives (#342 S5), as the natives' tests make one.
void Galaxy(Simulation& sim, uint64_t seed)
{
    LoadRegistries();
    Gen::WorldParams p;
    p.seed = seed;
    p.centres = 3;
    p.owners = Factions::MajorIds();
    p.pirates = Factions::Id(Factions::Pirates());
    p.unaffiliated = Factions::Id(Factions::Unaffiliated());
    REQUIRE(sim.OpenGalaxy(p, DATA, {}));
    sim.InitGalaxy();
    sim.MaterializeAllSystems(DATA + "systems/");
}

// The systems of the open frontiers that have a native name, region by region.
std::vector<std::string> NativeSystems(const Simulation& sim)
{
    std::vector<std::string> out;
    for (const Simulation::RegionOpening& o : sim.OpeningLog())
        if (o.parent >= 0)
            for (const std::string& id : sim.RegionSystems(o.slot))
                if (!sim.NativeNameOf(id).empty())
                    out.push_back(id);
    return out;
}
}  // namespace

TEST_CASE("a faction that settles charts the system for everyone, and the first pilot in "
          "found nothing (#431 slice 2)")
{
    auto               sim = World();
    const std::string  frontier = GuildSettles(*sim);
    const SystemState& st = *sim->SystemById(frontier);
    CHECK(sim->OnPublicCharts(frontier));
    CHECK(st.agg.chartedBy == Factions::Id(Fac::TradersGuild()));
    // An empty system a major is the first to chart takes a name in its style, and the
    // designation stays beside it; that it did is news.
    CHECK_FALSE(st.agg.givenName.empty());
    CHECK(NameOf(*sim, frontier) == st.agg.givenName);
    CHECK(Contacts(*sim, frontier) == 1);
    bool news = false;
    for (const std::string& line : sim->Events())
        news = news || (line.rfind("Traders Guild charted ", 0) == 0 &&
                        line.find(st.agg.givenName) != std::string::npos);
    CHECK(news);

    // Every pilot has it, by its new name, "charted by" the Guild.
    ClientSession&                 cy = Pilot(*sim, "cy", sim->Universe().startId);
    const WorldLoader::Universe    u = sim->KnownUniverseFor(cy);
    const WorldLoader::SystemInfo* si = In(u, frontier);
    REQUIRE(si != nullptr);
    CHECK(si->charted);
    CHECK(si->name == st.agg.givenName);
    CHECK_FALSE(si->designation.empty());
    CHECK(si->designation != si->name);
    CHECK(si->knownBy.empty());
    CHECK(Obs::ChartsOf(*si) == "charted by Traders Guild");

    // The first pilot in is no discoverer, and may not name it: nobody renames it.
    ClientSession& ann = Pilot(*sim, "ann", sim->Universe().startId);
    sim->ServerEnterSystem(ann, frontier, ann.systemId);
    CHECK(sim->SystemById(frontier)->agg.discoverer.empty());
    const std::string before = NameOf(*sim, frontier);
    CHECK_FALSE(sim->NameSystem(ann, "Annsworld"));
    CHECK(NameOf(*sim, frontier) == before);
    // ...and a second faction's charts change nothing.
    CHECK_FALSE(sim->ChartBy(frontier, Fac::Syndicate()));
    CHECK(sim->SystemById(frontier)->agg.chartedBy == Factions::Id(Fac::TradersGuild()));
    CHECK(Contacts(*sim, frontier) == 1);
}

TEST_CASE("what no faction reached is still a pilot's find, and theirs to name (#431 K3)")
{
    auto              sim = World();
    const std::string frontier = GuildFrontier(*sim);
    REQUIRE_FALSE(frontier.empty());
    ClientSession& ann = Pilot(*sim, "ann", sim->Universe().startId);
    sim->ServerEnterSystem(ann, frontier, ann.systemId);
    const SystemAggregate& a = sim->SystemById(frontier)->agg;
    CHECK(a.discoverer == "ann");
    CHECK_FALSE(sim->OnPublicCharts(frontier));
    CHECK(sim->Knows(ann, frontier));

    // A faction coming later puts it on the public charts but neither names it nor takes
    // the find: the discoverer keeps the right to name it (N2), and it was her news.
    CHECK(sim->ChartBy(frontier, Fac::TradersGuild()));
    CHECK(sim->OnPublicCharts(frontier));
    CHECK(a.givenName.empty());
    CHECK(a.discoverer == "ann");
    CHECK(Contacts(*sim, frontier) == 0);
    const WorldLoader::Universe    index = sim->KnownUniverse();  // alive while si is read
    const WorldLoader::SystemInfo* si = In(index, frontier);
    REQUIRE(si != nullptr);
    CHECK(Obs::ChartsOf(*si) == "charted by Traders Guild, found by ann");
    CHECK(sim->NameSystem(ann, "Annsworld"));
    CHECK(NameOf(*sim, frontier) == "Annsworld");
}

TEST_CASE("nobody charts a centre's space, nor natives their own home, for the centres (#431)")
{
    auto sim = World();
    CHECK_FALSE(sim->ChartBy(sim->Universe().startId, Fac::TradersGuild()));
    CHECK(sim->SystemById(sim->Universe().startId)->agg.chartedBy.empty());

    Simulation g;
    Galaxy(g, 4);
    const std::vector<std::string> homes = NativeSystems(g);
    REQUIRE_FALSE(homes.empty());
    const FactionId natives = g.NativesOf(Gen::SlotOf(homes.front()));
    REQUIRE(natives != Factions::Unaffiliated());
    CHECK_FALSE(g.ChartBy(homes.front(), natives));
    CHECK_FALSE(g.OnPublicCharts(homes.front()));
}

TEST_CASE("first contact reveals the natives' name, whoever makes it (#431 K4, N1)")
{
    Simulation sim;
    Galaxy(sim, 4);
    const std::vector<std::string> homes = NativeSystems(sim);
    REQUIRE(homes.size() >= 2);
    const std::string byPilot = homes[0], byFaction = homes[1];
    const std::string pilotName = sim.NativeNameOf(byPilot);
    const std::string factionName = sim.NativeNameOf(byFaction);

    // Before contact: a designation, to whoever has a gate to it on their map.
    const std::string start = sim.Universe().startId;
    const std::string pilotDesig = NameOf(sim, byPilot);
    const std::string factionDesig = NameOf(sim, byFaction);
    ClientSession&    ann = Pilot(sim, "ann", start);
    CHECK(NameOf(sim, byPilot) != pilotName);
    CHECK(sim.SystemById(byPilot)->agg.givenName.empty());

    // A pilot first in: hers to know, by its people's name, and news.
    sim.ServerEnterSystem(ann, byPilot, ann.systemId);
    const SystemAggregate& a = sim.SystemById(byPilot)->agg;
    CHECK(a.discoverer == "ann");
    CHECK(a.givenName == pilotName);
    CHECK_FALSE(sim.OnPublicCharts(byPilot));
    const WorldLoader::Universe    annIndex = sim.KnownUniverseFor(ann);  // alive while read
    const WorldLoader::SystemInfo* si = In(annIndex, byPilot);
    REQUIRE(si != nullptr);
    CHECK(si->name == pilotName);
    CHECK(si->designation == pilotDesig);
    CHECK(sim.Events().back().find("its people call it " + pilotName) != std::string::npos);
    // Named by its people: not hers to name.
    CHECK_FALSE(sim.NameSystem(ann, "Annsworld"));
    CHECK(NameOf(sim, byPilot) == pilotName);

    // A faction first in: public, by its people's name, never the faction's own.
    const FactionId major = Fac::TradersGuild();
    CHECK(sim.ChartBy(byFaction, major));
    CHECK(sim.OnPublicCharts(byFaction));
    CHECK(sim.SystemById(byFaction)->agg.givenName == factionName);
    CHECK(sim.SystemById(byFaction)->agg.discoverer.empty());
    CHECK(sim.Chronicle().back().kind == "contact");
    CHECK(sim.Chronicle().back().text == "Traders Guild made first contact in " + factionDesig +
                                             ": its people call it " + factionName);
    ClientSession&              bo = Pilot(sim, "bo", start);
    const WorldLoader::Universe boIndex = sim.KnownUniverseFor(bo);
    si = In(boIndex, byFaction);
    REQUIRE(si != nullptr);
    CHECK(si->name == factionName);
    CHECK(Obs::ChartsOf(*si) == "charted by Traders Guild");
    sim.ServerEnterSystem(bo, byFaction, bo.systemId);
    CHECK(sim.SystemById(byFaction)->agg.discoverer.empty());
}

TEST_CASE("a major names an empty system it charts first in its own style (#431)")
{
    auto              sim = World();
    const std::string frontier = GuildFrontier(*sim);
    REQUIRE_FALSE(frontier.empty());
    std::set<std::string> taken;  // every name in the world, folded (N3)
    for (const WorldLoader::SystemInfo& info : sim->Universe().systems)
    {
        taken.insert(Gen::FoldName(info.name));
        taken.insert(Gen::FoldName(info.id));
    }
    const std::string expect =
        Gen::ChartedName(Fac::Syndicate(), sim->RegionSeed(), frontier, taken);
    REQUIRE_FALSE(expect.empty());
    CHECK(sim->ChartBy(frontier, Fac::Syndicate()));
    CHECK(sim->SystemById(frontier)->agg.givenName == expect);

    // A faction with no naming culture leaves the designation, and nobody may name it.
    const std::string other = [&]
    {
        for (const auto& kv : sim->Systems())
            if (kv.first != frontier && !sim->OnPublicCharts(kv.first) &&
                kv.second.agg.discoverer.empty())
                return kv.first;
        return std::string();
    }();
    REQUIRE_FALSE(other.empty());
    REQUIRE(Factions::Def(Fac::Independent()).culture.empty());
    CHECK(sim->ChartBy(other, Fac::Independent()));
    CHECK(sim->SystemById(other)->agg.givenName.empty());
    ClientSession& ann = Pilot(*sim, "ann", sim->Universe().startId);
    sim->ServerEnterSystem(ann, other, ann.systemId);
    CHECK_FALSE(sim->NameSystem(ann, "Annsworld"));
    CHECK(sim->SystemById(other)->agg.givenName.empty());
}

TEST_CASE("a cold system is charted as a hot one is (#431 slice 2)")
{
    // One world at a time: a world's objects point into the registries the next one reloads.
    struct Charted
    {
        std::string system, by, name;
    };
    auto run = [](bool hot)
    {
        auto              sim = World(hot);
        const std::string at = GuildSettles(*sim);
        return Charted{ at, sim->SystemById(at)->agg.chartedBy,
                        sim->SystemById(at)->agg.givenName };
    };
    const Charted cold = run(false);
    const Charted hot = run(true);
    CHECK(cold.system == hot.system);
    CHECK(cold.by == Factions::Id(Fac::TradersGuild()));
    CHECK(cold.by == hot.by);
    CHECK_FALSE(cold.name.empty());
    CHECK(cold.name == hot.name);
}

TEST_CASE("charts and names survive a save; contact is not made again on load (#431)")
{
    const TempDir     tmp;
    const std::string world = tmp.Path("world.json");
    std::string       frontier, name;
    int               contacts = 0;
    {
        auto sim = World();
        frontier = GuildSettles(*sim);
        name = sim->SystemById(frontier)->agg.givenName;
        REQUIRE_FALSE(name.empty());
        contacts = Contacts(*sim, frontier);
        REQUIRE(sim->SaveWorld(world));
    }
    {
        Simulation sim;
        sim.LoadUniverse(std::string(TEST_WORLD_DIR) + "universe.json");
        sim.AttachRegion(7, SYSTEMS);
        REQUIRE(sim.LoadWorld(world) == Save::Result::Ok);
        CHECK(sim.OnPublicCharts(frontier));
        CHECK(sim.SystemById(frontier)->agg.chartedBy == Factions::Id(Fac::TradersGuild()));
        CHECK(NameOf(sim, frontier) == name);
        CHECK(Contacts(sim, frontier) == contacts);  // the history's own, not a new one
    }

    // A world from before factions charted (version 16): a system a faction held beyond
    // the wormhole with nobody's charts is charted by its holder -- the playtest's bug.
    {
        std::ifstream  in(world);
        nlohmann::json j = nlohmann::json::parse(in);
        j["version"] = 16;
        j["galaxy"][frontier].erase("chartedBy");
        j["galaxy"][frontier]["claimed"] = true;  // as its outpost, finished, would have it
        j["galaxy"][frontier]["controller"] = Factions::Id(Fac::TradersGuild());
        j["galaxy"][frontier]["name"] = "";
        std::ofstream(world) << j.dump();
    }
    Simulation sim;
    sim.LoadUniverse(std::string(TEST_WORLD_DIR) + "universe.json");
    sim.AttachRegion(7, SYSTEMS);
    REQUIRE(sim.LoadWorld(world) == Save::Result::Ok);
    CHECK(sim.OnPublicCharts(frontier));
    CHECK(sim.SystemById(frontier)->agg.chartedBy == Factions::Id(Fac::TradersGuild()));
    CHECK(sim.SystemById(frontier)->agg.givenName.empty());  // no name it was not given
}

// --- Naming rules (#431 slice 4; N2, N3) ------------------------------------------------------

namespace
{
// What the journal said last: where a refusal says which rule failed.
std::string LastSaid(const ClientSession& s)
{
    const std::vector<Ev::Event> all = s.EventsSince(0);
    return all.empty() ? std::string() : all.back().text;
}

// A frontier nobody reached, found by `who`.
std::string Found(Simulation& sim, ClientSession& who)
{
    const std::string frontier = GuildFrontier(sim);
    REQUIRE_FALSE(frontier.empty());
    sim.ServerEnterSystem(who, frontier, who.systemId);
    REQUIRE(sim.SystemById(frontier)->agg.discoverer == who.accountName);
    return frontier;
}
}  // namespace

TEST_CASE("only the discoverer names a system, never whoever comes or holds it later (#431 N2)")
{
    auto              sim = World();
    ClientSession&    ann = Pilot(*sim, "ann", sim->Universe().startId);
    const std::string frontier = Found(*sim, ann);
    ClientSession&    bo = Pilot(*sim, "bo", sim->Universe().startId);
    sim->ServerEnterSystem(bo, frontier, bo.systemId);
    // Even holding it: an owner gets the tax and the docks, not the history.
    sim->SystemById(frontier)->agg.holderCorp = "boco";
    CHECK_FALSE(sim->NameSystem(bo, "Bo's Rest"));
    CHECK(LastSaid(bo) == "Not named: only ann, who got here first, may name it");
    CHECK(sim->SystemById(frontier)->agg.givenName.empty());
    CHECK(sim->NameSystem(ann, "Annsworld"));
}

TEST_CASE("a name keeps the rules, and a refusal says which one (#431 N3)")
{
    auto              sim = World();
    ClientSession&    ann = Pilot(*sim, "ann", sim->Universe().startId);
    const std::string frontier = Found(*sim, ann);

    CHECK_FALSE(sim->NameSystem(ann, "Ab"));
    CHECK(LastSaid(ann) == "Not named: a name is 3 to 24 characters");
    CHECK_FALSE(sim->NameSystem(ann, "A Name Far Too Long For Any Map"));
    CHECK(LastSaid(ann) == "Not named: a name is 3 to 24 characters");
    CHECK_FALSE(sim->NameSystem(ann, "Haven!"));
    CHECK(LastSaid(ann) == "Not named: letters, digits, spaces, ' and - only");
    CHECK_FALSE(sim->NameSystem(ann, "-Haven"));
    CHECK(LastSaid(ann) == "Not named: a name starts with a letter");

    // The word filter: built in, and the server's own list on top.
    CHECK_FALSE(sim->NameSystem(ann, "Shit Creek"));
    CHECK(LastSaid(ann) == "Not named: it holds a word the server's filter refuses");
    // ...a word as a word, so a name that only spells one inside it is a name (Scunthorpe).
    CHECK(sim->NameProblem("Parsec").empty());
    CHECK(sim->NameProblem("Torpedo Run").empty());
    CHECK(sim->NameProblem("Peacock").empty());
    CHECK(sim->NameProblem("Essex Deep").empty());
    CHECK_FALSE(sim->NameProblem("Fuckton").empty());  // the few that no word holds innocently
    CHECK(sim->NameProblem("Badword Bay").empty());
    sim->SetBlockedNames({ "badword", "*vile", "rival corp" });
    CHECK_FALSE(sim->NameSystem(ann, "Badword Bay"));  // a listed word
    CHECK(LastSaid(ann) == "Not named: it holds a word the server's filter refuses");
    CHECK_FALSE(sim->NameProblem("Two-Badwords").empty());     // ...hyphenated, plural
    CHECK(sim->NameProblem("Badwordia").empty());              // ...not inside another word
    CHECK_FALSE(sim->NameProblem("Revileton").empty());        // `*`: anywhere
    CHECK_FALSE(sim->NameProblem("Rival Corp Hold").empty());  // an entry of two words
    CHECK(sim->NameProblem("Rival Hold").empty());

    // Unique in the world, whatever the case: another system's name or designation.
    std::string other, why;
    for (const WorldLoader::SystemInfo& si : sim->Universe().systems)
        if (si.id != frontier && other.empty() && Names::AllowedSystemName(si.name, {}, why))
            other = si.name;
    REQUIRE_FALSE(other.empty());
    std::string shouted = other;
    for (char& c : shouted)
        c = (char)std::toupper((unsigned char)c);
    CHECK_FALSE(sim->NameSystem(ann, shouted));
    CHECK(LastSaid(ann) == "Not named: there is already a system called " + shouted);
    CHECK(sim->SystemById(frontier)->agg.givenName.empty());
    CHECK(sim->NameSystem(ann, "Annsworld"));
}

TEST_CASE("a name natives will give is taken before anyone meets them (#431 N1, N3)")
{
    Simulation sim;
    Galaxy(sim, 4);
    const std::vector<std::string> homes = NativeSystems(sim);
    REQUIRE_FALSE(homes.empty());
    const std::string native = sim.NativeNameOf(homes[0]);
    REQUIRE(sim.SystemById(homes[0])->agg.givenName.empty());  // nobody has met them
    std::string lower = native;
    for (char& c : lower)
        c = (char)std::tolower((unsigned char)c);
    CHECK(sim.NameProblem(lower) == "there is already a system called " + lower);
}

TEST_CASE("an admin renames or clears a name; it is news, and the discoverer is told (#431 N3)")
{
    const TempDir     tmp;
    const std::string world = tmp.Path("world.json");
    std::string       frontier;
    {
        auto           sim = World();
        ClientSession& ann = Pilot(*sim, "ann", sim->Universe().startId);
        frontier = Found(*sim, ann);
        REQUIRE(sim->NameSystem(ann, "Rudeworld"));
        ClientSession& cy = Pilot(*sim, "cy", sim->Universe().startId);
        sim->ChartBy(frontier, Fac::TradersGuild());  // on everyone's map

        // By its name, in any case; the new one keeps the rules.
        std::string said;
        CHECK_FALSE(sim->RenameSystem(frontier, "Shit Creek", said));
        CHECK(said == "it holds a word the server's filter refuses");
        CHECK_FALSE(sim->RenameSystem("nowhere-at-all", "Calm", said));
        CHECK(sim->RenameSystem("rudeworld", "Calm Harbour", said));
        CHECK(said == "Rudeworld is now Calm Harbour");
        CHECK(sim->SystemById(frontier)->agg.givenName == "Calm Harbour");
        CHECK(sim->Chronicle().back().kind == "renamed");
        CHECK(sim->Chronicle().back().system == frontier);
        CHECK(sim->Events().back().find("renamed Rudeworld Calm Harbour") != std::string::npos);
        CHECK(LastSaid(ann).find("renamed Rudeworld, which you found, Calm Harbour") !=
              std::string::npos);
        // Every map that has it shows the new name.
        const WorldLoader::Universe    index = sim->KnownUniverseFor(cy);  // alive while read
        const WorldLoader::SystemInfo* si = In(index, frontier);
        REQUIRE(si != nullptr);
        CHECK(si->name == "Calm Harbour");
        // The discoverer may not overrule it.
        CHECK_FALSE(sim->NameSystem(ann, "Rudeworld"));

        // Known space is the world's, not a moderator's.
        CHECK_FALSE(sim->RenameSystem(sim->Universe().startId, "Mine", said));

        // Cleared while she is away: its designation again, hers to name, told at login.
        sim->DestroySession(ann.id);
        sim->TakeAbsentEvents();
        CHECK(sim->RenameSystem(frontier, "-", said));
        CHECK(sim->SystemById(frontier)->agg.givenName.empty());
        CHECK(NameOf(*sim, frontier) != "Calm Harbour");
        CHECK_FALSE(sim->RenameSystem(frontier, "-", said));  // nothing left to clear
        bool told = false;
        for (const auto& kv : sim->TakeAbsentEvents())
            told = told || (kv.first == "ann" &&
                            kv.second.text.find("you may name it again") != std::string::npos);
        CHECK(told);
        ClientSession& back = Pilot(*sim, "ann", sim->Universe().startId);
        sim->ServerEnterSystem(back, frontier, back.systemId);
        CHECK(sim->NameSystem(back, "Second Thought"));
        CHECK(sim->RenameSystem(frontier, "Final Word", said));
        REQUIRE(sim->SaveWorld(world));
    }
    // A moderator's name is world state like any other.
    Simulation sim;
    sim.LoadUniverse(std::string(TEST_WORLD_DIR) + "universe.json");
    sim.AttachRegion(7, SYSTEMS);
    REQUIRE(sim.LoadWorld(world) == Save::Result::Ok);
    CHECK(sim.SystemById(frontier)->agg.givenName == "Final Word");
    CHECK(sim.SystemById(frontier)->agg.discoverer == "ann");
}

TEST_CASE("the discoverer names a system from anywhere, and the journal says how (#513)")
{
    auto              sim = World();
    ClientSession&    ann = Pilot(*sim, "ann", sim->Universe().startId);
    const std::string frontier = Found(*sim, ann);
    // The discovery says where to name it.
    bool told = false;
    for (const Ev::Event& e : ann.EventsSince(0))
        told = told || e.text.find("you may name it (map") != std::string::npos;
    CHECK(told);

    // She leaves: the ship is nowhere near, and the name is still hers to give.
    sim->ServerEnterSystem(ann, sim->Universe().startId, frontier);
    REQUIRE(ann.systemId != frontier);
    CHECK(sim->NamingRefusal(ann, frontier, "Annsworld").empty());
    CHECK_FALSE(sim->NameSystem(ann, frontier, "Ab"));  // a bad name is refused, with why
    CHECK(sim->NamingRefusal(ann, frontier, "Ab") == "a name is 3 to 24 characters");
    CHECK(LastSaid(ann) == "Not named: a name is 3 to 24 characters");
    CHECK(sim->NameSystem(ann, frontier, "Annsworld"));
    CHECK(NameOf(*sim, frontier) == "Annsworld");
    // Once: a second name is refused with the name it has.
    CHECK(sim->NamingRefusal(ann, frontier, "Other Name") == "Annsworld already has its name");
}

TEST_CASE("a refusal to name says why, whoever asks and wherever they are (#513)")
{
    auto              sim = World();
    ClientSession&    ann = Pilot(*sim, "ann", sim->Universe().startId);
    ClientSession&    bo = Pilot(*sim, "bo", sim->Universe().startId);
    const std::string frontier = Found(*sim, ann);

    // Not the discoverer, from the other end of the map.
    CHECK(sim->NamingRefusal(bo, frontier, "Bo's Rest") ==
          "only ann, who got here first, may name it");
    CHECK_FALSE(sim->NameSystem(bo, frontier, "Bo's Rest"));
    CHECK(sim->SystemById(frontier)->agg.givenName.empty());
    // The same words, from what the index tells a client.
    {
        const WorldLoader::Universe    index = sim->KnownUniverseFor(ann);
        const WorldLoader::SystemInfo* si = In(index, frontier);
        REQUIRE(si != nullptr);
        CHECK(Names::NamingRefusal(*si, "ann").empty());
        CHECK(Names::NamingRefusal(*si, "bo") == "only ann, who got here first, may name it");
    }

    const std::string centre = sim->Universe().startId;

    // A faction came first, and gave it nothing: the faction's, not a pilot's.
    std::string other;
    for (const std::string& n : sim->Neighbors(frontier))
        if (n != centre && sim->SystemById(n) != nullptr &&
            sim->SystemById(n)->agg.discoverer.empty() && !sim->SystemById(n)->agg.publicChart &&
            sim->SystemById(n)->agg.givenName.empty())
            other = n;
    if (!other.empty())
    {
        sim->SystemById(other)->agg.chartedBy = Factions::Id(Fac::Syndicate());
        CHECK(sim->NamingRefusal(ann, other, "Annsworld") == "Syndicate charted it first");
    }

    // Not a system there is.
    CHECK(sim->NamingRefusal(ann, "no-such-system", "Annsworld") ==
          "only a system beyond the wormhole can be named");
}

TEST_CASE("a name that is its people's cannot be given, and the index says so (#513, N1)")
{
    Simulation sim;
    Galaxy(sim, 4);
    const std::vector<std::string> homes = NativeSystems(sim);
    REQUIRE_FALSE(homes.empty());
    ClientSession& ann = Pilot(sim, "ann", sim.Universe().startId);
    sim.ServerEnterSystem(ann, homes[0], ann.systemId);
    const std::string native = sim.NativeNameOf(homes[0]);
    CHECK(sim.NamingRefusal(ann, homes[0], "Annsworld") == "its people call it " + native);
    // A centre is named long before anyone is born.
    const std::string centre = sim.Starts().front().systemId;
    CHECK(sim.NamingRefusal(ann, centre, "Annsworld") ==
          sim.SystemName(centre) + " was named long before you were born");
    const WorldLoader::Universe    index = sim.KnownUniverseFor(ann);
    const WorldLoader::SystemInfo* si = In(index, homes[0]);
    REQUIRE(si != nullptr);
    CHECK(si->nativeNamed);
    CHECK(Names::NamingRefusal(*si, "ann") == "its people call it " + native);
    // ...and it survives the wire.
    WorldLoader::Universe back;
    REQUIRE(Proto::DecodeUniverse(Proto::EncodeUniverse(index), back));
    const WorldLoader::SystemInfo* sb = In(back, homes[0]);
    REQUIRE(sb != nullptr);
    CHECK(sb->nativeNamed);
}
