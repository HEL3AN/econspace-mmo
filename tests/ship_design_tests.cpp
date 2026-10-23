#include <doctest/doctest.h>

#include "goods_named.h"
#include "core/Archetype.h"
#include "core/ShipDesign.h"
#include "entities/NpcShip.h"
#include "entities/Ship.h"
#include "entities/ShipType.h"
#include "render/Modules.h"
#include "render/Silhouette.h"
#include "factions_named.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <string>

// Ships as designs (#279): a ship's stats are read off its parts, an NPC's (step 4) and a
// player's (step 5) alike. These tests hold the derived numbers near what was typed by hand
// before, and hold the format to the rule that a mistake is a load error.

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

TEST_CASE("the player's hulls are designs, and fly within 10% of the numbers they were typed as")
{
    // What GetShipCatalog() held, by hand, until the hulls became designs (#279 step 5):
    // thrust, turn, top speed, rcs, hold, mining -- and every hull had 100 hull and a 16 shot.
    struct Typed
    {
        const char* name;
        double      price;
        float       thrust, turn, maxSpeed, rcs;
        int         cargo;
        float       mining;
    };
    const Typed before[] = {
        { "Scout", 0.0, 340.0f, 3.2f, 440.0f, 600.0f, 50, 3.0f },
        { "Courier", 2500.0, 480.0f, 4.2f, 620.0f, 820.0f, 35, 2.0f },
        { "Hauler", 3000.0, 220.0f, 2.2f, 300.0f, 380.0f, 200, 2.0f },
        { "Miner", 4200.0, 260.0f, 2.6f, 360.0f, 460.0f, 120, 7.0f },
    };

    REQUIRE(Archetypes::Load(DataFile("archetypes.json")));
    const std::vector<ShipType>& catalog = GetShipCatalog();
    // An account owns hulls by their place in the hangar, so the old ones keep theirs.
    REQUIRE(catalog.size() >= 4);
    for (int i = 0; i < 4; i++)
    {
        const Typed&    was = before[i];
        const ShipType& t = catalog[i];
        CAPTURE(was.name);
        CHECK(t.name == was.name);
        CHECK(t.price == doctest::Approx(was.price));
        CHECK(Within(t.stats.thrustPower, was.thrust, 0.10f));
        CHECK(Within(t.stats.turnSpeed, was.turn, 0.10f));
        CHECK(Within(t.stats.maxSpeed, was.maxSpeed, 0.10f));
        CHECK(Within(t.stats.rcsAccel, was.rcs, 0.10f));
        CHECK(Within((float)t.stats.cargoCapacity, (float)was.cargo, 0.10f));
        CHECK(Within(t.stats.miningRate, was.mining, 0.10f));
        CHECK(Within(t.stats.hull, 100.0f, 0.10f));
        CHECK(Within(t.stats.damage, 16.0f, 0.10f));
    }
}

TEST_CASE("a hull's numbers are its design's, derived, and nothing typed beside them")
{
    REQUIRE(Archetypes::Load(DataFile("archetypes.json")));
    const Ships::Catalogue&      c = Archetypes::ShipCatalogue();
    const std::vector<ShipType>& catalog = GetShipCatalog();
    REQUIRE(catalog.size() == c.hangar.size());
    for (size_t i = 0; i < catalog.size(); i++)
    {
        const ShipType& t = catalog[i];
        CAPTURE(t.name);
        const Ships::Design* d = c.FindDesign(c.hangar[i].design);
        REQUIRE(d != nullptr);
        Ships::Stats s;
        std::string  error;
        REQUIRE_MESSAGE(Ships::Derive(c, *d, s, error), error);
        CHECK(t.stats.design == d->id);
        CHECK(t.name == d->name);
        CHECK(t.price == doctest::Approx(c.hangar[i].price));
        CHECK(t.stats.thrustPower == doctest::Approx(s.acceleration));
        CHECK(t.stats.turnSpeed == doctest::Approx(s.turnSpeed));
        CHECK(t.stats.maxSpeed == doctest::Approx(s.maxSpeed));
        CHECK(t.stats.rcsAccel == doctest::Approx(s.rcsAccel));
        CHECK(t.stats.cargoCapacity == s.cargoCapacity);
        CHECK(t.stats.miningRate == doctest::Approx(s.miningRate));
        // In a fight every design has more hull than its parts and a gun its fitted guns add to.
        CHECK(t.stats.hull == doctest::Approx(s.hull + c.rules.baseHull));
        CHECK(t.stats.damage == doctest::Approx(s.damage + c.rules.baseGun));
        CHECK(ShipTypeOf(d->id) == &t);
    }
    CHECK(ShipTypeOf("no such design") == nullptr);
}

