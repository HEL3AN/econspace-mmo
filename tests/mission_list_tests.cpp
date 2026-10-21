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
    snap.player.cargoByType[0] = 7;

    Proto::MissionView bounty = Job(MissionType::Bounty, 10, 0, 5, 2);
    Proto::MissionView mining = Job(MissionType::Mining, 10, 0, 20);
    mining.resource = 0;  // the first good
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

TEST_CASE("MissionList: an escort job says where its convoy is and points at its hauler (#430)")
{
    Proto::Snapshot    snap;
    Proto::MissionView m = Job(MissionType::Escort, 10, 0, 1, 0);
    m.planId = 7;
    m.systemId = "r1-2-3";
    m.systemName = "Kor";
    m.convoySystemId = "r1-1-0";
    m.convoySystemName = "Vela";
    m.convoyNextName = "Ost";
    snap.missionActive = { m };
    Obs::View view;
    view.snapshot = &snap;

    // Waiting to set out.
    std::vector<MissionList::Row> rows = MissionList::Build(view, MissionList::Board::Active);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].status == "waiting at Vela");
    CHECK(rows[0].convoy == "waiting at Vela");
    CHECK_FALSE(rows[0].rode);
    CHECK(rows[0].handInId == 0);
    CHECK_FALSE(rows[0].handInHere);

    // On its way, in another system: where it is and where it goes next.
    snap.missionActive[0].convoyMoving = true;
    rows = MissionList::Build(view, MissionList::Board::Active);
    CHECK(rows[0].status == "en route in Vela \xe2\x86\x92 Ost");
    CHECK(Obs::ConvoyStatus(snap.missionActive[0], "->") == "en route in Vela -> Ost");
    snap.missionActive[0].convoyNextName.clear();
    CHECK(MissionList::Build(view, MissionList::Board::Active)[0].status ==
          "en route in Vela \xe2\x86\x92 its site");

    // Here, with the ship: the hauler is what the job points at.
    Proto::EntitySnapshot hauler;
    hauler.id = 55;
    hauler.kind = Proto::EntityKind::Npc;
    hauler.name = "Guild hauler";
    snap.entities.push_back(hauler);
    snap.missionActive[0].haulerId = 55;
    snap.missionActive[0].convoyEscortIds = { 56, 57 };
    snap.missionActive[0].rode = true;
    rows = MissionList::Build(view, MissionList::Board::Active);
    CHECK(rows[0].status == "with convoy");
    CHECK(rows[0].rode);
    CHECK(rows[0].handInId == 55);
    CHECK(rows[0].handInName == "Guild hauler");
    CHECK(rows[0].handInHere);
    CHECK(rows[0].needs.find("hauler (#55)") != std::string::npos);

    // ...and the instruments know its ships for this pilot's convoy.
    CHECK(Obs::ConvoyPartOf(snap, 55) == Obs::ConvoyPart::Hauler);
    CHECK(Obs::ConvoyPartOf(snap, 57) == Obs::ConvoyPart::Escort);
    CHECK(Obs::ConvoyPartOf(snap, 58) == Obs::ConvoyPart::None);
    CHECK(Obs::ConvoyPartOf(snap, 0) == Obs::ConvoyPart::None);
}
