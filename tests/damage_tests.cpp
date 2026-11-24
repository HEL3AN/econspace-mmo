#include <doctest/doctest.h>

#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Fitting.h"
#include "core/ShipDesign.h"
#include "entities/NpcShip.h"
#include "entities/Ship.h"
#include "entities/Structure.h"
#include "entities/ShipType.h"
#include "factions_named.h"
#include "sim/Protocol.h"
#include "temp_dir.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <string>

// Damage types and layers (#464, slice 8): every weapon damages every layer, only better or
// worse, for a hit and for the cold pass alike; the owner's rule is that no gun is pointless.

namespace
{
std::string DataFile(const char* name)
{
    return std::string(TEST_DATA_DIR) + name;
}

Ships::Catalogue Shipped()
{
    Ships::Catalogue c;
    std::string      error;
    REQUIRE_MESSAGE(Ships::Load(DataFile("ships.json"), c, error), error);
    return c;
}

size_t TypeOf(const Ships::Catalogue& c, const char* name)
{
    for (size_t i = 0; i < c.damageTypes.size(); i++)
        if (c.damageTypes[i] == name)
            return i;
    FAIL("no such damage type: " << name);
    return 0;
}

Ships::TypeValues Pure(const Ships::Catalogue& c, const char* type)
{
    Ships::TypeValues m{};
    m[TypeOf(c, type)] = 1.0f;
    return m;
}
}  // namespace

TEST_CASE("every weapon damages every layer: lasers better against shields, railguns armour")
{
    const Ships::Catalogue c = Shipped();
    const auto             none = Ships::NeutralTable();
    const auto             energy = Pure(c, "energy"), kinetic = Pure(c, "kinetic");
    CHECK(Ships::Effect(c, energy, none, Ships::LayerShield) == doctest::Approx(1.25f));
    CHECK(Ships::Effect(c, kinetic, none, Ships::LayerShield) == doctest::Approx(0.75f));
    CHECK(Ships::Effect(c, energy, none, Ships::LayerArmour) == doctest::Approx(0.75f));
    CHECK(Ships::Effect(c, kinetic, none, Ships::LayerArmour) == doctest::Approx(1.25f));
    CHECK(Ships::Effect(c, energy, none, Ships::LayerHull) == doctest::Approx(1.0f));
    // The base gun is untyped: 1.0 everywhere, whatever the layer.
    for (int layer = 0; layer < Ships::LayerCount; layer++)
        CHECK(Ships::Effect(c, Ships::TypeValues{}, none, layer) == doctest::Approx(1.0f));
    // No family does less than minEffect to any layer of any line -- the load enforces it.
    for (const Ships::Family& f : c.families)
        for (int layer = 0; layer < Ships::LayerCount; layer++)
        {
            CHECK(Ships::Effect(c, f.damage, none, layer) >= c.minEffect);
            for (const Ships::Line& l : c.lines)
                CHECK(Ships::Effect(c, f.damage, l.resist, layer) >= c.minEffect);
        }
}

TEST_CASE("a table that would let a gun fall below minEffect is a load error")
{
    nlohmann::json j;
    {
        std::ifstream in(DataFile("ships.json"));
        REQUIRE(in.is_open());
        j = nlohmann::json::parse(in);
    }
    j["layers"]["armour"]["energy"] = 0.4;  // a laser would do 0.4 to armour
    Ships::Catalogue c;
    std::string      error;
    CHECK_FALSE(Ships::Parse(j, c, error));
    CHECK(error.find("minEffect") != std::string::npos);
}

TEST_CASE("a hit takes shield, armour and hull in order, each at its multiplier")
{
    const Ships::Catalogue c = Shipped();
    const auto             none = Ships::NeutralTable();
    const auto             energy = Pure(c, "energy"), kinetic = Pure(c, "kinetic");

    // Untyped: the old arithmetic, exactly -- shields, then the rest.
    Ships::Pools p{ 60.0f, 10.0f, 100.0f };
    Ships::Strike(c, Ships::TypeValues{}, none, 50.0f, p);
    CHECK(p.shield == doctest::Approx(10.0f));
    CHECK(p.armour == doctest::Approx(10.0f));
    CHECK(p.hull == doctest::Approx(100.0f));

    // A laser takes more off a shield than a railgun does; a railgun more off armour.
    Ships::Pools a{ 100.0f, 0.0f, 100.0f }, b = a;
    Ships::Strike(c, energy, none, 40.0f, a);
    Ships::Strike(c, kinetic, none, 40.0f, b);
    CHECK(a.shield == doctest::Approx(50.0f));  // 40 x 1.25
    CHECK(b.shield == doctest::Approx(70.0f));  // 40 x 0.75
    Ships::Pools x{ 0.0f, 100.0f, 100.0f }, y = x;
    Ships::Strike(c, energy, none, 40.0f, x);
    Ships::Strike(c, kinetic, none, 40.0f, y);
    CHECK(x.armour == doctest::Approx(70.0f));
    CHECK(y.armour == doctest::Approx(50.0f));

    // What a broken layer did not hold goes on to the next, raw: a 100-point shot into 25 of
    // shield at 1.25 uses 20 of it and 80 goes through.
    Ships::Pools q{ 25.0f, 0.0f, 100.0f };
    Ships::Strike(c, energy, none, 100.0f, q);
    CHECK(q.shield == doctest::Approx(0.0f));
    CHECK(q.hull == doctest::Approx(20.0f));
}

