#include "sim/PlayerStep.h"

#include "entities/Ship.h"

#include <cmath>

namespace Sim
{

void StepPlayerShip(Ship& s, const Proto::Command& cmd, float pilotBonus, float dt,
                    const HoldTarget* hold)
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
        s.EngageWarp(cmd.navTarget, cmd.navStopDist, cmd.navViaSet, cmd.navVia);
    }
    else if (cmd.navMode == 3 || cmd.navMode == 4 || cmd.navMode == 5)
    {
        const HoldMode mode = cmd.navMode == 3   ? HoldMode::Orbit
                              : cmd.navMode == 4 ? HoldMode::Keep
                                                 : HoldMode::Follow;
        s.EngageHold(mode, cmd.navHoldId, cmd.navRange);
    }

    // A standing hold re-aims the autopilot every tick, because the thing it is holding
    // station on is moving. Without a position it simply stops steering rather than
    // flying at a stale point -- a target that left the system should not drag the ship
    // to where it used to be.
    if (s.GetHoldMode() != HoldMode::None && hold != nullptr)
        s.UpdateHold(hold->pos, hold->vel);

    // Piloting bonus (player skill) — a speed/maneuverability multiplier.
    s.SetPilotBonus(pilotBonus);
    s.Update(dt);
}

Vector2 DockBearing(Vector2 stationPos, Vector2 shipPos)
{
    const float dx = shipPos.x - stationPos.x, dy = shipPos.y - stationPos.y;
    const float d = sqrtf(dx * dx + dy * dy);
    if (d < 0.001f)
        return { 1.0f, 0.0f };
    return { dx / d, dy / d };
}

Vector2 DockBerth(Vector2 stationPos, float stationSize, Vector2 bearing)
{
    const float r = stationSize + DOCK_BERTH_CLEARANCE;
    return { stationPos.x + bearing.x * r, stationPos.y + bearing.y * r };
}

}  // namespace Sim
