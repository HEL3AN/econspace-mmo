#pragma once

#include "raylib.h"
#include <string>
#include <string_view>

// The interface's typefaces (#297): ordinary, readable fonts in place of raylib's pixel one,
// and which fonts they are is data (a theme in data/ui_themes/, "fonts") -- a new typeface is a
// file and a path, never code.
//
// Text is crisp at any size because it is never scaled as a bitmap. A face is rasterised
// at exactly the pixel size asked for, the first time that size is asked for, and kept --
// so 14 px text is drawn from a 14 px atlas and, at a UI scale of 1.5, from a 21 px one.
// Signed-distance fields were the alternative; at the 11-16 px of a window they come out
// softer than a coverage atlas does, and the number of sizes a UI uses is small.
//
// A size is the font's em, as CSS means it, whatever the font: two typefaces at 14 px are
// the same size to the eye, where raylib's own size (the height from the highest ascender
// to the lowest descender) makes a font with long descenders look smaller. Each glyph
// advances by its exact width plus the font's kerning, and is drawn from one of three
// rasterisations a third of a pixel apart, so a word is spaced as its designer spaced it
// rather than by rounding -- with no filtering, so nothing is blurred. The rasteriser does
// no hinting; the theme's gamma puts back the weight that costs pale text on a dark ground.
namespace Ui
{

// What a run of text is for. Each is a font file of its own, named by the theme; two of
// them may well be the same file.
enum class Face
{
    Regular,  // body text, labels
    Strong,   // emphasis: a value that matters, a button's label
    Heading,  // window titles, a block's heading, a screen's heading
    Numbers,  // figures in columns: a font whose digits are all one width
};
constexpr int FACE_COUNT = 4;

// How a face sets the letters it is given. `Sentence` is for a theme that writes its titles
// as a sentence: its first letter is a capital ("dist" is "Dist"), and a run written all in
// capitals ("SELECTED ITEM", as the code names windows) keeps only that one; a run that has
// any small letter in it is a name or a sentence already ("Jita IV") and keeps the rest.
enum class TextCase
{
    AsWritten,
    Upper,     // capitals, whatever the text says
    Sentence,  // capitals written as a sentence
};

// The letters of `text` as a face in this case draws them: what a test can hold of the rule.
std::string SetCase(TextCase c, std::string_view text);

struct FontFiles
{
    // One per Face, in its order: the file, and how text in it is set.
    struct File
    {
        std::string path;
        TextCase    textCase = TextCase::AsWritten;
        float       tracking = 0.0f;  // extra room between letters, in ems
    } faces[FACE_COUNT];
    // Above 1 thickens the coverage of every glyph's edge: pale text on a dark ground blended
    // without gamma correction reads thinner than it was drawn, and this puts the weight back.
    float gamma = 1.0f;
};

// After InitWindow. A face whose file is missing falls back to raylib's own font and says
// so in the log: the game still runs, but nothing should ship like that.
void LoadFonts(const FontFiles& files);
// Counts LoadFonts calls: whatever measured text and kept the measure (Clay does, per layout)
// compares this to know its widths are another font's.
int  FontGeneration();
void UnloadFonts();
bool FontsLoaded();

// The size actually used for a requested one: whole pixels of em.
int FontPx(float px);

// Where the lines of a face fall in the box a run of text is drawn in, in pixels from the
// box's top, at one size. Read from the font's own tables, so they hold for any font.
struct LineMetrics
{
    float height = 0.0f;    // the box: ascender to descender (what MeasureString says)
    float baseline = 0.0f;  // where the letters stand
    float capTop = 0.0f;    // the top of a capital and of a digit
    float xTop = 0.0f;      // the top of a lowercase x
};
LineMetrics MetricsAt(Face face, float px);

// The top at which to draw a run of text so that it sits in the middle of [top, top+height]
// to the eye: its capitals and figures centred, on a whole pixel. Every widget that puts a
// label in a box asks this rather than centring the text's box, whose ascender and descender
// room differ from font to font -- that difference was why labels sat low.
float CentredTop(Face face, float px, float top, float height);
// The same rule on given metrics; a test can hold it.
float CentredTop(const LineMetrics& m, float top, float height);

// Measure and draw at a size in pixels. The text needs no terminating zero. A measure is
// as wide as the pen's advance and as tall as LineMetrics::height; `pos` is the top left.
Vector2 MeasureString(Face face, float px, std::string_view text);
// With `clip`, only what falls inside it is drawn -- the pixels a scissor would leave, with
// no scissor: setting one flushes the batch (#363). Ignored by raylib's fallback font, which
// a face without its file draws in (FaceReady says which).
void DrawString(Face face, std::string_view text, Vector2 pos, float px, Color color,
                const Rectangle* clip = nullptr);
bool FaceReady(Face face);
// The pixels DrawString with the same arguments can colour, as a rectangle: the letters'
// own bounds rather than the line's, so a clip knows whether it would cut any of them. Empty
// for a run with nothing to draw.
Rectangle TextInk(Face face, std::string_view text, Vector2 pos, float px);

// One large, filtered atlas for text that is scaled freely rather than set at a size:
// the glyphs of the world view, which grow and shrink with the camera.
const Font& GlyphFont();

}  // namespace Ui