TEST_CASE("armour is part of a design's structure, and the guns say what they are made of")
{
    const Ships::Catalogue c = Shipped();
    const Ships::Design*   cruiser = c.FindDesign("cruiser");
    REQUIRE(cruiser != nullptr);
    Ships::Stats s;
    std::string  why;
    REQUIRE(Ships::Derive(c, *cruiser, s, why));
    CHECK(s.armour > 0.0f);
    CHECK(s.combatHull == doctest::Approx(s.hull + s.armour + c.rules.baseHull));
    // A railgun design's volley is mostly kinetic; the rest, the base gun, is untyped.
    float typed = 0.0f;
    for (float m : s.mix)
        typed += m;
    CHECK(typed > 0.0f);
    CHECK(typed < 1.0f);
    CHECK(s.mix[TypeOf(c, "kinetic")] > s.mix[TypeOf(c, "energy")]);
    // The same hull in lasers is energy instead, and a heavy line resists kinetic on armour.
    Ships::Stats l;
    REQUIRE(Ships::Derive(c, *c.FindDesign("cruiser~laser"), l, why));
    CHECK(l.mix[TypeOf(c, "energy")] > l.mix[TypeOf(c, "kinetic")]);
    Ships::Stats h;
    REQUIRE(Ships::Derive(c, *c.FindDesign("cruiser~railgun~heavy"), h, why));
    CHECK(h.resist[Ships::LayerArmour][TypeOf(c, "kinetic")] == doctest::Approx(0.9f));
    CHECK(s.resist[Ships::LayerArmour][TypeOf(c, "kinetic")] == doctest::Approx(1.0f));
}

TEST_CASE("an NPC's armour goes before its hull, and the cold pass bites by the same layers")
{
    REQUIRE(Archetypes::Load(DataFile("archetypes.json")));
    const Ships::Catalogue& c = Archetypes::ShipCatalogue();
    NpcShip ship({ 0.0f, 0.0f }, Fac::Pirates(), NpcRole::Pirate, std::vector<Vector2>{},
                 "cruiser");
    REQUIRE(ship.GetMaxArmour() > 0.0f);
    const float armour = ship.GetMaxArmour(), hull = ship.GetMaxHull();
    ship.TakeDamage(armour * 0.5f, Ships::TypeValues{});
    CHECK(ship.GetArmour() == doctest::Approx(armour * 0.5f));
    CHECK(ship.GetHull() == doctest::Approx(hull));
    ship.TakeDamage(armour * 10.0f, Ships::TypeValues{});
    CHECK(ship.GetArmour() == doctest::Approx(0.0f));
    CHECK(ship.GetHull() < hull);

    // Untyped fire is dps over structure, as the cold pass had it; against an armoured target
    // a railgun bites more than a laser of the same dps, and a laser still bites.
    Ships::Fighter plain{ 6.0f, 100.0f };
    CHECK(Ships::Bite(c, plain, plain) == doctest::Approx(6.0f / 100.0f));
    const Ships::Fighter target = Ships::DesignFighter(c, "cruiser");
    Ships::Fighter       rails = Ships::DesignFighter(c, "cruiser");
    Ships::Fighter       beams = Ships::DesignFighter(c, "cruiser~laser");
    rails.dps = beams.dps = 6.0f;
    CHECK(Ships::Bite(c, rails, target) > Ships::Bite(c, beams, target));
    CHECK(Ships::Bite(c, beams, target) > 0.0f);
}

TEST_CASE("armour travels: a snapshot's entity and a player's view")
{
    Proto::Snapshot s;
    s.entities.push_back({});
    s.entities.back().id = 4;
    s.entities.back().kind = Proto::EntityKind::Npc;
    s.entities.back().armourFrac = 0.25f;
    s.player.armour = 3.0f;
    s.player.maxArmour = 12.0f;
    Proto::Snapshot back;
    REQUIRE(Proto::DecodeSnapshot(Proto::EncodeSnapshot(s), back));
    REQUIRE(back.entities.size() == 1);
    CHECK(back.entities[0].armourFrac == doctest::Approx(0.25f).epsilon(0.01));
    CHECK(back.player.armour == doctest::Approx(3.0f));
    CHECK(back.player.maxArmour == doctest::Approx(12.0f));
}

