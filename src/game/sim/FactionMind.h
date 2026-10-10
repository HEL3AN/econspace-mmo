#pragma once

#include "core/Faction.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// What factions know, and what they have set in motion (#295, slice 2).
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
    std::vector<FactionId>           defenders;   // owners of defensive stations
    std::array<float, FACTION_COUNT> presence{};  // each faction's ships there
    FactionId                        controller = FactionId::Independent;
    bool                             claimed = false;
};

// Something a faction has set in motion that ends at a time (#295). Today the only kind is
// a survey, and it exists only as this record: nobody flies it. It has an origin and a due
// time so that a real expedition (#295's variant B) can carry one later -- leave `from`,
// travel, arrive by `dueAt` -- without the decision or the result changing. Saved.
struct Plan
{
    enum class Kind
    {
        Survey
    };
    int         id = 0;
    Kind        kind = Kind::Survey;
    FactionId   faction = FactionId::Independent;
    std::string from;                          // the holding it set out from
    std::string target;                        // the system it is about
    double      startedAt = 0.0, dueAt = 0.0;  // world time
};

// One faction's knowledge, by system id. Saved.
struct FactionMind
{
    std::map<std::string, Intel> intel;
};

// The world's history as it happens (#295): what the 8-line news feed was, with a time, a
// sequence number and what it is about, so it can be read back and followed. Saved.
struct ChronicleEntry
{
    long long   seq = 0;
    double      time = 0.0;    // world time
    std::string kind;          // "survey", "news"
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
