#include "sim/WorldRules.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <map>

namespace
{
WorldRules::Tide    g_tide;
WorldRules::Mouths  g_mouths;
WorldRules::Archive g_archive;
std::string         g_error;

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
            it.key() != "archive")
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
    g_tide = t;
    g_mouths = m;
    g_archive = a;
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

}  // namespace WorldRules
