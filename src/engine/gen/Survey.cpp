#include "gen/Survey.h"

#include "gen/Region.h"

#include <cmath>
#include <set>

namespace Gen
{
using nlohmann::json;

std::vector<SurveySystem> GenerateSurvey(const RegionParams& base, uint64_t firstSeed, int count)
{
    std::vector<SurveySystem> out;
    for (int i = 0; i < count; i++)
    {
        RegionParams p = base;
        p.seed = firstSeed + (uint64_t)i;
        const Region region = GenerateRegion(p);
        // The index is the generator's order (ring by ring); the documents map is sorted
        // by id, which would put w10-1 before w2-1.
        for (const json& entry : region.systems)
        {
            SurveySystem s;
            s.seed = p.seed;
            s.id = entry.value("id", std::string());
            s.designation = entry.value("name", s.id);
            s.depth = DepthOf(s.id);
            const auto doc = region.documents.find(s.id);
            if (doc != region.documents.end())
                s.doc = doc->second;
            out.push_back(std::move(s));
        }
    }
    return out;
}

int SystemSummary::Count(const std::string& kind) const
{
    const auto it = things.find(kind);
    return it == things.end() ? 0 : it->second;
}

SystemSummary Summarize(const json& doc)
{
    SystemSummary s;
    if (!doc.is_object())
        return s;
    if (doc.contains("star") && doc["star"].is_object())
        s.star = doc["star"].value("type", std::string());
    if (doc.contains("character") && doc["character"].is_string())
        s.character = doc["character"].get<std::string>();

    for (const auto& kv : doc.items())
    {
        if (!kv.value().is_array())
            continue;
        const std::string& key = kv.key();
        if (key == "planets")
        {
            for (const json& p : kv.value())
            {
                s.planets++;
                if (p.is_object())
                    s.reach = std::fmax(s.reach, p.value("orbitRadius", 0.0));
            }
            continue;
        }
        if (key == "gates")
        {
            s.gates = (int)kv.value().size();
            continue;
        }
        for (const json& o : kv.value())
        {
            std::string kind = key;
            if (o.is_object() && o.contains("archetype") && o["archetype"].is_string())
                kind = o["archetype"].get<std::string>();
            s.things[kind]++;
            s.worth++;
        }
    }
    return s;
}

std::string Signature(const SystemSummary& s)
{
    // Gates are left out on purpose: they are the topology, not the system. Two systems
    // with the same contents and a different number of exits are the same place to stop.
    std::string sig = s.star + "|" + s.character + "|p" + std::to_string(s.planets);
    for (const auto& kv : s.things)
        if (kv.second > 0)
            sig += "|" + kv.first + "=" + std::to_string(kv.second);
    return sig;
}

SurveyResult Analyse(const std::vector<json>& docs)
{
    SurveyResult               r;
    SurveyStats&               st = r.stats;
    std::map<std::string, int> firstWith;  // signature -> index of its first system
    std::set<std::string>      kinds;

    for (const json& doc : docs)
    {
        SystemSummary s = Summarize(doc);
        const int     index = (int)r.summaries.size();
        const auto    seen = firstWith.emplace(Signature(s), index);
        r.twinOf.push_back(seen.second ? -1 : seen.first->second);
        if (!seen.second)
            st.twins++;

        st.systems++;
        if (s.Empty())
            st.empty++;
        else if (s.Thin())
            st.thin++;
        st.planets[s.planets]++;
        st.gates[s.gates]++;
        st.worth[s.worth]++;
        st.reach[(int)(s.reach / 100000.0)]++;
        st.stars[s.star.empty() ? "(none)" : s.star]++;
        if (!s.character.empty())
            st.characters[s.character]++;
        for (const auto& kv : s.things)
        {
            kinds.insert(kv.first);
            st.totals[kv.first] += kv.second;
        }
        r.summaries.push_back(std::move(s));
    }
    // Every kind for every system, zeros included: "no belt" is a value of the belt count.
    for (const std::string& kind : kinds)
        for (const SystemSummary& s : r.summaries)
            st.things[kind][s.Count(kind)]++;
    return r;
}

}  // namespace Gen
