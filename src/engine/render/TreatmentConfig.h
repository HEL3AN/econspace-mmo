#pragma once

#include <string>
#include <vector>

namespace Render
{

// What the screen treatment does, as data (#120).
//
// Most of what makes Duskers look like Duskers is not its art -- its shapes are outlined
// boxes. It is the treatment over the top: glow, pixels, scanlines, noise, the fiction of
// old equipment. That is the part worth taking, and the part that costs the least per
// object: something nobody drew is dressed by the same pass as everything else, which is
// what a world players can build needs (#44).
//
// This half is deliberately separate from the half that talks to the GPU. There is no
// graphics card on a CI runner, so a shader is untestable by construction -- but *which*
// passes run, in what order, with what numbers, and whether the file that says so survives
// a round trip, is ordinary data and is tested like any other.

enum class PassKind
{
    Bloom,      // bright parts bleed into their surroundings
    Pixelate,   // the image is resolved on a coarser grid
    Scanlines,  // horizontal lines, as a phosphor display has
    Noise,      // animated grain
    Fringe,     // colour separation and a little barrel distortion
    Vignette    // the corners fall off
};

const char* PassName(PassKind k);
bool        PassFromName(const std::string& s, PassKind& out);

// All of them in declaration order, for offering a list without writing one down.
const std::vector<PassKind>& AllPasses();

// One pass in the chain.
//
// Two knobs, named the same for every pass, rather than a struct per effect or a bag of
// name-to-number. `amount` is how much of the effect: zero is off, one is the full effect
// the shader was written for. `scale` is the one other thing each effect has -- how far
// the glow reaches, how large a pixel is, how far apart the lines sit -- and what it means
// is documented per pass rather than guessed.
struct Pass
{
    PassKind kind = PassKind::Bloom;
    bool     enabled = true;
    float    amount = 1.0f;
    float    scale = 1.0f;
};

// What `scale` means for this pass, in words, for a settings screen to show.
const char* ScaleMeaning(PassKind k);

struct TreatmentConfig
{
    // Off entirely. The direction says a player must be able to remove this, not merely
    // turn each piece down.
    bool enabled = true;

    // Whether the HUD goes through the chain with the world. Off by default, and that is
    // a game decision rather than a technical one: Duskers can pixelate everything because
    // everything there *is* the terminal, but here the HUD carries numbers people fly by,
    // and a pixelated fuel gauge is a worse game.
    bool treatHud = false;

    // In order. The order is a decision, not a detail: bloom before pixelation gives soft
    // fat pixels, after it gives hard pixel edges that glow. They are different games to
    // look at, so this is data and reorderable rather than a sequence of calls.
    std::vector<Pass> chain;

    // The chain as shipped, used when there is no file and when a player asks for the
    // defaults back.
    static TreatmentConfig Default();

    void MoveUp(size_t index);
    void MoveDown(size_t index);
};

// Reads data/look.json. Returns false and leaves `out` untouched when the file is missing
// or unreadable; `error` then says which. A malformed *entry* is skipped rather than
// failing the file: a look setting is not a save, and losing the whole treatment because
// one pass name was mistyped is worse than losing that pass.
bool LoadTreatment(const std::string& path, TreatmentConfig& out, std::string& error);

// Writes it back, in the shape LoadTreatment reads.
bool SaveTreatment(const std::string& path, const TreatmentConfig& cfg, std::string& error);

}  // namespace Render
