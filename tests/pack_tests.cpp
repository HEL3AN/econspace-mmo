#include <doctest/doctest.h>

#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "entities/JumpGate.h"
#include "gen/Galaxy.h"
#include "gen/Pack.h"
#include "gen/Region.h"
#include "sim/SaveSchema.h"
#include "sim/Simulation.h"
#include "sim/WorldRules.h"

#include "temp_dir.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

// Admin packs (#342 S7): hand-made regions and systems, checked line by line, woven into a
// new world by its seed and the pack's id, and kept as they were woven when the files change.

namespace
{
using nlohmann::json;

const std::string DATA = TEST_DATA_DIR;
const std::string PACKS = TEST_PACKS_DIR;

void LoadRegistries()
{
    REQUIRE(Factions::Load(DATA + "factions.json"));
    REQUIRE(Archetypes::Load(DATA + "archetypes.json"));
    REQUIRE(Blueprints::Load(DATA + "blueprints.json"));
    REQUIRE(WorldRules::Load(DATA + "world_rules.json"));
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

std::shared_ptr<const Gen::Pack> Fixture(const std::string& id)
{
    Gen::Pack                p;
    std::vector<std::string> problems;
    const bool               ok = Gen::LoadPack(PACKS + id, p, problems);
    for (const std::string& why : problems)
        MESSAGE(why);
    REQUIRE(ok);
    return std::make_shared<const Gen::Pack>(std::move(p));
}

std::vector<std::shared_ptr<const Gen::Pack>> Both()
{
    return { Fixture("helios"), Fixture("relay") };
}

void Make(Simulation& sim, uint64_t seed, std::vector<std::shared_ptr<const Gen::Pack>> packs,
          const std::string& seal = std::string(), const Simulation::WorldHeader* saved = nullptr)
{
    sim.SetSealDir(seal);
    sim.SetPacks(std::move(packs), saved != nullptr ? saved->packs : std::vector<Gen::Woven>{});
    REQUIRE(
        sim.OpenGalaxy(Params(seed), DATA,
                       saved != nullptr ? saved->log : std::vector<Simulation::RegionOpening>{}));
}

const Gen::Woven* WovenOf(const Simulation& sim, const std::string& id)
{
    for (const Gen::Woven& w : sim.WovenPacks())
        if (w.id == id)
            return &w;
    return nullptr;
}

const WorldLoader::SystemInfo* Info(const Simulation& sim, const std::string& id)
{
    for (const auto& s : sim.Universe().systems)
        if (s.id == id)
            return &s;
    return nullptr;
}

// The admin links of a region pack's slot: links with one end in it and one outside.
std::vector<std::pair<std::string, std::string>> AdminLinks(const Simulation& sim, int slot)
{
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& l : sim.Universe().links)
        if ((Gen::SlotOf(l.a) == slot) != (Gen::SlotOf(l.b) == slot))
            out.push_back(Gen::SlotOf(l.a) == slot ? std::make_pair(l.a, l.b)
                                                   : std::make_pair(l.b, l.a));
    return out;
}

bool HasGateTo(const Simulation& sim, const std::string& here, const std::string& there)
{
    const SystemState* st = sim.SystemById(here);
    if (st == nullptr)
        return false;
    for (const auto& e : st->entities)
        if (e->GetKind() == EntityKind::Gate &&
            static_cast<const JumpGate&>(*e).GetDestination() == there)
            return true;
    return false;
}

void WriteText(const std::string& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary);
    out << text;
}

