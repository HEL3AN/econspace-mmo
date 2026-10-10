#include <doctest/doctest.h>

#include "core/Archetype.h"
#include "core/ShipDesign.h"
#include "entities/ShipType.h"
#include "render/Modules.h"
#include "render/Silhouette.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <string>

// Ships as designs, first step (#279): a ship's stats are read off its parts. Nothing in the
// game uses the derived numbers yet; these tests hold them to the hand-typed catalog until
// the switch, and hold the format to the rule that a mistake is a load error.

namespace
{
std::string DataFile(const char* name)
{
    return std::string(TEST_DATA_DIR) + name;
}

nlohmann::json ShippedJson()
{
    std::ifstream in(DataFile("ships.json"));
    REQUIRE(in.is_open());
    return nlohmann::json::parse(in);
}

Ships::Catalogue Shipped()
{
    Ships::Catalogue c;
    std::string      error;
    REQUIRE_MESSAGE(Ships::Load(DataFile("ships.json"), c, error), error);
    return c;
}

// Parses the shipped catalogue after `edit` has changed it; the error, or "" if it loaded.
template <class F> std::string ErrorAfter(F edit)
{
    nlohmann::json j = ShippedJson();
    edit(j);
    Ships::Catalogue c;
    std::string      error;
    if (Ships::Parse(j, c, error))
        return "";
    return error;
}

bool Within(float derived, float typed, float tolerance)
{
    return std::fabs(derived - typed) <= tolerance * std::fabs(typed);
}

nlohmann::json& DesignJson(nlohmann::json& j, const char* id)
{
    for (auto& d : j["designs"])
        if (d["id"] == id)
            return d;
    FAIL("no design " << id);
    return j;
}
}  // namespace

TEST_CASE("today's four ships, written as designs, derive within 10% of their typed stats")
{
    const Ships::Catalogue c = Shipped();
    for (const ShipType& t : GetShipCatalog())
    {
        CAPTURE(t.name);
        const Ships::Design* d = nullptr;
        for (const Ships::Design& candidate : c.designs)
            if (candidate.name == t.name)
                d = &candidate;
        REQUIRE_MESSAGE(d != nullptr, "the catalog's " << t.name << " has no design");

        Ships::Stats s;
        std::string  error;
        REQUIRE_MESSAGE(Ships::Derive(c, *d, s, error), error);
        CHECK(Within(s.acceleration, t.stats.thrustPower, 0.10f));
        CHECK(Within(s.turnSpeed, t.stats.turnSpeed, 0.10f));
        CHECK(Within(s.maxSpeed, t.stats.maxSpeed, 0.10f));
        CHECK(Within(s.rcsAccel, t.stats.rcsAccel, 0.10f));
        CHECK(Within((float)s.cargoCapacity, (float)t.stats.cargoCapacity, 0.10f));
        CHECK(Within(s.miningRate, t.stats.miningRate, 0.10f));
        CHECK(s.mass > 0.0f);
        CHECK_FALSE(s.cost.empty());
        CHECK(s.buildSeconds > 0.0f);
    }
}

TEST_CASE("the classes still read in the numbers: the hauler holds most, the courier is fastest, "
          "the miner mines most")
{
    const Ships::Catalogue c = Shipped();
    auto                   stats = [&](const char* id)
    {
        Ships::Stats s;
        std::string  error;
        REQUIRE(Ships::Derive(c, *c.FindDesign(id), s, error));
        return s;
    };
    const Ships::Stats scout = stats("scout"), courier = stats("courier"), hauler = stats("hauler"),
                       miner = stats("miner");
    CHECK(hauler.cargoCapacity > miner.cargoCapacity);
    CHECK(miner.cargoCapacity > scout.cargoCapacity);
    CHECK(courier.maxSpeed > scout.maxSpeed);
    CHECK(scout.maxSpeed > hauler.maxSpeed);
    CHECK(miner.miningRate > scout.miningRate);
    CHECK(hauler.mass > courier.mass);
    CHECK(hauler.buildSeconds > courier.buildSeconds);
}

