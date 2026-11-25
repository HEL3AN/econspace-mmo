#pragma once

#include "render/Silhouette.h"
#include <vector>

namespace Render
{
namespace Built3D
{

// A built object as a solid (spike for #618).
//
// The composition is the same data the 2D path draws (#122): `Compose` places the pieces, and
// this turns each into a bevelled slab seen from straight above -- only its top surface, since
// a wall is edge-on in a top-down view and the heightfield the lighting reads has it anyway.
// Nothing here knows an archetype and nothing is written by hand: a piece's height comes from
// its role, its size and what it lies on (a rule, see `Heights`), so a design a player writes
// is built the same way.
//
// No GPU here: this is geometry, and tested.

// One vertex, interleaved as the shader reads it.
//   position  x, y, height (object-local world units, before spin and heading)
//   normal    x, y, z
//   uv        the part's own frame, in the object's radii: where the panels are laid out
//   uv2       the part's seed, its solidity
//   tangent   spin (degrees a second), blink period, blink phase, role code
//   tint      the part's own colour, alpha 0 for the object's
//   edge      how far inside the part's outline, in world units: what anti-aliases it
constexpr int VERTEX_FLOATS = 19;

// What `tangent.w` holds: the role, plus a flag for night-only parts.
constexpr float ROLE_NIGHT_ONLY = 16.0f;

struct MeshData
{
    std::vector<float>          vertex;
    std::vector<unsigned short> index;
    float                       maxHeight = 0.0f;  // above the ground, in world units
    float                       reach = 0.0f;      // furthest from the centre, in world units
    int                         pieces = 0;        // pieces turned into solids
    // Whether anything in it moves by itself (a spin, a blink): a mesh with nothing that does
    // draws the same picture every frame, and a height map made for it can be kept.
    bool animated = false;

    int VertexCount() const { return (int)(vertex.size() / VERTEX_FLOATS); }
};

// Whether a piece becomes a solid. What is not -- glows, marks on a sphere, orbiting moons --
// is drawn by the 2D path over the solid, as it always was.
bool Meshable(const Piece& p);

// The solids of the pieces `Compose` returned for an object of radius `objectSize` at the
// origin, heading 0 and time 0 (the GPU turns it). `pixelRadius` is how big it is on screen:
// the number of corners and bevel steps follow it, and a cache keyed on it rebuilds when it
// changes by enough to matter.
//
// `chase` gives the blinking pieces a phase from where they lie along the object's +x axis
// instead of from their own seeds: a gate's approach lights run toward its aperture.
MeshData BuildMesh(const std::vector<Piece>& pieces, float objectSize, float pixelRadius,
                   bool chase = false);

// How round to make something `pixelRadius` pixels in radius, in corners.
int Corners(float pixelRadius);

// The rule that stands for a hand-made model: how far a piece rises above what it lies on,
// from its size and role, in the object's radii. Exposed so a test can pin it.
float RiseOf(Role role, float shadeRadiusInRadii);

// How tall the object is allowed to get, in radii: the shadow's length and the height map's
// range follow it.
constexpr float MAX_HEIGHT_IN_RADII = 0.36f;

}  // namespace Built3D
}  // namespace Render