std::string ReadText(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// A copy of the helios fixture in `dir`, under `id`, with `edit` applied to its pack.json.
std::string CopyHelios(const TempDir& dir, const std::string& id,
                       const std::function<void(json&)>& edit = nullptr)
{
    const std::string to = dir.MakeDir(id);
    for (const char* f : { "core.json", "reach.json", "verge.json" })
        WriteText(to + "/" + f, ReadText(PACKS + "helios/" + f));
    json pack = json::parse(ReadText(PACKS + "helios/pack.json"));
    pack["id"] = id;
    if (pack.contains("factions"))
        for (json& f : pack["factions"])
            f["id"] = id + "-wardens";
    pack["systems"][1]["owner"] = id + "-wardens";
    pack["relations"][0][0] = id + "-wardens";
    if (edit)
        edit(pack);
    WriteText(to + "/pack.json", pack.dump(2));
    return to;
}

std::vector<std::string> ProblemsOf(const std::string& dir)
{
    Gen::Pack                p;
    std::vector<std::string> problems;
    Gen::LoadPack(dir, p, problems);
    return problems;
}

bool Says(const std::vector<std::string>& problems, const std::string& part)
{
    for (const std::string& p : problems)
        if (p.find(part) != std::string::npos)
            return true;
    std::string all;
    for (const std::string& p : problems)
        all += "\n  " + p;
    MESSAGE("expected a problem saying '" << part << "', got:" << all);
    return false;
}
}  // namespace

TEST_CASE("a pack is read whole, and its hash is what it says, not how it is written")
{
    LoadRegistries();
    const auto helios = Fixture("helios");
    CHECK(helios->kind == "region");
    CHECK(helios->systems.size() == 3);
    CHECK(helios->links.size() == 2);
    CHECK(helios->starts == std::vector<std::string>{ "core" });
    CHECK(helios->linksMin == 1);
    CHECK(helios->linksMax == 2);
    CHECK(helios->hash != 0);

    TempDir dir;
    // The same pack written another way is the same pack...
    const std::string same = dir.MakeDir("helios");
    for (const char* f : { "core.json", "reach.json", "verge.json" })
        WriteText(same + "/" + f, json::parse(ReadText(PACKS + "helios/" + f)).dump());
    WriteText(same + "/pack.json", json::parse(ReadText(PACKS + "helios/pack.json")).dump(8));
    Gen::Pack                p;
    std::vector<std::string> problems;
    REQUIRE(Gen::LoadPack(same, p, problems));
    CHECK(p.hash == helios->hash);
    // ...and one value changed in a system's document is another.
    json core = json::parse(ReadText(same + "/core.json"));
    core["stations"][0]["size"] = core["stations"][0]["size"].get<double>() + 1.0;
    WriteText(same + "/core.json", core.dump());
    REQUIRE(Gen::LoadPack(same, p, problems));
    CHECK(p.hash != helios->hash);
}

