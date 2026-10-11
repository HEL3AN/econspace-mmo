#include "sim/WorldRules.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <map>

namespace
{
WorldRules::Tide g_tide;
std::string      g_error;
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
        if (it.key() != "schema" && it.key() != "tide")
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
    const json&                       tj = j["tide"];
    for (auto it = tj.begin(); it != tj.end(); ++it)
    {
        const auto n = numbers.find(it.key());
        const auto c = counts.find(it.key());
        if (n != numbers.end() && it.value().is_number() && it.value().get<double>() >= 0.0)
            *n->second = it.value().get<double>();
        else if (c != counts.end() && it.value().is_number_integer() && it.value().get<int>() >= 0)
            *c->second = it.value().get<int>();
        else
        {
            g_error = path + ": tide: " +
                      (n == numbers.end() && c == counts.end()
                           ? "unknown field '" + it.key() + "'"
                           : "'" + it.key() + "' is not a number of zero or more");
            return false;
        }
    }
    // A zero here would be a world that opens a region every period, or never steps.
    if (t.period <= 0.0 || t.threshold <= 0.0 || t.exploreKnee <= 0.0 || t.playersPerMouth <= 0)
    {
        g_error = path + ": tide: period, threshold, exploreKnee and playersPerMouth must be more "
                         "than zero";
        return false;
    }
    g_tide = t;
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

void SetTideRules(const Tide& t)
{
    g_tide = t;
}

}  // namespace WorldRules
