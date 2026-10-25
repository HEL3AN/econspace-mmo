#include <doctest/doctest.h>

#include "sim/Events.h"

#include <set>
#include <string>

// The journal's kinds by name (#487). A save is read with KindFrom and an agent branches on
// KindName, so a kind that does not come back from its own name is written into a save and
// lost on the next load.

TEST_CASE("every journal kind comes back from its own name, and no two share one")
{
    std::set<std::string> names;
    for (int k = 0; k < (int)Ev::Kind::Count; k++)
    {
        const Ev::Kind    kind = (Ev::Kind)k;
        const std::string name = Ev::KindName(kind);
        CAPTURE(name);
        Ev::Kind back = Ev::Kind::Count;
        REQUIRE(Ev::KindFrom(name, back));
        CHECK(back == kind);
        CHECK(names.insert(name).second);
    }
    CHECK(names.size() == (size_t)Ev::Kind::Count);
}

TEST_CASE("a name no kind has is refused and leaves the kind as it was")
{
    Ev::Kind out = Ev::Kind::Docked;
    CHECK_FALSE(Ev::KindFrom("warp_drive_exploded", out));
    CHECK(out == Ev::Kind::Docked);
    CHECK_FALSE(Ev::KindFrom("", out));
    CHECK_FALSE(Ev::KindFrom("Docked", out));  // names are lower case, as agents see them
    CHECK(out == Ev::Kind::Docked);
}

TEST_CASE("the names are the ones the agent guide lists")
{
    // docs/agents/README.md, "Waiting: the event journal". An agent matches on these.
    CHECK(std::string(Ev::KindName(Ev::Kind::Notice)) == "notice");
    CHECK(std::string(Ev::KindName(Ev::Kind::OrderDone)) == "order_done");
    CHECK(std::string(Ev::KindName(Ev::Kind::OrderFailed)) == "order_failed");
    CHECK(std::string(Ev::KindName(Ev::Kind::Docked)) == "docked");
    CHECK(std::string(Ev::KindName(Ev::Kind::CargoFull)) == "cargo_full");
    CHECK(std::string(Ev::KindName(Ev::Kind::ShipDestroyed)) == "ship_destroyed");
    CHECK(std::string(Ev::KindName(Ev::Kind::StructureAttacked)) == "structure_attacked");
}
