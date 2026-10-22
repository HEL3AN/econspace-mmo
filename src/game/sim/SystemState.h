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
    // ...and which object of the system each end is, if it is one (#415): a beam from or at
    // something a pilot has not found is not theirs to see.
    int shooterEntityId = 0;
    int targetEntityId = 0;
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
    FactionId controller = Factions::Unaffiliated();

    // Spawn "pressure" by role (0..1): recent losses temporarily lower the spawn
    // director's target, so player sweeps/battles really affect the population instead
    // of being topped up instantly. Decays slowly (recovery). Transient — not written
    // to the save (0 on load = the world has "rested").
    float supTraders = 0.0f;
    float supMiners = 0.0f;
    float supPolice = 0.0f;
    float supPirates = 0.0f;

    // Whether the system is on the public charts (#431 K1): a civilisation's own space and
    // whatever a world faction has charted (`chartedBy`). Not saved: it follows from where
    // the system is and from `chartedBy`. Everything else on a pilot's map is the pilot's or
    // their corporation's (ClientSession::known). It replaced `visited` (#143), one flag
    // for everybody, which an older save's systems are translated from on load.
    bool publicChart = true;
    // The world faction whose charts put it on the public ones, by id (Simulation::ChartBy);
    // empty for none -- a centre's systems need nobody's. Saved.
    std::string chartedBy;

    // Macro passes in a row that one side has held the upper hand (#225). Transient: a
    // restart begins the count again.
    int contested = 0;

    // Each faction's committed strength here, in ships (#231), by FactionId. The spawn
    // director makes real ships of it; battles take it away; the faction step grows it
    // where the faction holds and moves it where the faction reaches. Saved.
    PerFaction<float> presence{};
    // Whether anybody holds it at all. A system beyond the wormhole starts held by nobody:
    // its controller reads Independent, but no garrison grows there and any faction may
    // take it. Tau Verge is held -- by the Independents -- and only an enemy of theirs
    // may. Saved.
    bool claimed = true;

    // Who got here first, and what they called it (#145). A system beyond the wormhole
    // has a designation until its discoverer names it; the name is world state, seen by
    // everyone and kept. Saved. The discoverer is a pilot who came before any faction; where
    // a faction came first it stays empty (#431 K3). The name may also be its natives', known
    // from first contact, or the one the major that charted it first gave it (#431 slice 2):
    // either way nobody names it again.
    std::string discoverer;
    std::string givenName;
    // The player corporation that holds it (#431 T2), by its id; empty for none. Exactly one
    // side holds a system: while a corporation does, no faction claims it (`claimed` false,
    // the controller nobody's). Saved.
    std::string holderCorp;
    // Who is taking it from its holder by influence, and for how many macro passes in a row
    // (slice 5): a party's key (InfluenceMap::Key). Transient, like `contested`.
    std::string claimant;
    int         claimContest = 0;
    // Ships destroyed since the last faction step, by side -- transient.
    float     lostPirates = 0.0f;
    float     lostPolice = 0.0f;
    FactionId policeFaction = Factions::Unaffiliated();  // whose police the director sent
    // What the holding corporation paid for police here in the last macro pass (#431 L5,
    // slice 11), in credits an hour; 0 when it pays nothing, or could not. It raises the
    // security target, puts the world's police in the air, and makes the corporation's law
    // the system's (Simulation::LawIn). Transient: the first pass after a start pays again.
    float paidPolice = 0.0f;
    // Corporations' patrols here (#431 C1, slice 12): ships each keeps, by corporation id --
    // presence, like a faction's, that the coarse pass pays for and makes good and that
    // fights and is lost by the same arithmetic as any NPC's, hot or cold. Saved.
    std::map<std::string, float> patrols;

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
    int belts = 0;      // asteroid fields
    int wrecks = 0;     // derelicts, searched or not
    int stations = 0;   // stations of any role
    int markets = 0;    // what a ship can trade at: a station's market, a finished outpost market
    int shipyards = 0;  // where a ship is refitted: a station's, or a finished structure's (#432)
    int planets = 0;
    int gates = 0;
    // The owner of each station that defends itself, one entry per station.
    std::vector<FactionId> defenders;
    // The owner of each finished faction outpost (#295), one entry per outpost: what a
    // claim on a system nobody held needs. A site still being built is not one yet -- unless
    // it is an outpost being upgraded (#342 S6), which holds the claim it held while it grows.
    std::vector<FactionId> outposts;
    std::vector<int>       outpostIds;           // ...and which entity each is, in the same order
    bool                   outpostSite = false;  // anyone's outpost stands or is going up
    // The owner of each faction's Anchor at a wormhole mouth (#342 S4), built or going up --
    // what a rival besieges to bring the race for the region back -- and which entity it is.
    std::vector<FactionId> anchors;
    std::vector<int>       anchorIds;
    // What its structures and stations weigh toward their owners' influence (#431 T1), by
    // tier (WorldRules::Influence): a faction's by FactionId, a player's by owner account --
    // an account and not a corporation, because who is in which changes without this layer
    // changing; InfluenceIn resolves it. A site still going up weighs nothing yet.
    PerFaction<float>            factionWeight{};
    std::map<std::string, float> playerWeight;
    // The accounts with a claim here (T2): a finished structure of claim tier -- an outpost or
    // above -- that can itself be besieged. Kept like `outposts` is for factions.
    std::set<std::string> playerClaims;
    // Every account with a structure here, finished or a site (#431 slice 6): where licences
    // are looked at each pass.
    std::set<std::string> builders;
    // What its belts, planets and markets do to its market every hour (#343).
    MarketRates rates;
};

