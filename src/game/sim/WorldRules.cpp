#include "sim/WorldRules.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <map>

namespace
{
WorldRules::Tide        g_tide;
WorldRules::Mouths      g_mouths;
WorldRules::Archive     g_archive;
WorldRules::Expeditions g_expeditions;
WorldRules::Growth      g_growth;
WorldRules::Convoys     g_convoys;
WorldRules::Jobs        g_jobs;
WorldRules::Contracts   g_contracts;
WorldRules::Holes       g_holes;
WorldRules::Scan        g_scan;
WorldRules::Law         g_law;
WorldRules::Influence   g_influence;
WorldRules::Licences    g_licences;
std::string             g_error;

// Reads one section's values into their places: every field known, every number zero or
// more. False with `error` saying which field, and what is wrong with it.
bool ReadSection(const nlohmann::json& j, const std::string& where,
                 const std::map<std::string, double*>& numbers,
                 const std::map<std::string, int*>&    counts,
                 const std::map<std::string, bool*>& flags, std::string& error)
{
    if (!j.is_object())
    {
        error = where + ": not an object";
        return false;
    }
    for (auto it = j.begin(); it != j.end(); ++it)
    {
        const auto n = numbers.find(it.key());
        const auto c = counts.find(it.key());
        const auto f = flags.find(it.key());
        if (n != numbers.end() && it.value().is_number() && it.value().get<double>() >= 0.0)
            *n->second = it.value().get<double>();
        else if (c != counts.end() && it.value().is_number_integer() && it.value().get<int>() >= 0)
            *c->second = it.value().get<int>();
        else if (f != flags.end() && it.value().is_boolean())
            *f->second = it.value().get<bool>();
        else
        {
            const bool known = n != numbers.end() || c != counts.end() || f != flags.end();
            error = where + ": " +
                    (!known             ? "unknown field '" + it.key() + "'"
                     : f != flags.end() ? "'" + it.key() + "' is not true or false"
                                        : "'" + it.key() + "' is not a number of zero or more");
            return false;
        }
    }
    return true;
}
}  // namespace

namespace WorldRules
{

bool Load(const std::string& path)
{
    using nlohmann::json;
    g_error.clear();
    std::ifstream in(path);
    if (!in.is_open())
    {
        g_error = "cannot open " + path;
        return false;
    }
    const json j = json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("tide") || !j["tide"].is_object())
    {
        g_error = path + ": not a world rules file (no 'tide' object)";
        return false;
    }
    for (auto it = j.begin(); it != j.end(); ++it)
        if (it.key() != "schema" && it.key() != "tide" && it.key() != "mouths" &&
            it.key() != "archive" && it.key() != "expeditions" && it.key() != "growth" &&
            it.key() != "convoys" && it.key() != "jobs" && it.key() != "holes" &&
            it.key() != "scan" && it.key() != "contracts" && it.key() != "law" &&
            it.key() != "influence" && it.key() != "licences")
        {
            g_error = path + ": unknown field '" + it.key() + "'";
            return false;
        }

    Tide                                 t;
    const std::map<std::string, double*> numbers = {
        { "period", &t.period },
        { "threshold", &t.threshold },
        { "base", &t.base },
        { "perPlayer", &t.perPlayer },
        { "perExplore", &t.perExplore },
        { "exploreKnee", &t.exploreKnee },
        { "exploreCurve", &t.exploreCurve },
        { "surveyedWeight", &t.surveyedWeight },
        { "perSurvey", &t.perSurvey },
        { "minGapHours", &t.minGapHours },
        { "mouthHours", &t.mouthHours },
    };
    const std::map<std::string, int*> counts = { { "playerCap", &t.playerCap },
                                                 { "playersPerMouth", &t.playersPerMouth } };
    if (!ReadSection(j["tide"], path + ": tide", numbers, counts, {}, g_error))
        return false;
    // A zero here would be a world that opens a region every period, or never steps.
    if (t.period <= 0.0 || t.threshold <= 0.0 || t.exploreKnee <= 0.0 || t.playersPerMouth <= 0)
    {
        g_error = path + ": tide: period, threshold, exploreKnee and playersPerMouth must be more "
                         "than zero";
        return false;
    }