TEST_CASE("a bad pack is refused with every problem named")
{
    LoadRegistries();
    TempDir dir;

    CHECK(Says(ProblemsOf(CopyHelios(dir, "a", [](json& j) { j["colour"] = "red"; })),
               "a/pack.json: unknown field \"colour\""));
    CHECK(Says(ProblemsOf(CopyHelios(dir, "b", [](json& j) { j["id"] = "other"; })),
               "but the pack's directory is \"b\""));
    CHECK(Says(ProblemsOf(CopyHelios(dir, "c", [](json& j) { j["schema"] = 2; })),
               "this build reads schema 1"));
    CHECK(Says(ProblemsOf(CopyHelios(dir, "d",
                                     [](json& j) { j["links"].push_back({ "core", "nowhere" }); })),
               "\"nowhere\", which is not one of the pack's systems"));
    // A link the documents have no gate for, and a gate pack.json does not link.
    CHECK(Says(ProblemsOf(CopyHelios(dir, "e",
                                     [](json& j) { j["links"].push_back({ "reach", "verge" }); })),
               "links it to \"verge\", and no gate here leads there"));
    CHECK(Says(ProblemsOf(CopyHelios(dir, "f", [](json& j)
                                     { j["links"] = json::array({ { "core", "reach" } }); })),
               "leads to \"verge\", which pack.json does not link this system to"));
    CHECK(Says(
        ProblemsOf(CopyHelios(dir, "g", [](json& j) { j["weave"]["links"]["kind"] = "dynamic"; })),
        "is not built yet"));
    {
        // A start where nothing can be docked at.
        const std::string h =
            CopyHelios(dir, "h", [](json& j) { j["start"] = json::array({ "verge" }); });
        json verge = json::parse(ReadText(h + "/verge.json"));
        verge.erase("stations");
        WriteText(h + "/verge.json", verge.dump());
        CHECK(Says(ProblemsOf(h), "start \"verge\" has no station a ship can dock at"));
    }
    CHECK(
        Says(ProblemsOf(CopyHelios(dir, "i", [](json& j) { j["systems"][2]["owner"] = "Nobody"; })),
             "owner \"Nobody\" is neither in factions.json nor one of the pack's"));
    CHECK(Says(
        ProblemsOf(CopyHelios(dir, "j", [](json& j) { j["systems"][0]["map"] = { 5000, 0 }; })),
        "it would lie in the next slot"));
    CHECK(Says(ProblemsOf(CopyHelios(dir, "k", [](json& j) { j["weave"]["place"] = "anywhere"; })),
               "weave.place \"anywhere\" is not one of"));
    CHECK(
        Says(ProblemsOf(CopyHelios(dir, "l", [](json& j) { j["factions"][0]["id"] = "Pirates"; })),
             "faction \"Pirates\" is already in the registry"));

    // Inside a system's document: an unknown field, a missing one, a place outside the system.
    const std::string m = CopyHelios(dir, "m");
    json              core = json::parse(ReadText(m + "/core.json"));
    core["stations"][0]["rol"] = "TradeHub";
    core["asteroidFields"][0].erase("ore");
    core["nebulae"][0]["pos"] = { 2000000, 0 };
    WriteText(m + "/core.json", core.dump());
    const std::vector<std::string> problems = ProblemsOf(m);
    CHECK(Says(problems, "m/core.json: stations[0]"));
    CHECK(Says(problems, "unknown field \"rol\""));
    CHECK(Says(problems, "\"ore\" is missing"));
    CHECK(Says(problems, "\"pos\" is outside the system"));

    // A system pack keeps the gates of the system it replaces.
    const std::string n = dir.MakeDir("n");
    WriteText(n + "/relay.json", ReadText(PACKS + "relay/relay.json"));
    json relay = json::parse(ReadText(PACKS + "relay/pack.json"));
    relay["id"] = "n";
    relay["weave"]["links"] = { { "count", 1 } };
    WriteText(n + "/pack.json", relay.dump());
    CHECK(Says(ProblemsOf(n), "a system pack has no links of its own"));

    // Two packs are read in id order, and a bad one does not take a good one with it.
    std::vector<std::string> all;
    const auto               packs = Gen::LoadPacks(dir.Path(""), all);
    CHECK(packs.empty());  // every one of them is broken
    CHECK(all.size() >= 14);
}

TEST_CASE("a new world weaves its packs in: an admin region, its links, its starts")
{
    LoadRegistries();
    Simulation sim;
    Make(sim, 4, Both());
    for (const std::string& why : sim.PackProblems())
        MESSAGE(why);
    REQUIRE(sim.PackProblems().empty());
    sim.InitGalaxy();
    sim.MaterializeAllSystems(DATA + "systems/");

    // The region: a free slot beside a region the world began with, systems renamed for it.
    const Gen::Woven* helios = WovenOf(sim, "helios");
    REQUIRE(helios != nullptr);
    const int slot = helios->slot;
    CHECK(helios->kind == "region");
    CHECK(sim.OpenSlots().count(slot) == 1);
    CHECK(sim.Skeleton().slots[(size_t)slot].kind == Gen::SlotKind::Frontier);
    const std::string core = Gen::SystemId(slot, 1, 1);
    REQUIRE(Info(sim, core) != nullptr);
    CHECK(Info(sim, core)->name == "Helios Core");
    CHECK(Info(sim, Gen::SystemId(slot, 2, 1))->name == "Sigma Reach");
    CHECK(Info(sim, Gen::SystemId(slot, 2, 2))->name == "Tau Verge");
    CHECK(HasGateTo(sim, core, Gen::SystemId(slot, 2, 1)));
    const Simulation::RegionOpening* o = sim.OpeningOf(slot);
    REQUIRE(o != nullptr);
    CHECK(o->by == "pack");
    CHECK(o->why == "helios");
    CHECK(o->parent == -1);
    CHECK(sim.IsPackSlot(slot));

    // Its admin links, with a gate at each end, lead into the rest of the galaxy.
    const auto links = AdminLinks(sim, slot);
    REQUIRE(!links.empty());
    CHECK(links.size() <= 2);
    for (const auto& l : links)
    {
        CHECK(HasGateTo(sim, l.first, l.second));
        CHECK(HasGateTo(sim, l.second, l.first));
        CHECK(sim.IsPackSlot(Gen::SlotOf(l.second)));  // what it links to never drifts away
    }
    CHECK_FALSE(sim.PlanRoute(links.front().second, Gen::SystemId(slot, 2, 2), false).empty());

    // Its faction is in the registry, at war with the pirates, and owns what the pack says.
    FactionId wardens;
    REQUIRE(Factions::Find("helios-wardens", wardens));
    CHECK(Factions::Relation(wardens, Factions::Pirates()) == Stance::War);
    CHECK(Info(sim, Gen::SystemId(slot, 2, 1))->owner == "helios-wardens");

    // The system pack took the place of a frontier system, keeping its gates.
    const Gen::Woven* relay = WovenOf(sim, "relay");
    REQUIRE(relay != nullptr);
    REQUIRE_FALSE(relay->system.empty());
    CHECK(Gen::SlotOf(relay->system) == relay->slot);
    CHECK(Info(sim, relay->system)->name == "Kestrel Relay");
    bool hasGate = false;
    for (const auto& e : sim.SystemById(relay->system)->entities)
        hasGate = hasGate || e->GetKind() == EntityKind::Gate;
    CHECK(hasGate);

    // Both are starts, beside the centres' hearts, each with the station a pilot docks at.
    bool atCore = false, atRelay = false;
    for (const Simulation::StartPoint& sp : sim.Starts())
    {
        atCore = atCore || (sp.systemId == core && sp.systemName == "Helios Core" &&
                            !sp.station.empty() && sp.owner == "TradersGuild");
        atRelay = atRelay || (sp.systemId == relay->system && sp.station == "Kestrel Relay");
    }
    CHECK(atCore);
    CHECK(atRelay);
    CHECK(sim.PackChanges().empty());
}

