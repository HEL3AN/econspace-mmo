#include "sim/MissionList.h"

#include "missions/Mission.h"

#include <algorithm>
#include <cstdio>

namespace MissionList
{
namespace
{
std::string Count(int done, int of)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d / %d", done, of);
    return buf;
}

// The station's name if this client can see it: the layout first, then the snapshot.
std::string NameHere(const Obs::View& view, int id)
{
    if (view.layout != nullptr)
    {
        auto it = view.layout->find(id);
        if (it != view.layout->end())
            return it->second.name.empty() ? std::string("station") : it->second.name;
    }
    if (view.snapshot != nullptr)
        for (const Proto::EntitySnapshot& e : view.snapshot->entities)
            if (e.id == id)
                return e.name.empty() ? std::string("station") : e.name;
    return std::string();
}
}  // namespace

const char* KindWord(int type)
{
    switch ((MissionType)type)
    {
        case MissionType::Bounty: return "bounty";
        case MissionType::Mining: return "mining";
        case MissionType::Delivery: return "delivery";
    }
    return "job";
}

std::vector<Row> Build(const Obs::View& view, Board board)
{
    std::vector<Row> rows;
    if (view.snapshot == nullptr)
        return rows;
    const Proto::Snapshot&                 snap = *view.snapshot;
    const std::vector<Proto::MissionView>& list =
        board == Board::Offers ? snap.missionOffers : snap.missionActive;

    for (size_t i = 0; i < list.size(); i++)
    {
        const Proto::MissionView& m = list[i];
        Row                       r;
        r.index = (int)i;
        r.title = m.title;
        r.description = m.description;
        r.kind = KindWord(m.type);
        r.faction = (FactionId)m.faction;
        r.reward = m.rewardMoney;
        r.rep = m.rewardRep;
        // A delivery ends at its destination; everything else goes back to the station that
        // gave it -- the rule Simulation::MissionCompletableNow and Obs::MissionNeeds use.
        r.handInId =
            (MissionType)m.type == MissionType::Delivery ? m.destStationId : m.giverStationId;
        r.handInName = NameHere(view, r.handInId);
        r.handInHere = !r.handInName.empty();

        if (board == Board::Offers)
        {
            r.status = r.kind;
            rows.push_back(std::move(r));
            continue;
        }

        r.ready = m.completable;
        r.needs = Obs::MissionNeeds(view, m);
        int done = 0;
        switch ((MissionType)m.type)
        {
            case MissionType::Bounty: done = m.progress; break;
            case MissionType::Mining:
                // What the hold carries is the progress: the ore is handed over at the end.
                if (m.resource >= 0 && m.resource < (int)snap.player.cargoByType.size())
                    done = snap.player.cargoByType[m.resource];
                break;
            case MissionType::Delivery: break;
        }
        if ((MissionType)m.type != MissionType::Delivery && m.targetCount > 0)
        {
            done = std::min(done, m.targetCount);
            r.progress = (float)done / (float)m.targetCount;
            r.status = Count(done, m.targetCount);
        }
        else
            r.status = "deliver";
        if (r.ready)
            r.status = "ready";
        rows.push_back(std::move(r));
    }
    return rows;
}

}  // namespace MissionList
