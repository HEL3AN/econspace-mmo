#pragma once

#include "render/Scene.h"
#include <string>
#include <vector>

namespace Render
{

// The world as lines of text, one per object.
//
// This backend calls no raylib drawing function at all — it is the one an agent and a
// headless test can use, and the reason the seam exists in the first place. It also
// gives the glyph work a check that runs in CI: whether a scene came out right stops
// being something only a human looking at a window can tell.
class TextBackend : public IBackend
{
public:
    // `origin` is the point distances are measured from — the player's ship, normally.
    explicit TextBackend(Vector2 origin = { 0.0f, 0.0f }) : origin_(origin) {}

    const char* Name() const override { return "text"; }

    void Begin() override;
    void Draw(const Item& item) override;

    const std::vector<std::string>& Lines() const { return lines_; }

    // The whole scene as one newline-separated block.
    std::string Text() const;

private:
    Vector2                  origin_;
    std::vector<std::string> lines_;
};

// The world as a fixed-size character grid, the way a glyph renderer lays it out but
// without a window. Used by tests to assert that an object landed where it should.
class GridBackend : public IBackend
{
public:
    // `span` is how many world units the grid covers across its width.
    GridBackend(int width, int height, float span, Vector2 center = { 0.0f, 0.0f });

    const char* Name() const override { return "grid"; }

    void Begin() override;
    void Draw(const Item& item) override;

    // Row `y`, left to right. Empty cells are spaces.
    std::string Row(int y) const;
    std::string Text() const;

    // What ended up at this cell, or ' ' if nothing did.
    char At(int x, int y) const;

private:
    int               width_;
    int               height_;
    float             span_;
    Vector2           center_;
    std::vector<char> cells_;
};

}  // namespace Render
