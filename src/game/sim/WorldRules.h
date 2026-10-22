#pragma once

#include <string>

// How the world grows (#342 S3): the rules of the tide, read from data/world_rules.json.
//
// Every region on the edge -- one with a free neighbouring slot in the skeleton -- gathers
// tide once a period, in the coarse pass, so it gathers it whether anybody is there or not:
//
//   tide += base * strain / (strain of the whole edge)       time, the world's, shared out
//         + perPlayer  * min(players in the region this period, playerCap)
//         + perExplore * knee * (explored / knee) ^ exploreCurve
//         + perSurvey  * faction surveys of the region this period
//
// `explored` is the share of the region's systems a player has been in, with those only a
// faction has surveyed counting `surveyedWeight` each. Bent by the curve, it counts for
// little below the knee and fast above it: a player who reaches the edge does not wait long.
// A region's strain is the seed's, per slot; it only staggers the edge, so that regions do
// not all break through on the same day.
//
// At `threshold * (1 + open regions / maxRegions)` -- slower toward the cap -- a region
// opens a mouth into its next slot, unless it opened one less than `minGapHours` ago or the
// world already has as many young mouths (under `mouthHours` old) as it allows: one, and one
// more for every `playersPerMouth` players on the server.
//
// Strict like every other data file (#191): an unknown field is a load error, not a
// default.
namespace WorldRules
{

struct Tide
{
    double period = 60.0;  // seconds of world time between two steps of the tide
    double threshold = 100.0;
    double base = 0.0278;  // per period, for the whole edge: one region in about 60 h
    double perPlayer = 0.15;
    int    playerCap = 10;
    double perExplore = 0.93;
    double exploreKnee = 0.6;
    double exploreCurve = 3.0;
    double surveyedWeight = 0.25;
    double perSurvey = 0.2;
    double minGapHours = 6.0;
    double mouthHours = 24.0;  // an unstable mouth's life before it collapses (S4)
    int    playersPerMouth = 20;
};

// What a mouth is until it is anchored, and what becomes of a region nobody comes to (#342
// S4): the file's `mouths` and `archive` sections.
//
// A mouth the tide opens is unstable: it lets through only ships of at most `maxMass`, and it
// collapses `mouthHours` after it opened unless an Anchor is finished at it. `warnHours`
// before, the chronicle says it is failing. With `anchors` off every mouth is permanent at
// once -- "the Rift", option A of the design.
struct Mouths
{
    bool   anchors = true;
    double maxMass =
        45.0;  // the most a ship may weigh to pass an unstable mouth: a scout or a courier
    double warnHours = 2.0;
};

// At the cap, a frontier region nobody has been in for `deadDays`, with no player structure
// in it, drifts away: the chronicle warns `warnDays` before, then it is archived and its slot
// is free. When a slot opens, an archived region comes back instead of a fresh one at
// `returnChance`.
struct Archive
{
    double deadDays = 7.0;
    double warnDays = 2.0;
    double returnChance = 0.35;
};

// A mouth opening is news (#342 S6): every faction holding a system within `reach` jumps of
// it learns what is on the far side -- intel on the entry system -- and for `hours` it values
// going that way `curiosity` more than it would: the systems between it and the mouth, the
// mouth, and the whole region beyond.
struct Expeditions
{
    int    reach = 3;
    double curiosity = 0.8;
    double hours = 8.0;
};

// Stations grown by need, as in X4 (#342 S6): an outpost becomes an outpost market (a market
// and a dock) where the nearest market is far -- `marketReach` jumps or more is as far as it
// matters -- and an outpost market becomes a station where enough passes through or is mined:
// a trade station or a mining station, whichever is the more, once it is at least
// `stationNeed`. What each is worth to a faction is `marketWorth` and `stationWorth` times
// the need, weighed against the risk like everything else it does. No region has more than
// `stationsPerRegion` stations, whoever built them.
// A shipyard (#432) is grown from an outpost market too, by need and never first: only where
// none stands within `shipyardReach` jumps, once the region has `shipyardAfter` stations of
// any other kind to bring pilots and at least `shipyardNeed` passes through or is mined there;
// never a second in a region, and counted among its stations. It is worth `shipyardWorth` times
// that need -- more than a further station, so the region's next station is its yard.
struct Growth
{
    int    stationsPerRegion = 4;
    int    marketReach = 3;
    double marketWorth = 1.2;
    double stationWorth = 1.2;
    double stationNeed = 0.45;
    int    shipyardAfter = 1;
    int    shipyardReach = 4;
    double shipyardWorth = 1.4;
    double shipyardNeed = 0.45;
};

// The builder convoy (#342 S6): an Anchor or an upgrade is built from what a convoy brings,
// along the gates from where the faction makes things -- of its holdings within `reach` jumps,
// the one with the most stations of its own (#407), or the nearest holding when none has one
// -- `hop` seconds in each system it passes.
//
// Its fate follows from its own strength against the dangers of its way (#407, the owner's
// decision of 2026-10-16): no convoy is doomed for want of a player.
// - Strength: the hauler's own `hauler` ships' worth, the escort its holding lends -- `escort`
//   of the faction's capacity, more for a dangerous way, `escortPerDanger` ships for every
//   ship of the worst danger on it, up to `escortMax` of its capacity -- and any pilots with it.
// - Danger: in each system it crosses, the ships of everyone hostile to it there. Before it
//   moves on it is attacked or not -- as likely as not where the danger is `calm` ships, the
//   less so the higher the security (`policed` less at security 1) -- and if it is, it loses
//   freight by how far the danger outweighs it (`harm`), escort by `bite` of the danger, and
//   takes `bite` of its strength off the attackers.
// - Fate: it arrives intact, or damaged (the shortfall made up from the faction's stock); with
//   less than `turnBack` of its freight left it turns back, as it does before a system whose
//   danger is `balk` times its strength; with none left, it is destroyed. The next sets out
//   `retryMinutes` later, and what was lost must be paid for again.
// - The faction chooses the way it fears least, a ship of danger counting as `avoid` jumps,
//   and holds a convoy back while it expects to lose more than `wait` of its freight -- at
//   most `maxWaits` times, after which it sends it anyway and it no longer balks.
struct Convoys
{
    double hop = 120.0;
    double escort = 0.25;
    double escortMax = 0.6;
    double escortPerDanger = 1.0;
    double hauler = 1.0;
    double calm = 3.0;
    double policed = 0.6;
    double harm = 1.5;
    double bite = 0.5;
    double turnBack = 0.5;
    double balk = 4.0;
    double avoid = 0.5;
    double wait = 0.35;
    int    maxWaits = 3;
    double retryMinutes = 10.0;
    int    reach = 6;
};

// Jobs a faction's plans post (#345). A station of the poster within `reach` jumps of what a
// plan is about shows its jobs, `perBoard` at most, nearest first. The faction pays out of its
// stock, `creditsPerStock` credits a unit, and posts nothing it cannot pay for.
// - Supply: `supplyShare` of the largest line of the site's cost, paid at `supplyMarkup` times
//   the good's price, handed in within `supplyRange` of the site's edge; each takes
//   `supplyCut` off the time the site has left, `supplyMax` deliveries a site at most.
// - Escort: `escortBase` and `escortPerJump` of the convoy's way; a pilot with the convoy counts
//   as `escortForce` ships of its escort. Due `escortSlack` seconds after it would arrive.
//   Posted within `reach` of where the convoy is, too: that is where a pilot meets it (#407).
// - Siege: shoot down `siegeKills` of the besiegers, `siegePerKill` each.
// Done, a job is worth `rep` standing with the poster; failed by the pilot, `failRep` less.
struct Jobs
{
    int    reach = 4;
    int    perBoard = 3;
    double creditsPerStock = 50.0;
    double supplyShare = 0.5;
    double supplyMarkup = 1.6;
    double supplyRange = 1500.0;
    double supplyCut = 0.35;
    int    supplyMax = 2;
    double escortBase = 400.0;
    double escortPerJump = 150.0;
    double escortForce = 2.0;
    double escortSlack = 600.0;
    int    siegeKills = 3;
    double siegePerKill = 250.0;
    double rep = 3.0;
    double failRep = 1.5;
};

// Roaming holes (#342 S9): EVE's wormholes, on a schedule of epochs of `epochHours`. At the
// start of each, every live region -- centres, frontiers, pack regions; not one archived --
// throws its own key: with `perRegion` (`atCap` once the galaxy is at its cap, where the tide
// can open nothing new; `drifted` for a region cut off by a collapsed mouth, so the stranded
// always have a way out) it gets a hole from one of its systems to a system of another live
// region. A hole opens somewhere in the epoch, lives `minHours` to `maxHours`, and lets
// through ships of at most its class's mass -- small, medium or large, drawn by the weights
// -- with a budget of `budgetMin` to `budgetMax` times that mass before it collapses. It is
// called ending in its last `endingHours`, and its mass low below `lowMass` of the budget.
struct Holes
{
    double epochHours = 4.0;
    double perRegion = 0.3;
    double atCap = 0.6;
    double drifted = 1.0;
    double minHours = 4.0;
    double maxHours = 24.0;
    double smallMass = 45.0;   // a scout or a courier
    double mediumMass = 60.0;  // ...and a cutter or a raider
    double largeMass = 100.0;  // anything the hangar sells
    double smallWeight = 0.4;
    double mediumWeight = 0.35;  // large is the rest
    double budgetMin = 4.0;
    double budgetMax = 10.0;
    double endingHours = 4.0;
    double lowMass = 0.5;
};

// The ship's scanner (#346): a pass takes `seconds` and adds `power` times a signature's
// strength to what the pilot has resolved of it; at 1 it is found.
struct Scan
{
    double seconds = 10.0;
    double power = 1.0;
};

// The police's answer to a crime (#431 slice 9): always real ships, never an invincible
// police, and stronger and quicker the higher the system's security. A system is in the high
// band from `highFrom`, the middle from `midFrom`, the low from `lowFrom`; below that nobody
// comes. In each band (high/mid/low):
// - `Local`: at most this many of the system's own free police warp to the crime -- all of
//   them, in practice, in the high band;
// - `Launch`: at most this many patrols a station of the law launches on top: first out of
//   the faction's presence here not yet flying, then bought out of its stock at `launchCost`
//   of an outpost's cost each;
// - `Reach` and `Called`: police from systems within that many jumps, at most that many in
//   all, lent like a siege's force: missing from home until they are back;
// - `Delay`: seconds before any of it comes; police from next door take `hop` a jump more.
// Police from elsewhere stay `stay` seconds, then go home. One call holds `callSeconds`: the
// next shot in that time brings nobody more. They drop out of warp `dropDistance` from the
// criminal, and a station's or a faction structure's guns fire on a criminal only at security
// `gunsFrom` and above.
//
// What a crime costs the pilot (#431 slice 10): a criminal flag for `flagSeconds`, renewed by
// each crime, that runs down only while they are in the world; a flagged pilot who drops the
// connection leaves the ship in space `flagLinger` seconds. Security status falls by
// `statusCrime` a crime and `statusKill` more for a kill, rises `statusPirateKill` a pirate
// shot down and `statusPerDay` a day in the world without a flag, back up to zero. At
// minus `onSightBelow` or below, police engage the pilot on sight in systems of `onSightFrom`.
struct Law
{
    double callSeconds = 60.0;
    double dropDistance = 1500.0;
    double gunsFrom = 0.5;
    double highFrom = 0.8;
    double midFrom = 0.5;
    double lowFrom = 0.1;
    int    highLocal = 99;
    int    midLocal = 4;
    int    lowLocal = 2;
    int    highLaunch = 4;
    int    midLaunch = 2;
    int    lowLaunch = 0;
    int    highReach = 2;
    int    midReach = 1;
    int    lowReach = 0;
    int    highCalled = 8;
    int    midCalled = 3;
    int    lowCalled = 0;
    double highDelay = 0.0;
    double midDelay = 20.0;
    double lowDelay = 45.0;
    double hop = 10.0;
    double stay = 180.0;
    double launchCost = 0.1;
    double flagSeconds = 900.0;
    double flagLinger = 60.0;
    double statusCrime = 1.0;
    double statusKill = 2.5;
    double statusPirateKill = 0.1;
    double statusPerDay = 0.5;
    double onSightBelow = 5.0;
    double onSightFrom = 0.5;

