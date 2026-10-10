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
                    // A mirrored pair is one part drawn twice.
                    for (const Render::Part& p : placed)
                        got += p.module == e.of ? (p.mirror && !p.mirrorOnly ? 2 : 1) : 0;
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

// --- Step 3: frames per class, a design drawn from its sections (#279) ---------------------

namespace
{
nlohmann::json& SectionJson(nlohmann::json& j, const char* id)
{
    for (auto& s : j["sections"])
        if (s["id"] == id)
            return s;
    FAIL("no section " << id);
    return j;
}

nlohmann::json& FrameJson(nlohmann::json& j, const char* id)
{
    for (auto& f : j["frames"])
        if (f["id"] == id)
            return f;
    FAIL("no frame " << id);
    return j;
}

std::vector<Render::Part> SectionsOf(const Render::Shape& s)
{
    std::vector<Render::Part> out;
    for (const Render::Part& p : s.parts)
        if (p.section)
            out.push_back(p);
    return out;
}
}  // namespace

TEST_CASE("every design keeps its class's silhouette, and the barge alone is heavy at the bow")
{
    const Ships::Catalogue c = Shipped();
    for (const Ships::Design& d : c.designs)
    {
        CAPTURE(d.id);
        const Ships::Frame*     f = c.FindFrame(d.frame);
        const Ships::Silhouette s = Ships::Measure(c, d);
        CHECK(s.length >= f->minAspect * s.width);
        CHECK((s.massAt > 0.0f) == f->bowHeavy);
    }
    // Every class but the barge is at least 1.6 long for its width: no oval as a main body.
    for (const Ships::Frame& f : c.frames)
        if (!f.bowHeavy)
            CHECK(f.minAspect >= 1.6f);
}

TEST_CASE("a hull that breaks its class rule is a load error, not a ship that looks wrong")
{
    // Too short for a frigate: the oval the NPCs were.
    CHECK(ErrorAfter([](nlohmann::json& j) { FrameJson(j, "frigate")["minAspect"] = 9.0; })
              .find("long") != std::string::npos);
    // A hauler carrying its mass at the bow is a barge, and only a barge may.
    CHECK(ErrorAfter([](nlohmann::json& j) { FrameJson(j, "barge").erase("bowHeavy"); })
              .find("forward") != std::string::npos);
    // A hull off the axis without its mirror, or turned across it, is not bilateral.
    CHECK(ErrorAfter([](nlohmann::json& j)
                     { SectionJson(j, "stern.twin")["shape"]["sections"][1].erase("mirror"); })
              .find("bilateral") != std::string::npos);
    CHECK(ErrorAfter([](nlohmann::json& j)
                     { SectionJson(j, "bow.blunt")["shape"]["sections"][1]["angle"] = 90; })
              .find("bilateral") != std::string::npos);
    // Mirrored, never repeated: `repeat: 2` puts the second wing in front of the nose.
    CHECK(ErrorAfter([](nlohmann::json& j)
                     { SectionJson(j, "mid.spine")["shape"]["sections"][0]["repeat"] = 2; })
              .find("repeat") != std::string::npos);
    // The hull is the same for every ship of a design: the seed rolls the trim, not the hull.
    CHECK(ErrorAfter(
              [](nlohmann::json& j)
              { SectionJson(j, "mid.spine")["shape"]["sections"][0]["length"] = { 0.6, 0.8 }; })
              .find("fixed") != std::string::npos);
    // A section is drawn as something; a class says how long it is.
    CHECK_FALSE(
        ErrorAfter([](nlohmann::json& j) { SectionJson(j, "mid.spine").erase("shape"); }).empty());
    CHECK_FALSE(
        ErrorAfter([](nlohmann::json& j) { FrameJson(j, "hauler").erase("minAspect"); }).empty());
    CHECK_FALSE(
        ErrorAfter([](nlohmann::json& j)
                   { SectionJson(j, "mid.spine")["shape"]["parst"] = nlohmann::json::array(); })
            .empty());
    // One stern: an aft end belongs to the stern section, a forward one to the bow.
    CHECK(ErrorAfter([](nlohmann::json& j) { SectionJson(j, "mid.keel")["sockets"]["stern"] = 1; })
              .find("stern") != std::string::npos);
    CHECK_FALSE(
        ErrorAfter([](nlohmann::json& j) { SectionJson(j, "stern.twin")["sockets"]["end"] = 1; })
            .empty());
    // How a module meets a socket is a known look, on a known kind.
    CHECK_FALSE(
        ErrorAfter([](nlohmann::json& j) { j["modules"][1]["look"]["stern"]["mnt"] = "out"; })
            .empty());
    CHECK_FALSE(ErrorAfter([](nlohmann::json& j) { j["modules"][1]["look"]["aft"] = {}; }).empty());
}

