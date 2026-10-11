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

    float GetReputation(FactionId faction) const { return reputation_[faction]; }
    void  AddReputation(FactionId faction, float amount) { reputation_[faction] += amount; }
    void  SetReputation(FactionId faction, float value) { reputation_[faction] = value; }

    // Wanted status: the bounty on the player's head accrued with a faction.
    double GetBounty(FactionId faction) const { return bounty_[faction]; }
    void   AddBounty(FactionId faction, double amount) { bounty_[faction] += amount; }
    void   SetBounty(FactionId faction, double value) { bounty_[faction] = value; }
    bool   IsWanted(FactionId faction) const { return bounty_[faction] > 0.0; }

    // Wanted status slowly decays over time (minor offenses are forgotten).
    void DecayBounty(double amount)
    {
        for (FactionId f : Factions::All())
        {
            double& b = bounty_[f];
            b -= amount;
            if (b < 0.0)
                b = 0.0;
        }
    }

private:
    double             money_;
    Skills             skills_;
    PerFaction<float>  reputation_;
    PerFaction<double> bounty_;
};
