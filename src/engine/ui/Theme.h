#pragma once

#include "raylib.h"    // Color: a plain struct, nothing here draws
#include "ui/Fonts.h"  // TextCase
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

// What the interface looks like, as data (#297): colours, spacing, corner radii, font sizes
// and typefaces, read from a theme file in data/ui_themes/. A screen asks the theme for "the
// dim text colour" or "the gap between rows" and never writes a number of its own, so the
// whole interface can be restyled in one file and scaled by one factor.
//
// A theme is a whole file, and the player picks one in Settings. Switching is a reload: the
// theme in use is replaced and the fonts it names are rasterised again (Ui::UseTheme), with
// no restart. Nothing may keep a theme value past a frame for that reason -- a window asks
// CurrentTheme() as it draws, and the names in UiTheme.h refer to the theme in use, not to
// a copy of it.
//
// Every value below is in *layout units*. A unit is a pixel at a scale of 1; Ui::Px turns
// one into pixels at the current scale, which is the player's own setting times the
// display's DPI (half of #179).
namespace Ui
{

// One of the four faces (Ui::Face) a theme names: its file, relative to data/, and how text
// in it is set. Capitals and tracking belong to the face rather than to each widget, so a
// theme whose headings are spaced capitals says so once and every heading follows.
struct FaceSpec
{
    std::string file;
    TextCase    textCase = TextCase::AsWritten;  // "case": as written, capitals, a sentence
    float       tracking = 0.0f;                 // extra room between letters, in ems
};

// What a theme's frame and widgets look like where a number cannot say it: a few renderer
// tokens, each a word from a short list, drawn once in Window.cpp and the widgets (Paint.h).
// A word not on its list is a load error, like an unknown field.
enum class Corner
{
    Square,   // no corner is rounded or cut, whatever the radii say
    Rounded,  // widgets by metrics.radius, windows by metrics.windowRadius
    Chamfer,  // the top-left and bottom-right corners cut at 45 degrees, by the same radii
};
enum class TitleBar
{
    Solid,  // colors.title, the width of the window
    Fade,   // colors.title fading out to the right, over the panel
    Clear,  // the panel itself, with a rule under it
};
enum class TabStyle
{
    Underline,  // a strip on a rule, the chosen tab lit and underlined (metrics.tabMark)
    Boxed,      // each tab a box of its own, the chosen one lit
    Filled,     // boxes on a strip of the border colour, the chosen one solid accent
    Segmented,  // one sunken track, the chosen tab a raised plate in it
};
enum class LineStyle
{
    Solid,
    Dashed,
};
enum class BarStyle
{
    Solid,
    Striped,  // segments, in a frame of the border colour
};
enum class Shadow
{
    None,
    Soft,    // a wide, faint shadow below the window
    Offset,  // a hard copy of the window, down and to the right
};
enum class PanelStyle
{
    Flat,
    Glass,  // a sheen down from the top, and the cut corners drawn in the accent
};

struct Theme
{
    std::string id = "fleet";            // its file's name, without .json
    std::string name = "Fleet console";  // what Settings calls it

    struct Colors
    {
        Color panel{ 13, 17, 23, 255 };         // a window's body: opaque, nothing shows through
        Color border{ 43, 53, 66, 255 };        // its outline, and dividers
        Color title{ 21, 28, 37, 255 };         // its title bar
        Color titleText{ 174, 189, 204, 255 };  // the title written on it
        Color accent{ 141, 178, 216, 255 };
        Color text{ 201, 211, 222, 255 };
        Color dim{ 125, 139, 155, 255 };      // labels, and whatever is off
        Color good{ 111, 191, 138, 255 };     // healthy, complete
        Color warn{ 217, 180, 90, 255 };      // attention
        Color bad{ 212, 97, 79, 255 };        // damage, failure
        Color money{ 227, 194, 107, 255 };    // credits, wherever they are shown
        Color track{ 42, 51, 64, 90 };        // the empty part of a bar or slider
        Color hover{ 141, 178, 216, 26 };     // under the cursor
        Color selected{ 141, 178, 216, 46 };  // the chosen row of a table, the open tab
        Color onAccent{ 11, 15, 20, 255 };    // text on a solid accent fill
        Color backdrop{ 6, 8, 12, 255 };      // behind the windows where there is no world: docked
        Color shade{ 6, 8, 14, 235 };         // over the world, under a screen drawn on it: the map
        Color sensor{ 5, 10, 9, 255 };        // the sensor screen's ground: opaque, a readout
    } colors;

    // Allegiance, as the *instruments* say it (#117): the overview, the radar, the sensor
    // screen and the target panel. The world view never colours an object by who owns it,
    // because the same station is a friend to one player and a target to another.
    //
    // Never colour alone (#362): green "you" against red "hostile" was the pair deuteranopia
    // cannot tell apart, so "own" is cyan in every theme, every instrument also says it by
    // shape, and the player can swap these for an orange/blue palette (ColourBlindSetting).
    struct Standing
    {
        Color own{ 103, 212, 224, 255 };
        Color friendly{ 111, 168, 232, 255 };
        Color neutral{ 201, 204, 196, 255 };
        Color hostile{ 224, 96, 79, 255 };
        Color unowned{ 118, 128, 143, 255 };
    } standing;