TEST_CASE("a player's ship flies, takes hits and looks as its design says")
{
    REQUIRE(Archetypes::Load(DataFile("archetypes.json")));
    const std::vector<ShipType>& catalog = GetShipCatalog();
    REQUIRE(catalog.size() >= 3);

    // `ship.player` is what a player's ship is; the starter's design is what it looks like
    // until it flies another. The dart that was drawn by hand is gone.
    const Archetype* player = Archetypes::Find("ship.player");
    REQUIRE(player != nullptr);
    CHECK(player->design == catalog[0].stats.design);
    // Not what an NPC of that design is drawn as: that is always an Npc archetype.
    REQUIRE(Archetypes::ForDesign(catalog[0].stats.design) != nullptr);
    CHECK(Archetypes::ForDesign(catalog[0].stats.design)->kind == EntityKind::Npc);

    Ship ship({ 0.0f, 0.0f }, catalog[0].stats);
    CHECK(ship.GetMaxHull() == doctest::Approx(catalog[0].stats.hull));
    CHECK(ship.GetHull() == doctest::Approx(ship.GetMaxHull()));
    CHECK(ship.GetDamage() == doctest::Approx(catalog[0].stats.damage));
    CHECK(ship.Describe().shape == &Archetypes::ForDesign(catalog[0].stats.design)->visual.shape);

    // Another hull: another shape, its own hull -- in the shape the last one was in.
    ship.TakeDamage(ship.GetShields() + ship.GetMaxHull() * 0.5f);
    REQUIRE(ship.GetHull() == doctest::Approx(ship.GetMaxHull() * 0.5f));
    ship.Refit(catalog[2].stats);
    CHECK(ship.GetMaxHull() == doctest::Approx(catalog[2].stats.hull));
    CHECK(ship.GetHull() == doctest::Approx(catalog[2].stats.hull * 0.5f));
    CHECK(ship.GetCargoCapacity() == catalog[2].stats.cargoCapacity);
    const Render::Item drawn = ship.Describe();
    CHECK(drawn.shape == &Archetypes::ForDesign(catalog[2].stats.design)->visual.shape);
    CHECK(drawn.kind == EntityKind::PlayerShip);

    // ...and its size: a hull is as big as an NPC flying that design, so a Hauler is drawn
    // and picked bigger than a Courier rather than every hull at one size.
    for (const ShipType& t : catalog)
    {
        ship.Refit(t.stats);
        const Archetype* look = Archetypes::ForDesign(t.stats.design);
        REQUIRE(look != nullptr);
        CHECK(ship.GetSize() == doctest::Approx(look->defaultSize));
        CHECK(ship.Describe().size == doctest::Approx(look->defaultSize));
    }
    ship.Refit(catalog[1].stats);  // the courier
    const float courier = ship.GetSize();
    ship.Refit(catalog[2].stats);  // the hauler
    CHECK(ship.GetSize() > courier);
}

