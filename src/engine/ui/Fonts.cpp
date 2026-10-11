#include "ui/Fonts.h"

#include "render/Perf.h"
#include "rlgl.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <unordered_map>
#include <vector>

// raylib rasterises with stb_truetype and keeps its copy private; this file reads the same
// tables through a copy of its own, for what raylib does not say: the font's metrics, each
// glyph's exact advance, and its kerning.
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "external/stb_truetype.h"

namespace Ui
{

namespace
{
// One rasterised size of a face.
struct Sized
{
    Font               font{};
    float              scale = 0.0f;  // font units to pixels
    LineMetrics        metrics;
    std::vector<float> advance;  // per slot, exact
};

struct FaceData
{
    std::string                path;
    TextCase                   textCase = TextCase::AsWritten;
    float                      tracking = 0.0f;  // ems between letters
    std::vector<unsigned char> file;             // the TTF, read once and rasterised per size
    stbtt_fontinfo             info{};
    bool                       ok = false;
    int                        ascent = 0, descent = 0;  // font units, as raylib reads them
    float                      unitsPerEm = 1.0f;
    float                      capHeight = 0.0f, xHeight = 0.0f;  // font units
    std::vector<int>           glyph;  // per slot: the font's glyph index, 0 if none
    std::map<int, Sized>       sizes;
    std::unordered_map<uint32_t, short> kern;  // pair of glyph indices -> font units
};

FaceData g_faces[FACE_COUNT];
float    g_gamma = 1.0f;
Font     g_glyph{};
bool     g_glyphLoaded = false;
bool     g_loaded = false;
int      g_generation = 0;

constexpr int MIN_PX = 6;
constexpr int MAX_PX = 160;
constexpr int GLYPH_PX = 96;
constexpr int PADDING = 2;  // around each glyph in the atlas, so neighbours never bleed in
constexpr int PHASES = 3;   // positions of a glyph within a pixel, rasterised apart

// Every character the client writes: printable ASCII, Latin-1 (names, the middle dot used
// as a separator) and the few typographic marks the interface uses. A glyph's place in this
// list is its slot in every atlas.
const int EXTRA[] = { 0x2013, 0x2014, 0x2022, 0x2026, 0x2190, 0x2191, 0x2192, 0x2193, 0x2212 };

std::vector<int>& Codepoints()
{
    static std::vector<int> cps;
    if (cps.empty())
    {
        for (int c = 32; c < 127; c++)
            cps.push_back(c);
        for (int c = 160; c < 256; c++)
            cps.push_back(c);
        for (int c : EXTRA)
            cps.push_back(c);
    }
    return cps;
}

int SlotOf(int cp)
{
    if (cp >= 32 && cp < 127)
        return cp - 32;
    if (cp >= 160 && cp < 256)
        return 95 + cp - 160;
    for (int i = 0; i < (int)(sizeof(EXTRA) / sizeof(EXTRA[0])); i++)
        if (EXTRA[i] == cp)
            return 95 + 96 + i;
    return '?' - 32;
}

// UTF-8 to code points, over text that need not end in a zero.
int NextCodepoint(std::string_view s, size_t& i)
{
    const unsigned char c = (unsigned char)s[i++];
    int                 extra = 0, cp = c;
    if (c >= 0xF0)
        extra = 3, cp = c & 0x07;
    else if (c >= 0xE0)
        extra = 2, cp = c & 0x0F;
    else if (c >= 0xC0)
        extra = 1, cp = c & 0x1F;
    else if (c >= 0x80)
        return '?';
    for (; extra > 0; extra--)
    {
        if (i >= s.size() || ((unsigned char)s[i] & 0xC0) != 0x80)
            return '?';
        cp = (cp << 6) | ((unsigned char)s[i++] & 0x3F);
    }
    return cp;
}

// The capital of a letter in the set the atlas holds: ASCII and Latin-1 (where the capitals
// are 0x20 below, except the sign of division and the two letters with no capital there).
int Upper(int cp)
{
    if (cp >= 'a' && cp <= 'z')
        return cp - 32;
    if (cp >= 0xE0 && cp <= 0xFE && cp != 0xF7)
        return cp - 32;
    return cp;
}

// ...and the small letter of a capital, the other way round.
int Lower(int cp)
{
    if (cp >= 'A' && cp <= 'Z')
        return cp + 32;
    if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7)
        return cp + 32;
    return cp;
}

bool IsSmallLetter(int cp)
{
    return Upper(cp) != cp;
}

bool IsDefault(const Font& font)
{
    return font.texture.id == GetFontDefault().texture.id;
}

// The pixel size raylib is asked for -- its size is ascender to descender -- for an em of `px`.
int RasterPx(const FaceData& f, int px)
{
    if (!f.ok)
        return px;
    const float lineUnits = (float)(f.ascent - f.descent);
    return std::max(1, (int)std::lround((float)px * lineUnits / f.unitsPerEm));
}

void ReadFace(FaceData& f, const std::string& path)
{
    f = FaceData();
    f.path = path;
    int            size = 0;
    unsigned char* data = LoadFileData(path.c_str(), &size);
    if (data == nullptr || size <= 0)
    {
        TraceLog(LOG_ERROR, "UI font missing: %s -- falling back to raylib's own", path.c_str());
        return;
    }
    f.file.assign(data, data + size);
    UnloadFileData(data);
    const int offset = stbtt_GetFontOffsetForIndex(f.file.data(), 0);
    if (offset < 0 || !stbtt_InitFont(&f.info, f.file.data(), offset))
    {
        TraceLog(LOG_ERROR, "UI font unreadable: %s -- falling back to raylib's own", path.c_str());
        f.file.clear();
        return;
    }
    int lineGap = 0;
    stbtt_GetFontVMetrics(&f.info, &f.ascent, &f.descent, &lineGap);
    if (f.ascent - f.descent <= 0)
    {
        f.file.clear();
        return;
    }
    f.unitsPerEm = 1.0f / stbtt_ScaleForMappingEmToPixels(&f.info, 1.0f);
    // The heights of an H and an x, from the outlines themselves: every font has those
    // glyphs, where the OS/2 table that also says them is optional and often wrong.
    int x0, y0, x1, y1;
    f.capHeight =
        stbtt_GetCodepointBox(&f.info, 'H', &x0, &y0, &x1, &y1) ? (float)y1 : 0.7f * f.unitsPerEm;
    f.xHeight =
        stbtt_GetCodepointBox(&f.info, 'x', &x0, &y0, &x1, &y1) ? (float)y1 : 0.5f * f.unitsPerEm;
    for (int cp : Codepoints())
        f.glyph.push_back(stbtt_FindGlyphIndex(&f.info, cp));
    f.ok = true;
}

Sized Rasterise(FaceData& f, int px)
{
    Sized s;
    if (!f.ok)
    {
        s.font = GetFontDefault();
        s.metrics.height = s.metrics.baseline = (float)px;
        s.metrics.capTop = s.metrics.xTop = 0.0f;
        return s;
    }
    const int raster = RasterPx(f, px);
    s.scale = stbtt_ScaleForMappingEmToPixels(&f.info, (float)px);
    const float baseline = std::round((float)f.ascent * s.scale);
    s.metrics.height = (float)raster;
    s.metrics.baseline = baseline;
    s.metrics.capTop = baseline - f.capHeight * s.scale;
    s.metrics.xTop = baseline - f.xHeight * s.scale;

    // Each glyph rasterised PHASES times, shifted by a fraction of a pixel each time: a pen
    // that has advanced 7.4 pixels draws the glyph made at 0.33 from pixel 7, rather than
    // the whole glyph at 7. Snapped to whole pixels instead, the gaps between letters
    // wander by a pixel from pair to pair, which at 12 px is the difference between
    // "HOSTILE" and "HOS TILE".
    unsigned char lut[256];
    for (int i = 0; i < 256; i++)
        lut[i] = (unsigned char)std::lround(255.0 * std::pow(i / 255.0, 1.0 / g_gamma));
    const std::vector<int>& cps = Codepoints();
    const int               n = (int)cps.size();
    // The atlas's row height. raylib packs a row exactly as tall as the size it is given, and
    // a glyph that reaches past the ascender or the descender -- '|' does in most faces --
    // would be packed over the top of the row beneath it (#366).
    int        tallest = raster;
    GlyphInfo* glyphs = (GlyphInfo*)MemAlloc((unsigned int)(sizeof(GlyphInfo) * n * PHASES));
    for (int p = 0; p < PHASES; p++)
        for (int i = 0; i < n; i++)
        {
            GlyphInfo&     g = glyphs[p * n + i];
            int            w = 0, h = 0, xoff = 0, yoff = 0;
            unsigned char* bm =
                f.glyph[(size_t)i] == 0
                    ? nullptr
                    : stbtt_GetGlyphBitmapSubpixel(&f.info, s.scale, s.scale, (float)p / PHASES,
                                                   0.0f, f.glyph[(size_t)i], &w, &h, &xoff, &yoff);
            const bool ink = bm != nullptr && w > 0 && h > 0;
            if (!ink)
            {
                // A space, or a glyph the font lacks: one clear pixel, which the atlas packs.
                w = h = 1;
                xoff = yoff = 0;
            }
            // Copied into memory raylib owns, since raylib frees it with its own allocator.
            unsigned char* data = (unsigned char*)MemAlloc((unsigned int)(w * h));
            if (ink)
                for (int k = 0; k < w * h; k++)
                    data[k] = lut[bm[k]];
            if (bm != nullptr)
                STBTT_free(bm, nullptr);
            g.value = cps[(size_t)i];
            g.offsetX = xoff;
            g.offsetY = (int)baseline + yoff;
            g.advanceX = 0;  // never read: the walk advances by the exact widths
            g.image = { data, w, h, 1, PIXELFORMAT_UNCOMPRESSED_GRAYSCALE };
            tallest = std::max(tallest, h);
        }
    Font font{};
    font.baseSize = raster;
    font.glyphCount = n * PHASES;
    font.glyphPadding = PADDING;
    font.glyphs = glyphs;
    Image atlas = GenImageFontAtlas(glyphs, &font.recs, font.glyphCount, tallest, PADDING, 0);
    font.texture = LoadTextureFromImage(atlas);
    UnloadImage(atlas);
    // Drawn at exactly the size it was rasterised at, on whole pixels: point sampling
    // keeps every edge where the rasteriser put it.
    SetTextureFilter(font.texture, TEXTURE_FILTER_POINT);
    s.font = font;

    s.advance.resize(cps.size(), 0.0f);
    for (size_t i = 0; i < cps.size(); i++)
    {
        int adv = 0, lsb = 0;
        stbtt_GetGlyphHMetrics(&f.info, f.glyph[i], &adv, &lsb);
        s.advance[i] = (float)adv * s.scale;
    }
    return s;
}

// The world's glyph font, built here rather than by LoadFontFromMemory (#366): raylib packs
// that atlas in rows exactly `GLYPH_PX` tall and warns about every glyph taller than that --
// '|', at every start -- before packing it over the row beneath. Rasterised the same way
// raylib would (scaled to the line, offsets from the ascender), into rows that fit.
Font BuildGlyphFont(FaceData& f)
{
    const float             scale = stbtt_ScaleForPixelHeight(&f.info, (float)GLYPH_PX);
    const int               baseline = (int)((float)f.ascent * scale);
    const std::vector<int>& cps = Codepoints();
    const int               n = (int)cps.size();
    constexpr int           GLYPH_PADDING = 4;  // raylib's own, so mipmaps do not bleed either
    int                     tallest = GLYPH_PX;
    GlyphInfo*              glyphs = (GlyphInfo*)MemAlloc((unsigned int)(sizeof(GlyphInfo) * n));
    for (int i = 0; i < n; i++)
    {
        GlyphInfo&     g = glyphs[i];
        int            w = 0, h = 0, xoff = 0, yoff = 0;
        unsigned char* bm = f.glyph[(size_t)i] == 0
                                ? nullptr
                                : stbtt_GetGlyphBitmap(&f.info, scale, scale, f.glyph[(size_t)i],
                                                       &w, &h, &xoff, &yoff);
        const bool     ink = bm != nullptr && w > 0 && h > 0;
        if (!ink)
        {
            w = h = 1;
            xoff = yoff = 0;
        }
        unsigned char* data = (unsigned char*)MemAlloc((unsigned int)(w * h));
        if (ink)
            std::memcpy(data, bm, (size_t)(w * h));
        if (bm != nullptr)
            STBTT_free(bm, nullptr);
        int adv = 0, lsb = 0;
        if (f.glyph[(size_t)i] != 0)
            stbtt_GetGlyphHMetrics(&f.info, f.glyph[(size_t)i], &adv, &lsb);
        g.value = cps[(size_t)i];
        g.offsetX = xoff;
        g.offsetY = baseline + yoff;
        g.advanceX = (int)((float)adv * scale);
        g.image = { data, w, h, 1, PIXELFORMAT_UNCOMPRESSED_GRAYSCALE };
        tallest = std::max(tallest, h);
    }
    Font font{};
    font.baseSize = GLYPH_PX;
    font.glyphCount = n;
    font.glyphPadding = GLYPH_PADDING;
    font.glyphs = glyphs;
    Image atlas = GenImageFontAtlas(glyphs, &font.recs, n, tallest, GLYPH_PADDING, 0);
    font.texture = LoadTextureFromImage(atlas);
    UnloadImage(atlas);
    return font;
}

const Sized& SizedAt(Face face, int px)
{
    FaceData& f = g_faces[(int)face];
    px = FontPx((float)px);
    auto it = f.sizes.find(px);
    if (it == f.sizes.end())
        it = f.sizes.emplace(px, Rasterise(f, px)).first;
    return it->second;
}

float Kerning(FaceData& f, int slotA, int slotB)
{
    const int a = f.glyph[(size_t)slotA], b = f.glyph[(size_t)slotB];
    if (a == 0 || b == 0)
        return 0.0f;
    const uint32_t key = ((uint32_t)a << 16) | (uint32_t)(b & 0xFFFF);
    auto           it = f.kern.find(key);
    if (it == f.kern.end())
        it = f.kern.emplace(key, (short)stbtt_GetGlyphKernAdvance(&f.info, a, b)).first;
    return (float)it->second;
}

// Sets the letters of one run as a face's case says (TextCase), one code point at a time.
class CaseSetter
{
public:
    CaseSetter(TextCase c, std::string_view text) : case_(c)
    {
        // A run is set as a sentence only if it is all capitals; one small letter says the
        // capitals in it are meant (a name), and it is drawn as written.
        if (c == TextCase::Sentence)
        {
            lower_ = true;
            for (size_t i = 0; i < text.size() && lower_;)
                lower_ = !IsSmallLetter(NextCodepoint(text, i));
        }
    }
    int operator()(int cp)
    {
        if (case_ == TextCase::Upper)
            return Upper(cp);
        if (case_ != TextCase::Sentence)
            return cp;
        // A sentence that starts with a letter starts with a capital ("1280 x 720" does not);
        // the letters after it are small if the run was all capitals, and as written otherwise.
        const bool first = first_ && cp != ' ';
        if (first)
            first_ = false;
        const bool letter = Lower(cp) != cp || Upper(cp) != cp;
        if (!letter)
            return cp;
        return first ? Upper(cp) : lower_ ? Lower(cp) : cp;
    }

private:
    TextCase case_;
    bool     lower_ = false;
    bool     first_ = true;
};

// Walks a run of text glyph by glyph: where each one goes, relative to the run's top left.
// Measuring and drawing are the same walk, so a measure is exactly what is drawn.
template <typename F> Vector2 Walk(Face face, int px, std::string_view text, F&& glyph)
{
    FaceData&    f = g_faces[(int)face];
    const Sized& s = SizedAt(face, px);
    float        x = 0.0f, y = 0.0f, widest = 0.0f;
    int          prev = -1;
    CaseSetter   setCase(f.textCase, text);
    for (size_t i = 0; i < text.size();)
    {
        const int cp = setCase(NextCodepoint(text, i));
        if (cp == '\n')
        {
            widest = std::max(widest, x);
            x = 0.0f;
            y += s.metrics.height;
            prev = -1;
            continue;
        }
        const int slot = SlotOf(cp);
        // Tracking goes between letters, never after the last: a label centred in a button
        // would otherwise sit left of its middle by one letter's tracking.
        if (prev >= 0)
            x += Kerning(f, prev, slot) * s.scale + f.tracking * (float)px;
        glyph(s, slot, x, y);
        x += s.advance[(size_t)slot];
        prev = slot;
    }
    return { std::max(widest, x), y + s.metrics.height };
}
}  // namespace

