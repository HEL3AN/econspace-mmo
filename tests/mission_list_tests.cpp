#include <doctest/doctest.h>

#include "economy/Resource.h"
#include "missions/Mission.h"
#include "sim/MissionList.h"

#include <map>
#include <string>

// What the missions window lists (#297), decided without a window.

namespace
{
Proto::MissionView Job(MissionType type, int giver, int dest = 0, int target = 0, int progress = 0)
{
    Proto::MissionView m;
    m.type = (int)type;
    m.title = "Job";
    m.giverStationId = giver;
    m.destStationId = dest;
    m.targetCount = target;
    m.progress = progress;
    m.rewardMoney = 1200.0;
    return m;
}

Proto::EntityLayout StationAt(int id, const char* name)
{
    Proto::EntityLayout l;
    l.id = id;
    l.kind = Proto::EntityKind::Station;
    l.name = name;
    return l;
}
}  // namespace

TEST_CASE("MissionList: active missions say how far along they are and where they end")
{
    Proto::Snapshot snap;
    snap.player.cargoByType.assign(8, 0);
    snap.player.cargoByType[(int)ResourceType::Iron] = 7;

    Proto::MissionView bounty = Job(MissionType::Bounty, 10, 0, 5, 2);
    Proto::MissionView mining = Job(MissionType::Mining, 10, 0, 20);
    mining.resource = (int)ResourceType::Iron;
    Proto::MissionView delivery = Job(MissionType::Delivery, 10, 99);
    Proto::MissionView done = Job(MissionType::Bounty, 10, 0, 3, 3);
    done.completable = true;
    snap.missionActive = { bounty, mining, delivery, done };

    std::map<int, Proto::EntityLayout> layout{ { 10, StationAt(10, "Vale Station") } };
    Obs::View                          view;
    view.snapshot = &snap;
    view.layout = &layout;

    const std::vector<MissionList::Row> rows = MissionList::Build(view, MissionList::Board::Active);
    REQUIRE(rows.size() == 4);

    CHECK(rows[0].index == 0);
    CHECK(rows[0].kind == "bounty");
    CHECK(rows[0].status == "2 / 5");
    CHECK(rows[0].progress == doctest::Approx(0.4f));
    CHECK(rows[0].handInName == "Vale Station");
    CHECK(rows[0].handInHere);
    CHECK_FALSE(rows[0].needs.empty());

    // Mining counts what the hold carries.
    CHECK(rows[1].status == "7 / 20");

    // A delivery ends at its destination, which is in another system: nothing to fly to here.
    CHECK(rows[2].handInId == 99);
    CHECK(rows[2].status == "deliver");
    CHECK(rows[2].progress < 0.0f);
    CHECK_FALSE(rows[2].handInHere);

    // Ready is the server's word, and then nothing more is needed.
    CHECK(rows[3].ready);
    CHECK(rows[3].status == "ready");
    CHECK(rows[3].needs.empty());
}

TEST_CASE("MissionList: offers are the docked station's board, numbered as the server numbers them")
{
    Proto::Snapshot snap;
    snap.missionOffers = { Job(MissionType::Mining, 10), Job(MissionType::Delivery, 10, 11) };
    Obs::View view;
    view.snapshot = &snap;

    const std::vector<MissionList::Row> rows = MissionList::Build(view, MissionList::Board::Offers);
    REQUIRE(rows.size() == 2);
    CHECK(rows[1].index == 1);
    CHECK(rows[1].kind == "delivery");
    CHECK(rows[1].reward == doctest::Approx(1200.0));
    CHECK(MissionList::Build(view, MissionList::Board::Active).empty());

    Obs::View nothing;
    CHECK(MissionList::Build(nothing, MissionList::Board::Offers).empty());
}
