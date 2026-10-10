#include <doctest/doctest.h>

#include "core/Actions.h"

#include <fstream>
#include <set>
#include <sstream>
#include <string>

// One list of actions for a thing (#297): what the context menu, the selected-item window
// and the overview's right click offer, and what an agent's tools mirror.

using Actions::Action;
using Actions::Target;
using Actions::Verb;

namespace
{
Target Thing(EntityKind kind, int id = 7, Vector2 pos = { 5000.0f, 0.0f }, float size = 100.0f)
{
    Target t;
    t.id = id;
    t.kind = kind;
    t.pos = pos;
    t.size = size;
    t.name = "Thing";
    return t;
}

bool Has(const std::vector<Action>& a, Verb v)
{
    for (const Action& x : a)
        if (x.verb == v)
            return true;
    return false;
}

const Action* Find(const std::vector<Action>& a, Verb v)
{
    for (const Action& x : a)
        if (x.verb == v)
            return &x;
    return nullptr;
}

// Every tool econagent offers, from its generated reference (CI keeps that in step with
// the binary).
std::set<std::string> AgentTools()
{
    std::ifstream in(std::string(TEST_DATA_DIR) + "../docs/agents/reference.md");
    REQUIRE(in);
    std::set<std::string> tools;
    std::string           line;
    while (std::getline(in, line))
        if (line.rfind("### `", 0) == 0)
        {
            const size_t end = line.find('`', 5);
            if (end != std::string::npos)
                tools.insert(line.substr(5, end - 5));
        }
    return tools;
}
}  // namespace

TEST_CASE("Actions: a station far away offers the common ones, the holds, a warp and docking")
{
    const std::vector<Action> a = Actions::For(Thing(EntityKind::Station), { 0.0f, 0.0f });
    CHECK(a.front().verb == Verb::Select);
    CHECK(Has(a, Verb::Approach));
    CHECK(Has(a, Verb::Orbit));
    CHECK(Has(a, Verb::Keep));
    CHECK(Has(a, Verb::Follow));
    CHECK(Has(a, Verb::SetRange));
    CHECK(Has(a, Verb::Warp));
    CHECK(Has(a, Verb::Dock));
    CHECK_FALSE(Has(a, Verb::Mine));

    // The holds are offered at multiples of the thing's own size.
    const Action* keep = Find(a, Verb::Keep);
    REQUIRE(keep != nullptr);
    CHECK(keep->distance == doctest::Approx(Actions::DefaultRange(100.0f)));
    CHECK(Actions::HoldMode(Verb::Orbit) == 3);
    CHECK(Actions::HoldMode(Verb::Keep) == 4);
    CHECK(Actions::HoldMode(Verb::Follow) == 5);
    CHECK(Actions::HoldMode(Verb::Dock) == 0);
}

TEST_CASE("Actions: close by there is no warp, and a thing without an id cannot be held on")
{
    const std::vector<Action> near = Actions::For(Thing(EntityKind::Planet), { 4900.0f, 0.0f });
    CHECK_FALSE(Has(near, Verb::Warp));
    const std::vector<Action> anon = Actions::For(Thing(EntityKind::Nebula, 0), { 0.0f, 0.0f });
    CHECK_FALSE(Has(anon, Verb::Orbit));
    CHECK_FALSE(Has(anon, Verb::SetRange));
    CHECK(Has(anon, Verb::Approach));
}

