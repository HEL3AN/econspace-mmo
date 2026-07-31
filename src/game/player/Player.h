#pragma once

#include "player/Skills.h"
#include "core/Faction.h"

// The player-pilot: the persona — wallet, skills, reputation with factions.
class Player
{
public:
    explicit Player(double startMoney);

    double GetMoney() const { return money_; }
    void   AddMoney(double amount) { money_ += amount; }
    void   SetMoney(double amount) { money_ = amount; }
    bool   CanAfford(double cost) const { return money_ >= cost; }

    Skills&       GetSkills() { return skills_; }
    const Skills& GetSkills() const { return skills_; }

    float GetReputation(FactionId faction) const { return reputation_[(int)faction]; }
    void  AddReputation(FactionId faction, float amount) { reputation_[(int)faction] += amount; }
    void  SetReputation(FactionId faction, float value) { reputation_[(int)faction] = value; }

    // Wanted status: the bounty on the player's head accrued with a faction.
    double GetBounty(FactionId faction) const { return bounty_[(int)faction]; }
    void   AddBounty(FactionId faction, double amount) { bounty_[(int)faction] += amount; }
    void   SetBounty(FactionId faction, double value) { bounty_[(int)faction] = value; }
    bool   IsWanted(FactionId faction) const { return bounty_[(int)faction] > 0.0; }

    // Wanted status slowly decays over time (minor offenses are forgotten).
    void DecayBounty(double amount)
    {
        for (double& b : bounty_)
        {
            b -= amount;
            if (b < 0.0)
                b = 0.0;
        }
    }

private:
    double money_;
    Skills skills_;
    float  reputation_[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    double bounty_[4] = { 0.0, 0.0, 0.0, 0.0 };
};
