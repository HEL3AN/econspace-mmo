#pragma once

#include "raylib.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

// A uniform grid over points on the plane, for "who is within r of here" (#350). A hot system's
// all-against-all loops (an NPC looking for its nearest target) read a few cells instead of
// every ship and pilot in it.
//
// It prunes and nothing else: Query returns every point that could be within `r` (a point within
// r is always among them), in ASCENDING index order, so a caller that then runs the very test it
// ran over everything -- same distance, same strict or loose comparison -- picks the same winner,
// ties included. No unordered containers and no hashing, so the order is the same on every
// compiler.
class SpatialGrid
{
public:
    // `cell` is the side of a square, about the largest radius that will be asked.
    void Build(const std::vector<Vector2>& points, float cell)
    {
        cell_ = cell > 1.0f ? cell : 1.0f;
        items_.clear();
        items_.reserve(points.size());
        for (size_t i = 0; i < points.size(); i++)
            items_.emplace_back(Key(CellOf(points[i].x), CellOf(points[i].y)), (int)i);
        std::sort(items_.begin(), items_.end());
    }

    // Appends to `out` (cleared first) the indices of the points near `at`, ascending.
    void Query(Vector2 at, float r, std::vector<int>& out) const
    {
        out.clear();
        // A little over r: rounding in a distance must never be able to lose a point.
        const float reach = r * 1.001f + 1.0f;
        const int   x0 = CellOf(at.x - reach), x1 = CellOf(at.x + reach);
        const int   y0 = CellOf(at.y - reach), y1 = CellOf(at.y + reach);
        for (int cx = x0; cx <= x1; cx++)
        {
            const uint64_t lo = Key(cx, y0), hi = Key(cx, y1);
            auto           it =
                std::lower_bound(items_.begin(), items_.end(), std::pair<uint64_t, int>(lo, -1));
            for (; it != items_.end() && it->first <= hi; ++it)
                out.push_back(it->second);
        }
        std::sort(out.begin(), out.end());
    }

    size_t Size() const { return items_.size(); }

private:
    static constexpr int LIMIT = 1 << 29;

    int CellOf(float v) const
    {
        float c = std::floor(v / cell_);
        if (!(c > -(float)LIMIT))  // also catches NaN: a point nothing is near anyway
            c = -(float)LIMIT;
        if (c > (float)LIMIT)
            c = (float)LIMIT;
        return (int)c;
    }
    static uint64_t Key(int cx, int cy)
    {
        return ((uint64_t)(uint32_t)(cx + LIMIT) << 32) | (uint32_t)(cy + LIMIT);
    }

    float                                 cell_ = 1.0f;
    std::vector<std::pair<uint64_t, int>> items_;  // (cell, index), sorted
};