TEST_CASE("the same packs make the same world, and a pack with no room says why")
{
    LoadRegistries();
    // Read once: a pack's factions are refused once the registry has them, and weaving puts
    // them there.
    const auto packs = Both();
    Simulation a, b;
    Make(a, 11, packs);
    Make(b, 11, packs);
    REQUIRE(a.WovenPacks().size() == 2);
    REQUIRE(b.WovenPacks().size() == 2);
    for (size_t i = 0; i < 2; i++)
    {
        CHECK(a.WovenPacks()[i].slot == b.WovenPacks()[i].slot);
        CHECK(a.WovenPacks()[i].system == b.WovenPacks()[i].system);
    }
    const int slot = WovenOf(a, "helios")->slot;
    CHECK(AdminLinks(a, slot) == AdminLinks(b, slot));
    CHECK(a.RegionDocuments() == b.RegionDocuments());

    // A slot that is a centre is no slot for a region pack.
    TempDir                  dir;
    Gen::Pack                p;
    std::vector<std::string> problems;
    const int                centre = a.Skeleton().centres.front();
    REQUIRE(Gen::LoadPack(
        CopyHelios(dir, "fixed", [&](json& j) { j["weave"]["place"] = { { "slot", centre } }; }), p,
        problems));
    Simulation c;
    Make(c, 11, { std::make_shared<const Gen::Pack>(p) });
    CHECK(Says(c.PackProblems(), "is taken"));
    CHECK(c.WovenPacks().empty());
}

