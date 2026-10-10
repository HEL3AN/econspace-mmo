#pragma once

#include <array>

#include "entities/Entity.h"
#include "economy/Market.h"
#include "core/Faction.h"
#include <map>
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

    // Server-side only; never encoded. Which player each end of the beam is, if any.
    // The two flags above are written per recipient when a snapshot is built, so a shot
    // is "mine" or "at me" for exactly the player it concerns and for nobody else (#3).
    int shooterSessionId = 0;
    int targetSessionId = 0;
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

    // Whether a player has ever been here (#143). Known space starts visited; a system
    // beyond the wormhole does not, and until somebody goes there it is nobody's to take:
    // a region that changed hands before anyone arrived is a world that ran without its
    // players. Saved; an older save without it reads the default.
    bool visited = true;

    // Macro passes in a row that one side has held the upper hand (#225). Transient: a
    // restart begins the count again.
    int contested = 0;

    // Each faction's committed strength here, in ships (#231), by FactionId. The spawn
    // director makes real ships of it; battles take it away; the faction step grows it
    // where the faction holds and moves it where the faction reaches. Saved.
    std::array<float, FACTION_COUNT> presence{};
    // Whether anybody holds it at all. A system beyond the wormhole starts held by nobody:
    // its controller reads Independent, but no garrison grows there and any faction may
    // take it. Tau Verge is held -- by the Independents -- and only an enemy of theirs
    // may. Saved.
    bool claimed = true;

    // Who got here first, and what they called it (#145). A system beyond the wormhole
    // has a designation until its discoverer names it; the name is world state, seen by
    // everyone and kept. Saved.
    std::string discoverer;
    std::string givenName;
    // Ships destroyed since the last faction step, by side -- transient.
    float     lostPirates = 0.0f;
    float     lostPolice = 0.0f;
    FactionId policeFaction = FactionId::Independent;  // whose police the director sent

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

    // Seconds until each defensive object fires again, by entity id (#193). Transient:
    // a station that has just been hydrated is simply ready.
    std::map<int, float> defenceCooldown;
};