// Every party's influence in one system as of now (#431 T1): the profile's structure weights
// with owner accounts resolved to their corporations, and each faction's presence. Made when
// asked (Simulation::InfluenceIn), never kept: corporation membership and presence change
// without the static layer changing. A party is a faction, a corporation (by id), or a pilot
// in none -- who counts as a corporation of one for influence, and can block a claim, but
// holds nothing: holding needs a treasury for the tax to go to.
struct InfluenceMap
{
    PerFaction<float>            faction{};
    std::map<std::string, float> corp;         // by corporation id, or PILOT + account
    std::set<std::string>        corpClaims;   // ...those with a claim here
    static constexpr const char* PILOT = "~";  // an account's key: no corporation id has it

    // A party's key: a faction's "f:<id>", a corporation's or pilot's as in `corp`.
    static std::string Key(FactionId f) { return "f:" + Factions::Id(f); }
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

    // Ships that leave (#295), by role (NpcRole's first four), as of this coarse pass: how many
    // it holds above what it keeps, how many it lacks of what the director would send counting
    // those on their way in, and whether a neighbour has one to spare -- then the director
    // leaves that role to next door. Transient: remade every pass before anything reads it.
    std::array<float, 4> spare{}, lack{};
    std::array<bool, 4>  fromNextDoor{};

    // Seconds until each defensive object fires again, by entity id (#193). Transient:
    // a station that has just been hydrated is simply ready.
    std::map<int, float> defenceCooldown;
    // Who has shot at each structure lately, by structure id: session id -> the world time
    // until which an armed structure fires back at them (#39). Transient, like the cooldown.
    std::map<int, std::map<int, double>> provokedBy;
    // ...and which NPCs, by structure id: NPC entity id -> until when (#340). An NPC never
    // shoots a structure, so it is here for shooting the owner's ship in its reach.
    std::map<int, std::map<int, double>> provokedByNpc;
    // Until when a structure's owner has been told that it is firing first where that is a
    // crime (#340): once a minute, not once a volley. Transient.
    std::map<int, double> crimeToldUntil;
    // Until when the police here are already answering a crime (#39, #431 slice 9): one call
    // brings them, and the next shot does not bring more. Transient.
    double policeCalledUntil = 0.0;
    // A call whose answer from here is still to come -- the slower the lower the security --
    // at what time, for which session, under whose law; a time of 0 is none. Transient.
    double    policeDueAt = 0.0;
    int       policeFor = 0;
    FactionId policeLaw = Factions::Unaffiliated();
    // Players' structures standing here without the holder's leave (#431 slice 6), by entity
    // id: what the holding faction weighs evicting. Remade each pass (StepLicences), not saved.
    std::vector<int> trespass;
    // How many of each corporation's patrol (#431 slice 12) are real ships here, while the
    // system is hot: what a pass counts the living against, as a siege does. Transient.
    std::map<std::string, int> patrolFielded;

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