TEST_CASE("a design's stats are the sum of its parts, and its cost is every part's cost")
{
    const Ships::Catalogue c = Shipped();
    Ships::Design          d = *c.FindDesign("hauler");
    Ships::Stats           before, after;
    std::string            error;
    REQUIRE(Ships::Derive(c, d, before, error));

    // Two more pods on the keel's spine sockets: more hold, more mass, slower, dearer.
    d.fit.push_back({ "hull.cargo", "mid", "spine", 2 });
    REQUIRE_MESSAGE(Ships::Derive(c, d, after, error), error);
    const Ships::ModulePart* pod = c.FindModule("hull.cargo");
    CHECK(after.cargoCapacity == before.cargoCapacity + 2 * (int)pod->provides.cargo);
    CHECK(after.mass == doctest::Approx(before.mass + 2 * pod->mass));
    CHECK(after.acceleration < before.acceleration);
    CHECK(after.parts == before.parts + 2);
    auto iron = [](const Ships::Stats& s)
    {
        for (const auto& [r, n] : s.cost)
            if (r == ResourceType::Iron)
                return n;
        return 0;
    };
    CHECK(iron(after) == iron(before) + 2 * pod->cost[0].second);

    // The same design derives the same numbers every time: nothing about it is drawn.
    Ships::Stats again;
    REQUIRE(Ships::Derive(c, d, again, error));
    CHECK(again.acceleration == after.acceleration);
    CHECK(again.buildSeconds == after.buildSeconds);
}

TEST_CASE("every module a ship's fit names is in the render library and fits the socket")
{
    std::string error;
    REQUIRE_MESSAGE(Render::Modules::Load(DataFile("modules.json"), error), error);
    const Ships::Catalogue c = Shipped();
    for (const Ships::ModulePart& part : c.modules)
    {
        CAPTURE(part.id);
        CHECK(Render::Modules::Find(part.id) != nullptr);
    }
    for (const Ships::Design& d : c.designs)
        for (const Ships::FitLine& line : d.fit)
        {
            CAPTURE(d.id);
            CAPTURE(line.module);
            const Render::Module* m = Render::Modules::Find(line.module);
            REQUIRE(m != nullptr);
            CHECK(std::find(m->sockets.begin(), m->sockets.end(), line.on) != m->sockets.end());
        }
}

TEST_CASE("a ship's look never decides what it can do: its function is in fixed kit lines (#279)")
{
    // A module that provides something is function, wherever a ship wears it: on a kit line
    // it must be a `fit` line, and every one of them is placed in full for every seed --
    // a drive that found no room would be a slower ship that nobody chose. A module part
    // written by hand may not have a chance of being there either.
    REQUIRE(Archetypes::Load(DataFile("archetypes.json")));
    const Ships::Catalogue c = Shipped();
    auto                   function = [&](const std::string& id)
    {
        const Ships::ModulePart* m = c.FindModule(id);
        return m != nullptr && (m->provides.thrust > 0.0f || m->provides.rcs > 0.0f ||
                                m->provides.cargo > 0.0f || m->provides.mining > 0.0f);
    };
    int ships = 0;
    for (const Archetype& a : Archetypes::All())
    {
        if (a.kind != EntityKind::Npc && a.kind != EntityKind::PlayerShip)
            continue;
        ships++;
        CAPTURE(a.id);
        const Render::Shape&      shape = a.visual.shape;
        std::vector<Render::Part> sections;
        for (const Render::Part& p : shape.parts)
        {
            if (p.section)
                sections.push_back(p);
            else if (!p.module.empty() && function(p.module))
                CHECK(p.chance >= 1.0f);
        }
        for (const Render::KitEntry& e : shape.kit.entries)
            if (!e.byTag && function(e.of))
            {
                CAPTURE(e.of);
                CHECK(e.fit);
            }
        for (int seed = 1; seed <= 32; seed++)
        {
            CAPTURE(seed);
            const std::vector<Render::Part> placed = Render::PlaceKit(shape.kit, sections, seed);
            for (const Render::KitEntry& e : shape.kit.entries)
                if (e.fit)
                {
                    CAPTURE(e.of);
                    // Lines of one module add up: what is placed is what they asked for.
                    int want = 0, got = 0;
                    for (const Render::KitEntry& o : shape.kit.entries)
                        want += o.fit && o.of == e.of ? (int)o.lo : 0;
                    for (const Render::Part& p : placed)
                        got += p.module == e.of;
                    CHECK(got == want);
                }
        }
    }
    CHECK(ships >= 2);
}

