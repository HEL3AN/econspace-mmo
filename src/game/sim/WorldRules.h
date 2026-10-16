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
struct Growth
{
    int    stationsPerRegion = 4;
    int    marketReach = 3;
    double marketWorth = 1.2;
    double stationWorth = 1.2;
    double stationNeed = 0.45;
};

// The builder convoy (#342 S6): an Anchor or an upgrade is built from what a convoy brings,
// from the faction's nearest holding along the gates, `hop` seconds in each system it passes
// -- `reach` jumps at most. It takes `escort` of the faction's capacity as escort, if the
// holding can spare it. In a system where anyone hostile to it is, both sides lose ships at
// `ambush` a second for every ship of the other, the escort first and then the hauler; the
// hauler gone, the convoy is lost, and the plan sends another `retryMinutes` later.
struct Convoys
{
    double hop = 120.0;
    double escort = 0.25;
    double ambush = 0.01;
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
// For a test: rules of its own, without a file.
void SetTideRules(const Tide& t);
void SetMouthRules(const Mouths& m);
void SetArchiveRules(const Archive& a);
void SetExpeditionRules(const Expeditions& e);
void SetGrowthRules(const Growth& g);
void SetConvoyRules(const Convoys& c);
void SetJobRules(const Jobs& j);

}  // namespace WorldRules
