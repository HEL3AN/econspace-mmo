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
            it.key() != "convoys" && it.key() != "jobs")
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
                                               { "stationNeed", &g.stationNeed } },
                                             { { "stationsPerRegion", &g.stationsPerRegion },
                                               { "marketReach", &g.marketReach } },
                                             {}, g_error))
        return false;
    Convoys c;
    if (j.contains("convoys") && !ReadSection(j["convoys"], path + ": convoys",
                                              { { "hop", &c.hop },
                                                { "escort", &c.escort },
                                                { "ambush", &c.ambush },
                                                { "retryMinutes", &c.retryMinutes } },
                                              { { "reach", &c.reach } }, {}, g_error))
        return false;
    // A convoy that takes no time anywhere, or reaches nowhere, would build at once.
    if (g.marketReach <= 0 || c.hop <= 0.0 || c.reach <= 0)
    {
        g_error =
            path + ": growth.marketReach, convoys.hop and convoys.reach must be more than zero";
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
    g_tide = t;
    g_mouths = m;
    g_archive = a;
    g_expeditions = e;
    g_growth = g;
    g_convoys = c;
    g_jobs = jb;
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

}  // namespace WorldRules
