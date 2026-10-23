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

    Target gate = Thing(EntityKind::Gate);
    // Kept, not a temporary: Find points into the list (the sanitizer job caught this, #341).
    const std::vector<Action> plain = Actions::For(gate, from);
    const Action*             jump = Find(plain, Verb::Jump);
    REQUIRE(jump != nullptr);
    CHECK(jump->label == "Jump");
    gate.destinationName = "Vela";
    jump = nullptr;
    const std::vector<Action> named = Actions::For(gate, from);
    jump = Find(named, Verb::Jump);
    REQUIRE(jump != nullptr);
    CHECK(jump->label == "Jump to Vela");
}

TEST_CASE("Actions: your own structure is taken apart, anyone else's is attacked (#39)")
{
    const Vector2 from{ 0.0f, 0.0f };
    Target        t = Thing(EntityKind::Structure);
    t.mine = true;
    t.refund = "5 Iron";
    const std::vector<Action> own = Actions::For(t, from);
    const Action*             dismantle = Find(own, Verb::Dismantle);
    REQUIRE(dismantle != nullptr);
    CHECK(dismantle->label == "Dismantle (back: 5 Iron)");
    CHECK(dismantle->tool == "dismantle");
    CHECK_FALSE(Has(own, Verb::Attack));
    CHECK_FALSE(Has(own, Verb::Defence));  // no guns, no rules of engagement (#340)
    t.armed = true;
    const std::vector<Action> armed = Actions::For(t, from);
    const Action*             roe = Find(armed, Verb::Defence);
    REQUIRE(roe != nullptr);
    CHECK(roe->tool == "set_defence");

    t.mine = false;
    const std::vector<Action> theirs = Actions::For(t, from);
    CHECK_FALSE(Has(theirs, Verb::Dismantle));
    CHECK_FALSE(Has(theirs, Verb::Defence));
    const Action* attack = Find(theirs, Verb::Attack);
    REQUIRE(attack != nullptr);
    CHECK(attack->tool == "attack");
}

TEST_CASE("Actions: a point in space offers flying, warping, and what can be built there")
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

    CHECK(build->distance == 0.0f);  // in reach: laid down at once
    CHECK(build->tool == "build");

    // Out of reach it is a build order, which flies there first (#39).
    const std::vector<Action> far = Actions::ForPoint({ 9000.0f, 0.0f }, { 0.0f, 0.0f }, bps);
    CHECK(Has(far, Verb::WarpHere));
    const Action* order = Find(far, Verb::Build);
    REQUIRE(order != nullptr);
    CHECK(order->distance == doctest::Approx(500.0f));
    CHECK(order->label.find("Build Beacon") == 0);  // named by its verb first (#365)
    CHECK(order->label.find("flying there first") != std::string::npos);
}

TEST_CASE("Actions: an upgrade is offered on one's own outpost, not in open space (#433)")
{
    Blueprint outpost;
    outpost.id = "outpost";
    outpost.name = "Outpost";
    outpost.reach = 600.0f;
    Blueprint market;
    market.id = "outpost_market";
    market.name = "Outpost Market";
    market.reach = 600.0f;
    market.onto = { "outpost" };
    const std::vector<Blueprint> bps{ outpost, market };

    const std::vector<Action> space = Actions::ForPoint({ 100.0f, 0.0f }, { 0.0f, 0.0f }, bps);
    for (const Action& a : space)
        CHECK(a.blueprint != "outpost_market");

    REQUIRE(Actions::UpgradesOnto("outpost", bps).size() == 1);
    CHECK(Actions::UpgradesOnto("outpost_market", bps).empty());

    Target t = Thing(EntityKind::Structure);
    t.upgrades = Actions::UpgradesOnto("outpost", bps);
    CHECK_FALSE(Has(Actions::For(t, { 0.0f, 0.0f }), Verb::Build));  // someone else's
    t.mine = t.dockable = true;
    const std::vector<Action> near = Actions::For(t, { t.pos.x + t.size + 100.0f, t.pos.y });
    CHECK(Has(near, Verb::Dock));
    const Action* grow = Find(near, Verb::Build);
    REQUIRE(grow != nullptr);
    CHECK(grow->blueprint == "outpost_market");
    CHECK(grow->distance == 0.0f);  // within reach of its edge: laid down at once
    const std::vector<Action> away = Actions::For(t, { t.pos.x + 90000.0f, t.pos.y });
    const Action*             far = Find(away, Verb::Build);  // into `away`, which outlives it
    REQUIRE(far != nullptr);
    CHECK(far->distance == doctest::Approx(600.0f));  // an order, flying there first
}

