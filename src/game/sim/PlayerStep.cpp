#include "sim/PlayerStep.h"

#include "entities/Ship.h"

namespace Sim
{

void StepPlayerShip(Ship& s, const Proto::Command& cmd, float pilotBonus, float dt,
                    const Vector2* holdTargetPos)
{
    bool warping = s.IsWarping();

    // Movement toggles (stabilizer/mining) — ship state.
    if (cmd.toggleStabilizer)
        s.ToggleStabilizer();
    if (cmd.toggleMining && !warping)
        s.ToggleMining();

    // Any manual control cancels the autopilot and interrupts the warp.
    if (cmd.thrust || cmd.brake || cmd.turn != 0.0f)
    {
        s.DisengageAutopilot();
        s.CancelWarp();
        s.ReleaseHold();  // a hold is standing, so nothing else would ever end it
    }
    s.SetControls(cmd.thrust, cmd.turn, cmd.brake);

    // Navigation order (one-shot): autopilot/warp to a point. Applied after manual
    // control — if the player did not touch the axes, the order takes effect.
    if (cmd.navMode == 1)
    {
        s.ReleaseHold();
        s.EngageAutopilot(cmd.navTarget, cmd.navStopDist);
    }
    else if (cmd.navMode == 2)
    {
        s.ReleaseHold();
        s.EngageWarp(cmd.navTarget, cmd.navStopDist);
    }
    else if (cmd.navMode == 3 || cmd.navMode == 4)
    {
        s.EngageHold(cmd.navMode == 3 ? HoldMode::Orbit : HoldMode::Keep, cmd.navHoldId,
                     cmd.navRange);
    }

    // A standing hold re-aims the autopilot every tick, because the thing it is holding
    // station on is moving. Without a position it simply stops steering rather than
    // flying at a stale point -- a target that left the system should not drag the ship
    // to where it used to be.
    if (s.GetHoldMode() != HoldMode::None && holdTargetPos != nullptr)
        s.UpdateHold(*holdTargetPos);

    // Piloting bonus (player skill) — a speed/maneuverability multiplier.
    s.SetPilotBonus(pilotBonus);
    s.Update(dt);
}

}  // namespace Sim