    struct Metrics
    {
        float titleHeight = 22.0f;  // a window's title bar
        float padding = 8.0f;       // inside a window, around its content
        float gap = 7.0f;           // between the blocks of a window
        float rowGap = 3.0f;        // between the rows of one block
        float radius = 0.0f;        // corners of buttons, chips and bars
        float border = 1.0f;        // outlines
        float barHeight = 5.0f;     // a progress bar
        float buttonHeight = 22.0f;
        float resizeGrip = 12.0f;   // the corner a resizable window is dragged by
        float rowHeight = 19.0f;    // a table's row
        float scrollbar = 4.0f;     // the width of a scroll indicator
        float snap = 12.0f;         // how near an edge a dragged window jumps to it
        float tabWidth = 120.0f;    // the widest a tab of a stacked window is
        float sidePanel = 260.0f;   // a screen's column of legends and news beside its picture
        float menuBar = 64.0f;      // the strip of window buttons down the left edge
        float formLabel = 104.0f;   // the column of labels on the left of a form (FormRow)
        float titleMark = 2.0f;     // an accent stripe down the left of a title bar; 0 for none
        float tabMark = 2.0f;       // the line under the chosen tab, in the underline style
        float windowRadius = 0.0f;  // a window's corners, rounded or cut (style.corner)
        float rowMark = 0.0f;       // an accent bar down the left of a table's lit row
    } metrics;

    // In pixels of em at a scale of 1, as CSS means a font size (Ui::Fonts).
    struct FontSizes
    {
        float small = 11.0f;
        float label = 12.0f;  // what a value is
        float body = 13.0f;   // the value itself, and running text
        float title = 13.0f;  // a window's title
        float heading = 21.0f;
    } fontSize;

    // One per Ui::Face, so changing the interface's typeface is a change to the theme file
    // and a font in data/fonts/, never to code; two faces may name one file. `numbers` is
    // for figures in columns and wants a font whose digits are all one width.
    struct Fonts
    {
        FaceSpec regular{ "fonts/BarlowSemiCondensed-Regular.ttf" };
        FaceSpec strong{ "fonts/BarlowSemiCondensed-SemiBold.ttf" };
        FaceSpec heading{ "fonts/BarlowCondensed-SemiBold.ttf", TextCase::Upper, 0.1f };
        FaceSpec numbers{ "fonts/BarlowSemiCondensedTnum-Regular.ttf" };
    } fonts;

    // How text is set, whatever the font.
    struct Text
    {
        // A line is this many times the text's size tall; the glyphs sit in its middle.
        float lineHeight = 1.35f;
        // Above 1 thickens each glyph's edge (Ui::FontFiles::gamma); 1 draws the coverage
        // the rasteriser gave.
        float gamma = 1.2f;
    } text;

    struct Style
    {
        Corner     corner = Corner::Square;
        TitleBar   titleBar = TitleBar::Solid;
        TabStyle   tabs = TabStyle::Underline;
        LineStyle  divider = LineStyle::Solid;
        BarStyle   bar = BarStyle::Solid;
        Shadow     shadow = Shadow::None;
        PanelStyle panel = PanelStyle::Flat;
        // 0..1: how strongly titles, the chosen tab, the lit row and bars glow in their own
        // colour. 0 draws no glow at all, and costs nothing.
        float glow = 0.0f;
        // The panel over the blurred world, when the player has "Blur behind windows" on
        // (Ui::BlurSetting) and the theme offers it: the colour the blur shows through. A
        // theme without one (null in the file) never blurs.
        bool  blurs = false;
        Color blurPanel{ 0, 0, 0, 0 };
    } style;

