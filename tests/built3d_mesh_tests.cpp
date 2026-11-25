#include <doctest/doctest.h>

#include "render/Built3DMesh.h"
#include <algorithm>
#include <cmath>

// A built object as solids (#618 spike): geometry only, no graphics card. The things that
// silently go wrong are a piece standing under what it lies on, a normal pointing down, and an
// index past the vertices.

namespace
{
Render::Piece Make(Render::Form form, float x, float y, float size, int z = 0)
{
    Render::Piece p;
    p.form = form;
    p.pos = { x, y };
    p.radius = size;
    p.width = size * 0.5f;
    p.length = size * 2.0f;
    p.z = z;
    return p;
}
}  // namespace

TEST_CASE("every form becomes a solid whose indices and normals are sound")
{
    using namespace Render;
    std::vector<Piece> pieces;
    for (Form f : { Form::Disc, Form::Ring, Form::Polygon, Form::Capsule, Form::Chevron, Form::Bar,
                    Form::Lattice, Form::Arc })
    {
        Piece p = Make(f, 0.0f, 0.0f, 50.0f);
        p.arcFrom = 0.0f;
        p.arcTo = 90.0f;
        pieces.push_back(p);
    }
    const Built3D::MeshData m = Built3D::BuildMesh(pieces, 100.0f, 100.0f);
    CHECK(m.pieces >= 8);
    CHECK(m.VertexCount() > 0);
    for (unsigned short i : m.index)
        CHECK((int)i < m.VertexCount());
    for (int v = 0; v < m.VertexCount(); v++)
    {
        const float* f = &m.vertex[(size_t)v * Built3D::VERTEX_FLOATS];
        CHECK(f[5] > 0.0f);  // up: seen from above, nothing faces down
        CHECK(std::fabs(f[3] * f[3] + f[4] * f[4] + f[5] * f[5] - 1.0f) < 1e-3f);
        CHECK(f[2] >= 0.0f);
        CHECK(f[2] <= m.maxHeight + 1e-4f);
    }
}

TEST_CASE("a part lying on another stands higher than it, by its own thickness")
{
    using namespace Render;
    Piece hull = Make(Form::Polygon, 0.0f, 0.0f, 60.0f);
    hull.sides = 6;
    Piece      pad = Make(Form::Disc, 10.0f, 0.0f, 8.0f);
    Piece      alone = Make(Form::Disc, 200.0f, 0.0f, 8.0f);
    const auto top = [](const std::vector<Piece>& ps)
    { return Built3D::BuildMesh(ps, 100.0f, 100.0f).maxHeight; };
    CHECK(top({ hull, pad }) > top({ hull }) + 0.5f);
    CHECK(top({ hull, pad }) > top({ alone }));
}

TEST_CASE("what a solid cannot be is left to the 2D path")
{
    using namespace Render;
    Piece glow = Make(Form::Disc, 0.0f, 0.0f, 10.0f);
    glow.soft = true;
    Piece moon = Make(Form::Disc, 0.0f, 0.0f, 10.0f);
    moon.orbits = true;
    CHECK_FALSE(Built3D::Meshable(glow));
    CHECK_FALSE(Built3D::Meshable(moon));
    CHECK(Built3D::Meshable(Make(Form::Bar, 0.0f, 0.0f, 10.0f)));
}

TEST_CASE("a thicker part is taller, and a mast is thinner and taller than plating")
{
    using namespace Render;
    CHECK(Built3D::RiseOf(Role::Hull, 0.5f) > Built3D::RiseOf(Role::Hull, 0.02f));
    CHECK(Built3D::RiseOf(Role::Antenna, 0.05f) > Built3D::RiseOf(Role::Hull, 0.05f));
    CHECK(Built3D::Corners(300.0f) > Built3D::Corners(10.0f));
}
