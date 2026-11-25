#pragma once

#include "core/ShipDesign.h"

// Locking a target (#619, documents/combat_and_flight.md 3.1): the one rule a pilot's ship and
// an NPC's both follow. A lock is a commitment -- it takes time, it holds a slot, it is shown
// to the one locked -- and it is what a gun fires at. Looking at something (selecting it) is
// free and does nothing to anyone.
//
// No simulation and no entities here: the rule is a time from a signature, and a small timer
// that forms, holds and loses. Whoever owns the ship asks the world whether the target is
// still there to be locked and feeds the answer in.
namespace Ships
{
// Seconds to lock a target of `signature` (its size, a design fact) with sensors that resolve
// at `scanRes`: LOCK_BASE * sqrt(reference / signature) / scanRes, clamped to the rules'
// [lockMin, lockMax]. A station is locked at once, a frigate takes a couple of seconds, and
// being small is a trade the design grammar already exposes.
float LockTime(const Rules& rules, float signature, float scanRes);

// One lock: forming (`held` false, `progress` below 1) or held. `lost` counts the seconds the
// target has been out of reach or out of sight, against the rules' grace.
struct Lock
{
    int   target = 0;      // an entity's id; 0 -- the slot is empty
    float need = 0.0f;     // seconds this lock takes
    float elapsed = 0.0f;  // seconds of it done
    float lost = 0.0f;
    bool  held = false;

    bool  Empty() const { return target == 0; }
    float Progress() const { return held ? 1.0f : (need > 0.0f ? elapsed / need : 1.0f); }

    // Begin a lock on `id` that takes `seconds`.
    void Begin(int id, float seconds)
    {
        *this = Lock();
        target = id;
        need = seconds;
    }
    void Clear() { *this = Lock(); }
    // Advance by `dt`. `reachable`: the target is within range and in sight this tick. A lock
    // still forming is lost the moment it is not; a held one lasts `grace` seconds without.
    // Returns false when the lock is lost and has been cleared.
    bool Step(float dt, bool reachable, float grace)
    {
        if (target == 0)
            return false;
        if (!reachable)
        {
            lost += dt;
            if (!held || lost > grace)
            {
                Clear();
                return false;
            }
            return true;
        }
        lost = 0.0f;
        if (!held)
        {
            elapsed += dt;
            if (elapsed >= need)
                held = true;
        }
        return true;
    }
};
}  // namespace Ships
