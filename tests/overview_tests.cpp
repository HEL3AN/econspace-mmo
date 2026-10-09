#include <doctest/doctest.h>

#include "sim/Overview.h"
#include <string>

// The overview is the instrument a player flies by (#157). What it lists and in what order
// is decided without a window, so it is tested here.

namespace
{
Proto::EntitySnapshot Ent(int id, Proto::EntityKind kind, float x, const char* name, int role = -1)
{
    Proto::EntitySnapshot e;
    e.id = id;
    e.kind = kind;
    e.pos = { x, 0.0f };
    e.name = name;
    e.role = role;
    return e;
}

std::vector<Proto::EntitySnapshot> World()
{
    return {
        Ent(1, Proto::EntityKind::Star, 5000.0f, "Sol"),
        Ent(2, Proto::EntityKind::Station, 900.0f, "Aurora Hub"),
        Ent(3, Proto::EntityKind::Gate, 20000.0f, "Gate to Reach"),
        Ent(4, Proto::EntityKind::Npc, 300.0f, "Guild trader", 0),
        Ent(5, Proto::EntityKind::Npc, 7000.0f, "Raider", 3),
        Ent(6, Proto::EntityKind::Field, 1500.0f, "Iron Belt"),
        Ent(7, Proto::EntityKind::PlayerShip, 400.0f, "bob"),
    };
}

bool PirateIsHostile(const Proto::EntitySnapshot& e)
{
    return e.kind == Proto::EntityKind::Npc && e.role == 3;
}

std::vector<int> Ids(const std::vector<Overview::Row>& rows)
{
    std::vector<int> ids;
    for (const Overview::Row& r : rows)
        ids.push_back(r.entity->id);
    return ids;
}
}  // namespace

TEST_CASE("the overview lists everything nearest first, with what would shoot you on top")
{
    const auto w = World();
    const auto rows = Overview::Build(w, { 0, 0 }, Overview::Filter::All, Overview::Sort::Distance,
                                      PirateIsHostile);
    // The raider is the furthest thing here and still comes first: it is the reason to
    // change plan, the same rule the agent's projection follows.
    CHECK(Ids(rows) == std::vector<int>{ 5, 4, 7, 2, 6, 1, 3 });
    CHECK(rows[0].hostile);
    CHECK(rows[0].kind == "pirate");
}

TEST_CASE("each tab is what a player is choosing between")
{
    const auto w = World();
    auto       ids = [&](Overview::Filter f)
    { return Ids(Overview::Build(w, { 0, 0 }, f, Overview::Sort::Distance, PirateIsHostile)); };

    CHECK(ids(Overview::Filter::Destinations) == std::vector<int>{ 2, 3 });  // dock or leave
    CHECK(ids(Overview::Filter::Ships) == std::vector<int>{ 5, 4, 7 });      // NPCs and players
    CHECK(ids(Overview::Filter::Features) == std::vector<int>{ 6, 1 });
    CHECK(ids(Overview::Filter::Hostile) == std::vector<int>{ 5 });
}

TEST_CASE("sorting by name or by kind, and ties never reshuffle")
{
    const auto w = World();
    const auto byName = Overview::Build(w, { 0, 0 }, Overview::Filter::Destinations,
                                        Overview::Sort::Name, PirateIsHostile);
    CHECK(Ids(byName) == std::vector<int>{ 2, 3 });  // Aurora before Gate

    // Two things of one kind at one distance: decided by id, the same way every frame. A list
    // that reshuffles under the cursor is a list nobody can click.
    std::vector<Proto::EntitySnapshot> twins = { Ent(9, Proto::EntityKind::Station, 100.0f, "Twin"),
                                                 Ent(8, Proto::EntityKind::Station, 100.0f,
                                                     "Twin") };
    for (int i = 0; i < 3; i++)
        CHECK(Ids(Overview::Build(twins, { 0, 0 }, Overview::Filter::All, Overview::Sort::Name,
                                  PirateIsHostile)) == std::vector<int>{ 8, 9 });
}

TEST_CASE("a row says what a thing does, not what class it is")
{
    CHECK(std::string(Overview::KindWord(Ent(1, Proto::EntityKind::Npc, 0, "", 1))) == "miner");
    CHECK(std::string(Overview::KindWord(Ent(1, Proto::EntityKind::PlayerShip, 0, ""))) == "pilot");
    CHECK(std::string(Overview::KindWord(Ent(1, Proto::EntityKind::Field, 0, ""))) == "belt");

    // A thing with no name is still listed, under what it is.
    const auto rows =
        Overview::Build({ Ent(1, Proto::EntityKind::Planet, 10.0f, "") }, { 0, 0 },
                        Overview::Filter::All, Overview::Sort::Distance, PirateIsHostile);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].name == "planet");
}
