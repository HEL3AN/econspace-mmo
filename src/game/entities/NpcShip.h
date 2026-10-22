#pragma once

#include "entities/Entity.h"
#include "entities/Combatant.h"
#include "core/Faction.h"
#include "core/Fitting.h"
#include "economy/Resource.h"
#include <string>
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

// The role as a doctrine names it in data/ships.json ("trader", "patrol", ...).
const char* NpcRoleId(NpcRole role);

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
//
// It flies a ship design (#279 step 4): its faction's doctrine names the designs a role flies,
// the server picks one, and the ship looks like that design and is as fast, as tough and as
// well armed as its parts say. The snapshot carries the design, so a client draws the same
// ship the server simulates.
class NpcShip : public Entity, public Combatant
{
public:
    // `design` is a design id from data/ships.json. Empty, or one the catalogue does not have,
    // is the faction's first pick for the role -- what a client falls back to rather than
    // draw nothing.
    NpcShip(Vector2 pos, FactionId faction, NpcRole role, std::vector<Vector2> waypoints,
            const std::string& design = std::string());

    void                    Update(float dt) override;
    Render::Item            Describe() const override;
    std::string             GetName() const override;
    std::unique_ptr<Entity> Clone() const override { return std::make_unique<NpcShip>(*this); }

    // Combatant: position comes from Entity (shared pos_).
    Vector2 GetPosition() const override { return pos_; }

    FactionId GetFaction() const { return faction_; }
    NpcRole   GetRole() const { return role_; }
    // The design it flies; empty only when no ship catalogue is loaded.
    const std::string& GetDesign() const { return design_; }
    float              GetDamage() const { return damage_; }  // per volley: the design's
    float              GetSpeed() const { return speed_; }
    AiState            GetState() const { return state_; }
    bool               IsPirate() const { return faction_ == Factions::Pirates(); }
    float              GetHeading() const { return heading_; }  // heading (for snapshots/render)
    void               SetHeading(float h) { heading_ = h; }    // for proxy reconciliation

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
    // Leaving the system (#295): it turns for the gate at `gate` and flies there, whatever
    // else is going on, until the world takes it away. It is no longer the system's.
    void Leave(Vector2 gate)
    {
        leaving_ = true;
        aiPoint_ = gate;
    }
    bool IsLeaving() const { return leaving_; }
    // ...and is at the gate, to go through it.
    bool AtGate() const
    {
        const float dx = aiPoint_.x - pos_.x, dy = aiPoint_.y - pos_.y;
        return leaving_ && dx * dx + dy * dy < 1.0f;
    }

    // The faction plan this ship is the face of (#342 S6): a builder convoy's hauler or escort,
    // shown where somebody is to see it. 0 for the system's own ships. A tasked ship is the
    // plan's to count and to take away, never the system's: no recount, top-up or migration
    // touches it, and it neither fights other NPCs nor draws a station's fire -- the plan's
    // arithmetic does that, the same where nobody is. Transient, like every ship.
    void SetTask(int plan) { task_ = plan; }
    int  GetTask() const { return task_; }

    // What a hauler carries (#343): goods bought in one system for another's market. Shot down,
    // it leaves them in its wreck. A hauler on its way out carries what its trip took from this
    // market; one that came in carries it to a market station here, and has delivered it by
    // `due` (world seconds) wherever it is. Transient, like every ship: where nobody is, the
    // cargo is a number on the trip (Simulation::Migrant).
    void SetCargo(ResourceType good, int amount, double due = 0.0)
    {
        cargo_ = good;
        cargoAmount_ = amount > 0 ? amount : 0;
        cargoDue_ = due;
    }
    ResourceType GetCargo() const { return cargo_; }
    int          GetCargoAmount() const { return cargoAmount_; }
    double       GetCargoDue() const { return cargoDue_; }
    // Where it flies from now on, picking among them as it always does.
    void SetRoute(std::vector<Vector2> waypoints)
    {
        waypoints_ = std::move(waypoints);
        PickNewTarget();
    }

    // Its guns fire at a player's cooldown (Ships::PLAYER_FIRE_INTERVAL): one combat model.
    bool ReadyToFire() const { return fireTimer_ <= 0.0f; }
    void ResetFireTimer() { fireTimer_ = Ships::PLAYER_FIRE_INTERVAL; }

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

    std::string  design_;
    float        damage_ = 0.0f;  // the design's; with no catalogue it has no guns
    float        maxHull_ = 60.0f;
    float        hull_ = 60.0f;
    float        fireTimer_ = 0.0f;
    bool         leaving_ = false;
    int          task_ = 0;
    ResourceType cargo_;
    int          cargoAmount_ = 0;
    double       cargoDue_ = 0.0;
};