TEST_CASE("a design is drawn from its sections, laid stern to bow, with its fit pinned on them")
{
    std::string error;
    REQUIRE_MESSAGE(Render::Modules::Load(DataFile("modules.json"), error), error);
    const Ships::Catalogue c = Shipped();
    for (const Ships::Design& d : c.designs)
    {
        CAPTURE(d.id);
        nlohmann::json shape;
        REQUIRE_MESSAGE(Ships::ShapeOf(c, d, shape, error), error);
        Render::Shape s;
        REQUIRE_MESSAGE(Render::ParseShape(shape, s, error), error);
        CHECK(s.kit.symmetry == "bilateral");

        // Every fit line is a kit line that carries function, with the design's count, on
        // the hull parts of the sections at its position and nowhere else.
        int fitLines = 0;
        for (const Render::KitEntry& e : s.kit.entries)
            if (e.fit)
            {
                const Ships::FitLine& f = d.fit[(size_t)fitLines++];
                CHECK(e.of == f.module);
                CHECK((int)e.lo == f.count);
                CHECK(e.on == f.on);
                CHECK_FALSE(e.in.empty());
            }
        CHECK(fitLines == (int)d.fit.size());

        // The bow is forward of the stern, and the whole is centred on its length.
        Render::Shape hullOnly;
        hullOnly.parts = SectionsOf(s);
        const std::vector<Render::Part>& hull = hullOnly.parts;
        const Rectangle                  box = Render::MeasuredBounds(hullOnly);
        CHECK(box.x + 0.5f * box.width == doctest::Approx(0.0f).epsilon(0.01));
        CHECK(hull.front().at.x > hull.back().at.x);
    }
}

TEST_CASE("the ship archetypes drawn from designs are those designs")
{
    REQUIRE(Archetypes::Load(DataFile("archetypes.json")));
    const Ships::Catalogue& c = Archetypes::ShipCatalogue();
    int                     drawn = 0;
    for (const Ships::Design& d : c.designs)
        for (const Archetype& a : Archetypes::All())
            if (a.design == d.id)
            {
                drawn++;
                CAPTURE(a.id);
                nlohmann::json json;
                std::string    error;
                REQUIRE(Ships::ShapeOf(c, d, json, error));
                Render::Shape s;
                REQUIRE(Render::ParseShape(json, s, error));
                CHECK(a.visual.shape.parts.size() == s.parts.size());
                CHECK(a.visual.shape.kit.entries.size() == s.kit.entries.size());
            }
    CHECK(drawn == (int)c.designs.size());
}

TEST_CASE("a wing is one of a pair: placed once, drawn both ways, swept the same (#279)")
{
    std::string error;
    REQUIRE_MESSAGE(Render::Modules::Load(DataFile("modules.json"), error), error);
    REQUIRE(Render::Modules::Find("hull.wing")->handed);
    Render::Shape ship;
    REQUIRE_MESSAGE(Render::ParseShape(nlohmann::json::parse(R"({
            "sections": [ { "form": "bar", "length": 1.2, "width": 0.4, "pitch": 0.15 } ],
            "kit": { "symmetry": "bilateral", "modules": [
                { "of": "hull.wing", "on": "side", "count": 2, "mount": "out", "scale": 0.3 } ] },
            "parts": [] })"),
                                       ship, error),
                    error);
    for (int seed = 1; seed <= 8; seed++)
    {
        CAPTURE(seed);
        const std::vector<Render::Part> placed = Render::PlaceKit(ship.kit, SectionsOf(ship), seed);
        REQUIRE(placed.size() == 1);
        // Placed as the +y wing as drawn, its pair its reflection: no turn of its own, so the
        // drawing's sweep is the wing's sweep on both sides.
        CHECK(placed[0].mirror);
        CHECK_FALSE(placed[0].mirrorOnly);
        CHECK(placed[0].at.y > 0.0f);
        CHECK(placed[0].angle == doctest::Approx(0.0f));
    }
}