    // The sections S3 did not have may be left out: their defaults are the design's.
    Mouths m;
    if (j.contains("mouths") &&
        !ReadSection(j["mouths"], path + ": mouths",
                     { { "maxMass", &m.maxMass }, { "warnHours", &m.warnHours } }, {},
                     { { "anchors", &m.anchors } }, g_error))
        return false;
    Archive a;
    if (j.contains("archive") && !ReadSection(j["archive"], path + ": archive",
                                              { { "deadDays", &a.deadDays },
                                                { "warnDays", &a.warnDays },
                                                { "returnChance", &a.returnChance } },
                                              {}, {}, g_error))
        return false;
    if (a.deadDays <= 0.0 || a.returnChance > 1.0)
    {
        g_error = path + ": archive: deadDays must be more than zero, and returnChance at most 1";
        return false;
    }
    // S6's: expeditions, growing stations and convoys.
    Expeditions e;
    if (j.contains("expeditions") &&
        !ReadSection(j["expeditions"], path + ": expeditions",
                     { { "curiosity", &e.curiosity }, { "hours", &e.hours } },
                     { { "reach", &e.reach } }, {}, g_error))
        return false;
    Growth g;
    if (j.contains("growth") && !ReadSection(j["growth"], path + ": growth",
                                             { { "marketWorth", &g.marketWorth },
                                               { "stationWorth", &g.stationWorth },
                                               { "stationNeed", &g.stationNeed },
                                               { "shipyardWorth", &g.shipyardWorth },
                                               { "shipyardNeed", &g.shipyardNeed } },
                                             { { "stationsPerRegion", &g.stationsPerRegion },
                                               { "marketReach", &g.marketReach },
                                               { "shipyardReach", &g.shipyardReach },
                                               { "shipyardAfter", &g.shipyardAfter } },
                                             {}, g_error))
        return false;
    Convoys c;
    if (j.contains("convoys") &&
        !ReadSection(j["convoys"], path + ": convoys",
                     { { "hop", &c.hop },
                       { "escort", &c.escort },
                       { "escortMax", &c.escortMax },
                       { "escortPerDanger", &c.escortPerDanger },
                       { "hauler", &c.hauler },
                       { "calm", &c.calm },
                       { "policed", &c.policed },
                       { "harm", &c.harm },
                       { "bite", &c.bite },
                       { "turnBack", &c.turnBack },
                       { "balk", &c.balk },
                       { "avoid", &c.avoid },
                       { "wait", &c.wait },
                       { "retryMinutes", &c.retryMinutes } },
                     { { "reach", &c.reach }, { "maxWaits", &c.maxWaits } }, {}, g_error))
        return false;
    // A convoy that takes no time anywhere, or reaches nowhere, would build at once; one with
    // no strength of its own, or danger that is never calm, divides by nothing.
    if (g.marketReach <= 0 || c.hop <= 0.0 || c.reach <= 0 || c.hauler <= 0.0 || c.calm <= 0.0)
    {
        g_error = path + ": growth.marketReach, convoys.hop, convoys.reach, convoys.hauler and "
                         "convoys.calm must be more than zero";
        return false;
    }
    // Shares are at most the whole, and the least escort is not more than the most.
    if (c.policed > 1.0 || c.turnBack > 1.0 || c.escort > c.escortMax)
    {
        g_error = path + ": convoys.policed and convoys.turnBack must be at most 1, and "
                         "convoys.escort at most convoys.escortMax";
        return false;
    }
    // #345's: the jobs plans post.
    Jobs jb;
    if (j.contains("jobs") && !ReadSection(j["jobs"], path + ": jobs",
                                           { { "creditsPerStock", &jb.creditsPerStock },
                                             { "supplyShare", &jb.supplyShare },
                                             { "supplyMarkup", &jb.supplyMarkup },
                                             { "supplyRange", &jb.supplyRange },
                                             { "supplyCut", &jb.supplyCut },
                                             { "escortBase", &jb.escortBase },
                                             { "escortPerJump", &jb.escortPerJump },
                                             { "escortForce", &jb.escortForce },
                                             { "escortSlack", &jb.escortSlack },
                                             { "siegePerKill", &jb.siegePerKill },
                                             { "rep", &jb.rep },
                                             { "failRep", &jb.failRep } },
                                           { { "reach", &jb.reach },
                                             { "perBoard", &jb.perBoard },
                                             { "supplyMax", &jb.supplyMax },
                                             { "siegeKills", &jb.siegeKills } },
                                           {}, g_error))
        return false;
    // A share of a cost or of a time line is at most the whole; a unit of stock worth nothing
    // would make every job free to post.
    if (jb.supplyShare > 1.0 || jb.supplyCut > 1.0 || jb.creditsPerStock <= 0.0)
    {
        g_error = path + ": jobs.supplyShare and jobs.supplyCut must be at most 1, and "
                         "jobs.creditsPerStock more than zero";
        return false;
    }
    // #343's and #407's: the contracts a short market posts.
    Contracts ct;
    if (j.contains("contracts") && !ReadSection(j["contracts"], path + ": contracts",
                                                { { "every", &ct.every },
                                                  { "shortAt", &ct.shortAt },
                                                  { "fillTo", &ct.fillTo },
                                                  { "premium", &ct.premium },
                                                  { "premiumShort", &ct.premiumShort },
                                                  { "sourceMargin", &ct.sourceMargin },
                                                  { "hours", &ct.hours },
                                                  { "rep", &ct.rep } },
                                                { { "minAmount", &ct.minAmount },
                                                  { "maxAmount", &ct.maxAmount },
                                                  { "sourceReach", &ct.sourceReach },
                                                  { "perMarket", &ct.perMarket },
                                                  { "perBoard", &ct.perBoard } },
                                                {}, g_error))
        return false;
    // A premium of the whole window or more would pay what buying here costs -- a contract
    // filled from the market it is for would make money; and a contract must ask for
    // something, by some time, of a market short of less than it is filled to.
    if (ct.every <= 0.0 || ct.hours <= 0.0 || ct.premium + ct.premiumShort >= 1.0 ||
        ct.shortAt > ct.fillTo || ct.fillTo > 1.0 || ct.minAmount <= 0 ||
        ct.maxAmount < ct.minAmount)
    {
        g_error = path + ": contracts: every, hours and minAmount must be more than zero, "
                         "maxAmount at least minAmount, shortAt at most fillTo at most 1, and "
                         "premium and premiumShort together less than 1";
        return false;
    }
    // S9's and #346's: roaming holes and the scanner.
    Holes h;
    if (j.contains("holes") && !ReadSection(j["holes"], path + ": holes",
                                            { { "epochHours", &h.epochHours },
                                              { "perRegion", &h.perRegion },
                                              { "atCap", &h.atCap },
                                              { "drifted", &h.drifted },
                                              { "minHours", &h.minHours },
                                              { "maxHours", &h.maxHours },
                                              { "smallMass", &h.smallMass },
                                              { "mediumMass", &h.mediumMass },
                                              { "largeMass", &h.largeMass },
                                              { "smallWeight", &h.smallWeight },
                                              { "mediumWeight", &h.mediumWeight },
                                              { "budgetMin", &h.budgetMin },
                                              { "budgetMax", &h.budgetMax },
                                              { "endingHours", &h.endingHours },
                                              { "lowMass", &h.lowMass } },
                                            {}, {}, g_error))
        return false;
    // An epoch of no time would schedule forever; chances and weights are shares.
    if (h.epochHours <= 0.0 || h.minHours <= 0.0 || h.maxHours < h.minHours || h.budgetMin <= 0.0 ||
        h.budgetMax < h.budgetMin || h.perRegion > 1.0 || h.atCap > 1.0 || h.drifted > 1.0 ||
        h.smallWeight + h.mediumWeight > 1.0 || h.lowMass > 1.0)
    {
        g_error = path + ": holes: epochHours, minHours and budgetMin must be more than zero, "
                         "the maxima at least the minima, and chances, weights and lowMass "
                         "at most 1";
        return false;
    }
    Scan sc;
    if (j.contains("scan") &&
        !ReadSection(j["scan"], path + ": scan",
                     { { "seconds", &sc.seconds }, { "power", &sc.power } }, {}, {}, g_error))
        return false;
    if (sc.seconds <= 0.0 || sc.power <= 0.0)
    {
        g_error = path + ": scan: seconds and power must be more than zero";
        return false;
    }
    // #431 slice 9's: the police's answer to a crime.
    Law lw;
    if (j.contains("law") && !ReadSection(j["law"], path + ": law",
                                          { { "callSeconds", &lw.callSeconds },
                                            { "dropDistance", &lw.dropDistance },
                                            { "gunsFrom", &lw.gunsFrom },
                                            { "highFrom", &lw.highFrom },
                                            { "midFrom", &lw.midFrom },
                                            { "lowFrom", &lw.lowFrom },
                                            { "highDelay", &lw.highDelay },
                                            { "midDelay", &lw.midDelay },
                                            { "lowDelay", &lw.lowDelay },
                                            { "hop", &lw.hop },
                                            { "stay", &lw.stay },
                                            { "launchCost", &lw.launchCost },
                                            { "flagSeconds", &lw.flagSeconds },
                                            { "flagLinger", &lw.flagLinger },
                                            { "statusCrime", &lw.statusCrime },
                                            { "statusKill", &lw.statusKill },
                                            { "statusPirateKill", &lw.statusPirateKill },
                                            { "statusPerDay", &lw.statusPerDay },
                                            { "onSightBelow", &lw.onSightBelow },
                                            { "onSightFrom", &lw.onSightFrom } },
                                          { { "highLocal", &lw.highLocal },
                                            { "midLocal", &lw.midLocal },
                                            { "lowLocal", &lw.lowLocal },
                                            { "highLaunch", &lw.highLaunch },
                                            { "midLaunch", &lw.midLaunch },
                                            { "lowLaunch", &lw.lowLaunch },
                                            { "highReach", &lw.highReach },
                                            { "midReach", &lw.midReach },
                                            { "lowReach", &lw.lowReach },
                                            { "highCalled", &lw.highCalled },
                                            { "midCalled", &lw.midCalled },
                                            { "lowCalled", &lw.lowCalled } },
                                          {}, g_error))
        return false;
    // The bands are security, high above middle above low; a call that holds no time would
    // bring the whole force again with every shot.
    if (lw.callSeconds <= 0.0 || lw.stay <= 0.0 || lw.lowFrom > lw.midFrom ||
        lw.midFrom > lw.highFrom || lw.highFrom > 1.0 || lw.gunsFrom > 1.0 ||
        lw.flagSeconds <= 0.0 || lw.onSightFrom > 1.0)
    {
        g_error = path + ": law: callSeconds, stay and flagSeconds must be more than zero, "
                         "lowFrom at most midFrom at most highFrom at most 1, and gunsFrom and "
                         "onSightFrom at most 1";
        return false;
    }
    // Slice 5's (#431): influence, and the most a holder may tax.
    Influence inf;
    if (j.contains("influence") && !ReadSection(j["influence"], path + ": influence",
                                                { { "outpost", &inf.outpost },
                                                  { "market", &inf.market },
                                                  { "station", &inf.station },
                                                  { "anchor", &inf.anchor },
                                                  { "perShip", &inf.perShip },
                                                  { "taxMax", &inf.taxMax } },
                                                {}, {}, g_error))
        return false;
    if (inf.taxMax > 1.0)
    {
        g_error = path + ": influence: taxMax is a share of a trade, at most 1";
        return false;
    }
    // Slice 6's (#431): licences to build in a holder's space.
    Licences lc;
    if (j.contains("licences") && !ReadSection(j["licences"], path + ": licences",
                                               { { "rent", &lc.rent },
                                                 { "periodDays", &lc.periodDays },
                                                 { "graceHours", &lc.graceHours },
                                                 { "standing", &lc.standing },
                                                 { "evict", &lc.evict } },
                                               {}, {}, g_error))
        return false;
    if (lc.periodDays <= 0.0)
    {
        g_error = path + ": licences: periodDays must be more than zero";
        return false;
    }
    g_tide = t;
    g_mouths = m;
    g_archive = a;
    g_expeditions = e;
    g_growth = g;
    g_convoys = c;
    g_jobs = jb;
    g_contracts = ct;
    g_holes = h;
    g_scan = sc;
    g_law = lw;
    g_influence = inf;
    g_licences = lc;
    return true;
}