    // One band's numbers.
    struct Band
    {
        int    local = 0, launch = 0, reach = 0, called = 0;
        double delay = 0.0;
    };
    // The band a system of `security` is in; false below `lowFrom`, where nobody comes.
    bool BandFor(double security, Band& out) const
    {
        if (security >= highFrom)
            out = { highLocal, highLaunch, highReach, highCalled, highDelay };
        else if (security >= midFrom)
            out = { midLocal, midLaunch, midReach, midCalled, midDelay };
        else if (security >= lowFrom)
            out = { lowLocal, lowLaunch, lowReach, lowCalled, lowDelay };
        else
            return false;
        return true;
    }
};

// Delivery contracts a market posts when it is short of a good (#343, #407). Once every `every`
// seconds each system's market looks at its stock: a good below `shortAt` of its target is
// short, and its owning faction posts a contract at its station for the units that bring it
// back to `fillTo` -- `minAmount` to `maxAmount` of them -- due `hours` later, at most
// `perMarket` open at a market. The price lies between what selling those units here would
// fetch and what buying them here would cost, both at the first unit's price and the fee: at
// `premium` of the way across when the stock is just short, and `premiumShort` more as it
// empties. So a contract always pays better than selling at the market, and buying the units
// at the market itself to hand them back always loses. It is posted only if a market within
// `sourceReach` jumps sells the units for `sourceMargin` less than it pays -- otherwise the
// shortage is the whole neighbourhood's, and the contract would be money for nothing -- and
// only if the owner's treasury holds the price, which is set aside until it is paid out or
// the contract ends. A station of the poster within the jobs' reach shows its contracts,
// `perBoard` at most, nearest first. Done, one is worth `rep` standing.
struct Contracts
{
    double every = 600.0;
    double shortAt = 0.5;
    double fillTo = 0.8;
    int    minAmount = 10;
    int    maxAmount = 40;
    double premium = 0.5;
    double premiumShort = 0.4;
    int    sourceReach = 3;
    double sourceMargin = 0.1;
    double hours = 2.0;
    int    perMarket = 1;
    int    perBoard = 2;
    double rep = 2.0;
};

// Territory (#431 slice 5, T1-T3): what each kind of structure weighs toward its owner's
// influence in a system -- an `outpost`, an outpost `market`, a `station` (a faction's, or one
// grown from a market), an `anchor` -- and what each ship of a faction's presence there weighs
// (`perShip`). A party holds a system with the most influence and a claim; a corporation that
// holds one sets the fee on trades at its markets, at most `taxMax` of a trade.
struct Influence
{
    double outpost = 1.0;
    double market = 2.0;
    double station = 4.0;
    double anchor = 2.0;
    double perShip = 0.25;
    double taxMax = 0.1;
};

// Reads `path`. False, with Error() saying why, and the rules as they were, when it does
// not read.
bool               Load(const std::string& path);
const std::string& Error();
const Tide&        TideRules();
const Mouths&      MouthRules();
const Archive&     ArchiveRules();
const Expeditions& ExpeditionRules();
const Growth&      GrowthRules();
const Convoys&     ConvoyRules();
const Jobs&        JobRules();
const Contracts&   ContractRules();
const Holes&       HoleRules();
const Scan&        ScanRules();
const Law&         LawRules();
const Influence&   InfluenceRules();
// For a test: rules of its own, without a file.
void SetTideRules(const Tide& t);
void SetMouthRules(const Mouths& m);
void SetArchiveRules(const Archive& a);
void SetExpeditionRules(const Expeditions& e);
void SetGrowthRules(const Growth& g);
void SetConvoyRules(const Convoys& c);
void SetJobRules(const Jobs& j);
void SetContractRules(const Contracts& c);
void SetHoleRules(const Holes& h);
void SetScanRules(const Scan& s);
void SetLawRules(const Law& l);
void SetInfluenceRules(const Influence& i);

}  // namespace WorldRules
