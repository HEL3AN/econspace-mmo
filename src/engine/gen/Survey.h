#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// Many systems at once, judged as a distribution (#141).
//
// A single good system proves nothing about a generator, because a single good system can
// be arranged. This is the half of the survey screen that has no window: it generates the
// regions of a run of consecutive seeds and says, of what came out, which systems are empty,
// which are the same as another, and what the range of each thing being tuned is. The
// editor draws it; the tests hold it to its definitions.
//
// It reads system documents only -- the same JSON WorldLoader reads -- and knows the
// generator through its public entry point and nothing else, so a new rule shows up here
// without a change: a new array in a document is a new kind of thing to count, and a
// document's "character" and an object's "archetype" are tallied when they are present.
namespace Gen
{

struct RegionParams;

// One generated system, with where it came from.
struct SurveySystem
{
    uint64_t       seed = 0;
    std::string    id;           // "w3-2"
    std::string    designation;  // "W-3.2", from the galaxy index entry
    int            depth = 0;
    nlohmann::json doc;  // the system document, in the shape of data/systems/*.json
};

// The regions of `count` consecutive seeds starting at `firstSeed`, every system of each,
// in the generator's own order: seed, then the order of the region's index. `base` is used
// for everything but the seed (home system, known map), so the survey sees the same
// regions a server would generate from those seeds.
std::vector<SurveySystem> GenerateSurvey(const RegionParams& base, uint64_t firstSeed, int count);

// What a system has, counted.
struct SystemSummary
{
    std::string star;       // the star's type, "" if it has none
    std::string character;  // the document's "character", "" if the generator writes none
    int         planets = 0;
    int         gates = 0;
    // Everything else in the document, by kind: "asteroidFields" -> 2. A kind is the
    // array's name, or the object's "archetype" when it names one -- a rare find must not
    // be counted as just another belt.
    std::map<std::string, int> things;
    // Objects in arrays other than planets and gates: what is worth stopping for.
    int worth = 0;
    // The outermost planet's orbit radius, 0 with no planets: how far out the system goes.
    double reach = 0.0;

    // Nothing besides the star, planets and gates. The failure a rule hides best (#141).
    bool Empty() const { return worth == 0; }
    // One thing worth stopping for, and that is all.
    bool Thin() const { return worth == 1; }
    int  Count(const std::string& kind) const;
};

SystemSummary Summarize(const nlohmann::json& doc);

// A cheap structural fingerprint: star type, character, and how many of each kind. Two
// systems with the same signature are the same system to anyone flying through them in
// an overview, whatever their coordinates say.
std::string Signature(const SystemSummary& s);

// What a run of systems looks like as a whole.
struct SurveyStats
{
    int systems = 0;
    int empty = 0;
    int thin = 0;
    int twins = 0;  // systems whose signature another system in the run already had

    // Value -> how many systems had it.
    std::map<int, int>         planets;
    std::map<int, int>         gates;
    std::map<int, int>         worth;
    std::map<int, int>         reach;  // outermost orbit, in hundreds of thousands of units
    std::map<std::string, int> stars;
    std::map<std::string, int> characters;
    // Per kind: how many of that kind a system had -> how many systems. Every kind that
    // appears anywhere in the run has an entry for every system, zeros included, so a
    // belt count of 0 is as visible as one of 2.
    std::map<std::string, std::map<int, int>> things;
    // Every object seen, by kind: the rare finds.
    std::map<std::string, int> totals;
};

struct SurveyResult
{
    std::vector<SystemSummary> summaries;  // parallel to the input
    // For each system, the index of the first earlier system with the same signature, or
    // -1 if it is the first of its kind.
    std::vector<int> twinOf;
    SurveyStats      stats;
};

SurveyResult Analyse(const std::vector<nlohmann::json>& docs);

}  // namespace Gen
