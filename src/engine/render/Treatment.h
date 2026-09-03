#pragma once

#include "render/TreatmentConfig.h"
#include "raylib.h"
#include <string>
#include <vector>

namespace Render
{

// The screen treatment: the world is drawn into a texture and then put through an ordered
// chain of full-screen passes before it reaches the window (#120).
//
// Everything about *what* the chain is lives in TreatmentConfig, which is data and is
// tested. This class is the part that talks to the GPU, and it is the part no test can
// reach: there is no graphics card on a CI runner.
//
// Which is why the contract here is that **it always works**. A shader that will not
// compile is a runtime, driver-specific fact, not a bug you can catch by building. When
// one fails, this says so plainly once, drops that pass, and keeps playing. If none of
// them load, Begin/End degrade to drawing straight to the screen and the game looks the
// way it did before this existed. Rendering nothing is the failure this is written to
// avoid.
class Treatment
{
public:
    ~Treatment();

    // Loads the shaders from `<dataDir>/shaders/` and the chain from
    // `<dataDir>/look.json`. Safe to call with neither present: the chain falls back to
    // the shipped default and any pass whose shader is missing is dropped.
    void Load(const std::string& dataDir);
    void Unload();

    // Whether any pass at all survived loading. False on a machine whose driver refused
    // every shader, and false in any process that never called Load -- the server and the
    // tests among them.
    bool Available() const { return !shaders_.empty(); }

    TreatmentConfig&       Config() { return config_; }
    const TreatmentConfig& Config() const { return config_; }

    // Whether this pass has a working shader behind it. A settings screen shows the pass
    // either way and says which ones the machine could not give it.
    bool Compiled(PassKind k) const;

    // Why loading a pass failed, for the settings screen to show rather than bury in a
    // log the player will not read.
    const std::vector<std::string>& Problems() const { return problems_; }

    // Everything drawn between these goes through the chain. Nesting them is not
    // supported; End without Begin does nothing.
    void Begin(int width, int height);
    void End();

    // Writes the chain back to `<dataDir>/look.json`. Returns false and fills `error`.
    bool Save(std::string& error) const;

private:
    struct Loaded
    {
        PassKind kind;
        Shader   shader;
        int      locAmount = -1;
        int      locScale = -1;
        int      locResolution = -1;
        int      locTime = -1;
        int      locScene = -1;  // bloom's second sampler: the untouched scene
    };

    const Loaded* Find(PassKind k) const;
    void          EnsureTargets(int width, int height);

    TreatmentConfig          config_ = TreatmentConfig::Default();
    std::string              dataDir_;
    std::vector<Loaded>      shaders_;
    std::vector<std::string> problems_;

    RenderTexture2D scene_{};  // what the world was drawn into
    RenderTexture2D ping_{};
    RenderTexture2D pong_{};
    bool            targetsReady_ = false;
    int             width_ = 0, height_ = 0;
    bool            capturing_ = false;
};

}  // namespace Render
