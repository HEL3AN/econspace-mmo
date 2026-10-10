#pragma once

#include <array>

#include "entities/Entity.h"
#include "economy/Market.h"
#include "core/Faction.h"
#include <map>
#include <memory>
#include <set>
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

// What a system IS, as opposed to what is happening in it (#295): counted once from its
// static layer, which is built from the document the seed makes plus what a save says
// changed, and counted again whenever that layer changes (AddStatic, RemoveStatic, a site
// finishing). Never
// saved -- it is remade exactly as the region is. The macro layer reads this rather than
// walking the system's entities, so a system need not be materialized to be weighed.
struct SystemProfile
{
    int belts = 0;     // asteroid fields
    int wrecks = 0;    // derelicts, searched or not
    int stations = 0;  // stations of any role
    int planets = 0;
    int gates = 0;
    // The owner of each station that defends itself, one entry per station.
    std::vector<FactionId> defenders;
    // The owner of each finished faction outpost (#295), one entry per outpost: what a
    // claim on a system nobody held needs. A site still being built is not one yet.
    std::vector<FactionId> outposts;
    bool                   outpostSite = false;  // anyone's outpost stands or is going up
};

// State of a single star system inside the simulation. Level of detail (#295):
//  - cold: the static layer (stations, bodies, gates, structures) and `agg`, and no ships.
//    Its population is the aggregate's numbers, kept by arithmetic once a coarse pass.
//  - hot:  the same plus real NPC ships, stepped every tick. A system is hot while a
//    player is in it and for a while after (Simulation::COOL_AFTER).
// `agg` always lives; ships are made from it when a system warms (hydrate) and counted back
// into it when it cools (dehydrate).
struct SystemState
{
    std::string                          id;
    std::vector<std::unique_ptr<Entity>> entities;
    Market                               market;
    bool                                 populated = false;  // NPC ships exist (hot)
    SystemAggregate                      agg;                // cold state (always)
    SystemProfile                        profile;            // static summary, not saved
    // The world time until which a hot system stays hot with nobody in it: pushed on by
    // every tick a player is here. Transient -- a restart begins with every system cold.
    double warmUntil = 0.0;

    // Seconds until each defensive object fires again, by entity id (#193). Transient:
    // a station that has just been hydrated is simply ready.
    std::map<int, float> defenceCooldown;

    // The static layer's revision and what changed in it since the last LayoutDelta went
    // out (#38), by entity id. Transient like the rest of a hot system: a restart begins
    // at revision 0, and every client is sent a whole layout then anyway.
    int           layoutRev = 0;
    std::set<int> pendingAdded, pendingChanged, pendingRemoved;

    // What a save calls each object of the static layer that may change (#38), by entity id.
    // An id is good for one run -- one counter runs through every system and the NPCs
    // hydrated beside them -- so a save names an object by a key that comes out the same on
    // every load: for the world's own, the array of the system document it was built from,
    // its name and which of that name it is ("stations/Aurora Hub#0"); for one added since,
    // "+" and a number this system never hands out twice. Remade on every load, not saved.
    std::map<int, std::string> keys;
    // The world's own objects somebody has taken away, by key, and the number the next one
    // added here will be keyed by. Saved: with what is added and what has changed, they are
    // what a restart replays on top of the generated or written system.
    std::set<std::string> removedKeys;
    int                   nextAddedKey = 1;
    // What a loaded save says to put back into this system, as JSON text (the header stays
    // free of the JSON library). Replayed once, when the static layer is built, then cleared.
    std::string restore;
};
