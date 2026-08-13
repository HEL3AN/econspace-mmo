#include "render/Textures.h"

#include <cstring>
#include <string>
#include <unordered_map>

namespace
{
// Cache of loaded textures keyed by name. id == 0 means "no such file" —
// so we don't try to load a missing file every frame.
std::unordered_map<std::string, Texture2D> cache;

// Layout of the shared atlas data/textures/atlas.png — a strict 4x2 grid.
constexpr int ATLAS_COLS = 4;
constexpr int ATLAS_ROWS = 2;

// Which object lives in which atlas cell (column, row).
struct AtlasCell
{
    const char* name;
    int         col;
    int         row;
};
constexpr AtlasCell ATLAS[] = {
    { "star", 0, 0 },      { "planet", 1, 0 }, { "station", 2, 0 },
    { "asteroids", 0, 1 }, { "ship", 1, 1 },
};

// Loads the atlas and makes the dark-blue background transparent. Generators
// barely handle alpha, so we do the "chroma key" ourselves: bluish dark pixels
// (the background and thin divider lines) are turned transparent.
// Gray hulls and bright star bodies are left alone — their b channel isn't dominant.
// If the image already has transparency (background removed by hand), the chroma
// key is skipped so we don't eat into object edges.
Texture2D LoadAtlasKeyed(const char* path)
{
    Image img = LoadImage(path);
    ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);

    Color* px = (Color*)img.data;
    int    n = img.width * img.height;

    bool hasAlpha = false;
    for (int i = 0; i < n && !hasAlpha; i++)
        if (px[i].a < 255)
            hasAlpha = true;

    if (!hasAlpha)
    {
        for (int i = 0; i < n; i++)
        {
            Color c = px[i];
            if (c.b > c.r + 12 && c.b > c.g + 6 && c.b < 120)
                px[i].a = 0;
        }
    }

    Texture2D tex = LoadTextureFromImage(img);
    UnloadImage(img);
    SetTextureFilter(tex, TEXTURE_FILTER_POINT);
    return tex;
}

// Loads texture name (data/textures/<name>.png) with caching.
// A missing file is cached as an empty texture (id == 0).
const Texture2D& Acquire(const char* name)
{
    auto it = cache.find(name);
    if (it != cache.end())
        return it->second;

    std::string path = std::string(GetApplicationDirectory()) + "data/textures/" + name + ".png";

    Texture2D tex{ 0, 0, 0, 0, 0 };
    if (FileExists(path.c_str()))
    {
        if (std::strcmp(name, "atlas") == 0)
        {
            tex = LoadAtlasKeyed(path.c_str());
        }
        else
        {
            tex = LoadTexture(path.c_str());
            SetTextureFilter(tex, TEXTURE_FILTER_POINT);  // pixel art — no smoothing
        }
    }
    return cache.emplace(name, tex).first->second;
}
}  // namespace

namespace Tex
{
void Unload()
{
    for (auto& kv : cache)
        if (kv.second.id != 0)
            UnloadTexture(kv.second);
    cache.clear();
}

bool DrawSprite(const char* name, Vector2 pos, float size, float rotationDeg, Color tint)
{
    float     d = size * 2.0f;
    Vector2   origin{ d / 2.0f, d / 2.0f };
    Rectangle dst{ pos.x, pos.y, d, d };

    // 1. A standalone file <name>.png takes priority over the atlas.
    const Texture2D& indiv = Acquire(name);
    if (indiv.id != 0)
    {
        Rectangle src{ 0.0f, 0.0f, (float)indiv.width, (float)indiv.height };
        DrawTexturePro(indiv, src, dst, origin, rotationDeg, tint);
        return true;
    }

    // 2. Shared atlas atlas.png — find the cell by name and slice it out of the grid.
    for (const AtlasCell& cell : ATLAS)
    {
        if (std::strcmp(cell.name, name) != 0)
            continue;

        const Texture2D& atlas = Acquire("atlas");
        if (atlas.id == 0)
            return false;  // no atlas either — fall back to a shape

        // A small inset into the cell — in case of leftover divider lines.
        float     cw = (float)atlas.width / ATLAS_COLS;
        float     ch = (float)atlas.height / ATLAS_ROWS;
        float     inset = cw * 0.04f;
        Rectangle src{ cell.col * cw + inset, cell.row * ch + inset, cw - 2.0f * inset,
                       ch - 2.0f * inset };
        DrawTexturePro(atlas, src, dst, origin, rotationDeg, tint);
        return true;
    }
    return false;
}
}  // namespace Tex
