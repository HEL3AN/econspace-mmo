#pragma once

#include "core/Blueprint.h"
#include "core/Faction.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
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
    double                 seenAt = 0.0;    // world time
    float                  traffic = 0.0f;  // prosperity, 0..1
    float                  security = 0.5f;
    int                    belts = 0, wrecks = 0, stations = 0;
    int                    planets = 0, gates = 0;  // what an outpost is for (#318)
    std::vector<FactionId> defenders;               // owners of defensive stations
    std::vector<FactionId> outposts;    // owners of finished outposts: what a siege is for
    PerFaction<float>      presence{};  // each faction's ships there
    FactionId              controller = Factions::Unaffiliated();
    bool                   claimed = false;
};

// Something a faction has set in motion that ends at a time (#295). A survey exists only
// as this record: nobody flies it. A settlement is this record and an outpost site standing
// in the target system, which anyone there can see. A siege is this record and, where somebody
// is to see it, the warships standing at the outpost it is against. Each has an origin and a due
// time so that a real expedition (#295's variant B) can carry one later -- leave `from`, travel,
// arrive by `dueAt` -- without the decision or the result changing. Saved.
//
// A siege is pooled (#295, the owner's decision of 2026-10-11): its force is lent by several
// of the faction's holdings within reach of the target, each contingent a number of hops away.
// They muster on the way and stand at the outpost together once the farthest has come, and
// what is left of them goes home the same way when it is over -- a Return, one per holding.
struct Plan
{
    enum class Kind
    {
        Survey,
        Settle,   // an outpost is being built; finished, it claims the system
        Siege,    // strength sent to bring down another faction's outpost (slice 5)
        Return,   // what is left of a siege's contingent, on its way home to `target`
        Anchor,   // an Anchor going up at a wormhole mouth in `target` (#342 S4)
        Upgrade,  // an outpost growing into a market, or a market into a station (#342 S6)
        // Police from next door answering a crime in `target` (#431 slice 9): lent like a
        // siege's force and sent home the same way. Never saved: a save writes it as the
        // Returns it would end in, as a restart begins with every system cold.
        Response
    };
    // One holding's share of a siege: where it came from, how much, and how many jumps away.
    struct Lent
    {
        std::string from;
        float       force = 0.0f;
        int         hops = 1;
    };
    int         id = 0;
    Kind        kind = Kind::Survey;
    FactionId   faction = Factions::Unaffiliated();
    std::string from;                          // the holding it set out from
    std::string target;                        // the system it is about
    double      startedAt = 0.0, dueAt = 0.0;  // world time
    // A settlement's site, or the outpost a siege is against, by the key a save knows it by
    // ("+3"): an entity id does not survive a restart, and the plan has to find it after one.
    std::string site;
    // A siege's: whose outpost it is, and the strength still standing before it, in ships.
    // The force is part of the faction's presence in the target -- the same ships, counted
    // once -- set apart from what the spawn director makes police or pirates of.
    // A Return's force is what is coming home.
    FactionId against = Factions::Unaffiliated();
    float     force = 0.0f;
    // A siege's contingents, as they set out (their sum is the force it began with), the
    // neighbour of the target they come in from, and when the last of them gets there. Until
    // then the force is on the way: counted nowhere, firing at nothing, and missing from home.
    std::vector<Lent> lent;
    std::string       via;
    double            arrivesAt = 0.0;
    bool              arrived = false;
    // How many of its ships are real, while the target is hot: what a pass counts the
    // living against. Transient -- a cold system has none, and a restart begins cold.
    int fielded = 0;

    // --- The builder convoy (#342 S6): an Anchor's and an Upgrade's, until it has arrived ---
    // What an Upgrade builds (a blueprint id), and the region whose mouth an Anchor holds.
    std::string blueprint;
    int         slot = -1;
    // The way from the holding it set out from to `target`, inclusive, and where along it the
    // convoy is: `route[leg]` since `legAt`, moving on `hop` seconds later. A leg of -1 is a
    // convoy yet to set out -- the first, or the next after one did not get through -- at
    // `legAt`; a route with it is the way it is waiting to take (#407). Its escort is `force`
    // (lent by `lent`, one holding), and `freight` is what it carries, 1 whole and 0 lost --
    // waiting to set out, what is left of the last one, the rest to be paid for again.
    // `setbacks` counts the convoys that did not get through, and `waits` the times the faction
    // held one back or one turned back: past the rules' most, it goes whatever the odds.
    std::vector<std::string> route;
    int                      leg = -1;
    double                   legAt = 0.0;
    float                    freight = 1.0f;
    int                      setbacks = 0;
    int                      waits = 0;
    // Where its ships are real, and which is the hauler. Transient, like `fielded`.
    std::string fieldedIn;
    int         hauler = 0;
    // The most escort strength pilots have lent it in the system it is crossing, and whether
    // any pilot has been with it at all (#407). Transient: a restart begins cold, and alone.
    float guard = 0.0f;
    bool  escorted = false;
    // A Response's: the pilot it answers, by account. Transient, like the Response.
    std::string pursued;
    // How many pilots' deliveries its site has had (#345): each one took a share off the time it
    // has left to build, up to the jobs rules' most. Saved.
    int supplied = 0;
    // Whether it is a convoy still on its way: an Anchor or an Upgrade before it has arrived.
    bool OnTheWay() const { return (kind == Kind::Anchor || kind == Kind::Upgrade) && !arrived; }
    // Where its convoy is (#407): the system it is crossing, or the one it waits to set out
    // from; empty while it has no way yet.
    std::string ConvoyAt() const
    {
        if (leg >= 0 && (size_t)leg < route.size())
            return route[(size_t)leg];
        return route.empty() ? std::string() : route.front();
    }
};

