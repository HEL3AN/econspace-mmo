#pragma once

#include "raylib.h"
#include <string>
#include <string_view>

// The interface's typeface (#297): an ordinary, readable font in place of raylib's pixel one.
//
// Text is crisp at any size because it is never scaled as a bitmap. A face is rasterised
// at exactly the pixel size asked for, the first time that size is asked for, and kept --
// so 14 px text is drawn from a 14 px atlas and, at a UI scale of 1.5, from a 21 px one.
// Signed-distance fields were the alternative; at the 11-16 px of a window they come out
// softer than a hinted atlas does, and the number of sizes a UI uses is small.
namespace Ui
{

enum class Face
{
    Regular,
    Strong,  // titles and emphasis
};

// After InitWindow. A face whose file is missing falls back to raylib's own font and says
// so in the log: the game still runs, but nothing should ship like that.
void LoadFonts(const std::string& regularPath, const std::string& strongPath);
void UnloadFonts();
bool FontsLoaded();

// The face at a whole number of pixels, rasterised on first use.
const Font& FontAt(Face face, int px);
int         FontPx(float px);  // the size actually used for a requested one

// Measure and draw at a size in pixels. The text needs no terminating zero.
Vector2 MeasureString(Face face, float px, std::string_view text);
void    DrawString(Face face, std::string_view text, Vector2 pos, float px, Color color);

// One large, filtered atlas for text that is scaled freely rather than set at a size:
// the glyphs of the world view, which grow and shrink with the camera.
const Font& GlyphFont();

}  // namespace Ui
