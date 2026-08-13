#pragma once

#include "entities/Entity.h"
#include "economy/Market.h"
#include "core/Faction.h"
#include <memory>
#include <string>
#include <vector>

// Fire event for a simulation step — a server-side, render-independent fact of "who
// shot whom". The client turns it into a beam (Beam) of the right color. This is the
// "server emits events, client draws" seam (track M, M2/M3).
struct FireEvent
{
    Vector2   from;
    Vector2   to;
    FactionId shooterFaction;
    bool      targetIsPlayer = false;  // target is the player (client colors the beam ORANGE)
    bool      fromPlayer = false;      // the player fired (client colors the beam SKYBLUE)
};

// Cold aggregate of a system: cheap statistical state that ALWAYS exists (even for
// unvisited systems). It drives the life of systems without the player, and real
// ships are materialized (hydrated) from it on entry.
struct SystemAggregate
{
    // Population by role — float for smooth evolution; rounded on hydrate.
    float traders = 0.0f;
    float miners = 0.0f;
    float police = 0.0f;
    float pirates = 0.0f;

    float security = 0.5f;      // dynamic security 0..1 (drifts)
    float baseSecurity = 0.5f;  // base (from universe.json) — attraction point
    float prosperity = 0.5f;    // economy health 0..1

    // Macrodynamics (L3): who actually controls the system. Can change:
    // pirate seizure on security collapse, reconquest by a strong neighbor.
    FactionId controller = FactionId::Independent;

    // Spawn "pressure" by role (0..1): recent losses temporarily lower the spawn
    // director's target, so player sweeps/battles really affect the population instead
    // of being topped up instantly. Decays slowly (recovery). Transient — not written
    // to the save (0 on load = the world has "rested").
    float supTraders = 0.0f;
    float supMiners = 0.0f;
    float supPolice = 0.0f;
    float supPirates = 0.0f;

    bool seeded = false;  // aggregate initialized with starting values
};

// State of a single star system inside the simulation. Level of detail:
//  - cold: only `agg` (entities empty) — for systems without the player;
//  - hot:  full `entities` (materialized on player entry).
// `agg` always lives; `entities` are filled on entry (hydrate) and cleared
// on exit (dehydrate, writing the numbers back into `agg`).
struct SystemState
{
    std::string                          id;
    std::vector<std::unique_ptr<Entity>> entities;
    Market                               market;
    bool                                 populated = false;  // entities filled (hot)
    SystemAggregate                      agg;                // cold state (always)
};