TEST_CASE("a woven pack is sealed with the world, and a changed pack is named, not woven")
{
    LoadRegistries();
    TempDir           dir;
    const std::string seal = dir.MakeDir("regions");
    const std::string path = dir.Path("world.json");
    const auto        packs = Both();
    const uint64_t    heliosHash = packs[0]->hash;
    Simulation        a;
    Make(a, 7, packs, seal);
    a.InitGalaxy();
    REQUIRE(a.PackProblems().empty());
    const int         slot = WovenOf(a, "helios")->slot;
    const std::string relaySystem = WovenOf(a, "relay")->system;
    const auto        links = AdminLinks(a, slot);
    REQUIRE(a.SaveWorld(path));

    Simulation::WorldHeader h;
    REQUIRE(Simulation::ReadWorldHeader(path, h));
    REQUIRE(h.packs.size() == 2);
    // System packs are placed before any region is made, region packs after.
    CHECK(h.packs[0].id == "relay");
    CHECK(h.packs[0].system == relaySystem);
    CHECK(h.packs[1].id == "helios");
    CHECK(h.packs[1].slot == slot);
    CHECK(h.packs[1].hash == heliosHash);

    // A restart with helios renamed in its files and a pack nobody wove: the world is as it
    // was woven -- the seal is what it reads -- and the admin is told about both.
    REQUIRE(Factions::Load(DATA + "factions.json"));
    Gen::Pack                changed, added;
    std::vector<std::string> problems;
    REQUIRE(Gen::LoadPack(
        CopyHelios(dir, "helios", [](json& j) { j["systems"][0]["name"] = "Helios Prime"; }),
        changed, problems));
    // helios-wardens is the fixture's own id again, so the copy's factions are renamed back.
    REQUIRE(Gen::LoadPack(CopyHelios(dir, "later"), added, problems));
    Simulation b;
    Make(b, 7,
         { std::make_shared<const Gen::Pack>(changed), packs[1],
           std::make_shared<const Gen::Pack>(added) },
         seal, &h);
    REQUIRE(b.LoadWorld(path) == Save::Result::Ok);
    b.MaterializeAllSystems(DATA + "systems/");
    CHECK(Info(b, Gen::SystemId(slot, 1, 1))->name == "Helios Core");
    CHECK(Info(b, relaySystem)->name == "Kestrel Relay");
    CHECK(AdminLinks(b, slot) == links);
    FactionId wardens;
    CHECK(Factions::Find("helios-wardens", wardens));  // registered from the seal
    const std::vector<std::string> told = b.PackChanges();
    CHECK(Says(told, "pack 'helios' has changed since this world was made"));
    CHECK(Says(told, "pack 'later' was added after this world was made"));
    CHECK(b.WovenPacks().size() == 2);
    CHECK(b.IsStart(Gen::SystemId(slot, 1, 1)));
    CHECK(b.IsStart(relaySystem));

    // ...and the next save still knows where they went.
    REQUIRE(b.SaveWorld(path));
    Simulation::WorldHeader again;
    REQUIRE(Simulation::ReadWorldHeader(path, again));
    CHECK(again.packs.size() == 2);
    CHECK(again.packs[1].hash == heliosHash);  // what it was woven from, not what it is now
}

TEST_CASE("the region an admin link reaches does not drift away from under it")
{
    LoadRegistries();
    Simulation sim;
    Make(sim, 4, { Fixture("helios") });
    const int   slot = WovenOf(sim, "helios")->slot;
    std::string why;
    CHECK(sim.ArchiveRegionNow(slot, why) < 0);  // not a frontier anybody opened
    for (const auto& l : AdminLinks(sim, slot))
    {
        const int                        far = Gen::SlotOf(l.second);
        const Simulation::RegionOpening* o = sim.OpeningOf(far);
        REQUIRE(o != nullptr);
        if (o->parent < 0)
            continue;  // a centre: it never drifts anyway
        CHECK(sim.ArchiveRegionNow(far, why) < 0);
        CHECK(why.find("pack") != std::string::npos);
    }
}

