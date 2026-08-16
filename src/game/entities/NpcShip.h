#pragma once

#include "entities/Entity.h"
#include "entities/Combatant.h"
#include "core/Faction.h"
#include <vector>

// NPC ship role — determines its behavior and whether it's armed.
//  Trader  — hauls cargo between points, unarmed, flees when threatened.
//  Miner   — mines at a belt, unarmed, flees.
//  Police  — patrol of a lawful faction, attacks pirates and wanted ships.
//  Pirate  — raider, attacks traders and the player.
//  Warship — faction combat ship (faction wars).
enum class NpcRole
{
    Trader,
    Miner,
    Police,
    Pirate,
    Warship
};

// Behavior state machine state (set by the AI pass in Game).
enum class AiState
{
    Patrol,
    Pursue,
    Flee
};

// NPC ship. Behavior is driven by role and a state machine: peaceful roles fly
// between points and flee when threatened, combat roles pursue and attack hostile
// targets. The "who counts as an enemy" decision is made by Game (which sees the
// whole world and the player) and each frame it commands the ship via
// Engage/FleeFrom/StandDown.
class NpcShip : public Entity, public Combatant
{
public:
    NpcShip(Vector2 pos, FactionId faction, NpcRole role, std::vector<Vector2> waypoints);

    void                    Update(float dt) override;
    Render::Item            Describe() const override;
    std::string             GetName() const override;
    std::unique_ptr<Entity> Clone() const override { return std::make_unique<NpcShip>(*this); }

    // Combatant: position comes from Entity (shared pos_).
    Vector2 GetPosition() const override { return pos_; }

    FactionId GetFaction() const { return faction_; }
    NpcRole   GetRole() const { return role_; }
    AiState   GetState() const { return state_; }
    bool      IsPirate() const { return faction_ == FactionId::Pirates; }
    float     GetHeading() const { return heading_; }  // heading (for snapshots/render)
    void      SetHeading(float h) { heading_ = h; }    // for proxy reconciliation

    // Stable agent id lives in the base Entity (GetId/SetId).

    // Restore state on load (hull from the save).
    void SetHull(float hull) { hull_ = (hull < 0.0f) ? 0.0f : (hull > maxHull_ ? maxHull_ : hull); }
    // Whether the ship is armed (fights). Peaceful roles only flee.
    bool IsCombatant() const
    {
        return role_ == NpcRole::Police || role_ == NpcRole::Pirate || role_ == NpcRole::Warship;
    }

    // Combat.
    void  TakeDamage(float amount) override;
    bool  IsAlive() const override { return hull_ > 0.0f; }
    float GetHull() const override { return hull_; }
    float GetMaxHull() const override { return maxHull_; }

    // Commands from the AI pass for the current frame.
    void Engage(Vector2 targetPos);    // fly to the target and hold firing range
    void FleeFrom(Vector2 threatPos);  // run from the threat at full speed
    void StandDown();                  // no threats — resume normal work (patrol)

    bool ReadyToFire() const { return fireTimer_ <= 0.0f; }
    void ResetFireTimer() { fireTimer_ = 0.9f; }

private:
    void PickNewTarget();
    void MoveToward(Vector2 dest, float dt, float stopDist);

    FactionId            faction_;
    NpcRole              role_;
    std::vector<Vector2> waypoints_;
    Vector2              target_;
    float                speed_;
    float                heading_;
    float                waitTimer_;

    AiState state_ = AiState::Patrol;
    Vector2 aiPoint_ = { 0.0f, 0.0f };  // pursuit target or threat point

    float maxHull_ = 60.0f;
    float hull_ = 60.0f;
    float fireTimer_ = 0.0f;
};
