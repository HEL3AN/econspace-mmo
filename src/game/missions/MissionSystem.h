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
    // How many missions one player may carry at once (#219). Without a cap one player
    // could take a station's whole board.
    static constexpr int MAX_ACTIVE = 5;

    // One piece of a station's own work, drawn by the station's role: bounty, mining or
    // delivery, at the base reward. The station's board keeps it for a while (#425,
    // Simulation::StationWork); the reputation multiplier is applied where it is shown.
    static Mission MakeOffer(Station* giver, const std::vector<Station*>& allStations);

    // Accepts an offer by index: moves it into the active list. false -- no such offer, or
    // MAX_ACTIVE missions are already active.
    bool Accept(int offerIndex);

    const std::vector<Mission>& Offers() const { return offers_; }
    std::vector<Mission>&       Active() { return active_; }
    const std::vector<Mission>& Active() const { return active_; }

    // Network (M4f-2): replace the contents with a mirror from the snapshot (the
    // client doesn't mutate missions directly — the server owns them; the client
    // only reflects the state it receives).
    // The board as the server made it: the station's own work, after its faction's jobs (#345).
    void SetOffers(std::vector<Mission> offers) { offers_ = std::move(offers); }

    void SetMirror(std::vector<Mission> offers, std::vector<Mission> active)
    {
        offers_ = std::move(offers);
        active_ = std::move(active);
    }

    void OnPirateKilled();  // +1 to the progress of every active Bounty mission

    // Clear ONLY the offer board (on jump/undock). Offers are tied to the station
    // the player was docked at — once they leave, the offers aren't needed (the next
    // station shows its own board). Active missions are NOT touched: they address
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
    static Mission Generate(MissionType type, Station* giver,
                            const std::vector<Station*>& allStations);

    std::vector<Mission> offers_;
    std::vector<Mission> active_;
};
