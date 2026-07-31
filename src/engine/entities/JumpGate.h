#pragma once

#include "entities/Entity.h"
#include <string>

// Jump gate: a marker for transit to another system. Still a stub — inter-system
// jumps aren't implemented yet (stores the destination name for future use).
class JumpGate : public Entity
{
public:
    JumpGate(Vector2 pos, float size, std::string name, std::string destination);

    void        Draw() const override;
    std::unique_ptr<Entity> Clone() const override { return std::make_unique<JumpGate>(*this); }
    std::string GetName() const override { return name_; }

    std::string GetDestination() const { return destination_; }

private:
    std::string name_;
    std::string destination_;
};
