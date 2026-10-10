#pragma once

#include "entities/Entity.h"
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

private:
    std::string name_;
    double      reward_;
    bool        looted_ = false;
};