TEST_CASE("a module covers the sockets under its footprint, not a circle round it (#279)")
{
    // A pod laid along a keel is long and narrow: it covers the keel's spine, and the flanks
    // beside it stay free for the thrusters a circle as wide as the pod is long would take.
    std::string error;
    REQUIRE_MESSAGE(Render::Modules::Load(DataFile("modules.json"), error), error);
    Render::Shape ship;
    REQUIRE_MESSAGE(Render::ParseShape(nlohmann::json::parse(R"({
            "sections": [ { "form": "bar", "length": 0.9, "width": 0.36, "pitch": 0.13 } ],
            "kit": { "symmetry": "bilateral", "plain": 0.0, "modules": [
                { "of": "hull.cargo", "variant": "pods", "on": "spine", "count": 1, "fit": true, "scale": 0.22 },
                { "of": "hull.rcs", "on": "edge", "count": 4, "fit": true, "scale": 0.04, "turn": -90 } ] },
            "parts": [] })"),
                                       ship, error),
                    error);
    const std::vector<Render::Part> placed = Render::PlaceKit(ship.kit, SectionsOf(ship), 1);
    int                             rcs = 0;
    for (const Render::Part& p : placed)
        rcs += p.module == "hull.rcs" ? (p.mirror ? 2 : 1) : 0;
    CHECK(rcs == 4);
}

TEST_CASE("a bilateral kit puts an even count in pairs and an odd one's odd module on the axis")
{
    std::string error;
    REQUIRE_MESSAGE(Render::Modules::Load(DataFile("modules.json"), error), error);
    // A stern block with a nacelle each side: two engines are the nacelles' pair, not one on
    // the block and one beside it; three are the pair and the block.
    auto engines = [&](int count)
    {
        Render::Shape ship;
        REQUIRE_MESSAGE(Render::ParseShape(
                            nlohmann::json::parse(
                                R"({
            "sections": [ { "form": "bar", "length": 0.3, "width": 0.34 },
                          { "form": "bar", "at": [-0.03, 0.2], "length": 0.36, "width": 0.15, "mirror": true } ],
            "kit": { "symmetry": "bilateral", "modules": [
                { "of": "hull.engine", "on": "stern", "count": )" +
                                std::to_string(count) +
                                R"(, "fit": true, "mount": "out", "turn": 180, "scale": 0.1 } ] },
            "parts": [] })"),
                            ship, error),
                        error);
        int onAxis = 0, paired = 0;
        for (const Render::Part& p : Render::PlaceKit(ship.kit, SectionsOf(ship), 3))
            (p.mirror ? paired : onAxis) += p.mirror ? 2 : 1;
        return std::make_pair(onAxis, paired);
    };
    CHECK(engines(2) == std::make_pair(0, 2));
    CHECK(engines(3) == std::make_pair(1, 2));

    // A crossbar across the axis is two half lines: four modules on its face are two pairs.
    Render::Shape bar;
    REQUIRE_MESSAGE(Render::ParseShape(nlohmann::json::parse(R"({
            "sections": [ { "form": "bar", "angle": 90, "length": 1.0, "width": 0.2, "pitch": 0.16 } ],
            "kit": { "symmetry": "bilateral", "modules": [
                { "of": "hull.rcs", "on": "edge", "count": 4, "fit": true, "scale": 0.04 } ] },
            "parts": [] })"),
                                       bar, error),
                    error);
    for (const Render::Part& p : Render::PlaceKit(bar.kit, SectionsOf(bar), 1))
    {
        CHECK(p.mirror);
        CHECK(p.at.y < 0.0f);
    }
}

TEST_CASE("a kit line may name several sections")
{
    std::string error;
    REQUIRE_MESSAGE(Render::Modules::Load(DataFile("modules.json"), error), error);
    Render::Shape s;
    REQUIRE_MESSAGE(Render::ParseShape(nlohmann::json::parse(R"({
            "sections": [ { "form": "bar", "at": [0.5, 0], "length": 0.8, "width": 0.2 },
                          { "form": "bar", "at": [-0.5, 0], "length": 0.8, "width": 0.2 } ],
            "kit": { "symmetry": "bilateral", "modules": [
                { "of": "hull.cargo", "in": [0, 1], "on": "side", "count": 4, "fit": true, "scale": 0.15 } ] },
            "parts": [] })"),
                                       s, error),
                    error);
    REQUIRE(s.kit.entries[0].in == std::vector<int>{ 0, 1 });
    int pods = 0;
    for (const Render::Part& p : Render::PlaceKit(s.kit, SectionsOf(s), 1))
        pods += p.mirror ? 2 : 1;
    CHECK(pods == 4);
    CHECK_FALSE(Render::ParseShape(nlohmann::json::parse(R"({ "sections": [], "parts": [],
            "kit": { "modules": [ { "of": "hull.cargo", "in": "keel" } ] } })"),
                                   s, error));
}
