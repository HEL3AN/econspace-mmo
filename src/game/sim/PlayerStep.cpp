#include "sim/PlayerStep.h"

#include "entities/Ship.h"

namespace Sim
{

void StepPlayerShip(Ship& s, const Proto::Command& cmd, float pilotBonus, float dt)
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
    }
    s.SetControls(cmd.thrust, cmd.turn, cmd.brake);

    // Navigation order (one-shot): autopilot/warp to a point. Applied after manual
    // control — if the player did not touch the axes, the order takes effect.
    if (cmd.navMode == 1)
        s.EngageAutopilot(cmd.navTarget, cmd.navStopDist);
    else if (cmd.navMode == 2)
        s.EngageWarp(cmd.navTarget, cmd.navStopDist);

    // Piloting bonus (player skill) — a speed/maneuverability multiplier.
    s.SetPilotBonus(pilotBonus);
    s.Update(dt);
}

}  // namespace Sim
