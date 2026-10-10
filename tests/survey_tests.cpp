#include <doctest/doctest.h>

#include "gen/Region.h"
#include "gen/Survey.h"

#include <string>
#include <vector>

// The survey (#141): what the screen flags is decided here, without a window, so the
// definitions of "empty" and "the same system" are held to something.

namespace
{
using nlohmann::json;

json System(const char* star, int planets, int belts, int wrecks, int clouds, int gates = 1)
{
    json doc;
    doc["star"] = { { "type", star }, { "size", 150000 } };
    doc["planets"] = json::array();
    for (int i = 0; i < planets; i++)
        doc["planets"].push_back({ { "type", "Rocky" }, { "orbitRadius", 200000 + 100000 * i } });
    doc["asteroidFields"] = json::array();
    for (int i = 0; i < belts; i++)
        doc["asteroidFields"].push_back({ { "name", "Belt" } });
    doc["derelicts"] = json::array();
    for (int i = 0; i < wrecks; i++)
        doc["derelicts"].push_back({ { "name", "Wreck" } });
    doc["nebulae"] = json::array();
    for (int i = 0; i < clouds; i++)
        doc["nebulae"].push_back({ { "name", "Cloud" } });
    doc["gates"] = json::array();
    for (int i = 0; i < gates; i++)
        doc["gates"].push_back({ { "name", "Gate" } });
    doc["stations"] = json::array();
    return doc;
}
}  // namespace

TEST_CASE("survey: a system with nothing but a star, planets and gates is empty")
{
    const Gen::SystemSummary bare = Gen::Summarize(System("Red", 4, 0, 0, 0, 3));
    CHECK(bare.Empty());
    CHECK(bare.planets == 4);
    CHECK(bare.gates == 3);
    CHECK(bare.reach == doctest::Approx(500000.0));

    const Gen::SystemSummary one = Gen::Summarize(System("Red", 4, 1, 0, 0));
    CHECK_FALSE(one.Empty());
    CHECK(one.Thin());
    CHECK(one.Count("asteroidFields") == 1);

    CHECK_FALSE(Gen::Summarize(System("Red", 1, 1, 1, 0)).Thin());
}

TEST_CASE("survey: an archetype on an object makes it its own kind, and new arrays count")
{
    json doc = System("Blue", 2, 2, 0, 0);
    doc["asteroidFields"][1]["archetype"] = "crystal_vein";
    doc["anomalies"] = json::array({ json::object() });  // a key the survey has never heard of
    doc["character"] = "graveyard";

    const Gen::SystemSummary s = Gen::Summarize(doc);
    CHECK(s.Count("asteroidFields") == 1);
    CHECK(s.Count("crystal_vein") == 1);
    CHECK(s.Count("anomalies") == 1);
    CHECK(s.worth == 3);
    CHECK(s.character == "graveyard");
}

TEST_CASE("survey: twins are the same contents, whatever the exits and coordinates")
{
    json a = System("Yellow", 3, 1, 1, 0, 2);
    json b = System("Yellow", 3, 1, 1, 0, 4);  // a different number of gates
    b["planets"][0]["orbitRadius"] = 123456;   // and somewhere else
    const json c = System("Red", 3, 1, 1, 0, 2);
    const json d = System("Yellow", 3, 2, 1, 0, 2);

    const Gen::SurveyResult r = Gen::Analyse({ a, b, c, d, a });
    REQUIRE(r.twinOf.size() == 5);
    CHECK(r.twinOf[0] == -1);
    CHECK(r.twinOf[1] == 0);
    CHECK(r.twinOf[2] == -1);  // another star is another system
    CHECK(r.twinOf[3] == -1);  // and so is another belt
    CHECK(r.twinOf[4] == 0);
    CHECK(r.stats.twins == 2);

    // A character the generator chose sets two systems apart.
    a["character"] = "quiet";
    CHECK(Gen::Signature(Gen::Summarize(a)) != Gen::Signature(Gen::Summarize(b)));
}

TEST_CASE("survey: the distributions count every system, zeros included")
{
    const Gen::SurveyResult r = Gen::Analyse(
        { System("Red", 2, 0, 0, 0), System("Red", 2, 2, 0, 1), System("Blue", 5, 1, 0, 0) });
    const Gen::SurveyStats& st = r.stats;
    CHECK(st.systems == 3);
    CHECK(st.empty == 1);
    CHECK(st.thin == 1);
    CHECK(st.planets.at(2) == 2);
    CHECK(st.planets.at(5) == 1);
    CHECK(st.stars.at("Red") == 2);
    CHECK(st.stars.at("Blue") == 1);
    // No belts is a value of the belt count, not a missing row.
    CHECK(st.things.at("asteroidFields").at(0) == 1);
    CHECK(st.things.at("asteroidFields").at(2) == 1);
    CHECK(st.things.at("nebulae").at(0) == 2);
    CHECK(st.totals.at("asteroidFields") == 3);
    CHECK(st.things.count("derelicts") == 0);  // never seen, so not a kind of this run
    CHECK(st.characters.empty());              // nobody wrote one
}

TEST_CASE("survey: consecutive seeds, every system of each region, in the index's order")
{
    Gen::RegionParams base;
    base.homeId = "core";
    const std::vector<Gen::SurveySystem> run = Gen::GenerateSurvey(base, 5, 3);
    REQUIRE(run.size() == (size_t)(3 * base.systems));
    CHECK(run.front().seed == 5);
    CHECK(run.back().seed == 7);
    CHECK(run.front().id == "w1-1");
    CHECK(run.front().depth == 1);
    CHECK(run.front().designation == "W-1.1");
    for (const Gen::SurveySystem& s : run)
    {
        CHECK(s.doc.is_object());
        CHECK((s.doc.contains("star") || s.doc.contains("stars")));  // a binary has two (#142)
    }
    // The same seeds survey the same way: the screen is a fact about the rules.
    const std::vector<Gen::SurveySystem> again = Gen::GenerateSurvey(base, 5, 3);
    for (size_t i = 0; i < run.size(); i++)
        CHECK(run[i].doc == again[i].doc);
}
