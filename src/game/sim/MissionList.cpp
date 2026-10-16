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
        case MissionType::Supply: return "supply";
        case MissionType::Escort: return "escort";
        case MissionType::Siege: return "siege";
        case MissionType::Contract: return "contract";
        case MissionType::Besiege: return "besiege";
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
        r.faction = m.faction;
        r.reward = m.rewardMoney;
        r.rep = m.rewardRep;
        // A delivery ends at its destination; everything else goes back to the station that
        // gave it -- the rule Simulation::MissionCompletableNow and Obs::MissionNeeds use.
        const bool contract = (MissionType)m.type == MissionType::Contract;
        r.handInId = (MissionType)m.type == MissionType::Delivery || contract ? m.destStationId
                                                                              : m.giverStationId;
        r.handInName = NameHere(view, r.handInId);
        r.handInHere = !r.handInName.empty();
        if (IsPlanJob((MissionType)m.type))
        {
            // The faction pays by wire: no station to fly back to. What there is to fly to is a
            // supply job's site, when it is here.
            r.job = true;
            r.where = m.systemName;
            r.due = Obs::JobDue(view, m);
            r.siteId = m.siteId;
            r.handInId = m.siteId;
            r.handInName = m.siteId != 0 ? NameHere(view, m.siteId) : std::string();
            r.handInHere = m.siteId != 0 && !r.handInName.empty();
            r.siege = IsSiegeJob((MissionType)m.type);
            r.against = m.against;
        }
        if (contract)
        {
            // A market's contract (#343): handed in at its station, but on the clock and about
            // a system that may well be another one, like a plan's job.
            r.where = m.systemName;
            r.due = Obs::JobDue(view, m);
        }

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
            case MissionType::Supply:
            case MissionType::Contract:
                // What the hold carries towards the delivery, as for mining.
                if (m.resource >= 0 && m.resource < (int)snap.player.cargoByType.size())
                    done = snap.player.cargoByType[m.resource];
                break;
            case MissionType::Escort: done = m.progress; break;
            case MissionType::Siege:
            case MissionType::Besiege: done = m.progress; break;
        }
        if ((MissionType)m.type == MissionType::Escort)
            r.status = "escort";
        else if ((MissionType)m.type != MissionType::Delivery && m.targetCount > 0)
        {
            done = std::min(done, m.targetCount);
            r.progress = (float)done / (float)m.targetCount;
            r.status = Count(done, m.targetCount);
        }
        else if ((MissionType)m.type == MissionType::Delivery)
            r.status = "deliver";
        if (r.ready)
            r.status = "ready";
        rows.push_back(std::move(r));
    }
    return rows;
}

}  // namespace MissionList
