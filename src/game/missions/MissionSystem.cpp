#include "missions/MissionSystem.h"

#include "entities/Station.h"
#include "raylib.h"  // GetRandomValue, TextFormat

void MissionSystem::GenerateOffers(Station* giver, const std::vector<Station*>& allStations,
                                   float rewardMul)
{
    offers_.clear();

    bool canDeliver = allStations.size() >= 2;

    // Weighted type pool biased by the station's role — each has its own character.
    std::vector<MissionType> pool;
    auto                     add = [&](MissionType t, int weight)
    {
        for (int i = 0; i < weight; i++)
            pool.push_back(t);
    };
    add(MissionType::Bounty, 1);
    add(MissionType::Mining, 1);
    if (canDeliver)
        add(MissionType::Delivery, 1);

    switch (giver->GetRole())
    {
        case StationRole::TradeHub:
            if (canDeliver)
                add(MissionType::Delivery, 3);
            break;
        case StationRole::MiningOutpost: add(MissionType::Mining, 3); break;
        case StationRole::Military: add(MissionType::Bounty, 3); break;
        case StationRole::Shipyard: break;  // shipyard — no bias, a bit of everything
    }

    for (int i = 0; i < 4; i++)
    {
        Mission m = Generate(pool[GetRandomValue(0, (int)pool.size() - 1)], giver, allStations);
        m.rewardMoney *= rewardMul;  // reward depends on reputation
        m.rewardRep *= rewardMul;
        offers_.push_back(m);
    }
}

void MissionSystem::Accept(int offerIndex)
{
    if (offerIndex < 0 || offerIndex >= (int)offers_.size())
        return;

    active_.push_back(offers_[offerIndex]);
    offers_.erase(offers_.begin() + offerIndex);
}

void MissionSystem::OnPirateKilled()
{
    for (Mission& m : active_)
        if (m.type == MissionType::Bounty && m.progress < m.targetCount)
            m.progress++;
}

Mission MissionSystem::Generate(MissionType type, Station* giver,
                                const std::vector<Station*>& allStations) const
{
    Mission m;
    m.type = type;
    m.giverStationId = giver->GetId();
    m.faction = giver->GetFaction();

    switch (type)
    {
        case MissionType::Bounty:
        {
            m.targetCount = GetRandomValue(2, 4);
            m.rewardMoney = m.targetCount * 250.0;
            m.rewardRep = m.targetCount * 1.5f;
            m.title = "Bounty";
            m.description = TextFormat("Destroy %d pirates", m.targetCount);
            break;
        }
        case MissionType::Mining:
        {
            std::vector<ResourceType> kinds = AllResourceTypes();
            m.resource = kinds[GetRandomValue(0, (int)kinds.size() - 1)];
            m.targetCount = GetRandomValue(20, 50);
            m.rewardMoney = m.targetCount * 16.0;
            m.rewardRep = 4.0f;
            m.title = "Mining";
            m.description =
                TextFormat("Deliver %d %s", m.targetCount, ResourceName(m.resource).c_str());
            break;
        }
        case MissionType::Delivery:
        {
            std::vector<Station*> others;
            for (Station* s : allStations)
                if (s != giver)
                    others.push_back(s);

            Station* dest = others[GetRandomValue(0, (int)others.size() - 1)];
            m.destStationId = dest->GetId();
            m.rewardMoney = GetRandomValue(400, 900);
            m.rewardRep = 5.0f;
            m.title = "Delivery";
            m.description = TextFormat("Deliver cargo to %s", dest->GetName().c_str());
            break;
        }
    }
    return m;
}
