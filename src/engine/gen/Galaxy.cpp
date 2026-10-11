#include "gen/Galaxy.h"
#include "gen/Rng.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace Gen
{
namespace
{
// Purposes, apart from every key a region uses (those start from a slot's own key).
constexpr uint64_t SKELETON_PLACE = 0x5EE1E700ull;
constexpr uint64_t SKELETON_LINK = 0x5EE1E701ull;

constexpr double SQRT3 = 1.7320508075688772;
// Neighbouring slots are SPACING * sqrt(3) apart, about 2 400 map units: a region of twenty
// systems is some 1 500 long and 700 wide, so two regions side by side do not overlap.
constexpr double SPACING = 1400.0;
constexpr int    JITTER = 260;      // so the lattice does not read as a lattice
constexpr int    LINK_CHANCE = 65;  // a link between neighbours that is not an inward one

// Axial directions, in the order a ring is walked.
constexpr int DIRS[6][2] = { { 1, 0 }, { 1, -1 }, { 0, -1 }, { -1, 0 }, { -1, 1 }, { 0, 1 } };

int RingOf(int q, int r)
{
    return (std::abs(q) + std::abs(r) + std::abs(q + r)) / 2;
}

// Slot `slot`'s lattice cell: slot 0 at the middle, then ring by ring, each ring walked
// from the same corner in the same direction.
void CellOf(int slot, int& q, int& r, int& ring)
{
    q = r = ring = 0;
    if (slot <= 0)
        return;
    int k = 1;
    while (1 + 3 * k * (k + 1) <= slot)
        k++;
    const int offset = slot - (1 + 3 * k * (k - 1));
    const int side = offset / k, step = offset % k;
    q = DIRS[4][0] * k;
    r = DIRS[4][1] * k;
    for (int i = 0; i < side; i++)
    {
        q += DIRS[i][0] * k;
        r += DIRS[i][1] * k;
    }
    q += DIRS[side][0] * step;
    r += DIRS[side][1] * step;
    ring = k;
}

// The slot in a cell -- the inverse of CellOf.
int SlotAt(int q, int r)
{
    const int k = RingOf(q, r);
    if (k == 0)
        return 0;
    const int first = 1 + 3 * k * (k - 1);
    for (int s = first; s < first + 6 * k; s++)
    {
        int sq, sr, ring;
        CellOf(s, sq, sr, ring);
        if (sq == q && sr == r)
            return s;
    }
    return -1;  // unreachable: every cell of ring k is one of its 6k slots
}

// The neighbour one ring in that a slot always links to: the lowest-numbered one, so the
// choice depends on the slot alone.
int InwardOf(int q, int r)
{
    const int k = RingOf(q, r);
    int       best = -1;
    for (const auto& d : DIRS)
    {
        const int nq = q + d[0], nr = r + d[1];
        if (RingOf(nq, nr) != k - 1)
            continue;
        const int s = SlotAt(nq, nr);
        if (best < 0 || s < best)
            best = s;
    }
    return best;
}

bool Linked(uint64_t seed, int a, int b, int inwardOfB)
{
    if (inwardOfB == a)
        return true;
    Rng rng(Key(seed, SKELETON_LINK, (uint64_t)a, (uint64_t)b));
    return rng.Chance(LINK_CHANCE);
}
}  // namespace

const char* SlotKindName(SlotKind kind)
{
    switch (kind)
    {
        case SlotKind::Frontier: return "frontier";
    }
    return "frontier";
}

void SlotMapPoint(uint64_t seed, int slot, double& x, double& y)
{
    int q, r, ring;
    CellOf(slot, q, r, ring);
    Rng rng(Key(seed, SKELETON_PLACE, (uint64_t)slot));
    // No angle anywhere: an axial cell to the plane is a sum of two fixed vectors.
    x = std::floor(SPACING * (SQRT3 * q + SQRT3 * 0.5 * r) + 0.5) + rng.Range(-JITTER, JITTER);
    y = std::floor(SPACING * 1.5 * r + 0.5) + rng.Range(-JITTER, JITTER);
}

GalaxySkeleton GenerateGalaxySkeleton(uint64_t seed, int cap)
{
    GalaxySkeleton g;
    g.seed = seed;
    cap = std::max(1, cap);
    for (int s = 0; s < cap; s++)
    {
        Slot slot;
        slot.index = s;
        CellOf(s, slot.q, slot.r, slot.ring);
        SlotMapPoint(seed, s, slot.mapX, slot.mapY);
        slot.inward = s == 0 ? -1 : InwardOf(slot.q, slot.r);
        g.slots.push_back(slot);
    }
    // Each pair once, from the higher slot: its lower neighbours all exist, so whether it
    // links to them is decided the same way whatever the cap is.
    for (int b = 1; b < cap; b++)
        for (const auto& d : DIRS)
        {
            const int a = SlotAt(g.slots[(size_t)b].q + d[0], g.slots[(size_t)b].r + d[1]);
            if (a < 0 || a >= b || !Linked(seed, a, b, g.slots[(size_t)b].inward))
                continue;
            g.links.push_back({ a, b });
            g.slots[(size_t)a].neighbours.push_back(b);
            g.slots[(size_t)b].neighbours.push_back(a);
        }
    std::sort(g.links.begin(), g.links.end());
    for (Slot& s : g.slots)
        std::sort(s.neighbours.begin(), s.neighbours.end());
    return g;
}

nlohmann::json SkeletonJson(const GalaxySkeleton& skeleton)
{
    nlohmann::json out;
    out["seed"] = skeleton.seed;
    out["slots"] = nlohmann::json::array();
    for (const Slot& s : skeleton.slots)
        out["slots"].push_back(
            { { "slot", s.index },
              { "ring", s.ring },
              { "cell", nlohmann::json::array({ s.q, s.r }) },
              { "map", nlohmann::json::array({ (int64_t)s.mapX, (int64_t)s.mapY }) },
              { "kind", SlotKindName(s.kind) },
              { "inward", s.inward },
              { "neighbours", s.neighbours } });
    out["links"] = nlohmann::json::array();
    for (const auto& l : skeleton.links)
        out["links"].push_back(nlohmann::json::array({ l.first, l.second }));
    return out;
}

}  // namespace Gen
