#pragma once

#include "core/Blueprint.h"
#include "core/Faction.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// What factions know, and what they have set in motion (#295, slices 2 and 3).
//
// A faction no longer reads the galaxy as it is. It decides on what it saw: its own
// holdings and wherever its ships are, which it sees for itself, and what its surveyors
// brought back from everywhere else -- which was true when they looked and may not be now.
// The longer ago that was, the less it trusts it, so a cautious faction looks again before
// it commits and a bold one gambles on old news.

// What a faction saw of one system, and when. Copied from the system's aggregate and
// profile at the moment of looking and never updated behind its back. Saved.
struct Intel
{
    double                           seenAt = 0.0;    // world time
    float                            traffic = 0.0f;  // prosperity, 0..1
    float                            security = 0.5f;
    int                              belts = 0, wrecks = 0, stations = 0;
    int                              planets = 0, gates = 0;  // what an outpost is for (#318)
    std::vector<FactionId>           defenders;               // owners of defensive stations
    std::array<float, FACTION_COUNT> presence{};              // each faction's ships there
    FactionId                        controller = FactionId::Independent;
    bool                             claimed = false;
};

// Something a faction has set in motion that ends at a time (#295). A survey exists only
// as this record: nobody flies it. A settlement is this record and an outpost site standing
// in the target system, which anyone there can see. Each has an origin and a due time so
// that a real expedition (#295's variant B) can carry one later -- leave `from`, travel,
// arrive by `dueAt` -- without the decision or the result changing. Saved.
struct Plan
{
    enum class Kind
    {
        Survey,
        Settle  // an outpost is being built; finished, it claims the system
    };
    int         id = 0;
    Kind        kind = Kind::Survey;
    FactionId   faction = FactionId::Independent;
    std::string from;                          // the holding it set out from
    std::string target;                        // the system it is about
    double      startedAt = 0.0, dueAt = 0.0;  // world time
    // A settlement's site, by the key a save knows it by ("+3"): an entity id does not
    // survive a restart, and the plan has to find its site after one.
    std::string site;
};

// One faction's knowledge, by system id, and what it has to spend. Saved.
struct FactionMind
{
    std::map<std::string, Intel> intel;
    // Materials and credits as one number (#295): what its holdings yield, and what an
    // outpost costs. It is what bounds expansion -- a timer does not.
    float stock = 0.0f;
};

