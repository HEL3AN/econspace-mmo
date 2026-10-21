#include <doctest/doctest.h>

#include "core/Goal.h"

// The goal prompt (#364): one line about what to do next, worked out from the player's state.

using Goal::Step;

namespace
{
Goal::Mission Bounty(bool ready = false)
{
    Goal::Mission m;
    m.title = "Clear the lane";
    m.ready = ready;
    m.needs = ready ? "" : "destroy 3 more pirates";
    m.handInId = 11;
    m.handInName = "Aurora Hub";
    return m;
}

Goal::State InSpace()
{
    Goal::State s;
    s.nearestStationId = 11;
    s.nearestStationName = "Aurora Hub";
    return s;
}

Goal::State DockedAt(int station, int offers)
{
    Goal::State s;
    s.docked = true;
    s.dockedStationId = station;
    s.offers = offers;
    return s;
}
}  // namespace

TEST_CASE("Goal: in space with nothing taken on, dock at the nearest station for work")
{
    const Goal::Prompt p = Goal::Next(InSpace());
    CHECK(p.step == Step::DockForWork);
    CHECK(p.text == "Dock at Aurora Hub to pick up work");
    CHECK(p.pointAt == 11);  // the pointer is at the station

    Goal::State        empty;
    const Goal::Prompt none = Goal::Next(empty);
    CHECK(none.step == Step::FindStation);
    CHECK(none.pointAt == 0);
}

TEST_CASE("Goal: docked, take work from the board -- or leave one that has none")
{
    const Goal::Prompt take = Goal::Next(DockedAt(11, 4));
    CHECK(take.step == Step::TakeMission);
    CHECK(take.missionsWindow);
    CHECK(take.text == "Take a mission from the board (Missions, J)");

    CHECK(Goal::Next(DockedAt(11, 0)).step == Step::Undock);
    Goal::State full = DockedAt(11, 4);
    full.roomForMore = false;
    CHECK(Goal::Next(full).step == Step::Undock);
}

TEST_CASE("Goal: with a mission under way, it says what the mission still needs")
{
    Goal::State s = InSpace();
    s.active.push_back(Bounty());
    const Goal::Prompt work = Goal::Next(s);
    CHECK(work.step == Step::Work);
    CHECK(work.text == "\"Clear the lane\": destroy 3 more pirates");

    Goal::State docked = DockedAt(11, 4);
    docked.active.push_back(Bounty());
    // Taken on already: the board is not the next thing, the work is.
    const Goal::Prompt out = Goal::Next(docked);
    CHECK(out.step == Step::Undock);
    CHECK(out.text == "Undock and destroy 3 more pirates");

    // A delivery's work is the trip: fly to it, with the pointer on it.
    Goal::Mission delivery = Bounty();
    delivery.needs = "dock at Vela Hub (#12)";
    delivery.handInId = 12;
    delivery.handInName = "Vela Hub";
    s.active = { delivery };
    const Goal::Prompt fly = Goal::Next(s);
    CHECK(fly.step == Step::FlyTo);
    CHECK(fly.pointAt == 12);
    CHECK(fly.text == "Fly to Vela Hub and dock");
    s.active.front().handInName.clear();  // in another system
    CHECK(Goal::Next(s).pointAt == 0);
}

TEST_CASE("Goal: a finished mission is taken back and handed in, before anything else")
{
    Goal::State s = InSpace();
    s.active.push_back(Bounty());
    s.active.push_back(Bounty(true));
    const Goal::Prompt fly = Goal::Next(s);
    CHECK(fly.step == Step::FlyTo);
    CHECK(fly.pointAt == 11);
    CHECK(fly.text == "Fly to Aurora Hub and dock to hand in \"Clear the lane\"");

    s.active.back().handInName.clear();
    CHECK(Goal::Next(s).step == Step::FarHandIn);

    Goal::State here = DockedAt(11, 4);
    here.active.push_back(Bounty(true));
    const Goal::Prompt hand = Goal::Next(here);
    CHECK(hand.step == Step::HandIn);
    CHECK(hand.missionsWindow);

    Goal::State elsewhere = DockedAt(99, 4);
    elsewhere.active.push_back(Bounty(true));
    const Goal::Prompt leave = Goal::Next(elsewhere);
    CHECK(leave.step == Step::Undock);
    CHECK(leave.text == "Undock and take \"Clear the lane\" to Aurora Hub");
}

TEST_CASE("Goal: the prompt goes once the loop is learned, or when it is turned off")
{
    CHECK(Goal::Shown(true, 0));
    CHECK(Goal::Shown(true, Goal::LEARNED_AFTER - 1));
    CHECK_FALSE(Goal::Shown(true, Goal::LEARNED_AFTER));
    CHECK_FALSE(Goal::Shown(false, 0));
}

TEST_CASE("Goal: a plan's job points at what it is about when that is here (#430)")
{
    Goal::State   s = InSpace();
    Goal::Mission escort;
    escort.title = "Escort: convoy to Kor";
    escort.needs = "stay in this system with the convoy's hauler (#55) until it reaches Kor";
    escort.anywhere = true;
    escort.handInId = 55;
    escort.handInName = "Guild hauler";
    s.active = { escort };
    const Goal::Prompt p = Goal::Next(s);
    CHECK(p.step == Step::Work);
    CHECK(p.pointAt == 55);
    // Elsewhere: nothing here to point at.
    s.active.front().handInId = 0;
    s.active.front().handInName.clear();
    CHECK(Goal::Next(s).pointAt == 0);
    // A station's mission under way points at nothing: its work is not at the station.
    s.active = { Bounty() };
    CHECK(Goal::Next(s).pointAt == 0);
}

TEST_CASE("Goal: a supply job points at its site once the goods are aboard, and delivers itself "
          "(#429)")
{
    Goal::Mission m;
    m.title = "Supply: Watch Post";
    m.needs = "carry 20 Iron (the hold has 0) to the Watch Post in Vela (system r1-2-3)";
    m.supply = true;
    m.handInId = 55;
    m.handInName = "Watch Post";
    Goal::State s = InSpace();
    s.active.push_back(m);
    // Without the goods: what it needs, the pointer at the site it is about (#430).
    Goal::Prompt p = Goal::Next(s);
    CHECK(p.step == Step::Work);
    CHECK(p.pointAt == 55);

    // With them, and the site here: fly to it.
    s.active[0].aboard = true;
    s.active[0].needs = "fly beside the Watch Post (#55), ...";
    p = Goal::Next(s);
    CHECK(p.step == Step::FlyTo);
    CHECK(p.pointAt == 55);
    CHECK(p.text == "Fly to Watch Post to deliver \"Supply: Watch Post\"");

    // Ready: it goes over by itself -- no Hand in to chase.
    s.active[0].ready = true;
    s.active[0].needs.clear();
    p = Goal::Next(s);
    CHECK(p.step == Step::Work);
    CHECK_FALSE(p.missionsWindow);
}