namespace
{
// Every action a menu could offer, from every list: each kind of thing near and far, its own
// structure and someone else's, a point near and far, a mission either way, a refit.
std::vector<Action> EveryAction()
{
    std::vector<Action> all;
    auto add = [&all](const std::vector<Action>& a) { all.insert(all.end(), a.begin(), a.end()); };
    for (EntityKind k :
         { EntityKind::Unknown, EntityKind::Star, EntityKind::Planet, EntityKind::Station,
           EntityKind::Field, EntityKind::Gate, EntityKind::Nebula, EntityKind::Derelict,
           EntityKind::Npc, EntityKind::PlayerShip, EntityKind::Structure })
    {
        Target t = Thing(k);
        add(Actions::For(t, { 0.0f, 0.0f }));
        add(Actions::For(t, { 4950.0f, 0.0f }));
        t.mine = t.armed = true;
        t.refund = "5 Iron";
        t.destinationName = "Vela";
        add(Actions::For(t, { 0.0f, 0.0f }));
    }
    Blueprint beacon;
    beacon.id = "beacon";
    beacon.name = "Beacon";
    beacon.reach = 500.0f;
    add(Actions::ForPoint({ 100.0f, 0.0f }, { 0.0f, 0.0f }, { beacon }));
    add(Actions::ForPoint({ 9000.0f, 0.0f }, { 0.0f, 0.0f }, { beacon }));
    Actions::MissionTarget m;
    m.index = 0;
    m.offer = m.docked = true;
    add(Actions::ForMission(m));
    m.offer = false;
    m.ready = true;
    add(Actions::ForMission(m));
    add(Actions::ForRefit({ Ships::FitLine{} }, true, true));
    Actions::LibraryTarget lib;
    lib.typed = "Miner";
    lib.typedOk = lib.room = lib.canApply = true;
    lib.selected = "Hauler";
    add(Actions::ForLibrary(lib));
    return all;
}
}  // namespace

TEST_CASE("Actions: one name per action, whichever list offers it (#365)")
{
    // A label is the verb's one name, and after it only what tells two of a verb apart. The
    // selected-item window's buttons say the name alone, so "Keep" there and "Hold 3500 from
    // it" in the menu cannot happen again.
    const std::vector<Action> all = EveryAction();
    std::set<Verb>            seen;
    for (const Action& a : all)
    {
        const std::string name = Actions::Name(a.verb);
        INFO("label '" << a.label << "' verb " << (int)a.verb);
        REQUIRE_FALSE(name.empty());
        CHECK(a.label.rfind(name, 0) == 0);
        CHECK((a.label.size() == name.size() || a.label[name.size()] == ' '));
        seen.insert(a.verb);
    }
    CHECK(seen.size() == 24);  // every verb was offered somewhere above

    // And no two verbs share a name: the name is what the player tells them apart by.
    std::set<std::string> names;
    for (Verb v : seen)
        CHECK(names.insert(Actions::Name(v)).second);

    const std::vector<Action> station = Actions::For(Thing(EntityKind::Station), { 0.0f, 0.0f });
    const Action*             keep = Find(station, Verb::Keep);
    REQUIRE(keep != nullptr);
    CHECK(keep->label == "Hold at 500");
}

TEST_CASE("Actions: a menu is headed with what it is about and how far (#365)")
{
    CHECK(Actions::Heading("Aurora Hub", 12500.0f) == "Aurora Hub  12.5k");
    CHECK(Actions::Heading("belt", 340.0f) == "belt  340");
}

TEST_CASE("Actions: every button an agent could want is an econagent tool, and the gaps are known")
{
    // The design rule (#297): a window offers nothing an agent cannot do. The interface's own
    // verbs need no tool; the rest name one that exists. A verb added without its tool goes
    // on `owed` until it has one -- and the list is empty now, so keep it that way.
    const std::set<std::string> tools = AgentTools();
    REQUIRE_FALSE(tools.empty());
    const std::set<Verb> interfaceOnly{ Verb::Select, Verb::SetRange };
    const std::set<Verb> owed{};

    for (Verb v : { Verb::Select,     Verb::Approach,    Verb::Orbit,        Verb::Keep,
                    Verb::Follow,     Verb::SetRange,    Verb::Warp,         Verb::Dock,
                    Verb::Mine,       Verb::Attack,      Verb::Investigate,  Verb::Jump,
                    Verb::FlyHere,    Verb::WarpHere,    Verb::Build,        Verb::Accept,
                    Verb::HandIn,     Verb::Dismantle,   Verb::Refit,        Verb::Defence,
                    Verb::SaveDesign, Verb::ApplyDesign, Verb::RenameDesign, Verb::DeleteDesign,
                    Verb::Take })
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

TEST_CASE("Actions: the design library offers what the names typed and chosen allow (#279)")
{
    Actions::LibraryTarget lib;
    CHECK(Actions::ForLibrary(lib).empty());  // nothing typed, nothing chosen
    lib.typed = "Miner";
    lib.typedOk = true;
    CHECK_FALSE(Has(Actions::ForLibrary(lib), Verb::SaveDesign));  // the library is full
    lib.typedTaken = true;
    const std::vector<Action> over = Actions::ForLibrary(lib);
    REQUIRE(Has(over, Verb::SaveDesign));  // ...but a name it has is saved over
    CHECK(Find(over, Verb::SaveDesign)->design == "Miner");
    CHECK(Find(over, Verb::SaveDesign)->tool == "save_design");

    lib.selected = "Hauler";
    std::vector<Action> chosen = Actions::ForLibrary(lib);
    CHECK_FALSE(Has(chosen, Verb::ApplyDesign));   // not here, or not this hull
    CHECK_FALSE(Has(chosen, Verb::RenameDesign));  // to a name it already has
    CHECK(Has(chosen, Verb::DeleteDesign));
    lib.typedTaken = false;
    lib.canApply = true;
    chosen = Actions::ForLibrary(lib);
    REQUIRE(Has(chosen, Verb::RenameDesign));
    CHECK(Find(chosen, Verb::RenameDesign)->design == "Hauler");
    CHECK(Find(chosen, Verb::RenameDesign)->renameTo == "Miner");
    REQUIRE(Has(chosen, Verb::ApplyDesign));
    CHECK(Find(chosen, Verb::ApplyDesign)->design == "Hauler");
}