    // A widget's corner radius as a layout should draw it: none for square corners.
    float WidgetRadius() const { return style.corner == Corner::Square ? 0.0f : metrics.radius; }
};

// Reads a theme. Strict both ways: an unknown field is an error, because read leniently a
// misspelt "acent" is indistinguishable from an absent one (#191) and the colour would
// silently stay what it was; a missing field is an error too, so the file is the whole
// truth and the defaults above are only what a broken file falls back to. `into` is left
// untouched on failure. The id is the file's name and is not in the file.
bool ParseTheme(const nlohmann::json& j, Theme& into, std::string& error);
bool LoadTheme(const std::string& path, Theme& into, std::string& error);
// The theme in use. Until LoadTheme writes into it, it is the defaults above -- which is
// what the server, the agent and the tests see, since none of them draws.
Theme&       MutableTheme();
const Theme& CurrentTheme();

// The themes a player can pick: every file in `dir` (data/ui_themes/) that loads, in the
// order of their ids after the default, which comes first. A file that does not load is left
// out, and said in `problems`.
struct ThemeEntry
{
    std::string id, name;
};
std::vector<ThemeEntry> ListThemes(const std::string& dir, std::string* problems = nullptr);
constexpr const char*   DEFAULT_THEME = "fleet";

// "#RRGGBB" or "#RRGGBBAA".
bool ParseColor(const std::string& text, Color& out);

// --- Legibility (#362) ---------------------------------------------------------------
// WCAG 2's contrast ratio between two opaque colours, 1 to 21; alpha is ignored, so lay a
// translucent colour over what it is drawn on first (Over).
float Contrast(Color a, Color b);
// `top` laid over `under` by its alpha; the result is opaque.
Color Over(Color top, Color under);
// What every shipped theme is held to (theme_tests.cpp): text a player reads -- dim labels
// included -- against the panel it is drawn on, and the size of body text at a scale of 1.
constexpr float MIN_TEXT_CONTRAST = 4.5f;
constexpr float MIN_BODY_SIZE = 13.0f;

// "Colour-blind palette": the standing colours replaced, in whichever theme is in use, by
// one built on orange and blue rather than red and green -- the axis the common forms of
// colour blindness keep. Off by default; kept in ui_settings.json.
bool                   ColourBlindSetting();
void                   SetColourBlindSetting(bool on);  // repaints the theme in use at once
const Theme::Standing& ColourBlindStanding();
// A theme has just become the one in use: its own standing colours are noted, and the
// palette laid over them if the setting is on. UseTheme calls it.
void AdoptStanding();

// --- Scale -------------------------------------------------------------------------
// The player's own factor, times the display's DPI. A layout unit is Scale() pixels.
float Scale();
float UserScale();
void  SetUserScale(float s);  // clamped to [MIN_USER_SCALE, MAX_USER_SCALE]
// What the display asks for (1 on an ordinary monitor, 1.5 on a laptop set to 150%); 1
// before a window exists. Overridable so a screenshot is the same on every machine.
float DisplayScale();
void  OverrideDisplayScale(float s);  // 0 clears the override
float Px(float units);                // units to pixels at the current scale, rounded

// The player's factor a machine starts from when ui_settings.json has none (#362). The
// display's DPI is honoured by DisplayScale already, so a 1080p laptop at 125% gets 1.25
// with a factor of 1; but a 1080p screen left at 100% asks for nothing, and an interface
// laid out for 720 lines then reads at two thirds of the size it was drawn for. So the
// interface is made at least as large as the monitor's height suggests (1080 lines 1.25,
// 1440 1.5, 2160 2) -- the display's own scale counted first -- in the steps Settings
// offers. Never below 1.
float AutoUserScale(int monitorHeight, float displayScale);
// Whether the factor in use is one the player chose (Settings, or a file that says so); a
// choice is kept and saved, the automatic one is neither.
bool UserScaleChosen();
void ChooseUserScale(float s);   // SetUserScale, as the player's own choice
void UseAutoUserScale(float s);  // SetUserScale, as the machine's default: not saved

constexpr float MIN_USER_SCALE = 0.75f;
constexpr float MAX_USER_SCALE = 2.5f;

// This machine's interface settings: { "version": 2, "scale": 1.25, "theme": "fleet",
// "blur": false, "colourBlind": false }. "scale" is written only when the player chose one;
// version 1 wrote it always, so a version-1 file's 1.0 is read as no choice at all.
// Next to the executable like the window layout, because they belong to the display, not
// the account. A missing file is not an error; a file from a newer build is read for
// nothing and the caller should not write over it.
constexpr int UI_SETTINGS_VERSION = 2;
bool          LoadUiSettings(const std::string& path, std::string& error);
bool          SaveUiSettings(const std::string& path);
// The theme the settings name: DEFAULT_THEME until a file or the player says otherwise.
const std::string& ThemeSetting();
void               SetThemeSetting(const std::string& id);
// "Blur behind windows": the world under a window blurred, in a theme that offers it
// (style.blurPanel). Off until the player turns it on -- it costs a copy of the world and
// two passes over a quarter of it every frame, which a laptop's GPU feels.
bool BlurSetting();
void SetBlurSetting(bool on);
// "Brackets in space" (#361): standing brackets on the ships in the world view, and the
// marker on the player's own. On until the player turns them off -- they are how a ship
// that is three pixels long is found at all.
bool BracketSetting();
void SetBracketSetting(bool on);
// The goal prompt (#364): the line under the location plate that says what to do next. On
// until the player turns it off, or has handed in enough missions to have learned the loop
// -- how many is counted here, so the prompt does not come back with the next session.
bool GoalSetting();
void SetGoalSetting(bool on);
int  GoalLearned();
void SetGoalLearned(int n);

}  // namespace Ui
