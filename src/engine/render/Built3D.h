#pragma once

#include "core/Archetype.h"
#include "render/Built3DMesh.h"
#include "render/Lighting.h"
#include "raylib.h"
#include <map>
#include <string>
#include <tuple>

namespace Render
{

struct Item;

namespace Built3D
{

// Built objects as solids, seen from above (spike for #618): the GPU half. The geometry is
// Built3DMesh.h and is tested; this compiles the shaders, keeps a mesh per object, and draws
// each object in two passes -- the heights into a small map, then the solid into the frame
// through the game's own 2D camera, shading itself from that map. Same world positions, same
// Item, same camera; only how the object is drawn changes.
//
// Like the screen treatment (#120) and the materials (#121) it is a property of the driver:
// if the shaders will not compile or a float target cannot be made, `Available` is false and
// the caller draws the 2D path it always had.

// How much shadow and occlusion to pay for. The height map is skipped at 0.
enum class Quality
{
    Flat = 0,     // lit solids, no shadows, no occlusion (no height pass at all)
    Shadows = 1,  // 12 steps toward the star, no occlusion
    Full = 2,     // 20 steps, occlusion
    High = 3      // 32 steps, occlusion
};

// The angle of the star above the plane, degrees. A fact of the picture and not of the world
// (the star is far away and flat in a 2D system): it decides how long a shadow is for a given
// height, and so how tall the parts are allowed to read.
constexpr float LIGHT_ELEVATION_DEG = 38.0f;

// How much the whole object is domed in the lighting, so it has a lit side and a dark one:
// the parts' normals tilt outward by this times how far from the centre they are, in radii.
constexpr float DOME = 0.55f;

// The height map's largest side, in texels.
constexpr int MAP_SIZE = 512;

struct Frame
{
    Camera2D         view{};
    double           clock = 0.0;
    Lighting::Sample light;
    bool             hasLight = false;
    float            ambient = 0.38f;
    Color            base = WHITE;
    Color            accent = { 200, 160, 60, 255 };
    float            livery[4] = { 0.3f, 0.3f, 1.0f, 1.0f };
    float            style[4] = { 2.0f, 0.1f, 0.5f, 0.3f };
};

class Renderer
{
public:
    ~Renderer();

    // Compiles the shaders and makes the height target. False, and `Problem` says why, if this
    // driver cannot.
    bool               Load(const std::string& dataDir);
    void               Unload();
    bool               Available() const { return available_; }
    const std::string& Problem() const { return problem_; }

    void    SetQuality(Quality q) { quality_ = q; }
    Quality GetQuality() const { return quality_; }

    // Once a frame, so meshes nobody drew for a while can go.
    void BeginFrame();

    // Draws the object as a solid into the current target, and returns false, having drawn
    // nothing, if it cannot.
    bool Draw(const Item& item, const Frame& f);

    // What the last frame cost to prepare, for a report: solids drawn and vertices.
    int Solids() const { return solids_; }
    int Vertices() const { return vertices_; }

private:
    struct Mesh
    {
        unsigned int vao = 0, vbo = 0, ebo = 0;
        int          indices = 0, vertices = 0;
        float        maxHeight = 0.0f, reach = 0.0f;
        bool         animated = false;
        long         used = 0;
    };
    using Key = std::tuple<int, const void*, int, int, bool>;
    Mesh* MeshFor(const Item& item, float zoom);
    void  Free(Mesh& m);

    bool        available_ = false;
    std::string problem_;
    Quality     quality_ = Quality::Full;

    unsigned int sceneShader_ = 0, heightShader_ = 0;
    // Uniform locations, per shader.
    struct Locs
    {
        int mode = -1, heading = -1, clock = -1, maxH = -1, obj = -1, cam = -1, viewport = -1,
            hExtent = -1, hFill = -1;
        int base = -1, accent = -1, livery = -1, style = -1, lightDir = -1, lightSurface = -1,
            lightTint = -1, ambient = -1, hasLight = -1, pixels = -1, radius = -1, damage = -1,
            quality = -1, height = -1, dome = -1;
    } sl_, hl_;

    unsigned int fbo_ = 0, heightTex_ = 0, depthRb_ = 0;

    std::map<Key, Mesh> meshes_;
    long                frame_ = 0;
    int                 solids_ = 0, vertices_ = 0;
};

}  // namespace Built3D
}  // namespace Render
