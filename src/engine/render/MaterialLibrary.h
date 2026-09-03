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

    // Why a material has no shader, for a settings or gallery panel to show rather than
    // bury in a log.
    const std::vector<std::string>& Problems() const { return problems_; }

    // Turns the material on for the draw calls that follow, with every binding resolved
    // from `in`. Returns false when there is no shader for it, in which case the caller
    // draws exactly as it would have with no material at all -- and must not call End.
    bool Begin(const Material& m, const MaterialInputs& in);
    void End();

private:
    struct Compiled
    {
        Shader                     shader;
        std::map<std::string, int> locations;  // uniform name -> location, -1 for absent
    };

    std::map<std::string, Compiled> shaders_;  // keyed by material id
    std::vector<std::string>        problems_;
    bool                            active_ = false;
};

}  // namespace Render
