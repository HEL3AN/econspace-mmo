#include <doctest/doctest.h>

#include "temp_dir.h"
#include "ui/Fonts.h"
#include "ui/Theme.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>

// The UI themes (#297): a file in data/ui_themes/ is the whole truth about the interface's
// colours, spacing, sizes and typefaces, so it is read strictly -- a misspelt field is an
// error, not a colour that silently stays what it was (#191).

namespace
{
std::string ThemeDir()
{
    return std::string(TEST_DATA_DIR) + "ui_themes/";
}

nlohmann::json Shipped(const std::string& id = Ui::DEFAULT_THEME)
{
    std::ifstream in(ThemeDir() + id + ".json");
    REQUIRE(in);
    return nlohmann::json::parse(in);
}

Ui::Theme Load(const std::string& id)
{
    Ui::Theme   t;
    std::string error;
    REQUIRE_MESSAGE(Ui::LoadTheme(ThemeDir() + id + ".json", t, error), error);
    return t;
}

bool Same(Color a, Color b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

bool Same(const Ui::FaceSpec& a, const Ui::FaceSpec& b)
{
    return a.file == b.file && a.textCase == b.textCase && a.tracking == b.tracking;
}
}  // namespace

TEST_CASE("the default theme loads, and says what the built-in one says")
{
    Ui::Theme   t;
    std::string error;
    REQUIRE_MESSAGE(Ui::LoadTheme(ThemeDir() + Ui::DEFAULT_THEME + ".json", t, error), error);
    // The defaults in Theme.h are what a broken file falls back to; the two are kept the same
    // so that falling back changes nothing anyone can see.
    const Ui::Theme d;
    CHECK(t.id == d.id);
    CHECK(t.name == d.name);
    CHECK(Same(t.colors.panel, d.colors.panel));
    CHECK(Same(t.colors.titleText, d.colors.titleText));
    CHECK(Same(t.colors.accent, d.colors.accent));
    CHECK(Same(t.colors.dim, d.colors.dim));
    CHECK(Same(t.colors.hover, d.colors.hover));
    CHECK(Same(t.standing.hostile, d.standing.hostile));
    CHECK(Same(t.standing.friendly, d.standing.friendly));
    CHECK(Same(t.colors.selected, d.colors.selected));
    CHECK(Same(t.colors.backdrop, d.colors.backdrop));
    CHECK(Same(t.colors.shade, d.colors.shade));
    CHECK(Same(t.colors.sensor, d.colors.sensor));
    CHECK(t.metrics.titleHeight == d.metrics.titleHeight);
    CHECK(t.metrics.rowHeight == d.metrics.rowHeight);
    CHECK(t.metrics.buttonHeight == d.metrics.buttonHeight);
    CHECK(t.metrics.radius == d.metrics.radius);
    CHECK(t.metrics.menuBar == d.metrics.menuBar);
    CHECK(t.metrics.formLabel == d.metrics.formLabel);
    CHECK(t.metrics.titleMark == d.metrics.titleMark);
    CHECK(t.metrics.tabMark == d.metrics.tabMark);
    CHECK(t.fontSize.body == d.fontSize.body);
    CHECK(t.fontSize.heading == d.fontSize.heading);
    CHECK(Same(t.fonts.regular, d.fonts.regular));
    CHECK(Same(t.fonts.strong, d.fonts.strong));
    CHECK(Same(t.fonts.heading, d.fonts.heading));
    CHECK(Same(t.fonts.numbers, d.fonts.numbers));
    CHECK(t.text.lineHeight == d.text.lineHeight);
    CHECK(t.text.gamma == d.text.gamma);
    CHECK(Same(t.colors.onAccent, d.colors.onAccent));
    CHECK(t.metrics.windowRadius == d.metrics.windowRadius);
    CHECK(t.metrics.rowMark == d.metrics.rowMark);
    CHECK(t.style.corner == d.style.corner);
    CHECK(t.style.titleBar == d.style.titleBar);
    CHECK(t.style.tabs == d.style.tabs);
    CHECK(t.style.divider == d.style.divider);
    CHECK(t.style.bar == d.style.bar);
    CHECK(t.style.shadow == d.style.shadow);
    CHECK(t.style.panel == d.style.panel);
    CHECK(t.style.glow == d.style.glow);
    CHECK(t.style.blurs == d.style.blurs);
}

TEST_CASE("every shipped theme loads, is offered, and names fonts that are in the repository")
{
    std::string                       problems;
    const std::vector<Ui::ThemeEntry> themes = Ui::ListThemes(ThemeDir(), &problems);
    CHECK_MESSAGE(problems.empty(), problems);
    // The four the owner chose from (#297), and the earlier look.
    REQUIRE(themes.size() >= 5);
    bool hasDefault = false;
    for (const Ui::ThemeEntry& e : themes)
    {
        hasDefault = hasDefault || e.id == Ui::DEFAULT_THEME;
        Ui::Theme   t;
        std::string error;
        REQUIRE(Ui::LoadTheme(ThemeDir() + e.id + ".json", t, error));
        CHECK(t.id == e.id);
        CHECK_FALSE(t.name.empty());
        // Opaque, or as near as makes no difference: a window that lets the one behind show
        // through is one whose text runs into the other's. Glass is the one that is not quite,
        // and what it lets through without the blur is a few percent.
        CHECK_MESSAGE(t.colors.panel.a >= 240, e.id);
        if (t.style.panel != Ui::PanelStyle::Glass)
            CHECK_MESSAGE(t.colors.panel.a == 255, e.id);
        // A theme that blurs says what its panel is over the blur, and it lets the blur show.
        if (t.style.blurs)
            CHECK_MESSAGE(t.style.blurPanel.a < 255, e.id);
        for (const Ui::FaceSpec* f :
             { &t.fonts.regular, &t.fonts.strong, &t.fonts.heading, &t.fonts.numbers })
        {
            std::ifstream in(std::string(TEST_DATA_DIR) + f->file, std::ios::binary);
            CHECK_MESSAGE(in.good(), (e.id + ": " + f->file));
        }
    }
    CHECK(hasDefault);
    CHECK(themes.front().id == Ui::DEFAULT_THEME);  // Settings offers the default first
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
    j["fonts"]["heading"]["weight"] = 600;
    CHECK_FALSE(Ui::ParseTheme(j, t, error));
    CHECK(error.find("weight") != std::string::npos);

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

    j = Shipped();
    j["fonts"]["numbers"].erase("case");
    CHECK_FALSE(Ui::ParseTheme(j, t, error));
    CHECK(error.find("case") != std::string::npos);

    j = Shipped();
    j.erase("style");
    CHECK_FALSE(Ui::ParseTheme(j, t, error));
    CHECK(error.find("style") != std::string::npos);

    j = Shipped();
    j["style"].erase("blurPanel");  // null says "never"; absent says nothing
    CHECK_FALSE(Ui::ParseTheme(j, t, error));
    CHECK(error.find("blurPanel") != std::string::npos);
}

TEST_CASE("the four styles and the earlier look say what they draw, in tokens")
{
    // The defaults are Fleet console's, and Plain keeps the look it had before the tokens:
    // switches that change nothing about either.
    const Ui::Theme fleet = Load("fleet");
    CHECK(fleet.style.corner == Ui::Corner::Square);
    CHECK(fleet.style.tabs == Ui::TabStyle::Underline);
    CHECK(fleet.style.shadow == Ui::Shadow::None);
    CHECK(fleet.style.glow == 0.0f);
    CHECK_FALSE(fleet.style.blurs);
    const Ui::Theme plain = Load("plain");
    CHECK(plain.style.corner == Ui::Corner::Rounded);
    CHECK(plain.metrics.windowRadius == 0.0f);  // rounded widgets, square windows, as before
    CHECK(plain.style.tabs == Ui::TabStyle::Boxed);
    CHECK_FALSE(plain.style.blurs);

    const Ui::Theme glass = Load("glass");
    CHECK(glass.style.corner == Ui::Corner::Chamfer);
    CHECK(glass.style.panel == Ui::PanelStyle::Glass);
    CHECK(glass.style.glow > 0.0f);
    CHECK(glass.style.blurs);

    const Ui::Theme industrial = Load("industrial");
    CHECK(industrial.metrics.border == 2.0f);
    CHECK(industrial.style.tabs == Ui::TabStyle::Filled);
    CHECK(industrial.style.divider == Ui::LineStyle::Dashed);
    CHECK(industrial.style.bar == Ui::BarStyle::Striped);

    const Ui::Theme clean = Load("clean");
    CHECK(clean.style.corner == Ui::Corner::Rounded);
    CHECK(clean.style.tabs == Ui::TabStyle::Segmented);
    CHECK(clean.style.shadow == Ui::Shadow::Soft);
    CHECK(clean.fonts.heading.textCase == Ui::TextCase::Sentence);
    // A square theme has no widget radius, whatever its metrics say.
    CHECK(fleet.WidgetRadius() == 0.0f);
    CHECK(clean.WidgetRadius() == clean.metrics.radius);
}

TEST_CASE("a renderer token not on its list is a load error that names the ones that are")
{
    std::string error;
    Ui::Theme   t;
    for (const char* key :
         { "corner", "titleBar", "tabStyle", "divider", "barStyle", "shadow", "panel" })
    {
        nlohmann::json j = Shipped();
        j["style"][key] = "bevelled";
        CHECK_FALSE(Ui::ParseTheme(j, t, error));
        CHECK_MESSAGE(error.find(key) != std::string::npos, error);
        CHECK_MESSAGE(error.find("bevelled") != std::string::npos, error);
        CHECK_MESSAGE(error.find("expected one of") != std::string::npos, error);

        j["style"][key] = 3;  // a token is a word, not a number
        CHECK_FALSE(Ui::ParseTheme(j, t, error));
    }

    nlohmann::json j = Shipped("glass");
    j["fonts"]["heading"]["case"] = "title";
    CHECK_FALSE(Ui::ParseTheme(j, t, error));
    CHECK(error.find("title") != std::string::npos);

    j = Shipped("glass");
    j["style"]["glow"] = 1.5;
    CHECK_FALSE(Ui::ParseTheme(j, t, error));

    j = Shipped("glass");
    j["style"]["blurPanel"] = "frosted";
    CHECK_FALSE(Ui::ParseTheme(j, t, error));

    j = Shipped("glass");
    j["style"]["shine"] = true;
    CHECK_FALSE(Ui::ParseTheme(j, t, error));
    CHECK(error.find("shine") != std::string::npos);
}

TEST_CASE("a sentence-case face writes an all-capitals label as a sentence, and a name as it is")
{
    using Ui::TextCase;
    CHECK(Ui::SetCase(TextCase::Sentence, "SELECTED ITEM") == "Selected item");
    CHECK(Ui::SetCase(TextCase::Sentence, "MISSIONS") == "Missions");
    // A run with a small letter in it means its capitals: a name, a word already in case.
    CHECK(Ui::SetCase(TextCase::Sentence, "Jita IV") == "Jita IV");
    CHECK(Ui::SetCase(TextCase::Sentence, "Undock") == "Undock");
    CHECK(Ui::SetCase(TextCase::Sentence, "W2-1") == "W2-1");
    CHECK(Ui::SetCase(TextCase::Sentence, "dist v") == "Dist v");  // a label written small
    CHECK(Ui::SetCase(TextCase::Sentence, "1280 x 720") == "1280 x 720");
    CHECK(Ui::SetCase(TextCase::Sentence, "\xC3\x89TAT") == "\xC3\x89tat");  // Latin-1 too
    CHECK(Ui::SetCase(TextCase::Upper, "Hull 40%") == "HULL 40%");
    CHECK(Ui::SetCase(TextCase::AsWritten, "Hull 40%") == "Hull 40%");
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

TEST_CASE("a font too small to read is refused, and so is a line shorter than its text")
{
    nlohmann::json j = Shipped();
    j["fontSize"]["small"] = 3;
    Ui::Theme   t;
    std::string error;
    CHECK_FALSE(Ui::ParseTheme(j, t, error));

    j = Shipped();
    j["text"]["lineHeight"] = 0.8;
    CHECK_FALSE(Ui::ParseTheme(j, t, error));
}

TEST_CASE("text is centred on its capitals, by the font's own metrics")
{
    // A face whose capitals stand from 4 to 14 in a 20-pixel box, with room below for
    // descenders: centred in a 30-pixel row from 100, the capitals' middle (9) lands on the
    // row's (115), whatever the box's own middle (10) would have said.
    Ui::LineMetrics m;
    m.height = 20.0f;
    m.baseline = 14.0f;
    m.capTop = 4.0f;
    m.xTop = 7.0f;
    CHECK(Ui::CentredTop(m, 100.0f, 30.0f) == 106.0f);
    // On a whole pixel, always.
    CHECK(Ui::CentredTop(m, 100.0f, 31.0f) == 107.0f);
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

// --- Legibility (#362) -------------------------------------------------------------------

TEST_CASE("contrast is WCAG's ratio, and a translucent colour is laid over its ground first")
{
    CHECK(Ui::Contrast(WHITE, BLACK) == doctest::Approx(21.0f));
    CHECK(Ui::Contrast(BLACK, WHITE) == doctest::Approx(21.0f));
    CHECK(Ui::Contrast(GRAY, GRAY) == doctest::Approx(1.0f));
    // #767676 is the classic lightest grey that reads on white at 4.5:1.
    CHECK(Ui::Contrast(Color{ 0x76, 0x76, 0x76, 255 }, WHITE) ==
          doctest::Approx(4.54f).epsilon(0.01));
    const Color half = Ui::Over(Color{ 255, 255, 255, 128 }, BLACK);
    CHECK(half.r == 128);
    CHECK(half.a == 255);
}

TEST_CASE("every shipped theme's text is readable: 4.5:1 on its panel, and body text 13 px")
{
    const std::vector<Ui::ThemeEntry> themes = Ui::ListThemes(ThemeDir());
    REQUIRE(themes.size() >= 5);
    for (const Ui::ThemeEntry& e : themes)
    {
        const Ui::Theme t = Load(e.id);
        // What the player actually sees under a window: the panel over the empty backdrop
        // (glass lets a few percent through), and the title bar over that.
        const Color panel = Ui::Over(t.colors.panel, t.colors.backdrop);
        const Color title = Ui::Over(t.colors.title, panel);
        struct Pair
        {
            const char* what;
            Color       fg, bg;
        };
        const Pair pairs[] = {
            { "text", t.colors.text, panel },
            { "dim", t.colors.dim, panel },  // the secondary text the review found grey (#362)
            { "accent", t.colors.accent, panel },
            { "good", t.colors.good, panel },
            { "warn", t.colors.warn, panel },
            { "bad", t.colors.bad, panel },
            { "money", t.colors.money, panel },
            { "titleText", t.colors.titleText, title },
            { "dim on title", t.colors.dim, title },
            { "standing.own", t.standing.own, panel },
            { "standing.friendly", t.standing.friendly, panel },
            { "standing.neutral", t.standing.neutral, panel },
            { "standing.hostile", t.standing.hostile, panel },
            { "standing.unowned", t.standing.unowned, panel },
        };
        for (const Pair& p : pairs)
        {
            const float c = Ui::Contrast(p.fg, p.bg);
            CHECK_MESSAGE(c >= Ui::MIN_TEXT_CONTRAST,
                          (e.id + ": " + p.what + " is " + std::to_string(c) + ":1"));
        }
        // The colour-blind palette replaces the standing colours in every theme, so it has
        // to read on every theme's panel too.
        const Ui::Theme::Standing& cb = Ui::ColourBlindStanding();
        for (const Color c : { cb.own, cb.friendly, cb.neutral, cb.hostile, cb.unowned })
            CHECK_MESSAGE(Ui::Contrast(c, panel) >= Ui::MIN_TEXT_CONTRAST, (e.id + ": palette"));
        CHECK_MESSAGE(t.fontSize.body >= Ui::MIN_BODY_SIZE, e.id);
        CHECK_MESSAGE(t.fontSize.title >= Ui::MIN_BODY_SIZE, e.id);
    }
}

namespace
{
// A colour as a deuteranope sees it: Machado, Oliveira and Fernandes (2009) at full
// severity, on linear RGB. Red and green fall onto one yellow-brown; what is left to tell
// colours apart by is blue against yellow, and lightness.
Color Deuteranope(Color c)
{
    auto lin = [](unsigned char v)
    {
        const double x = v / 255.0;
        return x <= 0.04045 ? x / 12.92 : std::pow((x + 0.055) / 1.055, 2.4);
    };
    auto enc = [](double x)
    {
        x = std::clamp(x, 0.0, 1.0);
        x = x <= 0.0031308 ? 12.92 * x : 1.055 * std::pow(x, 1.0 / 2.4) - 0.055;
        return (unsigned char)std::lround(x * 255.0);
    };
    const double r = lin(c.r), g = lin(c.g), b = lin(c.b);
    return { enc(0.367322 * r + 0.860646 * g - 0.227968 * b),
             enc(0.280085 * r + 0.672501 * g + 0.047413 * b),
             enc(-0.011820 * r + 0.042940 * g + 0.968881 * b), 255 };
}

float Distance(Color a, Color b)
{
    const float dr = (float)a.r - b.r, dg = (float)a.g - b.g, db = (float)a.b - b.b;
    return std::sqrt(dr * dr + dg * dg + db * db);
}
}  // namespace

TEST_CASE("for a deuteranope, the palette keeps hostile apart from everything else")
{
    // The old theme pair -- green "own" against red "hostile" -- for comparison.
    const Color greenRed = { 127, 214, 143, 255 }, red = { 224, 96, 79, 255 };
    const float old = Distance(Deuteranope(greenRed), Deuteranope(red));
    MESSAGE("the old green/red pair, as a deuteranope sees it: " << old);

    const Ui::Theme::Standing& p = Ui::ColourBlindStanding();
    const Color                own = Deuteranope(p.own), friendly = Deuteranope(p.friendly),
                               hostile = Deuteranope(p.hostile), neutral = Deuteranope(p.neutral);
    MESSAGE("hostile/friendly " << Distance(hostile, friendly) << ", hostile/own "
                                << Distance(hostile, own) << ", hostile/neutral "
                                << Distance(hostile, neutral) << ", own/friendly "
                                << Distance(own, friendly));
    CHECK(Distance(hostile, friendly) > 2.0f * old);
    CHECK(Distance(hostile, own) > 2.0f * old);
    CHECK(Distance(hostile, neutral) > old);
    CHECK(Distance(own, friendly) > old);
}

TEST_CASE("the colour-blind palette lays itself over the theme in use, and comes off again")
{
    Ui::Theme&      t = Ui::MutableTheme();
    const Ui::Theme saved = t;
    t = Load("industrial");
    Ui::AdoptStanding();
    Ui::SetColourBlindSetting(true);
    CHECK(Same(Ui::CurrentTheme().standing.hostile, Ui::ColourBlindStanding().hostile));
    // A theme switched to while it is on gets it too, and turning it off gives that theme
    // its own colours back.
    t = Load("glass");
    Ui::AdoptStanding();
    CHECK(Same(Ui::CurrentTheme().standing.own, Ui::ColourBlindStanding().own));
    Ui::SetColourBlindSetting(false);
    CHECK(Same(Ui::CurrentTheme().standing.own, Load("glass").standing.own));
    t = saved;
    Ui::AdoptStanding();
}

TEST_CASE("with no scale chosen, the interface starts from what the screen suggests")
{
    // A 1080p laptop at 125% is scaled by the display already; at 100% the factor does it.
    CHECK(Ui::AutoUserScale(1080, 1.25f) == doctest::Approx(1.0f));
    CHECK(Ui::AutoUserScale(1080, 1.0f) == doctest::Approx(1.25f));
    CHECK(Ui::AutoUserScale(720, 1.0f) == doctest::Approx(1.0f));
    CHECK(Ui::AutoUserScale(768, 1.0f) == doctest::Approx(1.0f));
    CHECK(Ui::AutoUserScale(1440, 1.0f) == doctest::Approx(1.5f));
    CHECK(Ui::AutoUserScale(2160, 1.5f) == doctest::Approx(1.35f));  // 2 over 1.5, in 5% steps
    CHECK(Ui::AutoUserScale(2160, 2.0f) == doctest::Approx(1.0f));
    CHECK(Ui::AutoUserScale(1080, 1.5f) == doctest::Approx(1.0f));   // never below 1
    CHECK(Ui::AutoUserScale(1080, 0.0f) == doctest::Approx(1.25f));  // no window yet
}

TEST_CASE("a chosen scale is saved and wins; the automatic one is not saved")
{
    TempDir           dir;
    const std::string path = dir.Path("ui_settings.json");
    std::string       error;

    Ui::UseAutoUserScale(1.25f);
    REQUIRE(Ui::SaveUiSettings(path));
    {
        std::ifstream  in(path);
        nlohmann::json j = nlohmann::json::parse(in);
        CHECK_FALSE(j.contains("scale"));
        CHECK(j["version"] == Ui::UI_SETTINGS_VERSION);
    }
    Ui::ChooseUserScale(1.5f);
    REQUIRE(Ui::SaveUiSettings(path));
    Ui::UseAutoUserScale(1.0f);
    REQUIRE(Ui::LoadUiSettings(path, error));
    CHECK(Ui::UserScaleChosen());
    CHECK(Ui::UserScale() == doctest::Approx(1.5f));

    // A version-1 file wrote 1 whether or not anyone chose it: that is no choice...
    {
        std::ofstream out(path);
        out << R"({ "version": 1, "scale": 1.0, "theme": "fleet", "blur": false })";
    }
    Ui::UseAutoUserScale(1.25f);
    REQUIRE(Ui::LoadUiSettings(path, error));
    CHECK_FALSE(Ui::UserScaleChosen());
    CHECK(Ui::UserScale() == doctest::Approx(1.25f));
    // ...but any other number in one was.
    {
        std::ofstream out(path);
        out << R"({ "version": 1, "scale": 1.5, "theme": "fleet", "blur": false })";
    }
    REQUIRE(Ui::LoadUiSettings(path, error));
    CHECK(Ui::UserScaleChosen());
    CHECK(Ui::UserScale() == doctest::Approx(1.5f));

    // The colour-blind palette is kept the same way.
    Ui::SetColourBlindSetting(true);
    REQUIRE(Ui::SaveUiSettings(path));
    Ui::SetColourBlindSetting(false);
    REQUIRE(Ui::LoadUiSettings(path, error));
    CHECK(Ui::ColourBlindSetting());
    Ui::SetColourBlindSetting(false);
    Ui::UseAutoUserScale(1.0f);
}