void LoadFonts(const FontFiles& files)
{
    UnloadFonts();
    g_gamma = files.gamma > 0.1f ? files.gamma : 1.0f;
    for (int i = 0; i < FACE_COUNT; i++)
    {
        ReadFace(g_faces[i], files.faces[i].path);
        g_faces[i].textCase = files.faces[i].textCase;
        g_faces[i].tracking = files.faces[i].tracking;
    }
    g_loaded = true;
    g_generation++;
}

void UnloadFonts()
{
    for (FaceData& f : g_faces)
    {
        for (auto& entry : f.sizes)
            if (!IsDefault(entry.second.font))
                UnloadFont(entry.second.font);
        f.sizes.clear();
    }
    if (g_glyphLoaded && !IsDefault(g_glyph))
        UnloadFont(g_glyph);
    g_glyphLoaded = false;
    g_loaded = false;
}

int FontGeneration()
{
    return g_generation;
}

bool FontsLoaded()
{
    return g_loaded;
}

std::string SetCase(TextCase c, std::string_view text)
{
    CaseSetter  setCase(c, text);
    std::string out;
    for (size_t i = 0; i < text.size();)
    {
        const int cp = setCase(NextCodepoint(text, i));
        // Back to UTF-8: the atlas holds nothing above three bytes.
        if (cp < 0x80)
            out += (char)cp;
        else if (cp < 0x800)
        {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        }
        else
        {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

int FontPx(float px)
{
    const int p = (int)std::lround(px);
    return p < MIN_PX ? MIN_PX : (p > MAX_PX ? MAX_PX : p);
}

LineMetrics MetricsAt(Face face, float px)
{
    if (!g_loaded)
    {
        const float p = (float)FontPx(px);
        return { p, p * 0.8f, p * 0.2f, p * 0.35f };
    }
    return SizedAt(face, FontPx(px)).metrics;
}

float CentredTop(const LineMetrics& m, float top, float height)
{
    const float capMiddle = (m.capTop + m.baseline) * 0.5f;
    return std::floor(top + height * 0.5f - capMiddle + 0.5f);
}

float CentredTop(Face face, float px, float top, float height)
{
    return CentredTop(MetricsAt(face, px), top, height);
}

Vector2 MeasureString(Face face, float px, std::string_view text)
{
    const int p = FontPx(px);
    if (!g_loaded || !g_faces[(int)face].ok)
    {
        const std::string s(text);
        return MeasureTextEx(GetFontDefault(), s.c_str(), (float)p, 1.0f);
    }
    return Walk(face, p, text, [](const Sized&, int, float, float) {});
}

namespace
{
// Every glyph of a run with something to draw, as the quad it is drawn in: the atlas cell
// (its padding included) and where on the screen, the run's top left at (x0, y0).
template <typename F>
void Quads(Face face, int p, std::string_view text, float x0, float y0, F&& quad)
{
    Walk(face, p, text,
         [&](const Sized& s, int slot, float x, float y)
         {
             // The whole pixel the pen is in, and the glyph made for where in it.
             float whole = std::floor(x);
             int   phase = (int)std::lround((x - whole) * PHASES);
             if (phase == PHASES)
                 phase = 0, whole += 1.0f;
             const Font&      font = s.font;
             const int        g = phase * (int)Codepoints().size() + slot;
             const Rectangle& r = font.recs[g];
             if (r.width <= 1.0f && r.height <= 1.0f)
                 return;  // nothing to draw: a space
             const float     pad = (float)font.glyphPadding;
             const float     gx = x0 + whole + (float)font.glyphs[g].offsetX;
             const float     gy = y0 + y + (float)font.glyphs[g].offsetY;
             const Rectangle src{ r.x - pad, r.y - pad, r.width + 2.0f * pad,
                                  r.height + 2.0f * pad };
             quad(src, Rectangle{ gx - pad, gy - pad, src.width, src.height });
         });
}
}  // namespace

bool FaceReady(Face face)
{
    return g_loaded && g_faces[(int)face].ok;
}

void DrawString(Face face, std::string_view text, Vector2 pos, float px, Color color,
                const Rectangle* clip)
{
    Render::Perf::Scope timed(Render::Perf::Section::Text);
    const int           p = FontPx(px);
    // On whole pixels, so the atlas lands on the screen's grid.
    const float x0 = std::floor(pos.x + 0.5f), y0 = std::floor(pos.y + 0.5f);
    if (!g_loaded || !g_faces[(int)face].ok)
    {
        const std::string s(text);
        DrawTextEx(GetFontDefault(), s.c_str(), { x0, y0 }, (float)p, 1.0f, color);
        return;
    }
    // One run of quads on one texture (#363): what DrawTexturePro does per glyph -- the same
    // corners, the same texture coordinates -- with the texture bound and the batch opened once
    // for the run rather than once a letter. Room for every byte as a glyph is made first,
    // since a batch must not be flushed half way through a quad.
    const Font& atlas = SizedAt(face, p).font;
    if (atlas.texture.id == 0)
        return;
    const float tw = (float)atlas.texture.width, th = (float)atlas.texture.height;
    rlCheckRenderBatchLimit(4 * (int)text.size());
    rlSetTexture(atlas.texture.id);
    rlBegin(RL_QUADS);
    rlColor4ub(color.r, color.g, color.b, color.a);
    rlNormal3f(0.0f, 0.0f, 1.0f);
    Quads(face, p, text, x0, y0,
          [&](Rectangle src, Rectangle dst)
          {
              if (clip != nullptr)
              {
                  // The atlas is drawn a texel to a pixel, so cutting the quad cuts its cell by
                  // as many texels: what is left samples exactly what it sampled before.
                  const float l = std::max(dst.x, clip->x), t = std::max(dst.y, clip->y);
                  const float r = std::min(dst.x + dst.width, clip->x + clip->width);
                  const float b = std::min(dst.y + dst.height, clip->y + clip->height);
                  if (l >= r || t >= b)
                      return;
                  if (l != dst.x || t != dst.y || r != dst.x + dst.width || b != dst.y + dst.height)
                  {
                      src = { src.x + (l - dst.x), src.y + (t - dst.y), r - l, b - t };
                      dst = { l, t, r - l, b - t };
                  }
              }
              const float l = dst.x, t = dst.y, r = dst.x + dst.width, b = dst.y + dst.height;
              rlTexCoord2f(src.x / tw, src.y / th);
              rlVertex2f(l, t);
              rlTexCoord2f(src.x / tw, (src.y + src.height) / th);
              rlVertex2f(l, b);
              rlTexCoord2f((src.x + src.width) / tw, (src.y + src.height) / th);
              rlVertex2f(r, b);
              rlTexCoord2f((src.x + src.width) / tw, src.y / th);
              rlVertex2f(r, t);
          });
    rlEnd();
    rlSetTexture(0);
}

Rectangle TextInk(Face face, std::string_view text, Vector2 pos, float px)
{
    const int   p = FontPx(px);
    const float x0 = std::floor(pos.x + 0.5f), y0 = std::floor(pos.y + 0.5f);
    if (!g_loaded || !g_faces[(int)face].ok)
    {
        // raylib's own font: its letters stay in the box it measures, a size's margin aside.
        const Vector2 m = MeasureString(face, px, text);
        return { x0 - (float)p, y0 - (float)p, m.x + 2.0f * (float)p, m.y + 2.0f * (float)p };
    }
    float lx = 0.0f, ty = 0.0f, rx = 0.0f, by = 0.0f;
    bool  any = false;
    Quads(face, p, text, x0, y0,
          [&](const Rectangle&, const Rectangle& d)
          {
              lx = any ? std::min(lx, d.x) : d.x;
              ty = any ? std::min(ty, d.y) : d.y;
              rx = any ? std::max(rx, d.x + d.width) : d.x + d.width;
              by = any ? std::max(by, d.y + d.height) : d.y + d.height;
              any = true;
          });
    return any ? Rectangle{ lx, ty, rx - lx, by - ty } : Rectangle{ x0, y0, 0.0f, 0.0f };
}

const Font& GlyphFont()
{
    if (!g_glyphLoaded)
    {
        g_glyphLoaded = true;
        FaceData& f = g_faces[(int)Face::Strong];
        if (f.ok)
            g_glyph = BuildGlyphFont(f);
        if (g_glyph.texture.id == 0)
            g_glyph = GetFontDefault();
        if (!IsDefault(g_glyph))
        {
            // Scaled freely with the camera, so filtered, and mipmapped for when it is small.
            GenTextureMipmaps(&g_glyph.texture);
            SetTextureFilter(g_glyph.texture, TEXTURE_FILTER_TRILINEAR);
        }
    }
    return g_glyph;
}

}  // namespace Ui