TEST_CASE("a design that does not fit is invalid, never a quietly weaker ship")
{
    const Ships::Catalogue c = Shipped();
    std::string            error;
    Ships::Stats           s;

    Ships::Design overfull = *c.FindDesign("scout");
    overfull.fit.push_back({ "hull.cargo", "mid", "spine", 1 });  // the spine holds one
    CHECK_FALSE(Ships::Derive(c, overfull, s, error));
    CHECK(error.find("sockets") != std::string::npos);

    Ships::Design driveForward = *c.FindDesign("scout");
    driveForward.fit.push_back({ "hull.engine", "bow", "front", 1 });
    CHECK_FALSE(Ships::Validate(c, driveForward, error));
    CHECK(error.find("stern") != std::string::npos);

    Ships::Design lopsided = *c.FindDesign("hauler");
    lopsided.fit.push_back({ "hull.rcs", "stern", "edge", 1 });
    CHECK_FALSE(Ships::Validate(c, lopsided, error));
    CHECK(error.find("pairs") != std::string::npos);

    Ships::Design backwards = *c.FindDesign("scout");
    backwards.bow = "stern.single";
    CHECK_FALSE(Ships::Validate(c, backwards, error));

    Ships::Design tooLong = *c.FindDesign("courier");  // an interceptor takes one mid
    tooLong.mids.push_back("mid.slim");
    CHECK_FALSE(Ships::Validate(c, tooLong, error));

    Ships::Design unknown = *c.FindDesign("scout");
    unknown.fit.push_back({ "hull.warpcore", "mid", "top", 1 });
    CHECK_FALSE(Ships::Validate(c, unknown, error));
}

TEST_CASE("a mistake in the ship catalogue is a load error, not a default")
{
    CHECK(ErrorAfter([](nlohmann::json&) {}).empty());

    // A misspelt field anywhere (#191).
    CHECK(ErrorAfter([](nlohmann::json& j) { j["modules"][0]["mas"] = 2; }).find("mas") !=
          std::string::npos);
    CHECK_FALSE(ErrorAfter([](nlohmann::json& j) { j["sections"][0]["sokets"] = 1; }).empty());
    CHECK_FALSE(ErrorAfter([](nlohmann::json& j) { j["frames"][0]["mid"] = 1; }).empty());
    CHECK_FALSE(ErrorAfter([](nlohmann::json& j) { j["designs"][0]["fits"] = 1; }).empty());
    CHECK_FALSE(ErrorAfter([](nlohmann::json& j) { DesignJson(j, "scout")["fit"][0]["cuont"] = 1; })
                    .empty());
    CHECK_FALSE(ErrorAfter([](nlohmann::json& j) { j["rules"]["speedbase"] = 1; }).empty());
    CHECK_FALSE(ErrorAfter([](nlohmann::json& j) { j["shipz"] = 1; }).empty());

    // What a part provides is only what a stat reads; a shield nothing reads is a typo now.
    CHECK(ErrorAfter([](nlohmann::json& j) { j["modules"][0]["provides"] = { { "shield", 5 } }; })
              .find("shield") != std::string::npos);
    // A cost in a resource that does not exist, or of nothing.
    CHECK_FALSE(
        ErrorAfter([](nlohmann::json& j) { j["modules"][0]["cost"] = { { "Gold", 5 } }; }).empty());
    CHECK_FALSE(ErrorAfter([](nlohmann::json& j) { j["modules"][0].erase("cost"); }).empty());
    // A part with no mass, a socket kind that does not exist, a count drawn from a range.
    CHECK_FALSE(ErrorAfter([](nlohmann::json& j) { j["modules"][0]["mass"] = 0; }).empty());
    CHECK_FALSE(
        ErrorAfter([](nlohmann::json& j) { j["sections"][0]["sockets"]["wingtip"] = 1; }).empty());
    CHECK_FALSE(
        ErrorAfter([](nlohmann::json& j) { DesignJson(j, "scout")["fit"][0]["count"] = { 0, 2 }; })
            .empty());
    // A section at the wrong position, and a part defined twice.
    CHECK_FALSE(ErrorAfter([](nlohmann::json& j) { DesignJson(j, "scout")["stern"] = "mid.spine"; })
                    .empty());
    CHECK(ErrorAfter([](nlohmann::json& j) { j["modules"].push_back(j["modules"][0]); })
              .find("twice") != std::string::npos);
}