TEST_CASE("Actions: each kind offers what it is for")
{
    const Vector2 from{ 0.0f, 0.0f };
    CHECK(Has(Actions::For(Thing(EntityKind::Field), from), Verb::Mine));
    CHECK(Has(Actions::For(Thing(EntityKind::Npc), from), Verb::Attack));
    Target wreck = Thing(EntityKind::Derelict);
    CHECK(Has(Actions::For(wreck, from), Verb::Investigate));
    wreck.looted = true;  // a wreck changes once (#38)
    CHECK_FALSE(Has(Actions::For(wreck, from), Verb::Investigate));

    Target        gate = Thing(EntityKind::Gate);
    const Action* jump = Find(Actions::For(gate, from), Verb::Jump);
    REQUIRE(jump != nullptr);
    CHECK(jump->label == "Jump");
    gate.destinationName = "Vela";
    jump = nullptr;
    const std::vector<Action> named = Actions::For(gate, from);
    jump = Find(named, Verb::Jump);
    REQUIRE(jump != nullptr);
    CHECK(jump->label == "Jump to Vela");
}

TEST_CASE("Actions: a point in space offers flying, warping, and what can be built in reach")
{
    Blueprint beacon;
    beacon.id = "beacon";
    beacon.name = "Beacon";
    beacon.reach = 500.0f;
    const std::vector<Blueprint> bps{ beacon };

    const std::vector<Action> near = Actions::ForPoint({ 100.0f, 0.0f }, { 0.0f, 0.0f }, bps);
    CHECK(Has(near, Verb::FlyHere));
    CHECK_FALSE(Has(near, Verb::WarpHere));
    const Action* build = Find(near, Verb::Build);
    REQUIRE(build != nullptr);
    CHECK(build->blueprint == "beacon");

    const std::vector<Action> far = Actions::ForPoint({ 9000.0f, 0.0f }, { 0.0f, 0.0f }, bps);
    CHECK(Has(far, Verb::WarpHere));
    CHECK_FALSE(Has(far, Verb::Build));
}

TEST_CASE("Actions: every button an agent could want is an econagent tool, and the gaps are known")
{
    // The design rule (#297): a window offers nothing an agent cannot do. The interface's own
    // verbs need no tool; the rest name one that exists, except the two listed here, which
    // are owed one. Adding the tool is the way off this list.
    const std::set<std::string> tools = AgentTools();
    REQUIRE_FALSE(tools.empty());
    const std::set<Verb> interfaceOnly{ Verb::Select, Verb::SetRange };
    const std::set<Verb> owed{ Verb::Attack, Verb::Investigate };

    for (Verb v :
         { Verb::Select, Verb::Approach, Verb::Orbit, Verb::Keep, Verb::Follow, Verb::SetRange,
           Verb::Warp, Verb::Dock, Verb::Mine, Verb::Attack, Verb::Investigate, Verb::Jump,
           Verb::FlyHere, Verb::WarpHere, Verb::Build, Verb::Accept, Verb::HandIn })
    {
        const std::string tool = Actions::ToolFor(v);
        INFO("verb " << (int)v << " tool '" << tool << "'");
        if (interfaceOnly.count(v) || owed.count(v))
            CHECK(tool.empty());
        else
            CHECK(tools.count(tool) == 1);
    }
}

TEST_CASE("Actions: a mission is taken at a station with room for it, and handed in when ready")
{
    Actions::MissionTarget offer;
    offer.index = 2;
    offer.offer = true;
    CHECK(Actions::ForMission(offer).empty());  // the board is a station's: not in flight
    offer.docked = true;
    const std::vector<Action> take = Actions::ForMission(offer);
    const Action*             accept = Find(take, Verb::Accept);
    REQUIRE(accept != nullptr);
    CHECK(accept->mission == 2);
    CHECK(accept->tool == "accept_mission");
    offer.active = 5;  // MissionSystem::MAX_ACTIVE
    CHECK_FALSE(Has(Actions::ForMission(offer), Verb::Accept));

    Actions::MissionTarget active;
    active.index = 0;
    CHECK(Actions::ForMission(active).empty());
    active.ready = true;
    const std::vector<Action> give = Actions::ForMission(active);
    const Action*             handIn = Find(give, Verb::HandIn);
    REQUIRE(handIn != nullptr);
    CHECK(handIn->mission == 0);
    CHECK_FALSE(Has(give, Verb::Accept));
}
