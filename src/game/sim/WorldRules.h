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

// Reads `path`. False, with Error() saying why, and the rules as they were, when it does
// not read.
bool               Load(const std::string& path);
const std::string& Error();
const Tide&        TideRules();
const Mouths&      MouthRules();
const Archive&     ArchiveRules();
// For a test: rules of its own, without a file.
void SetTideRules(const Tide& t);
void SetMouthRules(const Mouths& m);
void SetArchiveRules(const Archive& a);

}  // namespace WorldRules
