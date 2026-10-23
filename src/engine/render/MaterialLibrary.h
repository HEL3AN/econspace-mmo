#pragma once

#include "render/Material.h"
#include "raylib.h"
#include <map>
#include <string>
#include <vector>

namespace Render
{

// The half of a material that talks to the GPU (#121).
//
// Same split, and for the same reason, as the screen treatment (#120): a CI runner has no
// graphics card, so nothing here can be tested, and everything that decides *what* a
// material does lives in Material.h where a test can hold it.
//
// The contract is the same too. A shader that will not compile is a property of the
// player's driver. The material is dropped, the reason is said once, and the object is
// drawn the way it was drawn before materials existed. That fallback is not a courtesy:
// it is the only reason this can be turned on at all.
class MaterialLibrary
{
public:
    ~MaterialLibrary();

    // Compiles a shader for every material in the registry, from
    // `<dataDir>/shaders/materials/<shader>.fs`. Safe to call with none of them present.
    void Load(const std::string& dataDir);
    void Unload();

    bool Available() const { return !shaders_.empty(); }

    // Whether this material has a shader here, which decides whether a body is painted by
    // it or drawn as its stand-in composition (#426).
    bool Has(const std::string& id) const { return shaders_.count(id) > 0; }

    // Compile nothing: every material draws plain, exactly as on a driver that refused them
    // all. How the stand-ins are judged without a broken driver to hand (#426). Takes effect
    // at the next Load.
    void SetDisabled(bool off) { disabled_ = off; }

    // Compile only the materials an archetype wears as a whole -- `hull` -- and none of those
    // that paint a body: what a driver that refuses the painting shaders draws, which is the
    // likelier failure than refusing them all (#426). Takes effect at the next Load.
    void SetPaintingDisabled(bool off) { paintingOff_ = off; }

    // Why a material has no shader, for a settings or gallery panel to show rather than
    // bury in a log.
    const std::vector<std::string>& Problems() const { return problems_; }

    // The table noise is read from (Source::TextureNoise): 256 x 256, red a random byte and
    // green the red of the texel 37 across and 17 down -- the next layer of a 3D lattice, so
    // a 3D value noise is four fetches. From a fixed seed: every client has the same one.
    static constexpr int NOISE_SIZE = 256;
    // The unit it is bound to: past the four rlgl's batch hands out to SetShaderValueTexture
    // and resets after every batch, so it stays put however often the batch is drawn.
    static constexpr int NOISE_UNIT = 7;

    // Turns the material on for the draw calls that follow, with every binding resolved
    // from `in`. Returns false when there is no shader for it, in which case the caller
    // draws exactly as it would have with no material at all -- and must not call End.
    bool Begin(const Material& m, const MaterialInputs& in);
    void End();

    // An offscreen target of this size, kept and reused, for painting a body larger than the
    // screen can afford at full resolution (ShapeBackend::DrawOwnMaterial). Null if one cannot
    // be made, and the caller paints at full resolution instead.
    RenderTexture2D* PaintTarget(int width, int height);

    // What a painted body may cover, in pixels, before it is painted at reduced resolution:
    // the player's setting (TreatmentConfig::paintBudget), handed over by whoever owns both.
    void  SetPaintBudget(float pixels) { paintBudget_ = pixels; }
    float PaintBudget() const { return paintBudget_; }

private:
    struct Compiled
    {
        Shader                     shader;
        std::map<std::string, int> locations;  // uniform name -> location, -1 for absent
        // The same, in the order of the material's bindings: what Begin reads, every part of
        // every object, without a lookup by name (#516).
        std::vector<int> slots;
    };

    std::map<std::string, Compiled> shaders_;  // keyed by material id
    // Which material last set its constants on each program (#516): materials of one shader
    // share it, so its constants are set again only when the material changes.
    std::map<unsigned int, const Material*> constantsOn_;
    std::vector<std::string>                problems_;
    bool                                    active_ = false;
    bool                                    disabled_ = false;
    bool                                    paintingOff_ = false;
    float                                   paintBudget_ = 1000000.0f;
    Texture2D                               noise_{};
    RenderTexture2D                         paint_{};
};

}  // namespace Render