// One faction's knowledge, by system id, and what it has to spend. Saved.
struct FactionMind
{
    std::map<std::string, Intel> intel;
    // Materials and credits as one number (#295): what its holdings yield, and what an
    // outpost costs. It is what bounds expansion -- a timer does not.
    float stock = 0.0f;
    // Credits (#343): the fee every trade at its markets pays. Nothing spends it yet -- it is
    // what a faction will pay its hires from -- but the money is the faction's, not gone.
    // Saved.
    double treasury = 0.0;
    // Its outposts it can no longer supply, by system and key ("W-1.1/+3") (slice 5): the
    // history says so once when one is cut off, not every period it stays so. Saved.
    std::set<std::string> unsupplied;
    // When it last lost an outpost, or a site, in each system (slice 5): a faction remembers
    // where it was beaten, and thinks twice before building there again. Saved.
    std::map<std::string, double> lostAt;
    // Word of a mouth (#342 S6): the regions beyond mouths that opened near its space, by slot,
    // and until when going that way is worth its expedition curiosity. Saved.
    std::map<int, double> expeditions;
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

// What an outpost grows into (#342 S6), in data/blueprints.json: a market, then a station.
inline constexpr const char* MARKET_BLUEPRINT = "outpost_market";
inline constexpr const char* TRADE_STATION_BLUEPRINT = "trade_station";
inline constexpr const char* MINING_STATION_BLUEPRINT = "mining_station";
inline constexpr const char* SHIPYARD_BLUEPRINT = "shipyard";  // by need, at most one a region

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
// An outpost market (#342 S6): an outpost that has grown a market and a dock. Still an
// outpost in every other way -- the claim, its upkeep, a siege's target.
inline bool IsMarket(const std::string& archetype)
{
    const Blueprint* bp = Blueprints::Find(MARKET_BLUEPRINT);
    return bp != nullptr && bp->archetype == archetype;
}
inline bool IsOutpost(const std::string& archetype)
{
    Purpose p;
    return PurposeOf(archetype, p) || IsMarket(archetype);
}

// What an outpost is called without the article, for "the Traders Guild watch post": its
// purpose's word, or "outpost market" for one that has grown a market (#342 S6).
inline std::string Noun(const std::string& archetype)
{
    if (IsMarket(archetype))
        return "outpost market";
    Purpose p = Purpose::Open;
    PurposeOf(archetype, p);
    const std::string called = Called(p);
    return called.substr(called.find(' ') + 1);
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
    return Factions::Find(owner.substr(prefix.size()), f);
}
}  // namespace Outposts

// The world's history as it happens (#295): what the 8-line news feed was, with a time, a
// sequence number and what it is about, so it can be read back and followed. Saved.
struct ChronicleEntry
{
    long long                seq = 0;
    double                   time = 0.0;  // world time
    std::string              kind;        // "survey", "settle", "news"
    std::optional<FactionId> faction;     // or nobody in particular
    std::string              system;      // id, or empty
    std::string              text;
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
// What a faction fears in a system where it has lost an outpost (#295, slice 5), at the moment
// of the loss, fading to nothing over LOSS_MEMORY.
inline constexpr float  LOSS_RISK = 2.0f;
inline constexpr double LOSS_MEMORY = 10800.0;
inline float            LossRisk(const std::map<std::string, double>& lostAt, const std::string& id,
                                 double now)
{
    const auto it = lostAt.find(id);
    if (it == lostAt.end())
        return 0.0f;
    return LOSS_RISK * (float)std::max(0.0, 1.0 - (now - it->second) / LOSS_MEMORY);
}

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
