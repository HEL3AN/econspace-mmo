#include "render/Built3D.h"

#include "render/Built3DDetail.h"
#include "render/GlState.h"
#include "render/Scene.h"
#include "render/Silhouette.h"
#include "rlgl.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace Render
{
namespace Built3D
{

namespace
{

std::string ReadText(const std::string& path)
{
    if (!FileExists(path.c_str()))
        return {};
    char* t = LoadFileText(path.c_str());
    if (t == nullptr)
        return {};
    std::string s(t);
    UnloadFileText(t);
    return s;
}

// Compiles and links one program from source; 0 and a reason on failure.
unsigned int Program(const std::string& vs, const std::string& fs, std::string& why)
{
    if (vs.empty() || fs.empty())
    {
        why = "shader source missing";
        return 0;
    }
    const unsigned int v = rlCompileShader(vs.c_str(), RL_VERTEX_SHADER);
    const unsigned int f = rlCompileShader(fs.c_str(), RL_FRAGMENT_SHADER);
    if (v == 0 || f == 0)
    {
        why = v == 0 ? "vertex shader would not compile" : "fragment shader would not compile";
        return 0;
    }
    const unsigned int p = rlLoadShaderProgram(v, f);
    if (p == 0 || p == rlGetShaderIdDefault())
    {
        why = "program would not link";
        return 0;
    }
    return p;
}

void Set1(int loc, float v)
{
    if (loc >= 0)
        rlSetUniform(loc, &v, RL_SHADER_UNIFORM_FLOAT, 1);
}
void Set2(int loc, float a, float b)
{
    const float v[2] = { a, b };
    if (loc >= 0)
        rlSetUniform(loc, v, RL_SHADER_UNIFORM_VEC2, 1);
}
void Set3(int loc, float a, float b, float c)
{
    const float v[3] = { a, b, c };
    if (loc >= 0)
        rlSetUniform(loc, v, RL_SHADER_UNIFORM_VEC3, 1);
}
void Set4(int loc, float a, float b, float c, float d)
{
    const float v[4] = { a, b, c, d };
    if (loc >= 0)
        rlSetUniform(loc, v, RL_SHADER_UNIFORM_VEC4, 1);
}
void SetI(int loc, int v)
{
    if (loc >= 0)
        rlSetUniform(loc, &v, RL_SHADER_UNIFORM_INT, 1);
}

}  // namespace

Renderer::~Renderer()
{
    Unload();
}

bool Renderer::Load(const std::string& dataDir)
{
    Unload();
    const std::string dir = dataDir + "shaders/built3d/";
    const std::string vs = ReadText(dir + "built3d.vs");
    sceneShader_ = Program(vs, ReadText(dir + "lit.fs"), problem_);
    if (sceneShader_ == 0)
    {
        problem_ = "built3d lit: " + problem_;
        return false;
    }
    heightShader_ = Program(vs, ReadText(dir + "height.fs"), problem_);
    if (heightShader_ == 0)
    {
        problem_ = "built3d height: " + problem_;
        Unload();
        return false;
    }

    auto locate = [](unsigned int p, Locs& l)
    {
        auto u = [&](const char* n) { return rlGetLocationUniform(p, n); };
        l.mode = u("uMode");
        l.heading = u("uHeading");
        l.clock = u("uClock");
        l.maxH = u("uMaxH");
        l.obj = u("uObj");
        l.cam = u("uCam");
        l.viewport = u("uViewport");
        l.hExtent = u("uHExtent");
        l.hFill = u("uHFill");
        l.base = u("uBase");
        l.accent = u("uAccent");
        l.livery = u("uLivery");
        l.style = u("uStyle");
        l.lightDir = u("uLightDir");
        l.lightSurface = u("uLightSurface");
        l.lightTint = u("uLightTint");
        l.ambient = u("uAmbient");
        l.hasLight = u("uHasLight");
        l.pixels = u("uPixelsPerUnit");
        l.radius = u("uRadius");
        l.damage = u("uDamage");
        l.quality = u("uQuality");
        l.height = u("uHeight");
        l.dome = u("uDome");
        l.reach = u("uReach");
        l.tilt = u("uTilt");
        l.view = u("uView");
        l.exposure = u("uExposure");
    };
    locate(sceneShader_, sl_);
    locate(heightShader_, hl_);

    // A float target for the heights. Not every driver renders into one; then there is no
    // shadow and the 2D path draws instead (#120).
    fbo_ = rlLoadFramebuffer();
    heightTex_ = rlLoadTexture(nullptr, MAP_SIZE, MAP_SIZE, RL_PIXELFORMAT_UNCOMPRESSED_R32, 1);
    depthRb_ = rlLoadTextureDepth(MAP_SIZE, MAP_SIZE, true);
    if (fbo_ == 0 || heightTex_ == 0 || depthRb_ == 0)
    {
        problem_ = "built3d: no float render target";
        Unload();
        return false;
    }
    rlTextureParameters(heightTex_, RL_TEXTURE_MIN_FILTER, RL_TEXTURE_FILTER_LINEAR);
    rlTextureParameters(heightTex_, RL_TEXTURE_MAG_FILTER, RL_TEXTURE_FILTER_LINEAR);
    rlTextureParameters(heightTex_, RL_TEXTURE_WRAP_S, RL_TEXTURE_WRAP_CLAMP);
    rlTextureParameters(heightTex_, RL_TEXTURE_WRAP_T, RL_TEXTURE_WRAP_CLAMP);
    rlFramebufferAttach(fbo_, heightTex_, RL_ATTACHMENT_COLOR_CHANNEL0, RL_ATTACHMENT_TEXTURE2D, 0);
    rlFramebufferAttach(fbo_, depthRb_, RL_ATTACHMENT_DEPTH, RL_ATTACHMENT_RENDERBUFFER, 0);
    if (!rlFramebufferComplete(fbo_))
    {
        problem_ = "built3d: height target incomplete";
        Unload();
        return false;
    }
    // rlFramebufferComplete leaves the target bound.
    rlDisableFramebuffer();
    available_ = true;
    problem_.clear();
    return true;
}

void Renderer::Unload()
{
    for (auto& [k, m] : meshes_)
        Free(m);
    meshes_.clear();
    if (sceneShader_ != 0)
        rlUnloadShaderProgram(sceneShader_);
    if (heightShader_ != 0)
        rlUnloadShaderProgram(heightShader_);
    sceneShader_ = heightShader_ = 0;
    if (fbo_ != 0)
        rlUnloadFramebuffer(fbo_);
    fbo_ = 0;
    heightTex_ = depthRb_ = 0;
    available_ = false;
}

void Renderer::Free(Mesh& m)
{
    if (m.vao != 0)
        rlUnloadVertexArray(m.vao);
    if (m.vbo != 0)
        rlUnloadVertexBuffer(m.vbo);
    if (m.ebo != 0)
        rlUnloadVertexBuffer(m.ebo);
    m = Mesh{};
}

void Renderer::BeginFrame()
{
    frame_++;
    solids_ = 0;
    vertices_ = 0;
    if (frame_ % 300 != 0)
        return;
    for (auto it = meshes_.begin(); it != meshes_.end();)
    {
        if (frame_ - it->second.used > 600)
        {
            Free(it->second);
            it = meshes_.erase(it);
        }
        else
            ++it;
    }
}

Renderer::Mesh* Renderer::MeshFor(const Item& item, float zoom)
{
    // A new mesh when the object's size on screen has changed enough for another number of
    // corners, or when it is a different object.
    const int bucket = (int)std::floor(std::log2(std::max(1.0f, item.size * zoom)) * 3.0f);
    const Key key{ item.id, (const void*)item.shape, (int)item.size, bucket, item.thrusting };
    auto      it = meshes_.find(key);
    if (it != meshes_.end())
    {
        it->second.used = frame_;
        return &it->second;
    }

    Pose pose;
    pose.pos = { 0.0f, 0.0f };
    pose.size = item.size;
    pose.heading = 0.0f;
    pose.seed = item.id;
    pose.pixelsPerUnit = zoom;
    pose.time = 0.0;
    pose.thrusting = item.thrusting;
    const std::vector<Piece> composed = Compose(*item.shape, pose);
    const std::vector<Piece> pieces =
        Embellish(composed, item.size, item.id,
                  detail_ && item.kind != EntityKind::Gate ? DetailLevel(item.size * zoom) : 0);
    const MeshData data =
        BuildMesh(pieces, item.size, item.size * zoom, item.kind == EntityKind::Gate);
    if (data.pieces == 0 || data.index.empty())
        return nullptr;

    Mesh m;
    m.vao = rlLoadVertexArray();
    rlEnableVertexArray(m.vao);
    m.vbo =
        rlLoadVertexBuffer(data.vertex.data(), (int)(data.vertex.size() * sizeof(float)), false);
    const int stride = VERTEX_FLOATS * (int)sizeof(float);
    // position 3, normal 3, uv 2, seed+solidity 2, motion 4, tint 4, edge 1
    const int comps[7] = { 3, 3, 2, 2, 4, 4, 1 };
    int       offset = 0;
    for (int a = 0; a < 7; a++)
    {
        rlSetVertexAttribute((unsigned int)a, comps[a], RL_FLOAT, false, stride, offset);
        rlEnableVertexAttribute((unsigned int)a);
        offset += comps[a] * (int)sizeof(float);
    }
    m.ebo = rlLoadVertexBufferElement(data.index.data(),
                                      (int)(data.index.size() * sizeof(unsigned short)), false);
    rlDisableVertexArray();
    m.indices = (int)data.index.size();
    m.vertices = data.VertexCount();
    m.maxHeight = data.maxHeight;
    m.reach = data.reach;
    m.animated = data.animated;
    m.used = frame_;
    return &meshes_.emplace(key, m).first->second;
}

bool Renderer::Draw(const Item& item, const Frame& f)
{
    if (!available_ || item.shape == nullptr || item.shape->Empty())
        return false;
    Mesh* m = MeshFor(item, f.view.zoom);
    if (m == nullptr)
        return false;
    const float extent = std::max(m->reach * 1.04f, item.size * 0.1f);
    const float maxH = std::max(m->maxHeight * 1.02f, 0.001f);

    // Everything drawn so far reaches the target it was meant for before this one changes.
    rlDrawRenderBatchActive();

    int vp[4];
    GlState::Viewport(vp);
    const unsigned int     previous = rlGetActiveFramebuffer();
    const GlState::Scissor scissor = GlState::Current();
    const float            W = (float)vp[2], Hh = (float)vp[3];
    const double           whole = std::floor(f.clock);
    const float            clockWhole = (float)std::fmod(whole, 7200.0);
    const float            clockFrac = (float)(f.clock - whole);

    // The star, as a direction above the plane.
    const float el = f.elevationDeg * DEG2RAD;
    Vector3     L = { 0.0f, 0.0f, 1.0f };
    if (f.hasLight)
        L = { f.light.dir.x * std::cos(el), f.light.dir.y * std::cos(el), std::sin(el) };

    int   steps = 0;
    float ao = 0.0f;
    switch (quality_)
    {
        case Quality::Flat: break;
        case Quality::Shadows: steps = 12; break;
        case Quality::Full:
            steps = 20;
            ao = 1.0f;
            break;
        case Quality::High:
            steps = 32;
            ao = 1.0f;
            break;
    }
    if (!f.hasLight && ao == 0.0f)
        steps = 0;

    // --- Pass 1: the heights -----------------------------------------------------------------
    int   mapPixels = MAP_SIZE;
    float fill = 1.0f;
    if (steps > 0)
    {
        mapPixels = std::clamp((int)std::ceil(2.0f * extent * f.view.zoom * 0.75f), 96, MAP_SIZE);
        fill = (float)mapPixels / (float)MAP_SIZE;
        rlDisableScissorTest();
        rlEnableFramebuffer(fbo_);
        rlViewport(0, 0, mapPixels, mapPixels);
        rlClearColor(0, 0, 0, 0);
        rlClearScreenBuffers();
        rlEnableDepthTest();
        rlDisableBackfaceCulling();
        rlDisableColorBlend();
        rlEnableShader(heightShader_);
        SetI(hl_.mode, 1);
        Set1(hl_.heading, item.heading);
        Set2(hl_.clock, clockWhole, clockFrac);
        Set1(hl_.maxH, maxH);
        Set1(hl_.hExtent, extent);
        Set1(hl_.hFill, fill);
        Set1(hl_.reach, m->reach);
        Set3(hl_.tilt, 0.0f, 1.0f, 0.0f);
        rlEnableVertexArray(m->vao);
        rlDrawVertexArrayElements(0, m->indices, nullptr);
        rlDisableVertexArray();
        rlDisableShader();
        rlEnableColorBlend();
        rlDisableDepthTest();
        if (previous != 0)
            rlEnableFramebuffer(previous);
        else
            rlDisableFramebuffer();
        rlViewport(vp[0], vp[1], vp[2], vp[3]);
        GlState::Restore(scissor);
    }

    // --- Pass 2: the solid, into the frame ---------------------------------------------------
    // Depth is cleared over the object's box only: what an earlier solid left there is not in
    // front of this one.
    const Vector2 c = GetWorldToScreen2D(item.pos, f.view);
    const float   rpx = extent * f.view.zoom + 2.0f;
    const int     bx = (int)std::max(0.0f, std::floor(c.x - rpx));
    // Lifted by the lean: the box grows upward by the tallest thing's shift.
    const float lift = std::tan(f.tiltDeg * DEG2RAD) * maxH * f.view.zoom;
    const int   by = (int)std::max(0.0f, std::floor(Hh - c.y - rpx));
    const int   bw = (int)std::min(W - (float)bx, std::ceil(2.0f * rpx));
    const int   bh = (int)std::min(Hh - (float)by, std::ceil(2.0f * rpx + lift));
    if (bw > 0 && bh > 0)
        GlState::ClearDepth(bx, by, bw, bh);

    rlEnableDepthTest();
    rlDisableBackfaceCulling();
    rlEnableShader(sceneShader_);
    SetI(sl_.mode, 0);
    Set1(sl_.heading, item.heading);
    Set2(sl_.clock, clockWhole, clockFrac);
    Set1(sl_.maxH, maxH);
    // The object's place relative to the camera's target, in doubles until it is small.
    Set2(sl_.obj, (float)((double)item.pos.x - (double)f.view.target.x),
         (float)((double)item.pos.y - (double)f.view.target.y));
    Set4(sl_.cam, f.view.zoom, f.view.rotation * DEG2RAD, f.view.offset.x, f.view.offset.y);
    Set2(sl_.viewport, W, Hh);
    Set1(sl_.hExtent, extent);
    Set1(sl_.hFill, fill);
    // The lean: the footprint stays, heights go up the screen. The view vector is the screen's
    // down in the object's frame, tipped toward the viewer.
    const float tilt = f.tiltDeg * DEG2RAD;
    const float rot = f.view.rotation * DEG2RAD;
    Set1(sl_.reach, m->reach);
    Set3(sl_.tilt, std::tan(tilt), std::cos(tilt), std::sin(tilt));
    Set3(sl_.view, std::sin(tilt) * std::sin(rot), std::sin(tilt) * std::cos(rot), std::cos(tilt));
    Set1(sl_.exposure, f.exposure);
    Set4(sl_.base, f.base.r / 255.0f, f.base.g / 255.0f, f.base.b / 255.0f, 1.0f);
    Set4(sl_.accent, f.accent.r / 255.0f, f.accent.g / 255.0f, f.accent.b / 255.0f, 1.0f);
    Set4(sl_.livery, f.livery[0], f.livery[1], f.livery[2], f.livery[3]);
    Set4(sl_.style, f.style[0], f.style[1], f.style[2], f.style[3]);
    Set3(sl_.lightDir, L.x, L.y, L.z);
    Set4(sl_.lightSurface, f.light.surface.r / 255.0f, f.light.surface.g / 255.0f,
         f.light.surface.b / 255.0f, f.light.strength);
    Set3(sl_.lightTint, f.light.tint.r / 255.0f, f.light.tint.g / 255.0f, f.light.tint.b / 255.0f);
    Set1(sl_.ambient, f.ambient * 0.62f);
    Set1(sl_.hasLight, f.hasLight ? 1.0f : 0.0f);
    Set1(sl_.pixels, f.view.zoom);
    Set1(sl_.radius, item.size);
    Set1(sl_.damage, item.intensity);
    Set2(sl_.quality, (float)steps, ao);
    Set1(sl_.dome, DOME);
    // The height map on its own unit, then back to 0 for whoever draws next.
    rlActiveTextureSlot(1);
    rlEnableTexture(heightTex_);
    SetI(sl_.height, 1);
    rlActiveTextureSlot(0);
    rlEnableVertexArray(m->vao);
    rlDrawVertexArrayElements(0, m->indices, nullptr);
    rlDisableVertexArray();
    rlActiveTextureSlot(1);
    rlDisableTexture();
    rlActiveTextureSlot(0);
    rlDisableShader();
    rlDisableDepthTest();
    rlEnableBackfaceCulling();

    solids_++;
    vertices_ += m->vertices;
    return true;
}

}  // namespace Built3D
}  // namespace Render