TEST_CASE("the hangar: a list of designs, the starter first, and a mistake is a load error")
{
    CHECK(ErrorAfter([](nlohmann::json& j) { j.erase("hangar"); }).find("hangar") !=
          std::string::npos);
    CHECK(ErrorAfter([](nlohmann::json& j) { j["hangar"] = nlohmann::json::array(); })
              .find("hangar") != std::string::npos);
    CHECK(ErrorAfter([](nlohmann::json& j) { j["hangar"][1]["design"] = "dreadnought"; })
              .find("not a design") != std::string::npos);
    CHECK(ErrorAfter([](nlohmann::json& j) { j["hangar"][1]["design"] = "scout"; }).find("twice") !=
          std::string::npos);
    CHECK(ErrorAfter([](nlohmann::json& j) { j["hangar"][1].erase("price"); }).find("price") !=
          std::string::npos);
    CHECK(ErrorAfter([](nlohmann::json& j) { j["hangar"][1]["price"] = -5; }).find("price") !=
          std::string::npos);
    CHECK(ErrorAfter([](nlohmann::json& j) { j["hangar"][1]["cost"] = 5; }).find("cost") !=
          std::string::npos);
    CHECK(ErrorAfter([](nlohmann::json& j) { j["rules"].erase("baseHull"); }).find("baseHull") !=
          std::string::npos);
    CHECK(ErrorAfter([](nlohmann::json& j) { j["rules"].erase("baseGun"); }).find("baseGun") !=
          std::string::npos);
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
            if (r == Good::Iron())
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
        return m != nullptr &&
               (m->provides.thrust > 0.0f || m->provides.rcs > 0.0f || m->provides.cargo > 0.0f ||
                m->provides.mining > 0.0f || m->provides.scan > 0.0f || m->provides.hull > 0.0f ||
                m->provides.damage > 0.0f);
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
            if (a.design == d.id && a.kind == EntityKind::Npc)  // ship.player draws one too
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

// --- Step 4: NPC roles fly designs (#279) --------------------------------------------------

TEST_CASE("every faction flies a design in every role, and an armed role's design has guns")
{
    const Ships::Catalogue c = Shipped();
    for (const char* faction : { "Independent", "TradersGuild", "Syndicate", "Pirates" })
        for (const char* role : Ships::NPC_ROLES)
        {
            CAPTURE(faction);
            CAPTURE(role);
            const std::vector<std::string>* designs = c.DesignsFor(faction, role);
            REQUIRE(designs != nullptr);
            REQUIRE_FALSE(designs->empty());
            for (const std::string& id : *designs)
            {
                Ships::Stats s;
                std::string  error;
                REQUIRE(Ships::Derive(c, *c.FindDesign(id), s, error));
                CHECK(s.hull > 0.0f);
                CHECK(s.cruise > 0.0f);
                if (Ships::ArmedRole(role))
                    CHECK(s.damage > 0.0f);
            }
        }

    // A faction's own doctrine wins where it names the role; the default covers the rest. A
    // role with several designs flies each of them, and one ship keeps its own by its id.
    CHECK(c.Pick("TradersGuild", "trader", 7) == "hauler");
    CHECK(c.Pick("Syndicate", "patrol", 0) == "cruiser");
    CHECK(c.Pick("Syndicate", "patrol", 1) == "cutter");
    CHECK(c.Pick("Independent", "patrol", 1) == "cutter");
    CHECK(c.Pick("Pirates", "pirate", 3) == "raider");
}

TEST_CASE("NPCs fly within 15% of what every NPC flew before designs (#279 step 4)")
{
    // Before step 4 every NPC was the same: a hull of 60, a volley of 6 against a ship, and a
    // speed rolled between 120 and 180. The switch kept every role's parts close to that. Since
    // #186 an NPC fights with what the same design has under a player -- these plus the
    // catalogue's baseHull and baseGun -- so the parts still order the roles, and the fight is
    // a player's.
    const Ships::Catalogue c = Shipped();
    for (const Ships::Doctrine& d : c.doctrines)
        for (const auto& [role, designs] : d.roles)
            for (const std::string& id : designs)
            {
                Ships::Stats s;
                std::string  error;
                REQUIRE(Ships::Derive(c, *c.FindDesign(id), s, error));
                INFO(d.faction << " " << id);
                CHECK(Within(s.cruise, 150.0f, 0.15f));
                CHECK(Within(s.hull, 60.0f, 0.15f));
                if (Ships::ArmedRole(role))
                    CHECK(Within(s.damage, 6.0f, 0.15f));
                else
                    CHECK(s.damage == 0.0f);
            }
}

TEST_CASE("a doctrine that cannot be flown is a load error")
{
    CHECK(ErrorAfter([](nlohmann::json& j)
                     { j["doctrines"]["Syndicate"]["smuggler"] = { "hauler" }; })
              .find("smuggler") != std::string::npos);
    CHECK(
        ErrorAfter([](nlohmann::json& j) { j["doctrines"]["Merchants"]["trader"] = { "hauler" }; })
            .find("Merchants") != std::string::npos);
    CHECK(ErrorAfter([](nlohmann::json& j) { j["doctrines"]["Pirates"]["pirate"] = { "galleon" }; })
              .find("galleon") != std::string::npos);
    // A patrol that cannot shoot is a trader in a uniform.
    CHECK(ErrorAfter([](nlohmann::json& j) { j["doctrines"]["Pirates"]["pirate"] = { "hauler" }; })
              .find("guns") != std::string::npos);
    // The default names a design for every role, so no NPC goes without one.
    CHECK(ErrorAfter([](nlohmann::json& j) { j["doctrines"]["default"].erase("miner"); })
              .find("miner") != std::string::npos);
    CHECK_FALSE(ErrorAfter([](nlohmann::json& j) { j.erase("doctrines"); }).empty());
    CHECK_FALSE(
        ErrorAfter([](nlohmann::json& j) { j["modules"][0]["provides"]["armour"] = 1; }).empty());
}

TEST_CASE("an NPC is drawn as the design it flies and is as fast, tough and armed as it says")
{
    REQUIRE(Archetypes::Load(DataFile("archetypes.json")));
    const Ships::Catalogue& c = Archetypes::ShipCatalogue();

    // `ship.npc` is gone: there is no shape that stands for every role any more.
    CHECK(Archetypes::Find("ship.npc") == nullptr);

    NpcShip      raider({ 0.0f, 0.0f }, Fac::Pirates(), NpcRole::Pirate, {}, "raider");
    Ships::Stats s;
    std::string  error;
    REQUIRE(Ships::Derive(c, *c.FindDesign("raider"), s, error));
    CHECK(raider.GetDesign() == "raider");
    REQUIRE(raider.GetArchetype() != nullptr);
    CHECK(raider.GetArchetype()->design == "raider");
    // In a fight it is what the design is under a player too (#186): its parts plus the
    // catalogue's baseHull and baseGun.
    CHECK(raider.GetMaxHull() == doctest::Approx(s.combatHull));
    CHECK(raider.GetDamage() == doctest::Approx(s.volley));
    CHECK(raider.GetSpeed() == doctest::Approx(s.cruise));
    CHECK(raider.GetSize() == doctest::Approx(raider.GetArchetype()->defaultSize));

    // No design, or one this build does not know, is the doctrine's own pick for the role --
    // never nothing, and never another role's ship.
    NpcShip trader({ 0.0f, 0.0f }, Fac::TradersGuild(), NpcRole::Trader, {});
    CHECK(trader.GetDesign() == c.Pick("TradersGuild", "trader", 0));
    NpcShip stranger({ 0.0f, 0.0f }, Fac::Syndicate(), NpcRole::Police, {}, "dreadnought");
    CHECK(stranger.GetDesign() == c.Pick("Syndicate", "patrol", 0));

    // Every NpcRole has the name the doctrines use.
    for (NpcRole r : { NpcRole::Trader, NpcRole::Miner, NpcRole::Police, NpcRole::Pirate,
                       NpcRole::Warship, NpcRole::Guard })
        CHECK(c.DesignsFor("default", NpcRoleId(r)) != nullptr);
}

// Variants (#464): every armed stock design comes with every family's guns on its own hull and
// on every hull line, made from data on both sides of the wire.
TEST_CASE("every armed stock design has a variant per family and line, and each one derives")
{
    const Ships::Catalogue c = Shipped();
    REQUIRE(c.families.size() >= 2);
    REQUIRE(c.lines.size() >= 3);
    int armed = 0;
    for (const Ships::Design& d : c.designs)
    {
        const Ships::Family* own = c.FamilyOf(d);
        if (own == nullptr)
        {
            for (const Ships::Family& f : c.families)
                CHECK(c.VariantOf(d.id, f.id, "").empty());
            continue;
        }
        armed++;
        CAPTURE(d.id);
        for (const Ships::Family& f : c.families)
            for (size_t l = 0; l <= c.lines.size(); ++l)
            {
                const std::string line = l == 0 ? std::string() : c.lines[l - 1].id;
                const std::string id = c.VariantOf(d.id, f.id, line);
                CAPTURE(id);
                if (&f == own && line.empty())
                {
                    CHECK(id == d.id);
                    continue;
                }
                const Ships::Variant* v = c.FindVariant(id);
                REQUIRE(v != nullptr);
                CHECK(v->base == d.id);
                CHECK(v->family == f.id);
                CHECK(v->line == line);
                CHECK(c.FindDesign(id) == &v->design);
                CHECK(c.FamilyOf(v->design) == &f);
                Ships::Stats s;
                std::string  error;
                CHECK_MESSAGE(Ships::Derive(c, v->design, s, error), error);
                CHECK(s.damage > 0.0f);
            }
    }
    CHECK(armed >= 3);  // the cutter, the raider and the cruiser
    CHECK(c.variants.size() == (size_t)armed * (c.families.size() * (c.lines.size() + 1) - 1));
    // A stock design keeps its id, and no player's design is taken for a variant.
    CHECK(c.FindVariant("cutter") == nullptr);
    CHECK(c.FindVariant("u:0000abcd") == nullptr);
}

TEST_CASE("a variant's guns are its family's, as many as before; its line changes its hull")
{
    const Ships::Catalogue c = Shipped();
    const Ships::Design*   cutter = c.FindDesign("cutter");
    const Ships::Design*   rail = c.FindDesign("cutter~railgun");
    REQUIRE(cutter != nullptr);
    REQUIRE(rail != nullptr);
    auto guns = [](const Ships::Design& d, const char* module)
    {
        int n = 0;
        for (const Ships::FitLine& l : d.fit)
            if (l.module == module)
                n += l.count;
        return n;
    };
    CHECK(guns(*cutter, "weapons.laser") == 2);
    CHECK(guns(*rail, "weapons.laser") == 0);
    CHECK(guns(*rail, "weapons.railgun") == 2);
    // ...on the stock hull, and nothing else changed.
    CHECK(rail->bow == cutter->bow);
    CHECK(rail->mids == cutter->mids);
    CHECK(rail->stern == cutter->stern);
    CHECK(rail->fit.size() == cutter->fit.size());
    CHECK(rail->name == "Cutter (railguns)");
    Ships::Stats a, b;
    std::string  error;
    REQUIRE(Ships::Derive(c, *cutter, a, error));
    REQUIRE(Ships::Derive(c, *rail, b, error));
    CHECK(b.damage == doctest::Approx(2 * c.FindModule("weapons.railgun")->provides.damage));
    CHECK(b.mass > a.mass);

    // Each line turns at least one design's hull, so a line is never a name for nothing.
    for (const Ships::Line& line : c.lines)
    {
        CAPTURE(line.id);
        bool turned = false;
        for (const Ships::Variant& v : c.variants)
            if (v.line == line.id)
            {
                const Ships::Design* base = c.FindDesign(v.base);
                turned = turned || v.design.bow != base->bow || v.design.mids != base->mids ||
                         v.design.stern != base->stern;
            }
        CHECK(turned);
    }
    // The same catalogue makes the same variants, every time and on every side.
    const Ships::Catalogue again = Shipped();
    REQUIRE(again.variants.size() == c.variants.size());
    for (size_t i = 0; i < c.variants.size(); ++i)
        CHECK(Ships::ToJson(again.variants[i].design) == Ships::ToJson(c.variants[i].design));
}

TEST_CASE("a family or a line that cannot be one is a load error")
{
    CHECK(
        ErrorAfter([](nlohmann::json& j) { j["families"]["laser"]["modules"] = { "hull.armour" }; })
            .find("is not a gun") != std::string::npos);
    CHECK(ErrorAfter([](nlohmann::json& j)
                     { j["families"]["railgun"]["modules"] = { "weapons.laser" }; })
              .find("already in") != std::string::npos);
    CHECK(ErrorAfter([](nlohmann::json& j) { j["families"]["laser"]["goods"] = { "Unobtainium" }; })
              .find("is not a good") != std::string::npos);
    CHECK(ErrorAfter([](nlohmann::json& j) { j["families"]["laser"]["colour"] = "red"; }) != "");
    CHECK(ErrorAfter([](nlohmann::json& j) { j["lines"]["heavy"]["bow"] = { "mid.belt" }; })
              .find("is not a bow section") != std::string::npos);
    CHECK(ErrorAfter([](nlohmann::json& j) { j["lines"]["heavy"].erase("goods"); }) != "");
    // A catalogue may have none: then nothing has variants.
    nlohmann::json j = ShippedJson();
    j.erase("families");
    j.erase("lines");
    Ships::Catalogue c;
    std::string      error;
    REQUIRE_MESSAGE(Ships::Parse(j, c, error), error);
    CHECK(c.variants.empty());
}

TEST_CASE("a variant is drawn and flown as its stock design is, with its own shape and numbers")
{
    REQUIRE(Archetypes::Load(DataFile("archetypes.json")));
    const Ships::Catalogue& c = Archetypes::ShipCatalogue();
    REQUIRE(!c.variants.empty());
    for (const Ships::Variant& v : c.variants)
    {
        CAPTURE(v.design.id);
        const Archetype* look = Archetypes::ForDesign(v.design.id);
        const Archetype* base = Archetypes::ForDesign(v.base);
        REQUIRE(look != nullptr);
        REQUIRE(base != nullptr);
        CHECK(look->design == v.design.id);
        CHECK(look->visual.glyph == base->visual.glyph);
        CHECK(Archetypes::FindDesign(v.design.id) == &v.design);
        Ships::Stats s;
        std::string  error;
        REQUIRE(Ships::Derive(c, v.design, s, error));
        CHECK(look->ship.volley == doctest::Approx(s.volley));
        CHECK(look->ship.combatHull == doctest::Approx(s.combatHull));
    }
    NpcShip pirate({ 0.0f, 0.0f }, Fac::Pirates(), NpcRole::Pirate, {}, "raider~laser~light");
    CHECK(pirate.GetDesign() == "raider~laser~light");
}
