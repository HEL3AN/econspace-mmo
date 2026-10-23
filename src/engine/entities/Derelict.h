#pragma once

#include "entities/Entity.h"
#include "economy/Inventory.h"
#include "economy/Resource.h"
#include <string>

// Derelict: a ship/station wreck. A one-time find — searching it yields a reward
// (credits), after which it is considered searched.
class Derelict : public Entity
{
public:
    Derelict(Vector2 pos, float size, std::string name, double reward);

    std::unique_ptr<Entity> Clone() const override { return std::make_unique<Derelict>(*this); }
    Render::Item            Describe() const override;
    std::string             GetName() const override;

    // What a wreck is called, and what it is called once searched. One rule, because the
    // wire carries the name and the state apart (#38) and a client puts them back together.
    static std::string DisplayName(const std::string& baseName, bool looted);
    const std::string& GetBaseName() const { return name_; }

    bool   IsLooted() const { return looted_; }
    void   SetLooted() { looted_ = true; }
    double GetReward() const { return reward_; }
    // What it holds (#343, #472): the cargo of the ship it was -- every good of it, so a lost
    // ship leaves one wreck rather than a row of pods -- or what a pilot jettisoned into it. A
    // container like a hold: a search takes what the hold has room for, `move` takes any of it,
    // and it is searched once nothing is left.
    Inventory&       Contents() { return goods_; }
    const Inventory& Contents() const { return goods_; }
    int              GetCargoAmount() const { return goods_.Used(); }

    // When it is gone by itself (#41): the wreck of a structure decays, so a world players
    // can shoot at does not fill up with ruins. An instant of the world clock, like a
    // structure's own end (#136), and part of what it is rather than its state; 0 -- it
    // stays until somebody removes it.
    double GetExpiresAt() const { return expiresAt_; }
    void   SetExpiresAt(double t) { expiresAt_ = t; }

private:
    std::string name_;
    double      reward_;
    bool        looted_ = false;
    double      expiresAt_ = 0.0;
    Inventory   goods_;
};
