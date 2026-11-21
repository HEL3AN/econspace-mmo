#pragma once

#include "core/ShipDesign.h"
#include "raylib.h"

// Combat agent — a shared interface for everything that takes part in combat:
// the player ship (Ship) and NPCs (NpcShip). Lets you target and deal damage
// uniformly, WITHOUT distinguishing the concrete type. This is the "player = just
// another agent, like an NPC" seam (track M, M1): the simulation operates on
// Combatants rather than special-casing the player.
//
// Deliberately separate from the scene list (the active system's entities): that
// one is client-side (selection, overview, radar, station detection). The combat
// set of agents is logically a different thing.
class Combatant
{
public:
    virtual ~Combatant() = default;

    virtual Vector2 GetPosition() const = 0;
    // A volley of `amount`, made of `mix` (Ships::TypeValues: the share of each damage type, the
    // rest untyped): shield, armour and hull each take it at their own multiplier (#464, slice
    // 8). Without a mix it is untyped -- 1.0 against everything.
    virtual void  TakeDamage(float amount, const Ships::TypeValues& mix) = 0;
    void          TakeDamage(float amount) { TakeDamage(amount, Ships::TypeValues{}); }
    virtual bool  IsAlive() const = 0;
    virtual float GetHull() const = 0;
    virtual float GetMaxHull() const = 0;
};
