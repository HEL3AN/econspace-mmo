#pragma once

#include "entities/Entity.h"
#include "entities/Combatant.h"
#include "entities/ShipType.h"
#include "economy/Resource.h"
#include <map>

// Warp jump phases: aligning to the target (spin-up), the jump itself, or no warp.
enum class WarpPhase { None, Aligning, Warping };

// Player ship. Flight physics; parameters (thrust, cargo, mining) come from the
// ShipStats struct and change when switching to another ship (Refit).
//
// Two flight models: with the stabilizer (omni-thrusters cancel inertia) and
// without it (classic Newtonian inertia). Autopilot always uses the stabilizer.
class Ship : public Entity, public Combatant
{
public:
    Ship(Vector2 startPos, const ShipStats& stats);

    // Combatant: position comes from Entity (shared pos_).
    Vector2 GetPosition() const override { return pos_; }

    // Control intents for the current frame (turn: -1 left, +1 right).
    void SetControls(bool thrust, float turn, bool brake);

    // Autopilot: fly to target and stop at stopDistance.
    void EngageAutopilot(Vector2 target, float stopDistance);
    void DisengageAutopilot() { apActive_ = false; }

    // Warp jump: fast travel to target, dropping out at dropDistance.
    void      EngageWarp(Vector2 target, float dropDistance);
    void      CancelWarp();  // aborts warp and clamps speed back to normal max
    bool      IsWarping() const { return warpPhase_ != WarpPhase::None; }
    WarpPhase GetWarpPhase() const { return warpPhase_; }

    // Alignment phase progress (0..1, 1 means ready to jump).
    float GetWarpAlignProgress() const
    {
        if (warpPhase_ != WarpPhase::Aligning)
            return 1.0f;
        float p = 1.0f - warpAlignTimer_ / WARP_ALIGN_TIME;
        return p < 0.0f ? 0.0f : (p > 1.0f ? 1.0f : p);
    }

    void ToggleStabilizer() { stabilizerOn_ = !stabilizerOn_; }
    bool IsStabilizerOn() const { return stabilizerOn_; }
    void SetStabilizerOn(bool on) { stabilizerOn_ = on; }  // network: sync from snapshot

    void SetMiningOn(bool on) { miningOn_ = on; }          // network: sync from snapshot

    void Stop() { velocity_ = { 0.0f, 0.0f }; }

    // Piloting skill bonus — a multiplier on speed and maneuverability.
    void SetPilotBonus(float bonus) { pilotBonus_ = bonus; }

    // Combat system: shields absorb damage first, then the hull.
    void  TakeDamage(float amount) override;
    bool  IsAlive() const override { return hull_ > 0.0f; }
    void  Repair() { hull_ = maxHull_; shields_ = maxShields_; }
    void  Teleport(Vector2 pos) { pos_ = pos; velocity_ = { 0.0f, 0.0f }; }
    void  ClearCargo() { cargo_.clear(); }
    float GetHull() const override { return hull_; }
    float GetMaxHull() const override { return maxHull_; }
    float GetShields() const { return shields_; }
    float GetMaxShields() const { return maxShields_; }

    void ToggleMining() { miningOn_ = !miningOn_; }
    bool IsMiningOn() const { return miningOn_; }

    // Switch to another ship — applies the new parameters.
    void             Refit(const ShipStats& stats) { stats_ = stats; }
    const ShipStats& GetStats() const { return stats_; }

    bool AddCargo(ResourceType type, int amount);  // false if it doesn't fit
    void RemoveCargo(ResourceType type, int amount);
    int  GetCargoAmount(ResourceType type) const;
    int  GetCargoUsed() const;
    int  GetCargoCapacity() const { return stats_.cargoCapacity; }

    void Update(float dt) override;
    void Draw() const override;

    float   GetHeading() const { return heading_; }
    Vector2 GetVelocity() const { return velocity_; }
    void    SetHeading(float h) { heading_ = h; }  // for loading a save

    // Network client (M4e): apply the authoritative ship view from the server so
    // the camera/renderer/HUD reading this object reflect the remote state. Unused
    // in single-player (ship physics is computed by Simulation::StepPlayerShip).
    void ApplyView(Vector2 pos, float heading, Vector2 vel, float hull, float shields);
    // Network client (M4e): apply the authoritative navigation state (warp/
    // autopilot) from the server. Warp/AP are server-authoritative — the client has
    // no spin-up timer of its own, it mirrors the server's; this way "bar ready" and
    // the real flight start line up (the client used to have its own drifting warp timer).
    void ApplyNavView(int warpPhase, float warpAlignTimer, Vector2 warpTarget, float warpDrop,
                      bool apActive, Vector2 apTarget, float apStopDistance);
    float   GetSpeed() const;
    bool    IsAutopilotOn() const { return apActive_; }
    Vector2 GetAutopilotTarget() const { return apTarget_; }
    // Access to warp/AP state for serialization into the snapshot (server -> client).
    float   GetWarpAlignTimer() const { return warpAlignTimer_; }
    Vector2 GetWarpTarget() const { return warpTarget_; }
    float   GetWarpDrop() const { return warpDrop_; }
    float   GetAutopilotStopDistance() const { return apStopDistance_; }

private:
    void RunAutopilot(float dt);       // sets desiredVelocity_, turns the nose
    void RunWarp(float dt);            // warp phases: alignment and jump
    void ApplyStabilizer(float dt);    // omni-correction of velocity toward desired
    void ApplyNewtonFlight(float dt);  // classic inertia without the stabilizer

    // Effective stats including the piloting bonus.
    float MaxSpeed() const { return stats_.maxSpeed * pilotBonus_; }
    float RcsAccel() const { return stats_.rcsAccel * pilotBonus_; }
    float ThrustPower() const { return stats_.thrustPower * pilotBonus_; }

    ShipStats stats_;
    float     pilotBonus_ = 1.0f;

    // Combat stats.
    float maxHull_ = 100.0f;
    float hull_ = 100.0f;
    float maxShields_ = 60.0f;
    float shields_ = 60.0f;
    float shieldRegen_ = 8.0f;  // shield regen per second

    float   heading_;   // heading in radians
    Vector2 velocity_;  // velocity, units/sec

    // Control intents for the frame (from SetControls).
    bool  ctrlThrust_ = false;
    float ctrlTurn_ = 0.0f;
    bool  ctrlBrake_ = false;

    bool    stabilizerOn_ = true;
    Vector2 desiredVelocity_ = { 0.0f, 0.0f };
    bool    engineActive_ = false;

    // Mining and cargo.
    bool                        miningOn_ = false;
    std::map<ResourceType, int> cargo_;

    // Autopilot state.
    bool    apActive_ = false;
    bool    apArrived_ = false;
    Vector2 apTarget_ = { 0.0f, 0.0f };
    float   apStopDistance_ = 0.0f;

    // Warp state.
    static constexpr float WARP_ALIGN_TIME = 1.8f;  // spin-up duration
    WarpPhase warpPhase_ = WarpPhase::None;
    Vector2   warpTarget_ = { 0.0f, 0.0f };
    float     warpDrop_ = 0.0f;
    float     warpAlignTimer_ = 0.0f;
    float     warpPrevDist_ = 0.0f;  // progress guard (prevents getting stuck)
};
