#include "ui/Fonts.h"

#include <cmath>
#include <map>
#include <vector>

namespace Ui
{

namespace
{
struct FaceData
{
    std::string         path;
    std::vector<char>   file;  // the TTF, read once and rasterised per size from memory
    std::map<int, Font> sizes;
};

FaceData g_faces[2];
Font     g_glyph{};
bool     g_glyphLoaded = false;
bool     g_loaded = false;

constexpr int MIN_PX = 6;
constexpr int MAX_PX = 160;
constexpr int GLYPH_PX = 96;

// Every character the client writes: printable ASCII, Latin-1 (names, the middle dot used
// as a separator) and the few typographic marks the interface uses.
std::vector<int>& Codepoints()
{
    static std::vector<int> cps;
    if (cps.empty())
    {
        for (int c = 32; c < 127; c++)
            cps.push_back(c);
        for (int c = 160; c < 256; c++)
            cps.push_back(c);
        for (int c : { 0x2013, 0x2014, 0x2022, 0x2026, 0x2190, 0x2191, 0x2192, 0x2193, 0x2212 })
            cps.push_back(c);
    }
    return cps;
}

bool IsDefault(const Font& font)
{
    return font.texture.id == GetFontDefault().texture.id;
}

Font Rasterise(FaceData& f, int px)
{
    if (f.file.empty())
        return GetFontDefault();
    std::vector<int>& cps = Codepoints();
    Font font = LoadFontFromMemory(".ttf", (const unsigned char*)f.file.data(), (int)f.file.size(),
                                   px, cps.data(), (int)cps.size());
    if (font.texture.id == 0)
        return GetFontDefault();
    // Drawn at exactly the size it was rasterised at, on whole pixels: point sampling
    // keeps every edge where the rasteriser put it.
    SetTextureFilter(font.texture, TEXTURE_FILTER_POINT);
    return font;
}

void ReadFace(FaceData& f, const std::string& path)
{
    f.path = path;
    f.file.clear();
    int            size = 0;
    unsigned char* data = LoadFileData(path.c_str(), &size);
    if (data == nullptr || size <= 0)
    {
        TraceLog(LOG_ERROR, "UI font missing: %s -- falling back to raylib's own", path.c_str());
        return;
    }
    f.file.assign((const char*)data, (const char*)data + size);
    UnloadFileData(data);
}

// raylib wants a zero-terminated string; a short copy on the stack covers nearly
// everything a window says.
class Terminated
{
public:
    explicit Terminated(std::string_view text)
    {
        if (text.size() < sizeof(small_))
        {
            text.copy(small_, text.size());
            small_[text.size()] = '\0';
        }
        else
            big_.assign(text);
    }
    const char* c_str() const { return big_.empty() ? small_ : big_.c_str(); }

private:
    char        small_[256];
    std::string big_;
};
}  // namespace

void LoadFonts(const std::string& regularPath, const std::string& strongPath)
{
    UnloadFonts();
    ReadFace(g_faces[0], regularPath);
    ReadFace(g_faces[1], strongPath);
    g_loaded = true;
}

void UnloadFonts()
{
    for (FaceData& f : g_faces)
    {
        for (auto& entry : f.sizes)
            if (!IsDefault(entry.second))
                UnloadFont(entry.second);
        f.sizes.clear();
    }
    if (g_glyphLoaded && !IsDefault(g_glyph))
        UnloadFont(g_glyph);
    g_glyphLoaded = false;
    g_loaded = false;
}

bool FontsLoaded()
{
    return g_loaded;
}

int FontPx(float px)
{
    const int p = (int)std::lround(px);
    return p < MIN_PX ? MIN_PX : (p > MAX_PX ? MAX_PX : p);
}

const Font& FontAt(Face face, int px)
{
    static Font fallback{};
    if (!g_loaded)
    {
        fallback = GetFontDefault();
        return fallback;
    }
    FaceData& f = g_faces[face == Face::Strong ? 1 : 0];
    px = FontPx((float)px);
    auto it = f.sizes.find(px);
    if (it == f.sizes.end())
        it = f.sizes.emplace(px, Rasterise(f, px)).first;
    return it->second;
}

Vector2 MeasureString(Face face, float px, std::string_view text)
{
    const int        p = FontPx(px);
    const Font&      font = FontAt(face, p);
    const Terminated s(text);
    return MeasureTextEx(font, s.c_str(), (float)p, IsDefault(font) ? 1.0f : 0.0f);
}

void DrawString(Face face, std::string_view text, Vector2 pos, float px, Color color)
{
    const int        p = FontPx(px);
    const Font&      font = FontAt(face, p);
    const Terminated s(text);
    // On whole pixels, so the atlas lands on the screen's grid.
    DrawTextEx(font, s.c_str(), { std::floor(pos.x + 0.5f), std::floor(pos.y + 0.5f) }, (float)p,
               IsDefault(font) ? 1.0f : 0.0f, color);
}

const Font& GlyphFont()
{
    if (!g_glyphLoaded)
    {
        g_glyphLoaded = true;
        g_glyph = Rasterise(g_faces[1], GLYPH_PX);
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
