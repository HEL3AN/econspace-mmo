#include <doctest/doctest.h>

#include "ui/Theme.h"

#include <nlohmann/json.hpp>
#include <fstream>
#include <string>

// The UI theme (#297): data/ui_theme.json is the whole truth about the interface's colours,
// spacing and sizes, so it is read strictly -- a misspelt field is an error, not a colour
// that silently stays what it was (#191).

namespace
{
nlohmann::json Shipped()
{
    std::ifstream in(std::string(TEST_DATA_DIR) + "ui_theme.json");
    REQUIRE(in);
    return nlohmann::json::parse(in);
}

bool Same(Color a, Color b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}
}  // namespace

TEST_CASE("the shipped theme loads, and says what the built-in one says")
{
    Ui::Theme   t;
    std::string error;
    REQUIRE_MESSAGE(Ui::ParseTheme(Shipped(), t, error), error);
    // The defaults in Theme.h are what a broken file falls back to; the two are kept the same
    // so that falling back changes nothing anyone can see.
    const Ui::Theme d;
    CHECK(Same(t.colors.panel, d.colors.panel));
    CHECK(Same(t.colors.accent, d.colors.accent));
    CHECK(Same(t.colors.hover, d.colors.hover));
    CHECK(Same(t.standing.hostile, d.standing.hostile));
    CHECK(Same(t.standing.friendly, d.standing.friendly));
    CHECK(Same(t.colors.selected, d.colors.selected));
    CHECK(Same(t.colors.backdrop, d.colors.backdrop));
    CHECK(Same(t.colors.shade, d.colors.shade));
    CHECK(Same(t.colors.sensor, d.colors.sensor));
    CHECK(t.metrics.titleHeight == d.metrics.titleHeight);
    CHECK(t.metrics.rowHeight == d.metrics.rowHeight);
    CHECK(t.metrics.scrollbar == d.metrics.scrollbar);
    CHECK(t.metrics.snap == d.metrics.snap);
    CHECK(t.metrics.tabWidth == d.metrics.tabWidth);
    CHECK(t.metrics.sidePanel == d.metrics.sidePanel);
    CHECK(t.fontSize.body == d.fontSize.body);
    CHECK(t.fonts.regular == d.fonts.regular);
}

TEST_CASE("the fonts the theme names are in the repository")
{
    Ui::Theme   t;
    std::string error;
    REQUIRE(Ui::ParseTheme(Shipped(), t, error));
    for (const std::string& f : { t.fonts.regular, t.fonts.strong })
    {
        std::ifstream in(std::string(TEST_DATA_DIR) + f, std::ios::binary);
        CHECK_MESSAGE(in.good(), f);
    }
}

TEST_CASE("an unknown field is a load error, wherever it is")
{
    std::string error;
    Ui::Theme   t;

    nlohmann::json j = Shipped();
    j["colors"]["acent"] = "#FFFFFF";
    CHECK_FALSE(Ui::ParseTheme(j, t, error));
    CHECK(error.find("acent") != std::string::npos);

    j = Shipped();
    j["metrics"]["paddding"] = 4;
    CHECK_FALSE(Ui::ParseTheme(j, t, error));
    CHECK(error.find("paddding") != std::string::npos);

    j = Shipped();
    j["extra"] = {};
    CHECK_FALSE(Ui::ParseTheme(j, t, error));
}

TEST_CASE("a missing field is a load error too")
{
    std::string    error;
    Ui::Theme      t;
    nlohmann::json j = Shipped();
    j["standing"].erase("hostile");
    CHECK_FALSE(Ui::ParseTheme(j, t, error));
    CHECK(error.find("hostile") != std::string::npos);
}

TEST_CASE("a failed load leaves the theme as it was")
{
    Ui::Theme t;
    t.colors.accent = { 1, 2, 3, 4 };
    nlohmann::json j = Shipped();
    j["fontSize"]["body"] = "large";
    std::string error;
    CHECK_FALSE(Ui::ParseTheme(j, t, error));
    CHECK(Same(t.colors.accent, Color{ 1, 2, 3, 4 }));
}

TEST_CASE("colours are #RRGGBB or #RRGGBBAA")
{
    Color c{};
    CHECK(Ui::ParseColor("#5CAAE8", c));
    CHECK(Same(c, Color{ 92, 170, 232, 255 }));
    CHECK(Ui::ParseColor("#5caae880", c));
    CHECK(Same(c, Color{ 92, 170, 232, 128 }));
    CHECK_FALSE(Ui::ParseColor("5CAAE8", c));
    CHECK_FALSE(Ui::ParseColor("#5CAAE", c));
    CHECK_FALSE(Ui::ParseColor("#5CAAEG", c));

    nlohmann::json j = Shipped();
    j["colors"]["text"] = "white";
    Ui::Theme   t;
    std::string error;
    CHECK_FALSE(Ui::ParseTheme(j, t, error));
}

TEST_CASE("a font too small to read is refused")
{
    nlohmann::json j = Shipped();
    j["fontSize"]["small"] = 3;
    Ui::Theme   t;
    std::string error;
    CHECK_FALSE(Ui::ParseTheme(j, t, error));
}

TEST_CASE("the UI scale is the player's setting times the display's, and is clamped")
{
    Ui::OverrideDisplayScale(1.5f);
    Ui::SetUserScale(1.0f);
    CHECK(Ui::Scale() == doctest::Approx(1.5f));
    CHECK(Ui::Px(10.0f) == 15.0f);
    Ui::SetUserScale(2.0f);
    CHECK(Ui::Scale() == doctest::Approx(3.0f));
    Ui::SetUserScale(100.0f);
    CHECK(Ui::UserScale() == Ui::MAX_USER_SCALE);
    Ui::SetUserScale(0.0f);
    CHECK(Ui::UserScale() == Ui::MIN_USER_SCALE);

    Ui::OverrideDisplayScale(0.0f);
    Ui::SetUserScale(1.0f);
    CHECK(Ui::Scale() == doctest::Approx(1.0f));  // no window here: the display says 1
}