// Outposts (#295, slice 3): a faction's claim on a system nobody held is a structure (#39)
// it built there, owned by the faction rather than by an account.
//
// An outpost is built FOR something (#318), and what it is for decides where it stands and
// what it looks like: a mining outpost by the richest belt, a watch post on the way between
// gates, an orbital outpost round the planet worth holding, a salvage post by the wrecks, a
// lawless faction's base where nobody looks. The purpose is what the faction valued in the
// system; each has its own blueprint and archetype in data, and all of them cost the same.
namespace Outposts
{
// The blueprint a faction builds one from when it has no particular purpose, in
// data/blueprints.json. What it costs is what every outpost costs.
inline constexpr const char* BLUEPRINT = "outpost";

enum class Purpose
{
    Open,     // nothing in particular: a sensible open point, not the gate
    Mining,   // beside the richest belt
    Watch,    // on the way between gates, or at the junction, touching none
    Orbital,  // a satellite of the planet worth holding (#210)
    Salvage,  // beside the wrecks
    Hidden    // a lawless faction's base: in a cloud, or out at the edge, off the gate lines
};
inline constexpr int PURPOSE_COUNT = 6;

// The blueprint each purpose is built from, and how the history names one.
inline const char* BlueprintOf(Purpose p)
{
    switch (p)
    {
        case Purpose::Mining: return "outpost_mining";
        case Purpose::Watch: return "outpost_watch";
        case Purpose::Orbital: return "outpost_orbital";
        case Purpose::Salvage: return "outpost_salvage";
        case Purpose::Hidden: return "outpost_hidden";
        case Purpose::Open: break;
    }
    return BLUEPRINT;
}
inline const char* Called(Purpose p)  // "a mining outpost"
{
    switch (p)
    {
        case Purpose::Mining: return "a mining outpost";
        case Purpose::Watch: return "a watch post";
        case Purpose::Orbital: return "an orbital outpost";
        case Purpose::Salvage: return "a salvage post";
        case Purpose::Hidden: return "a hidden base";
        case Purpose::Open: break;
    }
    return "an outpost";
}

// Which outpost an archetype is; false for anything that is not one.
inline bool PurposeOf(const std::string& archetype, Purpose& out)
{
    for (int i = 0; i < PURPOSE_COUNT; i++)
    {
        const Blueprint* bp = Blueprints::Find(BlueprintOf((Purpose)i));
        if (bp != nullptr && bp->archetype == archetype)
        {
            out = (Purpose)i;
            return true;
        }
    }
    return false;
}
inline bool IsOutpost(const std::string& archetype)
{
    Purpose p;
    return PurposeOf(archetype, p);
}

// Below this, nothing the faction values in a system is worth building for: an open point.
inline constexpr float PURPOSE_FLOOR = 0.2f;

// What a faction builds for, from what it saw (#318) -- by the same weights it chose the
// system by. A lawless faction hides, whatever it came for; anyone else builds for what it
// values most there: ore, the traffic of a corridor or a junction, a planet to hold, wrecks.
// A tie goes to the first in that order.
inline Purpose ChoosePurpose(const Temperament& t, bool lawful, const Intel& seen)
{
    if (!lawful)
        return Purpose::Hidden;
    // Two gates make a corridor everything passing through must use; three or more, a
    // junction. Nobody passes through a dead end.
    const float through = seen.gates >= 3 ? 1.0f : seen.gates == 2 ? 0.5f : 0.0f;
    const float worth[] = {
        0.0f,  // Open
        seen.belts > 0 ? t.ore * std::min(1.0f, seen.belts / 3.0f) : 0.0f,
        through > 0.0f ? t.traffic * std::max(through, std::clamp(seen.traffic, 0.0f, 1.0f)) : 0.0f,
        seen.planets > 0 ? t.unclaimed * std::min(1.0f, seen.planets / 2.0f) : 0.0f,
        seen.wrecks > 0 ? t.salvage * std::min(1.0f, seen.wrecks / 4.0f) : 0.0f,
    };
    Purpose best = Purpose::Open;
    float   most = 0.0f;
    for (int i = 1; i < (int)(sizeof(worth) / sizeof(worth[0])); i++)
        if (worth[i] > most)
        {
            most = worth[i];
            best = (Purpose)i;
        }
    return most >= PURPOSE_FLOOR ? best : Purpose::Open;
}
// The owner a structure a faction built carries. An account name has no colon in it, so
// the two never meet.
inline constexpr const char* OWNER_PREFIX = "faction:";
inline std::string           OwnerOf(FactionId f)
{
    return OWNER_PREFIX + Factions::Id(f);
}
// Which faction owns a structure; false for an account's or the world's.
inline bool FactionOf(const std::string& owner, FactionId& f)
{
    const std::string prefix = OWNER_PREFIX;
    if (owner.compare(0, prefix.size(), prefix) != 0)
        return false;
    const std::string id = owner.substr(prefix.size());
    f = FactionFromString(id);
    return Factions::Id(f) == id;
}
}  // namespace Outposts

// The world's history as it happens (#295): what the 8-line news feed was, with a time, a
// sequence number and what it is about, so it can be read back and followed. Saved.
struct ChronicleEntry
{
    long long   seq = 0;
    double      time = 0.0;    // world time
    std::string kind;          // "survey", "settle", "news"
    int         faction = -1;  // FactionId, or -1 for nobody in particular
    std::string system;        // id, or empty
    std::string text;
};

namespace Intelligence
{
// After this long what a faction saw is old news: its risk counts double, and a system it
// cares about is worth looking at again. A quarter of an hour of world time -- fifteen
// faction periods.
inline constexpr double STALE = 900.0;
// What a faction fears in a place it has not seen lately, beyond what it did see there:
// anything could have moved in since.
inline constexpr float UNSEEN_RISK = 0.25f;

// How old a piece of intel is, in units of STALE.
inline float Staleness(const Intel& i, double now)
{
    return (float)std::max(0.0, (now - i.seenAt) / STALE);
}

// The risk a faction believes in: what it saw grows with the age of the sighting, and so
// does what it could not have seen.
inline float BelievedRisk(float seenRisk, float staleness)
{
    return seenRisk * (1.0f + staleness) + UNSEEN_RISK * std::min(staleness, 2.0f);
}

// What looking at a system is worth: everything, for one it has never seen; nothing, for
// one it saw recently; and its curiosity again as the sighting goes stale.
inline float SurveyValue(float curiosity, const Intel* known, double now)
{
    if (known == nullptr)
        return curiosity;
    return curiosity * std::clamp(Staleness(*known, now) - 1.0f, 0.0f, 1.0f);
}

// A stable 64-bit name for a system id, for keying randomness by what is decided rather
// than by when (#140's rule, kept here although the server is the only authority).
inline uint64_t KeyOf(const std::string& id)
{
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : id)
        h = (h ^ c) * 1099511628211ull;
    return h;
}
}  // namespace Intelligence
