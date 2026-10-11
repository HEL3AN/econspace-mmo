#pragma once

#include "raylib.h"
#include "ui/Fonts.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

// What a run of text measured, kept (#363). A measure is a function of the face, the size and
// the letters -- and of the fonts, which only a theme change replaces -- so the same label
// measured every frame is the same answer every frame. A table of a hundred rows asked it a
// thousand times a frame; the HUD spent most of its CPU there.
//
// The owner clears the cache when the fonts change (FontGeneration), the same hook that tells
// Clay its kept widths are another font's. Looking a text up allocates nothing: the key is a
// hash, and the entry keeps its text to tell a collision from a hit.
namespace Ui
{

class MeasureCache
{
public:
    using Measure = std::function<Vector2(Face face, float px, std::string_view text)>;

    // What `measure` says of the text, from the cache when it has been asked before.
    Vector2 Get(Face face, float px, std::string_view text, const Measure& measure);
    void    Clear() { entries_.clear(); }
    size_t  Size() const { return entries_.size(); }

    // More than this many and the cache starts again rather than grow without end: a window
    // of changing figures writes a new text every frame.
    static constexpr size_t LIMIT = 8192;

private:
    struct Entry
    {
        Face        face;
        float       px;
        std::string text;
        Vector2     size;
    };
    std::unordered_map<uint64_t, Entry> entries_;
};

}  // namespace Ui
