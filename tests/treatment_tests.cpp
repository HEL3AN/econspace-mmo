#include <doctest/doctest.h>

#include "render/TreatmentConfig.h"
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

// The screen treatment is a picture, and there is no graphics card on a CI runner, so what
// it *looks* like cannot be tested here at all -- that is the cost the M6 decision accepts.
//
// What can be tested is everything that decides what the picture will be: which passes run,
// in what order, with what numbers, and whether the file that says so survives being read
// and written. That is the half this file exists to hold, and it is the half that can
// silently lose a player's settings.

namespace
{
std::string TempPath(const char* name)
{
    return std::string(TEST_DATA_DIR) + "../build/" + name;
}

void Write(const std::string& path, const std::string& text)
{
    std::ofstream out(path);
    REQUIRE(out.is_open());
    out << text;
}
}  // namespace

TEST_CASE("a pass is named the same way in data and in the settings screen")
{
    for (Render::PassKind k : Render::AllPasses())
    {
        Render::PassKind back;
        REQUIRE(Render::PassFromName(Render::PassName(k), back));
        CHECK(back == k);
        // Every pass says what its second knob means; a slider labelled "scale" tells a
        // player nothing about what moving it will do.
        CHECK(std::string(Render::ScaleMeaning(k)).size() > 0);
    }
}

TEST_CASE("the shipped chain is the one the game draws with")
{
    const Render::TreatmentConfig d = Render::TreatmentConfig::Default();
    CHECK(d.enabled);
    CHECK_FALSE(d.treatHud);  // the HUD carries numbers people fly by
    CHECK(d.chain.size() == Render::AllPasses().size());

    SUBCASE("bloom comes before pixelation, which is a decision and not an accident")
    {
        // Bloom first resolves the glow on the coarse grid with everything else; after it,
        // the glow is drawn in fractions of a pixel the display cannot show. Reversing
        // these two is the single biggest change available in this chain, so it is pinned.
        size_t bloom = d.chain.size(), pixels = d.chain.size();
        for (size_t i = 0; i < d.chain.size(); i++)
        {
            if (d.chain[i].kind == Render::PassKind::Bloom)
                bloom = i;
            if (d.chain[i].kind == Render::PassKind::Pixelate)
                pixels = i;
        }
        CHECK(bloom < pixels);
    }

    SUBCASE("the vignette is last, so it sees everything the others did")
    {
        CHECK(d.chain.back().kind == Render::PassKind::Vignette);
    }
}

TEST_CASE("the chain survives being written and read back")
{
    Render::TreatmentConfig cfg = Render::TreatmentConfig::Default();
    cfg.treatHud = true;
    cfg.enabled = false;
    cfg.chain[0].amount = 0.25f;
    cfg.chain[0].scale = 3.5f;
    cfg.chain[1].enabled = false;
    cfg.MoveDown(0);  // a reordered chain is the case a naive writer loses

    const std::string path = TempPath("treatment_roundtrip.json");
    std::string       error;
    REQUIRE(Render::SaveTreatment(path, cfg, error));
    CHECK(error.empty());

    Render::TreatmentConfig back;
    REQUIRE(Render::LoadTreatment(path, back, error));
    CHECK(back.enabled == cfg.enabled);
    CHECK(back.treatHud == cfg.treatHud);
    REQUIRE(back.chain.size() == cfg.chain.size());
    for (size_t i = 0; i < cfg.chain.size(); i++)
    {
        CHECK(back.chain[i].kind == cfg.chain[i].kind);  // the order, not just the contents
        CHECK(back.chain[i].enabled == cfg.chain[i].enabled);
        CHECK(back.chain[i].amount == doctest::Approx(cfg.chain[i].amount));
        CHECK(back.chain[i].scale == doctest::Approx(cfg.chain[i].scale));
    }
    std::remove(path.c_str());
}

TEST_CASE("a broken look file loses as little as it can")
{
    std::string             error;
    Render::TreatmentConfig cfg;

    SUBCASE("a missing file is not an error worth stopping for")
    {
        CHECK_FALSE(Render::LoadTreatment(TempPath("no_such_look.json"), cfg, error));
        CHECK_FALSE(error.empty());
    }

    SUBCASE("one unknown pass costs that pass and nothing else")
    {
        const std::string path = TempPath("treatment_unknown.json");
        Write(path, R"({
            "enabled": true,
            "chain": [
                { "pass": "bloom", "amount": 0.5, "scale": 1.5 },
                { "pass": "kaleidoscope", "amount": 1.0 },
                { "pass": "vignette", "amount": 0.4, "scale": 1.0 }
            ]
        })");
        REQUIRE(Render::LoadTreatment(path, cfg, error));
        REQUIRE(cfg.chain.size() == 2);
        CHECK(cfg.chain[0].kind == Render::PassKind::Bloom);
        CHECK(cfg.chain[1].kind == Render::PassKind::Vignette);
        std::remove(path.c_str());
    }

    SUBCASE("a chain with nothing usable in it falls back rather than turning the look off")
    {
        // Silently drawing nothing is the failure mode this whole feature is written to
        // avoid, and an empty chain is the data-side version of it.
        const std::string path = TempPath("treatment_empty.json");
        Write(path, R"({ "enabled": true, "chain": [] })");
        REQUIRE(Render::LoadTreatment(path, cfg, error));
        CHECK(cfg.chain.size() == Render::TreatmentConfig::Default().chain.size());
        std::remove(path.c_str());
    }

    SUBCASE("text that is not JSON is refused, not half-read")
    {
        const std::string path = TempPath("treatment_broken.json");
        Write(path, "{ this is not json");
        CHECK_FALSE(Render::LoadTreatment(path, cfg, error));
        CHECK_FALSE(error.empty());
        std::remove(path.c_str());
    }
}

TEST_CASE("the order can be changed, and the ends do not fall off")
{
    Render::TreatmentConfig cfg = Render::TreatmentConfig::Default();
    const Render::PassKind  first = cfg.chain[0].kind;
    const Render::PassKind  second = cfg.chain[1].kind;

    cfg.MoveDown(0);
    CHECK(cfg.chain[0].kind == second);
    CHECK(cfg.chain[1].kind == first);

    cfg.MoveUp(1);
    CHECK(cfg.chain[0].kind == first);

    // A settings screen puts an arrow on every row, including the first and the last.
    const size_t n = cfg.chain.size();
    cfg.MoveUp(0);
    cfg.MoveDown(n - 1);
    CHECK(cfg.chain.size() == n);
    CHECK(cfg.chain[0].kind == first);
}

TEST_CASE("the look file that ships is one the game can read")
{
    // It is data the game loads at startup, so a typo in it is a broken build that
    // compiles. This is the same reason archetypes.json is loaded in a test.
    std::string             error;
    Render::TreatmentConfig cfg;
    REQUIRE(Render::LoadTreatment(std::string(TEST_DATA_DIR) + "look.json", cfg, error));
    CHECK(cfg.chain.size() == Render::AllPasses().size());
    for (const Render::Pass& p : cfg.chain)
    {
        CHECK(p.amount >= 0.0f);
        CHECK(p.amount <= 1.0f);
        CHECK(p.scale > 0.0f);
    }
}
