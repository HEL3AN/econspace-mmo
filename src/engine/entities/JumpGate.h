#pragma once

#include "entities/Entity.h"
#include <string>

// Jump gate: a link to another system, used by standing at it and asking.
//
// A wormhole the tide opened is a gate too (#342 S4): until somebody anchors it, it is an
// unstable mouth that collapses at `closesAt` (world time; 0 -- a gate that stays), and the
// name and look it has change when it is anchored.
class JumpGate : public Entity
{
public:
    JumpGate(Vector2 pos, float size, std::string name, std::string destination);

    std::unique_ptr<Entity> Clone() const override { return std::make_unique<JumpGate>(*this); }
    std::string             GetName() const override { return name_; }
    void                    SetName(const std::string& name) { name_ = name; }

    std::string GetDestination() const { return destination_; }

    double GetClosesAt() const { return closesAt_; }
    void   SetClosesAt(double t) { closesAt_ = t; }
    bool   IsUnstable() const { return closesAt_ > 0.0; }

private:
    std::string name_;
    std::string destination_;
    double      closesAt_ = 0.0;
};
