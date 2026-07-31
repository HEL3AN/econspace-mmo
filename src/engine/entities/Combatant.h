#pragma once

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
    virtual void    TakeDamage(float amount) = 0;
    virtual bool    IsAlive() const = 0;
    virtual float   GetHull() const = 0;
    virtual float   GetMaxHull() const = 0;
};