// The editor's side (#342 S8): a region of a world written as a pack, and one begun from
// nothing, read back as the server reads them and woven into a new world.
TEST_CASE("a region exported as a pack reads as the server reads it, and weaves in")
{
    LoadRegistries();
    const Gen::GalaxySkeleton      g = Gen::GenerateGalaxySkeleton(Params(5));
    const std::vector<Gen::Region> world = Gen::GenerateWorld(g);
    for (const Gen::Region& r : world)
        Gen::RegisterNatives(r.natives);
    const Gen::Region* centre = nullptr;
    const Gen::Region* frontier = nullptr;
    for (const Gen::Region& r : world)
    {
        if (centre == nullptr && !r.heartId.empty())
            centre = &r;
        if (frontier == nullptr && r.heartId.empty() && !r.natives.empty())
            frontier = &r;
    }
    REQUIRE(centre != nullptr);
    REQUIRE(frontier != nullptr);

    TempDir                          dir;
    std::shared_ptr<const Gen::Pack> woven;
    for (const Gen::Region* r : { centre, frontier })
    {
        const std::string        id = "export-" + std::to_string(r->slot);
        std::vector<std::string> notes;
        const Gen::PackFiles     files = Gen::RegionAsPack(*r, id, "", &notes);
        std::string              error;
        REQUIRE(Gen::WritePack(dir.Path("packs/" + id), files, false, error));
        // Written once: a second export over it is refused rather than mixed with the first.
        CHECK_FALSE(Gen::WritePack(dir.Path("packs/" + id), files, false, error));

        Gen::Pack                p;
        std::vector<std::string> problems;
        const bool               ok = Gen::LoadPack(dir.Path("packs/" + id), p, problems);
        for (const std::string& why : problems)
            MESSAGE(why);
        REQUIRE(ok);
        CHECK(p.systems.size() == r->systems.size());
        // The way in first, under the pack's own id, the slot taken off.
        CHECK(p.systems.front().id == "s" + r->entryId.substr(r->entryId.find('-') + 1));
        for (const Gen::PackSystem& s : p.systems)
        {
            CHECK(std::fabs(s.mapX) <= Gen::PACK_MAP_REACH);
            CHECK(std::fabs(s.mapY) <= Gen::PACK_MAP_REACH);
        }
        if (r == centre)
        {
            // Its capital is where a pilot may start.
            REQUIRE(p.starts.size() == 1);
            CHECK(p.starts.front() == "s" + r->heartId.substr(r->heartId.find('-') + 1));
        }
        else
        {
            // Its natives are the pack's own faction now, and still hold their stations.
            REQUIRE(p.factions.size() == 1);
            CHECK(p.factions[0]["id"] == id + "-natives");
            CHECK(p.factions[0]["name"] == r->natives["name"]);
            bool flown = false;
            for (const Gen::PackSystem& s : p.systems)
                for (const json& st : s.document.value("stations", json::array()))
                    flown = flown || st.value("faction", std::string()) == id + "-natives";
            CHECK(flown);
            // The wormhole home leads out of it, and is left to the weaving.
            CHECK_FALSE(notes.empty());
            woven = std::make_shared<const Gen::Pack>(std::move(p));
        }
    }

    // Woven into a world of another seed, as a server would weave it.
    Simulation sim;
    Make(sim, 9, { woven });
    for (const std::string& why : sim.PackProblems())
        MESSAGE(why);
    CHECK(sim.PackProblems().empty());
    REQUIRE(sim.WovenPacks().size() == 1);
    const int slot = sim.WovenPacks().front().slot;
    CHECK(sim.OpenSlots().count(slot) == 1);
    // Its designations were the old slot's, so they were left out: it is designated afresh.
    REQUIRE(Info(sim, Gen::SystemId(slot, 1, 1)) != nullptr);
    CHECK(Info(sim, Gen::SystemId(slot, 1, 1))->name == Gen::SystemDesignation(slot, 1, 1));
}

TEST_CASE("a pack begun from nothing reads, has a start, and weaves in")
{
    LoadRegistries();
    TempDir     dir;
    std::string error;
    const auto  files = Gen::NewRegionPack("outpost", "Far Outpost");
    REQUIRE(Gen::WritePack(dir.Path("outpost"), files, false, error));
    CHECK_FALSE(Gen::PackIdValid("Not A Pack"));
    CHECK(Gen::PackIdValid("outpost"));

    Gen::Pack                p;
    std::vector<std::string> problems;
    const bool               ok = Gen::LoadPack(dir.Path("outpost"), p, problems);
    for (const std::string& why : problems)
        MESSAGE(why);
    REQUIRE(ok);
    CHECK(p.systems.size() == 1);
    CHECK(p.starts == std::vector<std::string>{ "core" });

    Simulation sim;
    Make(sim, 2, { std::make_shared<const Gen::Pack>(std::move(p)) });
    for (const std::string& why : sim.PackProblems())
        MESSAGE(why);
    CHECK(sim.PackProblems().empty());
    REQUIRE(sim.WovenPacks().size() == 1);
    bool start = false;
    for (const Simulation::StartPoint& sp : sim.Starts())
        start = start || sp.systemName == "Far Outpost Core";
    CHECK(start);
}
