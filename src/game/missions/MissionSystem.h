#pragma once

#include "missions/Mission.h"
#include <vector>

class Station;

// Mission log: the current station's offers and the accepted active missions.
// Reward and completion logic lives in Game (it needs Player/Ship); this system
// only holds the missions themselves and their progress.
class MissionSystem
{
public:
    // Generates a fresh list of offers for the issuing station. rewardMul is the
    // reward multiplier (depends on the player's reputation with the station's faction).
    void GenerateOffers(Station* giver, const std::vector<Station*>& allStations,
                        float rewardMul = 1.0f);

    // Accepts an offer by index: moves it into the active list.
    void Accept(int offerIndex);

    const std::vector<Mission>& Offers() const { return offers_; }
    std::vector<Mission>&       Active() { return active_; }
    const std::vector<Mission>& Active() const { return active_; }

    // Network (M4f-2): replace the contents with a mirror from the snapshot (the
    // client doesn't mutate missions directly — the server owns them; the client
    // only reflects the state it receives).
    void SetMirror(std::vector<Mission> offers, std::vector<Mission> active)
    {
        offers_ = std::move(offers);
        active_ = std::move(active);
    }

    void OnPirateKilled();  // +1 to the progress of every active Bounty mission

    // Clear ONLY the offer board (on jump/undock). Offers are tied to the station
    // the player was docked at — once they leave, the offers aren't needed (a new
    // station regenerates its board). Active missions are NOT touched: they address
    // stations by id and survive a system change (Bounty/Delivery often require
    // flying to another system — a jump used to wipe the mission, which was a bug).
    void ClearOffers() { offers_.clear(); }

    // Full reset (new start / loading a save) — both offers and active missions.
    void Clear()
    {
        offers_.clear();
        active_.clear();
    }

private:
    Mission Generate(MissionType type, Station* giver,
                     const std::vector<Station*>& allStations) const;

    std::vector<Mission> offers_;
    std::vector<Mission> active_;
};
