#include "core/Prediction.h"

#include "entities/Ship.h"
#include "entities/ShipType.h"

#include <algorithm>
#include <string>

const Proto::Command& Prediction::Predict(Ship& ship, const Proto::Command& cmd, const HoldFn& hold)
{
    pending_.push_back(cmd);
    pending_.back().seq = (int)++seq_;
    if (pending_.size() > MAX_PENDING)  // guard against growth if the server stalls
        pending_.erase(pending_.begin());
    // pilotBonus = 1, as on the server.
    Sim::StepPlayerShip(ship, pending_.back(), 1.0f, Sim::SIM_DT, hold ? hold() : nullptr);
    return pending_.back();
}

void Prediction::Reconcile(Ship& ship, const Proto::PlayerView& p, const HoldFn& hold)
{
    // The server sent the authoritative state and the sequence number of the last input it
    // processed (lastInput). Drop the acked inputs, reset the ship to the server state, and
    // REPLAY the remaining ones. Hull/shields come from the server (combat is server-side).
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                  [&](const Proto::Command& c) { return c.seq <= p.lastInput; }),
                   pending_.end());
    // The hull being flown, before anything is replayed: prediction flies the ship by the
    // same derived numbers the server does (#279 step 5), and it used to fly every hull as
    // the starter -- a Courier predicted at a Scout's speed and was pulled back. A refit is
    // a design of its own (#279 fitting), and the server sent what it is first.
    {
        std::string design;
        for (size_t i = 0; i < p.ownedShips.size() && i < p.shipDesigns.size(); i++)
            if (p.ownedShips[i] == p.shipIndex)
                design = p.shipDesigns[i];
        const std::vector<ShipType>& catalog = GetShipCatalog();
        if (design.empty() && p.shipIndex >= 0 && p.shipIndex < (int)catalog.size())
            design = catalog[p.shipIndex].stats.design;
        ShipStats stats;
        if (ship.GetStats().design != design && StatsOfDesign(design, stats))
            ship.Refit(stats);
    }
    ship.ApplyView(p.pos, p.heading, p.vel, p.hull, p.shields);
    // Warp/AP are server-authoritative: we mirror the server's warp scale and don't keep our
    // own. Applied BEFORE replay so unacked orders (which the server hasn't seen yet)
    // correctly "carry through" via prediction over the authoritative state.
    ship.ApplyNavView(p.warpPhase, p.warpAlign, p.warpTarget, p.warpDrop, p.warpViaSet, p.warpVia,
                      p.autopilot, p.apTarget, p.apStop, p.holdMode, p.holdTargetId, p.holdRange);
    // Toggles (stabilizer/mining) are server-authoritative: restore from the snapshot BEFORE
    // replay, otherwise unacked toggle commands would flicker during replay (like warp).
    ship.SetStabilizerOn(p.stabilizer);
    ship.SetMiningOn(p.mining);
    for (const Proto::Command& c : pending_)
        Sim::StepPlayerShip(ship, c, 1.0f, Sim::SIM_DT, hold ? hold() : nullptr);
}
