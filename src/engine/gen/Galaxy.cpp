#include "gen/Galaxy.h"
#include "gen/Rng.h"

#include <nlohmann/json.hpp>

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
constexpr uint64_t CENTRE_PLACE = 0x5EE1E702ull;
constexpr uint64_t CENTRE_OWNER = 0x5EE1E703ull;
constexpr uint64_t FIRST_FRONTIER = 0x5EE1E704ull;

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
        case SlotKind::Centre: return "centre";
    }
    return "frontier";
}

int LatticeDistance(int a, int b)
{
    int aq, ar, bq, br, ring;
    CellOf(a, aq, ar, ring);
    CellOf(b, bq, br, ring);
    return RingOf(aq - bq, ar - br);
}

std::vector<int> CentreSlots(uint64_t seed, int count)
{
    count = std::min(count, MAX_CENTRES);
    if (count <= 0)
        return {};
    if (count == 1)
        return { 0 };  // the middle: the frontier grows round it on every side
    // The smallest ring with room, so the civilisations are as close as the spacing allows
    // and the skeleton between them is not wasted. Each ring has its own turn, so the rule
    // for one count does not depend on what another tried.
    for (int k = 2;; k++)
    {
        const int        cells = 6 * k, first = 1 + 3 * k * (k - 1);
        Rng              rng(Key(seed, CENTRE_PLACE, (uint64_t)k));
        const int        turn = rng.Range(0, cells - 1);
        std::vector<int> slots;
        for (int i = 0; i < count; i++)
            slots.push_back(first + (turn + i * cells / count) % cells);
        int closest = 1 << 30;
        for (size_t i = 0; i < slots.size(); i++)
            for (size_t j = i + 1; j < slots.size(); j++)
                closest = std::min(closest, LatticeDistance(slots[i], slots[j]));
        if (closest >= CENTRE_SPACING)
            return slots;
    }
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

GalaxySkeleton GenerateGalaxySkeleton(const WorldParams& params)
{
    GalaxySkeleton   g = GenerateGalaxySkeleton(params.seed, params.cap);
    const int        cap = (int)g.slots.size();
    std::vector<int> placed;
    for (int c : CentreSlots(params.seed, params.centres))
        if (c < cap)
            placed.push_back(c);
    Rng       rng(Key(params.seed, CENTRE_OWNER));
    const int turn = params.owners.empty() ? 0 : rng.Range(0, (int)params.owners.size() - 1);
    for (size_t i = 0; i < placed.size(); i++)
    {
        Slot& s = g.slots[(size_t)placed[i]];
        s.kind = SlotKind::Centre;
        if (!params.owners.empty())
            s.owner = params.owners[((size_t)turn + i) % params.owners.size()];
    }
    if (!placed.empty())  // without centres every slot is as near as the one region ever was
        for (Slot& s : g.slots)
        {
            int nearest = 1 << 30;
            for (int c : placed)
                nearest = std::min(nearest, LatticeDistance(s.index, c));
            s.hops = nearest;
        }
    g.centres = placed;
    g.pirates = params.pirates;
    g.unaffiliated = params.unaffiliated;
    return g;
}

std::vector<Opening> InitialOpenings(const GalaxySkeleton& skeleton)
{
    std::vector<Opening> out;
    for (int c : skeleton.centres)
        out.push_back({ c, -1 });
    std::vector<int> taken;
    for (int c : skeleton.centres)
    {
        // A linked neighbour that is frontier and nobody else's first: the seed chooses.
        std::vector<int> choice;
        for (int n : skeleton.slots[(size_t)c].neighbours)
            if (skeleton.slots[(size_t)n].kind == SlotKind::Frontier &&
                std::find(taken.begin(), taken.end(), n) == taken.end())
                choice.push_back(n);
        if (choice.empty())
            continue;
        Rng       rng(Key(skeleton.seed, FIRST_FRONTIER, (uint64_t)c));
        const int pick = choice[(size_t)rng.Range(0, (int)choice.size() - 1)];
        taken.push_back(pick);
        out.push_back({ pick, c });
    }
    return out;
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
    // Only a skeleton with centres says what they make of it, so one without hashes as it
    // always has.
    if (!skeleton.centres.empty())
    {
        for (const Slot& s : skeleton.slots)
        {
            nlohmann::json& j = out["slots"][(size_t)s.index];
            j["hops"] = s.hops;
            if (!s.owner.empty())
                j["owner"] = s.owner;
        }
        out["centres"] = skeleton.centres;
    }
    out["links"] = nlohmann::json::array();
    for (const auto& l : skeleton.links)
        out["links"].push_back(nlohmann::json::array({ l.first, l.second }));
    return out;
}

}  // namespace Gen