TEST_CASE("a structure's walls have layers: armour first, railguns best against it (#557)")
{
    REQUIRE(Archetypes::Load(DataFile("archetypes.json")));
    REQUIRE(Blueprints::Load(DataFile("blueprints.json")));
    const Ships::Catalogue& c = Archetypes::ShipCatalogue();
    const Blueprint*        bp = Blueprints::Find("outpost");
    REQUIRE(bp != nullptr);
    REQUIRE(bp->armour > 0.0f);

    auto wall = [&]
    {
        Structure s({ 0.0f, 0.0f }, 100.0f, "wall", bp->archetype);
        s.SetBlueprint("outpost");
        return s;
    };
    // Inside the armour a hit takes its multiplier: a laser 0.75, a railgun 1.25, untyped 1.
    Structure beam = wall(), slug = wall(), plain = wall();
    beam.StrikeWith(100.0f, Pure(c, "energy"), 0.0);
    slug.StrikeWith(100.0f, Pure(c, "kinetic"), 0.0);
    plain.StrikeWith(100.0f, Ships::TypeValues{}, 0.0);
    CHECK(beam.GetDamage(0.0) == doctest::Approx(75.0f));
    CHECK(slug.GetDamage(0.0) == doctest::Approx(125.0f));
    CHECK(plain.GetDamage(0.0) == doctest::Approx(100.0f));

    // Past the armour the hull takes it at the hull's 1: the carry-over is raw, as a ship's.
    Structure   broken = wall();
    const float armour = bp->armour * bp->hull;
    broken.StrikeWith(armour / 1.25f + 100.0f, Pure(c, "kinetic"), 0.0);
    CHECK(broken.GetDamage(0.0) == doctest::Approx(armour + 100.0f));
    // ...and nothing is left of a wall that is shot to pieces.
    broken.StrikeWith(bp->hull * 10.0f, Pure(c, "energy"), 0.0);
    CHECK(broken.HullFraction(0.0) == doctest::Approx(0.0f));

    // The planner's one number agrees: kinetic strips a wall faster than energy, untyped is 1.
    CHECK(Blueprints::WallEffect(*bp, Pure(c, "kinetic")) >
          Blueprints::WallEffect(*bp, Pure(c, "energy")));
    CHECK(Blueprints::WallEffect(*bp, Ships::TypeValues{}) == doctest::Approx(1.0f));
    const float k = Blueprints::WallEffect(*bp, Pure(c, "kinetic"));
    CHECK(k > 1.0f);
    CHECK(k < 1.25f);  // the hull's layer is 1 and is part of it
}

TEST_CASE("a tier that would let a gun fall below minEffect is a load error (#557)")
{
    REQUIRE(Archetypes::Load(DataFile("archetypes.json")));
    const TempDir tmp;
    auto          loads = [&](const char* tiers)
    {
        const std::string path = tmp.Path("bp.json");
        {
            std::ofstream f(path);
            f << "{ \"tiers\": { " << tiers << " }, \"blueprints\": [ { \"id\": \"b\", "
              << "\"name\": \"B\", \"archetype\": \"structure.beacon\", "
              << "\"cost\": { \"Iron\": 1 }, \"buildSeconds\": 1, \"hull\": 10, "
              << "\"tier\": \"t\", \"placement\": { \"reach\": 100 } } ] }";
        }
        return Blueprints::Load(path);
    };
    CHECK(loads("\"t\": { \"mend\": 0.01, \"armour\": 0.5 }"));
    // Kinetic against armour at 1.25 * 0.4 would be 0.5, under the 0.6 floor.
    CHECK_FALSE(
        loads("\"t\": { \"mend\": 0.01, \"resist\": { \"armour\": { \"kinetic\": 0.4 } } }"));
    CHECK_FALSE(loads("\"t\": { \"mend\": 0.01, \"armour\": 1.5 }"));  // a share, at most 1
    CHECK_FALSE(loads("\"t\": { \"mend\": 0.01, \"resist\": { \"cloak\": {} } }"));
}

TEST_CASE("a battery's guns have a damage type (#557)")
{
    REQUIRE(Archetypes::Load(DataFile("archetypes.json")));
    const Archetype* station = Archetypes::Find("station.military");
    REQUIRE(station != nullptr);
    CHECK(station->weaponType == "energy");
    const Ships::TypeValues mix = Ships::MixOf(Archetypes::ShipCatalogue(), station->weaponType);
    CHECK(mix[TypeOf(Archetypes::ShipCatalogue(), "energy")] == doctest::Approx(1.0f));
    // A name the catalogue lacks is untyped here (the archetype load refuses it earlier).
    CHECK(Ships::MixOf(Archetypes::ShipCatalogue(), "plasma") == Ships::TypeValues{});
}