const std::string& Error()
{
    return g_error;
}

const Tide& TideRules()
{
    return g_tide;
}

const Mouths& MouthRules()
{
    return g_mouths;
}

const Archive& ArchiveRules()
{
    return g_archive;
}

void SetTideRules(const Tide& t)
{
    g_tide = t;
}

void SetMouthRules(const Mouths& m)
{
    g_mouths = m;
}

void SetArchiveRules(const Archive& a)
{
    g_archive = a;
}

const Expeditions& ExpeditionRules()
{
    return g_expeditions;
}

const Growth& GrowthRules()
{
    return g_growth;
}

const Convoys& ConvoyRules()
{
    return g_convoys;
}

void SetExpeditionRules(const Expeditions& e)
{
    g_expeditions = e;
}

void SetGrowthRules(const Growth& g)
{
    g_growth = g;
}

void SetConvoyRules(const Convoys& c)
{
    g_convoys = c;
}

const Jobs& JobRules()
{
    return g_jobs;
}

void SetJobRules(const Jobs& j)
{
    g_jobs = j;
}

const Contracts& ContractRules()
{
    return g_contracts;
}

void SetContractRules(const Contracts& c)
{
    g_contracts = c;
}

const Holes& HoleRules()
{
    return g_holes;
}

const Scan& ScanRules()
{
    return g_scan;
}

void SetHoleRules(const Holes& h)
{
    g_holes = h;
}

void SetScanRules(const Scan& s)
{
    g_scan = s;
}

const Law& LawRules()
{
    return g_law;
}

void SetLawRules(const Law& l)
{
    g_law = l;
}

const Influence& InfluenceRules()
{
    return g_influence;
}

void SetInfluenceRules(const Influence& i)
{
    g_influence = i;
}

const Licences& LicenceRules()
{
    return g_licences;
}

void SetLicenceRules(const Licences& l)
{
    g_licences = l;
}

}  // namespace WorldRules
