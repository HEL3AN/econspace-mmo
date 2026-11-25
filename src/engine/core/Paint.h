#pragma once

// A pilot's own paint on a palette livery (#552, documents/player_livery.md). A livery spec
// is a palette id, optionally followed by '/' and the paint: `guild` or `guild/c8b89a2f5fa6p2l1`
// -- hull tint and band as six hex digits each, `p` the stripe pattern, `l` the window light.
//
// The limits that keep paint art and never an allegiance tag (#117) are here, in one place
// the server and the Fit tab both read: colours are cells of a curated grid, never free RGB; a
// hull tint is quiet; the band must contrast with it; and no pattern is flat.

#include <array>
#include <string>

namespace Paint
{
inline constexpr int HUES = 24;
inline constexpr int LIGHTS = 5;
inline constexpr int SATS = 3;
inline constexpr int PATTERN_MIN =
    1;  // 0, no stripes, is never offered: a flat hull reads as a flag
inline constexpr int   PATTERN_MAX = 3;       // bands, spine, chevrons
inline constexpr int   LIGHT_MAX = 3;         // cold, white, warm, amber
inline constexpr float MIN_CONTRAST = 0.15f;  // band lightness against the hull's, at least

enum class Role
{
    Base,  // the hull plating's tint: low saturation, a mid band of lightness
    Band,  // the stripes: bolder, but never as dark or as pale as space or a star
};

struct Cell
{
    int  h = 0, l = 0, s = 0;
    bool operator==(const Cell& o) const { return h == o.h && l == o.l && s == o.s; }
};

struct Paint
{
    Cell base{ 0, 2, 0 };
    Cell band{ 0, 2, 2 };
    int  pattern = 1;
    int  light = 2;
};

using Rgb = std::array<unsigned char, 3>;

Rgb   RgbOf(Role role, Cell c);
float LightnessOf(Role role, Cell c);
// Whether this band may stand on this hull.
bool Contrasts(Cell base, Cell band);
// The cell of the grid nearest a colour.
Cell Nearest(Role role, Rgb rgb);
// A window-warmth value for the shader (0 cold .. 1 amber).
float Warmth(int light);

// Brings a paint inside the rules: grid cells, a band moved along lightness until it
// contrasts, the pattern and light clamped.
Paint Snap(Paint p);

// "id" and "id/paint". Split() reports false for a spec that is not of one of those shapes;
// a paint it reads is already snapped to the grid, and Snap() finishes the rules.
std::string Format(const std::string& palette, const Paint& p);
bool        Split(const std::string& spec, std::string& palette, bool& painted, Paint& p);
// The palette id of a spec, whatever follows it.
std::string PaletteOf(const std::string& spec);
}  // namespace Paint
