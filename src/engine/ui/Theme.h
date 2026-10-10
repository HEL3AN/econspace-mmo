#pragma once

#include "raylib.h"  // Color: a plain struct, nothing here draws
#include <nlohmann/json.hpp>
#include <string>

// What the interface looks like, as data (#297): colours, spacing, corner radii and font
// sizes, read from data/ui_theme.json. A screen asks the theme for "the dim text colour" or
// "the gap between rows" and never writes a number of its own, so the whole interface can
// be retuned in one file and scaled by one factor.
//
// Every value below is in *layout units*. A unit is a pixel at a scale of 1; Ui::Px turns
// one into pixels at the current scale, which is the player's own setting times the
// display's DPI (half of #179).
namespace Ui
{

struct Theme
{
    struct Colors
    {
        Color panel{ 18, 20, 28, 235 };    // a window's body
        Color border{ 70, 80, 100, 255 };  // its outline, and dividers
        Color title{ 34, 38, 52, 255 };    // its title bar
        Color accent{ 92, 170, 232, 255 };
        Color text{ 222, 226, 234, 255 };
        Color dim{ 132, 142, 158, 255 };   // labels, and whatever is off
        Color good{ 120, 210, 130, 255 };  // healthy, complete
        Color warn{ 232, 190, 90, 255 };   // attention
        Color bad{ 230, 90, 80, 255 };     // damage, failure
        Color money{ 255, 203, 0, 255 };   // credits, wherever they are shown
        Color track{ 80, 86, 100, 110 };   // the empty part of a bar or slider
        Color hover{ 92, 170, 232, 40 };   // under the cursor
    } colors;

    // Allegiance, as the *instruments* say it (#117): the overview, the radar, the sensor
    // screen and the target panel. The world view never colours an object by who owns it,
    // because the same station is a friend to one player and a target to another.
    struct Standing
    {
        Color own{ 120, 235, 130, 255 };
        Color friendly{ 90, 170, 255, 255 };
        Color neutral{ 215, 215, 205, 255 };
        Color hostile{ 230, 90, 80, 255 };
        Color unowned{ 125, 135, 155, 255 };
    } standing;

    struct Metrics
    {
        float titleHeight = 26.0f;  // a window's title bar
        float padding = 12.0f;      // inside a window, around its content
        float gap = 8.0f;           // between the blocks of a window
        float rowGap = 4.0f;        // between the rows of one block
        float radius = 3.0f;        // corners of buttons, chips and bars
        float border = 1.0f;        // outlines
        float barHeight = 8.0f;     // a progress bar
        float buttonHeight = 26.0f;
        float resizeGrip = 14.0f;  // the corner a resizable window is dragged by
    } metrics;

    struct FontSizes
    {
        float small = 11.0f;
        float label = 13.0f;  // what a value is
        float body = 14.0f;   // the value itself, and running text
        float title = 15.0f;  // a window's title
        float heading = 22.0f;
    } fontSize;

    // Paths relative to data/. Two faces: one for reading, one for titles and emphasis.
    struct Fonts
    {
        std::string regular = "fonts/Inter-Regular.ttf";
        std::string strong = "fonts/Inter-SemiBold.ttf";
    } fonts;
};

// Reads a theme. Strict both ways: an unknown field is an error, because read leniently a
// misspelt "acent" is indistinguishable from an absent one (#191) and the colour would
// silently stay what it was; a missing field is an error too, so the file is the whole
// truth and the defaults above are only what a broken file falls back to. `into` is left
// untouched on failure.
bool ParseTheme(const nlohmann::json& j, Theme& into, std::string& error);
bool LoadTheme(const std::string& path, Theme& into, std::string& error);
// The theme in use. Until LoadTheme writes into it, it is the defaults above -- which is
// what the server, the agent and the tests see, since none of them draws.
Theme&       MutableTheme();
const Theme& CurrentTheme();

// "#RRGGBB" or "#RRGGBBAA".
bool ParseColor(const std::string& text, Color& out);

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

constexpr float MIN_USER_SCALE = 0.75f;
constexpr float MAX_USER_SCALE = 2.5f;

// This machine's interface settings: { "version": 1, "scale": 1.25 }. Next to the
// executable like the window layout, because they belong to the display, not the account.
// A missing file is not an error; a file from a newer build is read for nothing and the
// caller should not write over it.
constexpr int UI_SETTINGS_VERSION = 1;
bool          LoadUiSettings(const std::string& path, std::string& error);
bool          SaveUiSettings(const std::string& path);

}  // namespace Ui
