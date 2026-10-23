#include "gen/Region.h"
#include "gen/Names.h"
#include "gen/Rng.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <utility>

namespace Gen
{
namespace
{
using nlohmann::json;

constexpr double TWO_PI = 6.283185307179586;
constexpr double EDGE = 860000.0;      // nothing generated orbits further out than this
constexpr double CLEARANCE = 15000.0;  // between a planet's swept band and anything static

// Purposes, so that each thing a system has draws from its own stream: a new rule for
// derelicts must not move every planet (Rng.h).
enum Purpose : uint64_t
{
    TOPOLOGY = 1,
    MAP = 2,
    STAR = 10,
    PLANETS = 11,
    BELTS = 12,
    NEBULA = 13,
    DERELICTS = 14,
    GATES = 15,
    WORMHOLE = 16,
    CHARACTER = 17,
    FINDS = 18,
    RUINS = 19,
    REGION = 20,  // a slot's key, from the world's seed
    STATIONS = 21,
    NAMES = 22,
    NATIVES = 23,
};

// How much richer a frontier is for being further from a centre (#342 S2): steps beyond the
// first, to a limit. 0 next to a centre, where the frontier is what it always was.
int Richness(const RegionParams& p)
{
    return p.kind == SlotKind::Centre ? 0 : std::max(0, std::min(p.hops - 1, 4));
}

struct Band  // the ring a planet sweeps
{
    double radius;
    double size;
};

struct Disc  // something static already placed
{
    double x, y, size;
};

struct Node
{
    std::string id;
    std::string designation;
    int         depth = 1;
    int         index = 0;  // order of generation, for keys
    double      mapX = 0.0, mapY = 0.0;
    int         dir = 0;  // a centre's: which of twelve bearings from the heart it lies on
};

// Twelve bearings a twelfth of a turn apart, written out rather than computed (no sin/cos).
constexpr double C30 = 0.8660254037844386;
constexpr double DIRS12[12][2] = { { 1, 0 },       { C30, 0.5 },  { 0.5, C30 },  { 0, 1 },
                                   { -0.5, C30 },  { -C30, 0.5 }, { -1, 0 },     { -C30, -0.5 },
                                   { -0.5, -C30 }, { 0, -1 },     { 0.5, -C30 }, { C30, -0.5 } };

// A centre's systems were named long ago (#342 S2), unlike the frontier's designations:
// two or three syllables the seed picks.
std::string CentreName(Rng& rng)
{
    static const char* HEAD[] = { "Al",  "Bel", "Cor", "Dra", "El",  "Fen", "Gal", "Hel",
                                  "Ior", "Kal", "Lun", "Mor", "Nev", "Or",  "Pra", "Quel",
                                  "Rho", "Sol", "Tar", "Ul",  "Vey", "Xan", "Yr",  "Zen",
                                  "Ash", "Cer", "Dun", "Eri", "Hal", "Ket", "Lys", "Sar" };
    static const char* MID[] = {
        "a", "e", "i", "o", "u", "ae", "ar", "en", "is", "on", "or", "ul"
    };
    static const char* TAIL[] = { "",    "n",  "s",   "th",  "x",   "ra",
                                  "dor", "ia", "mir", "tis", "vel", "gard" };
    std::string        name = HEAD[rng.Range(0, 31)];
    name += MID[rng.Range(0, 11)];
    name += TAIL[rng.Range(0, 11)];
    if (rng.Chance(12))
        name += rng.Chance(50) ? " Prime" : " Minor";
    return name;
}

// What a player calls a system until somebody names it (#145). The first region keeps the
// bare "W-2.3"; any other says which region it is in, since the ids are galaxy-wide.
std::string Designation(int slot, int depth, int k)
{
    const std::string w = "W-" + std::to_string(depth) + "." + std::to_string(k);
    return slot == 0 ? w : "R" + std::to_string(slot) + " " + w;
}

double Round(double v, double step)
{
    return std::floor(v / step + 0.5) * step;
}

// A direction without sin/cos: an integer point in a ring, normalised. sqrt is correctly
// rounded under IEEE 754, so this is the same direction on every platform.
void RandomDirection(Rng& rng, double& ux, double& uy)
{
    for (;;)
    {
        const double x = rng.Range(-1000, 1000), y = rng.Range(-1000, 1000);
        const double l2 = x * x + y * y;
        if (l2 < 100.0 * 100.0 || l2 > 1000.0 * 1000.0)
            continue;
        const double l = std::sqrt(l2);
        ux = x / l;
        uy = y / l;
        return;
    }
}

bool ClearOfTaken(double x, double y, double size, const std::vector<Disc>& taken)
{
    for (const Disc& d : taken)
    {
        const double dx = x - d.x, dy = y - d.y;
        const double need = d.size + size + 20000.0;
        if (dx * dx + dy * dy < need * need)
            return false;
    }
    return true;
}

// The planet whose path (x, y) is in, or -1.
int BandAt(double x, double y, double size, const std::vector<Band>& bands)
{
    const double r = std::sqrt(x * x + y * y);
    for (size_t i = 0; i < bands.size(); i++)
        if (std::fabs(r - bands[i].radius) < MoonZone(bands[i].size) + size + CLEARANCE)
            return (int)i;
    return -1;
}

bool Clear(double x, double y, double size, const std::vector<Band>& bands,
           const std::vector<Disc>& taken)
{
    return BandAt(x, y, size, bands) < 0 && ClearOfTaken(x, y, size, taken);
}

// Room left around each planet for satellites, innermost first.
struct Moons
{
    std::vector<double> next;   // the inner edge of the next free orbit
    std::vector<double> limit;  // how far out a satellite may reach
};

Moons MoonsFor(const std::vector<Band>& bands)
{
    Moons m;
    for (const Band& b : bands)
    {
        m.next.push_back(b.size + 4000.0);
        m.limit.push_back(MoonZone(b.size));
    }
    return m;
}

// Somewhere between rMin and rMax from the star, out of every planet's path and clear of
// what is already placed. `dirX/dirY` (non-zero) asks for a bearing -- a gate faces the
// system it leads to -- and is held to within a few degrees.
bool Place(Rng& rng, double rMin, double rMax, double size, const std::vector<Band>& bands,
           std::vector<Disc>& taken, json& pos, double dirX = 0.0, double dirY = 0.0)
{
    for (int attempt = 0; attempt < 96; attempt++)
    {
        double ux, uy;
        if (dirX != 0.0 || dirY != 0.0)
        {
            // Nudged sideways a little, more on each try, so a blocked bearing finds room.
            double jx, jy;
            RandomDirection(rng, jx, jy);
            const double spread = 0.02 + 0.01 * attempt;
            ux = dirX + jx * spread;
            uy = dirY + jy * spread;
            const double l = std::sqrt(ux * ux + uy * uy);
            ux /= l;
            uy /= l;
        }
        else
            RandomDirection(rng, ux, uy);
        const double r = rng.Between(rMin, rMax);
        const double x = Round(ux * r, 100.0), y = Round(uy * r, 100.0);
        if (!Clear(x, y, size, bands, taken))
            continue;
        taken.push_back({ x, y, size });
        pos = json::array({ (int64_t)x, (int64_t)y });
        return true;
    }
    return false;
}

// Somewhere between rMin and rMax, like Place -- but a spot in a planet's path is not
// refused: the object belongs to that planet and orbits it (#210). Writes "pos" or
// "orbits" into `obj`. A satellite's angle needs no sin or cos here; the loader turns it.
// The next free orbit round one planet, written into `obj` as its "orbits" (#210).
bool OrbitPlanet(Rng& rng, int planet, double size, Moons& moons, json& obj)
{
    const double radius = Round(moons.next[planet] + size, 100.0);
    if (radius + size > moons.limit[planet])
        return false;
    moons.next[planet] = radius + size + 2000.0;
    obj["orbits"] = { { "planet", planet },
                      { "radius", (int64_t)radius },
                      { "speed", rng.Range(20, 50) },
                      { "phase", rng.Range(0, 65535) * (TWO_PI / 65536.0) } };
    return true;
}

bool PlaceOrOrbit(Rng& rng, double rMin, double rMax, double size, const std::vector<Band>& bands,
                  Moons& moons, std::vector<Disc>& taken, json& obj)
{
    for (int attempt = 0; attempt < 96; attempt++)
    {
        double ux, uy;
        RandomDirection(rng, ux, uy);
        const double r = rng.Between(rMin, rMax);
        const double x = Round(ux * r, 100.0), y = Round(uy * r, 100.0);
        if (!ClearOfTaken(x, y, size, taken))
            continue;
        const int planet = BandAt(x, y, size, bands);
        if (planet < 0)
        {
            taken.push_back({ x, y, size });
            obj["pos"] = json::array({ (int64_t)x, (int64_t)y });
            return true;
        }
        if (OrbitPlanet(rng, planet, size, moons, obj))
            return true;
        // this planet has no room left; somewhere else, then
    }
    return false;
}

// Beside something that gives it a reason to be there (#146): a wreck at the belt its
// prospector came for, a ruin by the ore it was built over. Just clear of the anchor and
// of everything else, and out of every planet's path.
bool PlaceNear(Rng& rng, double ax, double ay, double anchorSize, double size,
               const std::vector<Band>& bands, std::vector<Disc>& taken, json& pos)
{
    for (int attempt = 0; attempt < 48; attempt++)
    {
        double ux, uy;
        RandomDirection(rng, ux, uy);
        const double r = anchorSize + size + rng.Between(21000.0, 40000.0);
        const double x = Round(ax + ux * r, 100.0), y = Round(ay + uy * r, 100.0);
        if (std::sqrt(x * x + y * y) + size > 970000.0)
            continue;  // past the edge of the system
        if (!Clear(x, y, size, bands, taken))
            continue;
        taken.push_back({ x, y, size });
        pos = json::array({ (int64_t)x, (int64_t)y });
        return true;
    }
    return false;
}

// The last resort when nothing fits: on the bearing, at the radius, whatever is there. A
// gate with no position is not a gate, and a load error is worse than an overlap.
void PlaceAnyway(json& pos, double dirX, double dirY, double r)
{
    if (!pos.is_null())
        return;
    if (dirX == 0.0 && dirY == 0.0)
        dirX = 1.0;
    pos = json::array({ (int64_t)Round(dirX * r, 100.0), (int64_t)Round(dirY * r, 100.0) });
}

std::vector<Band> BandsOf(const json& system)
{
    std::vector<Band> bands;
    if (system.contains("planets") && system["planets"].is_array())
        for (const json& p : system["planets"])
            bands.push_back({ p.value("orbitRadius", 0.0), p.value("size", 0.0) });
    return bands;
}

// ---- The galaxy's shape (#143 refines it) ----------------------------------------------

std::vector<Node> Topology(const RegionParams& params, json& links)
{
    Rng               rng(Key(params.seed, TOPOLOGY));
    std::vector<Node> nodes;
    // Ring 1 is one system: what is on the far side of the wormhole.
    nodes.push_back({ SystemId(params.slot, 1, 1), Designation(params.slot, 1, 1), 1, 0 });
    links.push_back(json::array({ params.homeId, nodes[0].id }));

    std::vector<size_t> prevRing = { 0 };
    int                 depth = 1;
    while ((int)nodes.size() < params.systems)
    {
        depth++;
        const int           width = std::min(rng.Range(2, 4), params.systems - (int)nodes.size());
        std::vector<size_t> ring;
        for (int k = 1; k <= width; k++)
        {
            Node n{ SystemId(params.slot, depth, k), Designation(params.slot, depth, k), depth,
                    (int)nodes.size() };
            const size_t parent = prevRing[(size_t)rng.Range(0, (int)prevRing.size() - 1)];
            links.push_back(json::array({ nodes[parent].id, n.id }));
            ring.push_back(nodes.size());
            nodes.push_back(n);
        }
        // A few links across a ring, fewer the further out: the frontier is a frontier.
        for (size_t i = 0; i + 1 < ring.size(); i++)
            if (rng.Chance(std::max(0, 40 - 8 * depth)))
                links.push_back(json::array({ nodes[ring[i]].id, nodes[ring[i + 1]].id }));
        prevRing = ring;
    }
    return nodes;
}

// A centre (#342 S2): a heart and rings round it rather than a line of rings away from a
// wormhole. Every system of the ring inside it is a neighbour of the heart's, and links run
// round each ring as often as not -- settled space is well connected, the frontier is not.
std::vector<Node> CentreTopology(const RegionParams& params, json& links)
{
    Rng                   rng(Key(params.seed, TOPOLOGY));
    Rng                   names(Key(params.seed, NAMES));
    std::set<std::string> used;
    auto                  name = [&]()
    {
        for (;;)
        {
            std::string n = CentreName(names);
            if (used.insert(n).second)
                return n;
        }
    };
    std::vector<Node> nodes;
    nodes.push_back({ SystemId(params.slot, 1, 1), name(), 1, 0 });  // the heart

    const int        total = rng.Range(8, 14);
    std::vector<int> widths;
    int              left = total - 1;
    widths.push_back(std::min(rng.Range(3, 5), left));
    left -= widths.back();
    while (left > 0)
    {
        widths.push_back(std::min(8, left));
        left -= widths.back();
    }

    std::vector<size_t> prevRing = { 0 };
    for (size_t r = 0; r < widths.size(); r++)
    {
        const int           depth = (int)r + 2, width = widths[r];
        const int           turn = rng.Range(0, 11);
        std::vector<size_t> ring;
        for (int k = 1; k <= width; k++)
        {
            Node n{ SystemId(params.slot, depth, k), name(), depth, (int)nodes.size() };
            n.dir = (turn + (k - 1) * 12 / width) % 12;
            // Joined to the system of the ring inside that lies nearest its bearing.
            size_t parent = prevRing.front();
            int    best = 99;
            for (size_t p : prevRing)
            {
                int d = std::abs(nodes[p].dir - n.dir) % 12;
                d = depth == 2 ? 0 : std::min(d, 12 - d);
                if (d < best)
                {
                    best = d;
                    parent = p;
                }
            }
            links.push_back(json::array({ nodes[parent].id, n.id }));
            ring.push_back(nodes.size());
            nodes.push_back(n);
        }
        // Round the ring, neighbour to neighbour by bearing, the last back to the first.
        for (size_t i = 0; i < ring.size() && ring.size() >= 2; i++)
        {
            if (ring.size() == 2 && i == 1)
                break;  // two systems are one pair, not two
            if (rng.Chance(depth == 2 ? 60 : 40))
                links.push_back(
                    json::array({ nodes[ring[i]].id, nodes[ring[(i + 1) % ring.size()]].id }));
        }
        prevRing = ring;
    }
    return nodes;
}

// A centre on the galaxy map: the heart on its slot's point, each ring round it.
void PlaceCentreOnMap(const RegionParams& params, double cx, double cy, std::vector<Node>& nodes)
{
    Rng rng(Key(params.seed, MAP));
    for (Node& n : nodes)
    {
        if (n.depth == 1)
        {
            n.mapX = cx;
            n.mapY = cy;
            continue;
        }
        const double r = 250.0 * (n.depth - 1) + rng.Range(-20, 20);
        n.mapX = Round(cx + DIRS12[n.dir][0] * r + rng.Range(-15, 15), 1.0);
        n.mapY = Round(cy + DIRS12[n.dir][1] * r + rng.Range(-15, 15), 1.0);
    }
}

// Where the region sits on the galaxy map: away from home, in rings.
void PlaceOnMap(const RegionParams& params, std::vector<Node>& nodes)
{
    // Eight bearings, written out rather than computed from an angle (no sin/cos).
    static const double H = 0.7071067811865476;
    static const double DIRS[8][2] = { { 1, 0 },  { H, -H }, { 0, -1 }, { -H, -H },
                                       { -1, 0 }, { -H, H }, { 0, 1 },  { H, H } };
    Rng                 rng(Key(params.seed, MAP));
    // The bearing that keeps the region furthest from what is already mapped; among equals,
    // the seed decides. No angle is computed, so no sin/cos.
    const int first = rng.Range(0, 7);
    int       d = first;
    double    best = -1.0;
    for (int i = 0; i < 8; i++)
    {
        const int    c = (first + i) % 8;
        const double cx = params.homeMap.x + DIRS[c][0] * 500.0;
        const double cy = params.homeMap.y + DIRS[c][1] * 500.0;
        double       nearest = 1e18;
        for (const Vector2& k : params.knownMap)
            nearest = std::min(nearest, (cx - k.x) * (cx - k.x) + (cy - k.y) * (cy - k.y));
        if (nearest > best + 1.0)
        {
            best = nearest;
            d = c;
        }
    }
    double dx = DIRS[d][0], dy = DIRS[d][1];
    // A bearing given -- from the skeleton, towards the region's slot -- wins.
    const double bl = std::sqrt((double)params.bearing.x * params.bearing.x +
                                (double)params.bearing.y * params.bearing.y);
    if (bl > 0.0)
    {
        dx = params.bearing.x / bl;
        dy = params.bearing.y / bl;
    }
    const double px = -dy, py = dx;  // across the bearing

    std::map<int, int> perRing;
    for (const Node& n : nodes)
        perRing[n.depth]++;
    std::map<int, int> seen;
    for (Node& n : nodes)
    {
        const int k = seen[n.depth]++;
        // Spaced for the galaxy map's labels: a name and a line of statistics under it.
        const double along = 240.0 + 210.0 * (n.depth - 1) + rng.Range(-30, 30);
        const double across = (k - (perRing[n.depth] - 1) * 0.5) * 230.0 + rng.Range(-35, 35);
        n.mapX = Round(params.homeMap.x + dx * along + px * across, 1.0);
        n.mapY = Round(params.homeMap.y + dy * along + py * across, 1.0);
    }
}

// ---- One system (#142) ------------------------------------------------------------------
//
// A system has a character, rolled first, and every part of it asks: a belt cluster has
// belts where an ordinary system has planets, a graveyard has wrecks, a barren system has
// next to nothing -- and the best odds of something rare (#211). Variety comes from the
// character, not from noise in every number: a player who has seen two graveyards knows
// what a third one is for.

enum class Character
{
    Ordinary,
    Binary,
    BeltCluster,
    Shrouded,
    Graveyard,
    Giants,
    Frozen,
    Barren,
};

const char* CharacterName(Character c)
{
    switch (c)
    {
        case Character::Ordinary: return "ordinary";
        case Character::Binary: return "binary";
        case Character::BeltCluster: return "belt cluster";
        case Character::Shrouded: return "shrouded";
        case Character::Graveyard: return "graveyard";
        case Character::Giants: return "giants";
        case Character::Frozen: return "frozen";
        case Character::Barren: return "barren";
    }
    return "ordinary";
}

// Weighted by depth: ordinary near the wormhole, stranger and more dangerous further out.
Character PickCharacter(Rng& rng, int depth)
{
    const int d = depth - 1;
    const int weights[] = {
        std::max(8, 34 - 5 * d),  // ordinary
        10,                       // binary
        12,                       // belt cluster
        std::min(16, 3 + 3 * d),  // shrouded
        std::min(15, 2 + 3 * d),  // graveyard
        10,                       // giants
        8,                        // frozen
        std::min(18, 6 + 2 * d),  // barren
    };
    int total = 0;
    for (int w : weights)
        total += w;
    int roll = rng.Range(0, total - 1);
    for (int i = 0; i < 8; i++)
    {
        if (roll < weights[i])
            return (Character)i;
        roll -= weights[i];
    }
    return Character::Ordinary;
}

// Settled space: nobody settles a graveyard, a shroud or a dead rock first. The heart is an
// ordinary system, the kind a civilisation grows up in.
Character PickCentreCharacter(Rng& rng, int depth)
{
    if (depth == 1)
        return Character::Ordinary;
    const int roll = rng.Range(0, 99);
    if (roll < 52)
        return Character::Ordinary;
    if (roll < 64)
        return Character::Binary;
    if (roll < 78)
        return Character::BeltCluster;
    if (roll < 91)
        return Character::Giants;
    return Character::Frozen;
}

const char* PlanetTypeAt(Rng& rng, double fraction, Character c)
{
    if (c == Character::Frozen)
        return rng.Chance(80) ? "Ice" : "Oceanic";
    if (c == Character::Barren)
        return rng.Chance(30) ? "Lava" : "Rocky";
    if (fraction < 0.35)
        return rng.Chance(45) ? "Lava" : "Rocky";
    if (fraction < 0.6)
        return rng.Chance(50) ? "Oceanic" : "Rocky";
    return rng.Chance(50) ? "Gas" : "Ice";
}

int PlanetSize(Rng& rng, const std::string& type)
{
    if (type == "Gas")
        return rng.Range(26, 36) * 1000;
    if (type == "Ice")
        return rng.Range(15, 21) * 1000;
    if (type == "Oceanic")
        return rng.Range(14, 20) * 1000;
    return rng.Range(12, 18) * 1000;  // rock, lava
}

const char* DepositOf(const std::string& type)
{
    if (type == "Gas")
        return "Crystal";
    if (type == "Ice" || type == "Oceanic")
        return "Ice";
    return "Iron";
}

json Star(Rng& rng, const char* type)
{
    const std::string t = type;
    const double      base = t == "Yellow" ? 150000.0 : (t == "Red" ? 137500.0 : 125000.0);
    return { { "type", t }, { "size", (int64_t)Round(base * rng.Between(0.85, 1.15), 100.0) } };
}

json GenerateSystem(const RegionParams& params, const Node& n, const std::vector<Node>& nodes,
                    const json& links, const Node* homeAsNode)
{
    const uint64_t key = (uint64_t)n.index + 1000;  // never collides with a purpose
    const bool     centre = params.kind == SlotKind::Centre;
    const int      rich = Richness(params);
    json           sys;

    Character character;
    {
        Rng rng(Key(params.seed, key, CHARACTER));
        character = centre ? PickCentreCharacter(rng, n.depth) : PickCharacter(rng, n.depth);
    }
    sys["character"] = CharacterName(character);

    // The star -- or two. How far out the stars reach is where the planets may start.
    double starReach = 0.0;
    {
        Rng         rng(Key(params.seed, key, STAR));
        const int   roll = rng.Range(0, 99);
        const char* type = roll < 50 ? "Yellow" : (roll < 80 ? "Red" : "Blue");
        if (character == Character::Barren || character == Character::Frozen)
            type = "Red";  // dim, old, cold
        if (character == Character::Giants)
            type = "Yellow";
        if (character == Character::Binary)
        {
            // Two smaller stars either side of the middle, lighting everything from two
            // directions (#119). The second is a different colour more often than not.
            json         a = Star(rng, type);
            json         b = Star(rng, rng.Chance(65) ? (roll < 50 ? "Red" : "Yellow") : type);
            const double sa = a["size"].get<double>() * 0.7, sb = b["size"].get<double>() * 0.7;
            a["size"] = (int64_t)Round(sa, 100.0);
            b["size"] = (int64_t)Round(sb, 100.0);
            double ux, uy;
            RandomDirection(rng, ux, uy);
            const double half = std::max(sa, sb) + 70000.0;  // two discs, not one blob
            a["pos"] =
                json::array({ (int64_t)Round(ux * half, 100.0), (int64_t)Round(uy * half, 100.0) });
            b["pos"] = json::array(
                { (int64_t)Round(-ux * half, 100.0), (int64_t)Round(-uy * half, 100.0) });
            sys["stars"] = json::array({ a, b });
            starReach = half + std::max(sa, sb);
        }
        else
        {
            sys["star"] = Star(rng, type);
            starReach = sys["star"]["size"].get<double>();
        }
    }

    // Planets, typed by how far out they are -- unless the character says otherwise.
    std::vector<Band> bands;
    {
        Rng  rng(Key(params.seed, key, PLANETS));
        json planets = json::array();
        int  want = rng.Range(2, 6);
        switch (character)
        {
            case Character::Binary: want = rng.Range(1, 4); break;
            case Character::BeltCluster: want = rng.Range(1, 3); break;
            case Character::Shrouded:
            case Character::Graveyard: want = rng.Range(2, 4); break;
            case Character::Giants: want = rng.Range(2, 3); break;
            case Character::Barren: want = rng.Range(1, 3); break;
            default: break;
        }
        double r = starReach + rng.Range(60, 90) * 1000.0;
        int    giants = character == Character::Giants ? rng.Range(1, 2) : 0;
        for (int i = 0; i < want; i++)
        {
            // In a system of giants the outer planets are gas, whatever the distance says.
            const bool        giant = giants > 0 && want - i <= giants;
            const std::string type = giant ? "Gas" : PlanetTypeAt(rng, r / EDGE, character);
            const int         size = giant ? rng.Range(32, 36) * 1000 : PlanetSize(rng, type);
            if (r + size > EDGE)
                break;
            planets.push_back({ { "type", type },
                                { "size", size },
                                { "orbitRadius", (int64_t)r },
                                { "orbitSpeed", rng.Range(28, 52) },
                                { "angle", rng.Range(0, 65535) * (TWO_PI / 65536.0) },
                                { "deposit", DepositOf(type) } });
            bands.push_back({ r, (double)size });
            // The gaps tighten when the planets still to come would not fit otherwise: the
            // survey (#141) found six-planet systems cut off at the edge and counted as five.
            const int    left = want - i - 1;
            const double room = EDGE - (r + size);
            double       gap = rng.Range(70, 150) * 1000.0;
            if (left > 0 && (gap + 40000.0) * left > room)
                gap = std::max(30000.0, room / left - 40000.0);
            r += size + gap;
        }
        sys["planets"] = planets;
    }

    // Stars take room too: a binary's two stand off the middle, and nothing is placed in
    // either of them.
    std::vector<Disc> taken;
    if (sys.contains("star"))
        taken.push_back({ 0.0, 0.0, sys["star"]["size"].get<double>() });
    if (sys.contains("stars"))
        for (const json& s : sys["stars"])
            taken.push_back(
                { s["pos"][0].get<double>(), s["pos"][1].get<double>(), s["size"].get<double>() });
    Moons moons = MoonsFor(bands);

    // Gates first: each faces the system it leads to, and their bearings are not free.
    {
        Rng  rng(Key(params.seed, key, GATES));
        json gates = json::array();
        for (const json& l : links)
        {
            const std::string a = l[0], b = l[1];
            if (a != n.id && b != n.id)
                continue;
            const std::string other = a == n.id ? b : a;
            const Node*       o = nullptr;
            for (const Node& m : nodes)
                if (m.id == other)
                    o = &m;
            if (o == nullptr && homeAsNode != nullptr && other == homeAsNode->id)
                o = homeAsNode;
            if (o == nullptr)
                continue;
            double       dx = o->mapX - n.mapX, dy = o->mapY - n.mapY;
            const double l2 = std::sqrt(dx * dx + dy * dy);
            dx = l2 > 0.0 ? dx / l2 : 1.0;
            dy = l2 > 0.0 ? dy / l2 : 0.0;
            json pos;
            if (!Place(rng, 880000.0, 930000.0, 900.0, bands, taken, pos, dx, dy))
                Place(rng, 880000.0, 950000.0, 900.0, bands, taken, pos);
            PlaceAnyway(pos, dx, dy, 900000.0);
            const bool home = homeAsNode != nullptr && other == homeAsNode->id;
            gates.push_back(
                { { "name", home ? std::string("Wormhole") : "Gate to " + o->designation },
                  { "pos", pos },
                  { "size", 900 },
                  { "destination", other } });
        }
        sys["gates"] = gates;
    }

    // Belts: what a miner crosses the wormhole for. Crystal grows more likely further out.
    {
        Rng  rng(Key(params.seed, key, BELTS));
        json belts = json::array();
        int  count = rng.Range(1, 2) + (n.depth >= 3 && rng.Chance(40) ? 1 : 0);
        if (character == Character::BeltCluster)
            count = rng.Range(3, 5);
        else if (character == Character::Barren)
            count = rng.Chance(50) ? 1 : 0;
        if (centre && n.depth == 1)
            count = std::max(count, 2);  // what the capital's mining posts are there for

        // A belt where a planet did not finish forming (#146): the widest gap between two
        // planets' paths, or past the last one, is where a world that never came together
        // left its rubble. Most systems with room have one; a belt cluster always does.
        double gapLo = 0.0, gapHi = 0.0;
        for (size_t b = 0; b < bands.size(); b++)
        {
            const double lo = bands[b].radius + MoonZone(bands[b].size);
            const double hi =
                b + 1 < bands.size() ? bands[b + 1].radius - MoonZone(bands[b + 1].size) : 840000.0;
            if (hi - lo > gapHi - gapLo)
            {
                gapLo = lo;
                gapHi = hi;
            }
        }
        const bool remnant = count > 0 && (character == Character::BeltCluster || rng.Chance(45));

        for (int i = 0; i < count; i++)
        {
            const int   roll = rng.Range(0, 99);
            const char* res = roll < 15 + 12 * n.depth ? "Crystal" : (roll < 60 ? "Iron" : "Ice");
            if (character == Character::Frozen)
                res = roll < 75 ? "Ice" : "Crystal";
            const int size = rng.Range(50, 75) * 100;
            json belt = { { "name", n.designation + " Belt " + std::string(1, (char)('A' + i)) },
                          { "size", size },
                          { "resource", res } };
            const double slack = (gapHi - gapLo) / 2.0 - size - CLEARANCE;
            json         pos;
            if (i == 0 && remnant && slack > 0.0 &&
                Place(rng, (gapLo + gapHi) / 2.0 - slack, (gapLo + gapHi) / 2.0 + slack, size,
                      bands, taken, pos))
            {
                belt["name"] = n.designation + " Remnant";
                belt["pos"] = pos;
            }
            // In a planet's path a belt is its ring -- a gas giant's, most often (#210).
            else if (!PlaceOrOrbit(rng, 260000.0, 840000.0, size, bands, moons, taken, belt))
                continue;
            belt["ore"] = rng.Range(200, 400) + 40 * n.depth +
                          (character == Character::BeltCluster ? 100 : 0) + 50 * rich;
            belts.push_back(belt);
        }
        sys["asteroidFields"] = belts;
    }

    // Stations (#342 S2). A centre is settled space: its heart has one of every kind -- the
    // trade hub, a shipyard, the fleet's bastion and a mining post at its ore -- and every
    // other system somewhere to dock: a mining post where there is ore, a market or a
    // garrison otherwise. Nobody has built anything on the frontier yet.
    sys["stations"] = json::array();
    if (centre)
    {
        Rng                      rng(Key(params.seed, key, STATIONS));
        std::vector<std::string> roles;
        if (n.depth == 1)
            roles = { "TradeHub", "Shipyard", "Military", "MiningOutpost" };
        else
        {
            const int roll = rng.Range(0, 99);
            if (!sys["asteroidFields"].empty() && roll < 55)
                roles = { "MiningOutpost" };
            else
                roles = { roll < 85 ? "TradeHub" : "Military" };
            if (rng.Chance(25))
                roles.push_back(roles[0] == "TradeHub" ? "Military" : "TradeHub");
            if (n.depth == 2 && rng.Chance(20))
                roles.push_back("Shipyard");  // a second yard near the capital
        }
        for (const std::string& role : roles)
        {
            const int   size = role == "TradeHub" ? 700 : (role == "MiningOutpost" ? 500 : 600);
            const char* suffix = role == "TradeHub"   ? " Exchange"
                                 : role == "Shipyard" ? " Yards"
                                 : role == "Military" ? " Bastion"
                                                      : " Mining Post";
            // A yard builds for anyone who pays; the rest fly the owner's flag.
            const std::string faction =
                role == "Shipyard" || params.owner.empty() ? params.unaffiliated : params.owner;
            json station = { { "name", n.designation + suffix },
                             { "role", role },
                             { "size", size },
                             { "faction", faction } };
            bool placed = false;
            if (role == "MiningOutpost")
                for (const json& b : sys["asteroidFields"])
                {
                    json pos;
                    if (!placed && b.contains("pos") &&
                        PlaceNear(rng, b["pos"][0], b["pos"][1], b["size"], size, bands, taken,
                                  pos))
                    {
                        station["pos"] = pos;
                        placed = true;
                    }
                }
            // Anywhere else -- and in a planet's path it orbits that planet (#210), as a
            // satellite station does: an order closes on a moving target with a hold that
            // follows it (#377).
            if (!placed &&
                !PlaceOrOrbit(rng, 200000.0, 820000.0, size, bands, moons, taken, station))
            {
                json pos;
                if (!Place(rng, 120000.0, 960000.0, size, bands, taken, pos))
                {
                    // A crowded system: a gap between paths that only other objects fill.
                    std::vector<Disc> none;
                    if (!Place(rng, 120000.0, 960000.0, size, bands, none, pos))
                        continue;
                }
                station["pos"] = pos;
            }
            sys["stations"].push_back(station);
        }
    }

    // A cloud, sometimes -- and in a shrouded system one that covers half of it, where a
    // ship cannot be seen and an ambush can (the nebula's hazard hides ships).
    {
        Rng        rng(Key(params.seed, key, NEBULA));
        json       nebulae = json::array();
        const bool shroud = character == Character::Shrouded;
        if (shroud || rng.Chance(25))
        {
            const int radius = shroud ? rng.Range(160, 240) * 1000 : rng.Range(28, 40) * 1000;
            json      pos;
            std::vector<Disc> none;  // a cloud may drift over anything
            const double      lo = shroud ? 250000.0 : 300000.0;
            if (Place(rng, lo, 900000.0 - radius, 0.0, {}, none, pos))
                nebulae.push_back({ { "name", n.designation + (shroud ? " Shroud" : " Cloud") },
                                    { "pos", pos },
                                    { "radius", radius } });
        }
        sys["nebulae"] = nebulae;
    }

    // Something already here: more of them, and richer, further from home (#146). A
    // graveyard is a battle: a dozen wrecks around the place it was fought.
    {
        static const char* NAMES[] = { "Unmarked Wreck", "Silent Hulk", "Broken Survey Ship",
                                       "Lost Prospector", "Gutted Freighter" };
        static const char* BATTLE[] = { "Burnt-out Frigate", "Split Cruiser", "Holed Escort",
                                        "Gutted Gunship", "Drifting Bridge" };
        Rng                rng(Key(params.seed, key, DERELICTS));
        json               wrecks = json::array();
        if (character == Character::Graveyard)
        {
            json centre;
            if (Place(rng, 250000.0, 800000.0, 40000.0, bands, taken, centre))
            {
                const double cx = centre[0], cy = centre[1];
                const int    count = rng.Range(6, 11);
                for (int i = 0; i < count; i++)
                {
                    double ux, uy;
                    RandomDirection(rng, ux, uy);
                    const double r = rng.Between(2000.0, 38000.0);
                    wrecks.push_back(
                        { { "name", BATTLE[rng.Range(0, 4)] },
                          { "pos", json::array({ (int64_t)Round(cx + ux * r, 100.0),
                                                 (int64_t)Round(cy + uy * r, 100.0) }) },
                          { "size", rng.Range(35, 55) },
                          { "reward", 700 + 300 * n.depth + rng.Range(0, 6) * 100 } });
                }
            }
        }
        else
        {
            // Rarer than the survey (#141) found them: a wreck in three systems of four was
            // scenery, not a find. A graveyard is where wrecks are.
            //
            // And each one is where something went wrong (#146): a prospector at the belt it
            // came for, a freighter caught at a gate, a ship lost in a cloud. Only a wreck
            // with no such story lies wherever it fell.
            static const char* AT_BELT[] = { "Lost Prospector", "Broken Survey Ship" };
            static const char* AT_GATE[] = { "Gutted Freighter", "Ambushed Hauler" };
            static const char* IN_CLOUD[] = { "Silent Hulk", "Drifted Courier" };
            const int          chance = character == Character::Barren ? 10 : 8 + 6 * n.depth;
            const int          count = rng.Chance(chance) ? rng.Range(1, 2) : 0;
            for (int i = 0; i < count; i++)
            {
                const int   size = rng.Range(30, 50);
                json        wreck = { { "size", size } };
                const char* name = nullptr;
                json        pos;
                const int   reason = rng.Range(0, 3);
                if (reason == 0)
                {
                    for (const json& b : sys["asteroidFields"])
                        if (name == nullptr && b.contains("pos") &&
                            PlaceNear(rng, b["pos"][0], b["pos"][1], b["size"], size, bands, taken,
                                      pos))
                            name = AT_BELT[rng.Range(0, 1)];
                }
                else if (reason == 1)
                {
                    for (const json& g : sys["gates"])
                        if (name == nullptr && PlaceNear(rng, g["pos"][0], g["pos"][1], g["size"],
                                                         size, bands, taken, pos))
                            name = AT_GATE[rng.Range(0, 1)];
                }
                else if (reason == 2)
                {
                    // Inside the cloud, where its hazard hid whatever happened.
                    for (const json& c : sys["nebulae"])
                        for (int a = 0; a < 24 && name == nullptr; a++)
                        {
                            double ux, uy;
                            RandomDirection(rng, ux, uy);
                            const double r = rng.Between(0.0, c["radius"].get<double>() * 0.6);
                            const double x = Round(c["pos"][0].get<double>() + ux * r, 100.0);
                            const double y = Round(c["pos"][1].get<double>() + uy * r, 100.0);
                            if (std::sqrt(x * x + y * y) + size < 970000.0 &&
                                Clear(x, y, size, bands, taken))
                            {
                                taken.push_back({ x, y, (double)size });
                                pos = json::array({ (int64_t)x, (int64_t)y });
                                name = IN_CLOUD[rng.Range(0, 1)];
                            }
                        }
                }
                if (name != nullptr)
                    wreck["pos"] = pos;
                else if (PlaceOrOrbit(rng, 200000.0, 850000.0, size, bands, moons, taken, wreck))
                    name = NAMES[rng.Range(0, 4)];
                else
                    continue;
                wreck["name"] = name;
                wreck["reward"] = 1000 + 500 * n.depth + rng.Range(0, 8) * 100;
                wrecks.push_back(wreck);
            }
        }
        sys["derelicts"] = wrecks;
    }

    // Somebody lived here (#146): the ruin of an outpost, by what it was built for -- the
    // ore of a belt, or in orbit round a planet. Further out, more likely: whoever came this
    // far before stayed long enough to build.
    {
        static const char* BY_BELT[] = { "Abandoned Claim", "Old Refinery" };
        static const char* IN_ORBIT[] = { "Dead Outpost", "Empty Dock" };
        Rng                rng(Key(params.seed, key, RUINS));
        // Not in a centre: what was abandoned there was built over long ago.
        const int chance = centre ? 0 : (character == Character::Barren ? 20 : 4 + 4 * n.depth);
        if (rng.Chance(chance))
        {
            const double size = 260.0;
            json         ruin = { { "size", 260 },
                                  { "reward", 3000 + 800 * n.depth },
                                  { "archetype", "derelict.outpost_ruin" } };
            const char*  name = nullptr;
            json         pos;
            if (rng.Chance(50))
                for (const json& b : sys["asteroidFields"])
                    if (name == nullptr && b.contains("pos") &&
                        PlaceNear(rng, b["pos"][0], b["pos"][1], b["size"], size, bands, taken,
                                  pos))
                    {
                        ruin["pos"] = pos;
                        name = BY_BELT[rng.Range(0, 1)];
                    }
            if (name == nullptr && !bands.empty() &&
                OrbitPlanet(rng, rng.Range(0, (int)bands.size() - 1), size, moons, ruin))
                name = IN_ORBIT[rng.Range(0, 1)];
            if (name != nullptr)
            {
                ruin["name"] = name;
                sys["derelicts"].push_back(ruin);
            }
        }
    }

    return sys;
}

// ---- Rare finds (#211) -----------------------------------------------------------------
//
// Unique things, at most one of each per region, so a find is a story players tell rather
// than loot that drops twice. Each is an archetype of an ordinary kind (#213) put in a
// system through the per-object "archetype" field (#142); the seed decides it, so everyone
// on a server is hunting the same leviathan.

std::vector<Disc> TakenOf(const json& sys)
{
    std::vector<Disc> taken;
    if (sys.contains("star"))
        taken.push_back({ 0.0, 0.0, sys["star"]["size"].get<double>() });
    if (sys.contains("stars"))
        for (const json& s : sys["stars"])
            taken.push_back(
                { s["pos"][0].get<double>(), s["pos"][1].get<double>(), s["size"].get<double>() });
    for (const char* group : { "gates", "asteroidFields", "derelicts", "stations" })
        if (sys.contains(group))
            for (const json& o : sys[group])
                if (o.contains("pos"))  // a satellite moves; its planet's path covers it
                    taken.push_back({ o["pos"][0].get<double>(), o["pos"][1].get<double>(),
                                      o["size"].get<double>() });
    return taken;
}

// The room around a finished system's planets, less what already orbits them.
Moons MoonsOf(const json& sys)
{
    Moons moons = MoonsFor(BandsOf(sys));
    for (const char* group : { "asteroidFields", "derelicts" })
        if (sys.contains(group))
            for (const json& o : sys[group])
                if (o.contains("orbits"))
                {
                    const int p = o["orbits"]["planet"].get<int>();
                    if (p >= 0 && p < (int)moons.next.size())
                        moons.next[p] =
                            std::max(moons.next[p], o["orbits"]["radius"].get<double>() +
                                                        o["size"].get<double>() + 2000.0);
                }
    return moons;
}

// Puts a find somewhere in `sys`, in a planet's path or out of it, and adds it.
void PlaceFind(Rng& rng, json& sys, const char* group, double rMin, double rMax, json find)
{
    std::vector<Disc> taken = TakenOf(sys);
    Moons             moons = MoonsOf(sys);
    if (PlaceOrOrbit(rng, rMin, rMax, find["size"].get<double>(), BandsOf(sys), moons, taken, find))
        sys[group].push_back(find);
}

// Which system gets it: deeper is likelier, and a barren system -- the one that otherwise has
// least reason to be entered -- is three times as likely as its depth alone would make it.
const Node* PickHost(Rng& rng, const std::vector<Node>& nodes, const Region& region, int minDepth)
{
    std::vector<int> weights;
    int              total = 0;
    for (const Node& n : nodes)
    {
        int w = n.depth >= minDepth ? n.depth * n.depth : 0;
        if (w > 0 && region.documents.at(n.id).value("character", "") == std::string("barren"))
            w *= 3;
        weights.push_back(w);
        total += w;
    }
    if (total == 0)
        return nullptr;
    int roll = rng.Range(0, total - 1);
    for (size_t i = 0; i < nodes.size(); i++)
    {
        if (roll < weights[i])
            return &nodes[i];
        roll -= weights[i];
    }
    return nullptr;
}

// The finds' streams, each its own: one more never moves another.
enum Find : uint64_t
{
    LEVIATHAN = 1,
    MOTHERLODE,
    HULK,
    ROGUE,
    ANCIENT_GATE,
    RICH_BELT,  // PlaceHidden's
};

void PlaceFinds(const RegionParams& params, const std::vector<Node>& nodes, Region& region)
{
    auto stream = [&](Find f) { return Rng(Key(params.seed, FINDS, (uint64_t)f)); };
    // Further from a centre, likelier (#342 S2): fewer have been out there before you.
    const int more = 8 * Richness(params);

    // Something to salvage, big.
    {
        static const char* NAMES[] = { "The Grey Leviathan", "The Sleeper", "Old Colossus",
                                       "The Drowned Giant" };
        Rng                rng = stream(LEVIATHAN);
        if (rng.Chance(45 + more))
            if (const Node* n = PickHost(rng, nodes, region, 3))
            {
                const char* name = NAMES[rng.Range(0, 3)];
                PlaceFind(rng, region.documents[n->id], "derelicts", 250000.0, 820000.0,
                          { { "name", name },
                            { "size", 2600 },
                            { "reward", 12000 + 1500 * n->depth },
                            { "archetype", "derelict.leviathan" } });
            }
    }

    // A belt worth the whole trip.
    {
        static const char* NAMES[] = { "Saint Vey's Motherlode", "The Glass Vein",
                                       "Heartstone Field", "The Bright Seam" };
        Rng                rng = stream(MOTHERLODE);
        if (rng.Chance(55 + more))
            if (const Node* n = PickHost(rng, nodes, region, 2))
            {
                const char* name = NAMES[rng.Range(0, 3)];
                PlaceFind(rng, region.documents[n->id], "asteroidFields", 260000.0, 840000.0,
                          { { "name", name },
                            { "size", 6000 },
                            { "resource", "Crystal" },
                            { "ore", 2400 + 200 * n->depth },
                            { "archetype", "field.motherlode" } });
            }
    }

    // Somebody lived here once -- and somebody may again (#44).
    {
        static const char* NAMES[] = { "Last Light Station", "Station Absent", "The Quiet Hub",
                                       "Outpost Nine" };
        Rng                rng = stream(HULK);
        if (rng.Chance(45 + more))
            if (const Node* n = PickHost(rng, nodes, region, 2))
            {
                const char* name = NAMES[rng.Range(0, 3)];
                PlaceFind(rng, region.documents[n->id], "derelicts", 250000.0, 820000.0,
                          { { "name", name },
                            { "size", 600 },
                            { "reward", 8000 + 1000 * n->depth },
                            { "archetype", "derelict.station_hulk" } });
            }
    }

    // A world without a sun of its own, at the edge, with a deposit worth the cold.
    {
        static const char* NAMES[] = { "Wanderer", "Nightfall", "The Orphan", "Coldheart" };
        Rng                rng = stream(ROGUE);
        if (rng.Chance(55 + more))
            if (const Node* n = PickHost(rng, nodes, region, 2))
            {
                json&  sys = region.documents[n->id];
                double outer = 0.0;
                for (const json& pl : sys["planets"])
                    outer = std::max(outer, pl["orbitRadius"].get<double>() +
                                                MoonZone(pl["size"].get<double>()));
                const double r = 800000.0;  // clear of the gates at 880 000 and beyond
                // Added after everything else, so its path -- and its satellites' (#210) --
                // must miss what is already there: the rule every planet keeps (Clear),
                // applied the other way round.
                bool clear = outer + MoonZone(14000.0) + 30000.0 < r;
                for (const Disc& d : TakenOf(sys))
                    if (std::fabs(std::sqrt(d.x * d.x + d.y * d.y) - r) <
                        MoonZone(14000.0) + d.size + CLEARANCE)
                        clear = false;
                if (clear)
                    sys["planets"].push_back(
                        { { "name", NAMES[rng.Range(0, 3)] },
                          { "type", "Rocky" },
                          { "size", 14000 },
                          { "orbitRadius", (int64_t)r },
                          { "orbitSpeed", 6 },  // barely held
                          { "angle", rng.Range(0, 65535) * (TWO_PI / 65536.0) },
                          { "deposit", "Crystal" },
                          { "archetype", "planet.rogue" } });
            }
    }

    // An old way through: from somewhere deep straight back to the first ring.
    {
        Rng         rng = stream(ANCIENT_GATE);
        const Node* deep = rng.Chance(30 + more) ? PickHost(rng, nodes, region, 4) : nullptr;
        const Node* near = nodes.empty() ? nullptr : &nodes.front();  // ring 1
        if (deep != nullptr && near != nullptr && deep->id != near->id)
        {
            region.links.push_back(json::array({ near->id, deep->id }));
            for (const Node* side : { near, deep })
            {
                const Node*       other = side == near ? deep : near;
                json&             sys = region.documents[side->id];
                std::vector<Disc> taken = TakenOf(sys);
                double            dx = other->mapX - side->mapX, dy = other->mapY - side->mapY;
                const double      l = std::sqrt(dx * dx + dy * dy);
                dx = l > 0.0 ? dx / l : 1.0;
                dy = l > 0.0 ? dy / l : 0.0;
                json pos;
                if (!Place(rng, 880000.0, 950000.0, 1300.0, BandsOf(sys), taken, pos, dx, dy))
                    Place(rng, 880000.0, 960000.0, 1300.0, BandsOf(sys), taken, pos);
                PlaceAnyway(pos, dx, dy, 940000.0);
                sys["gates"].push_back({ { "name", "Ancient Gate" },
                                         { "pos", pos },
                                         { "size", 1300 },
                                         { "destination", other->id },
                                         { "archetype", "gate.ancient" } });
            }
        }
    }
}

// ---- Natives (#342 S5) -----------------------------------------------------------------
//
// Beyond the first wormhole is not dark and silent. Every frontier region has a minor
// faction of its own that fits what the region is -- colonists who broke away, a pirate
// clan, a sect keeping the ruins, drone automata left running, free traders -- with one to
// three stations that dock and trade. It is in the region's documents like everything else,
// so it is sealed with the region and comes back with it from the archive.

enum class Theme
{
    Colonists,
    Clan,
    Sect,
    Automata,
    Traders,
};
constexpr int THEMES = 5;

const char* ThemeName(Theme t)
{
    switch (t)
    {
        case Theme::Colonists: return "colonists";
        case Theme::Clan: return "clan";
        case Theme::Sect: return "sect";
        case Theme::Automata: return "automata";
        case Theme::Traders: return "traders";
    }
    return "colonists";
}

// A name nobody outside the region has heard: two syllables and an ending.
std::string NativeRoot(Rng& rng)
{
    static const char* HEAD[] = { "Ab",  "Bra", "Cael", "Dov", "Esk", "Fal", "Gor", "Hav",
                                  "Ith", "Jor", "Kes",  "Lor", "Mag", "Nor", "Ost", "Pel",
                                  "Rav", "Sev", "Tor",  "Ush", "Var", "Wen", "Yev", "Zor" };
    static const char* MID[] = { "a", "e", "i", "o", "u", "ai", "ar", "el", "in", "or" };
    static const char* TAIL[] = { "n", "s", "k", "th", "ra", "ve", "rin", "dal", "mor", "ek" };
    std::string        root = HEAD[rng.Range(0, 23)];
    root += MID[rng.Range(0, 9)];
    root += TAIL[rng.Range(0, 9)];
    return root;
}

struct StationKind
{
    const char* role;
    const char* suffix;
    int         size;
};

// What each theme builds, in the order it builds it: the first is its home.
const StationKind* KindsOf(Theme t)
{
    static const StationKind COLONISTS[3] = { { "TradeHub", " Settlement", 560 },
                                              { "MiningOutpost", " Claim", 460 },
                                              { "TradeHub", " Homestead", 480 } };
    static const StationKind CLAN[3] = { { "Military", " Den", 520 },
                                         { "TradeHub", " Trading Post", 460 },
                                         { "Military", " Hold", 480 } };
    static const StationKind SECT[3] = { { "TradeHub", " Sanctum", 540 },
                                         { "Military", " Reliquary", 480 },
                                         { "TradeHub", " Hermitage", 440 } };
    static const StationKind AUTOMATA[3] = { { "MiningOutpost", " Node", 500 },
                                             { "TradeHub", " Exchange Node", 520 },
                                             { "MiningOutpost", " Relay", 440 } };
    static const StationKind TRADERS[3] = { { "TradeHub", " Trading Post", 520 },
                                            { "TradeHub", " Waystation", 460 },
                                            { "MiningOutpost", " Claim", 440 } };
    switch (t)
    {
        case Theme::Colonists: return COLONISTS;
        case Theme::Clan: return CLAN;
        case Theme::Sect: return SECT;
        case Theme::Automata: return AUTOMATA;
        case Theme::Traders: return TRADERS;
    }
    return COLONISTS;
}

std::string NativeName(Rng& rng, Theme t, const std::string& root)
{
    const int pick = rng.Range(0, 2);
    switch (t)
    {
        case Theme::Colonists:
            return pick == 0 ? "Free Colony of " + root
                             : (pick == 1 ? root + " Breakaway" : "Settlers of " + root);
        case Theme::Clan:
            return pick == 0 ? root + " Clan" : (pick == 1 ? "Clan " + root : root + " Reavers");
        case Theme::Sect:
            return pick == 0 ? "Keepers of " + root
                             : (pick == 1 ? "Order of " + root : root + " Remnant");
        case Theme::Automata:
            return pick == 0 ? root + " Automata"
                             : (pick == 1 ? "The " + root + " Swarm" : root + " Collective");
        case Theme::Traders:
            return pick == 0 ? root + " Free Traders"
                             : (pick == 1 ? root + " Compact" : root + " Exchange");
    }
    return root;
}

// How they act on the galaxy (#231): the same machinery as every faction's, with numbers that
// keep a minor faction mostly at home.
json NativeTemperament(Theme t)
{
    // appetite, capacity, growth, curiosity; traffic, ore, salvage, unclaimed
    static const double T[THEMES][8] = {
        { 0.15, 3.0, 0.08, 0.15, 0.3, 0.5, 0.1, 0.5 },  // colonists
        { 0.40, 4.0, 0.10, 0.10, 0.8, 0.1, 0.3, 0.2 },  // clan
        { 0.10, 3.0, 0.06, 0.30, 0.1, 0.1, 0.9, 0.3 },  // sect
        { 0.25, 4.0, 0.12, 0.40, 0.1, 0.9, 0.2, 0.6 },  // automata
        { 0.20, 3.0, 0.08, 0.20, 0.9, 0.2, 0.1, 0.2 },  // traders
    };
    const double* v = T[(int)t];
    return {
        { "appetite", v[0] },
        { "capacity", v[1] },
        { "growth", v[2] },
        { "curiosity", v[3] },
        { "values",
          { { "traffic", v[4] }, { "ore", v[5] }, { "salvage", v[6] }, { "unclaimed", v[7] } } }
    };
}

// A pick weighted by `weights`; -1 when every weight is zero.
int PickWeighted(Rng& rng, const std::vector<int>& weights)
{
    int total = 0;
    for (int w : weights)
        total += std::max(0, w);
    if (total == 0)
        return -1;
    int roll = rng.Range(0, total - 1);
    for (size_t i = 0; i < weights.size(); i++)
    {
        if (roll < std::max(0, weights[i]))
            return (int)i;
        roll -= std::max(0, weights[i]);
    }
    return -1;
}

int CountOf(const json& sys, const char* group, const char* archetype = nullptr)
{
    int n = 0;
    if (sys.contains(group) && sys[group].is_array())
        for (const json& o : sys[group])
            if (archetype == nullptr || o.value("archetype", std::string()) == archetype)
                n++;
    return n;
}

void PlaceNatives(const RegionParams& params, const std::vector<Node>& nodes, Region& region)
{
    if (nodes.empty())
        return;
    Rng rng(Key(params.seed, NATIVES));

    // What the region is decides who lives in it: ruins and battlefields keep a sect, ore and
    // dead rock keep machines busy, the far frontier keeps a clan.
    int graveyards = 0, belts = 0, barren = 0, ruins = 0, lawless = 0;
    for (const Node& n : nodes)
    {
        const json&       sys = region.documents.at(n.id);
        const std::string c = sys.value("character", std::string());
        graveyards += c == "graveyard" ? 1 : 0;
        belts += c == "belt cluster" ? 1 : 0;
        barren += c == "barren" || c == "frozen" ? 1 : 0;
        ruins += CountOf(sys, "derelicts", "derelict.outpost_ruin") +
                 CountOf(sys, "derelicts", "derelict.station_hulk");
    }
    for (const json& s : region.systems)
        lawless += s.value("owner", std::string()) == params.pirates ? 1 : 0;
    const int              rich = Richness(params);
    const std::vector<int> themeWeights = {
        30,                               // colonists
        8 + 8 * rich + 4 * lawless,       // a clan
        5 + 10 * graveyards + 5 * ruins,  // a sect at the ruins
        8 + 6 * belts + 4 * barren,       // automata
        20,                               // free traders
    };
    const Theme theme = (Theme)std::max(0, PickWeighted(rng, themeWeights));
    const bool  lawful = theme != Theme::Clan;

    // Their id is the region's key -- (seed, slot) -- and nothing else: what a save, the wire
    // and every other document calls them.
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)Key(params.seed, NATIVES, 1));
    const std::string id = std::string("native-") + std::string(hex).substr(0, 12);
    const std::string root = NativeRoot(rng);
    const std::string name = NativeName(rng, theme, root);
    // A colour of their own for the instruments to show them by (#117), from the theme's
    // family so a player learns to read it.
    static const int COLOURS[THEMES][3] = {
        { 120, 200, 140 }, { 210, 90, 60 }, { 190, 150, 230 }, { 110, 190, 220 }, { 225, 200, 110 }
    };
    json colour = json::array();
    for (int c = 0; c < 3; c++)
        colour.push_back(std::max(0, std::min(255, COLOURS[(int)theme][c] + rng.Range(-20, 20))));

    // Where they live: a home, then up to two more systems, a station in each, a little way
    // in and with what the theme lives on. Never the entry: the first ship through the mouth
    // finds open space, and somebody living further on.
    int deepest = 1;
    for (const Node& n : nodes)
        deepest = std::max(deepest, n.depth);
    const int                count = std::min((int)nodes.size(), rng.Range(1, 3));
    std::vector<std::string> hosts;
    for (int i = 0; i < count; i++)
    {
        std::vector<int> weights;
        for (const Node& n : nodes)
        {
            const json& sys = region.documents.at(n.id);
            int         w = n.depth == 1 ? (deepest == 1 ? 2 : 0) : (n.depth <= 3 ? 6 : 3);
            if (theme == Theme::Colonists || theme == Theme::Automata)
                w += 3 * CountOf(sys, "asteroidFields");
            if (theme == Theme::Sect)
                w += 4 * CountOf(sys, "derelicts");
            if (theme == Theme::Clan)
                w += n.depth;
            if (std::find(hosts.begin(), hosts.end(), n.id) != hosts.end())
                w = 0;
            weights.push_back(w);
        }
        const int pick = PickWeighted(rng, weights);
        if (pick < 0)
            break;
        hosts.push_back(nodes[(size_t)pick].id);
    }

    const StationKind* kinds = KindsOf(theme);
    json               systems = json::array();
    for (size_t i = 0; i < hosts.size(); i++)
    {
        json&                   sys = region.documents[hosts[i]];
        const StationKind&      k = kinds[i];
        const std::vector<Band> bands = BandsOf(sys);
        std::vector<Disc>       taken = TakenOf(sys);
        json                    pos;
        bool                    placed = false;
        // A claim by its ore; a sanctum by the ruin it keeps.
        const char* by = std::string(k.role) == "MiningOutpost" ? "asteroidFields"
                         : theme == Theme::Sect                 ? "derelicts"
                                                                : nullptr;
        if (by != nullptr && sys.contains(by))
            for (const json& o : sys[by])
                if (!placed && o.contains("pos") &&
                    PlaceNear(rng, o["pos"][0], o["pos"][1], o.value("size", 100.0), k.size, bands,
                              taken, pos))
                    placed = true;
        if (!placed && !Place(rng, 200000.0, 820000.0, k.size, bands, taken, pos) &&
            !Place(rng, 120000.0, 960000.0, k.size, bands, taken, pos))
        {
            std::vector<Disc> none;
            if (!Place(rng, 120000.0, 960000.0, k.size, bands, none, pos))
                continue;
        }
        if (!sys.contains("stations") || !sys["stations"].is_array())
            sys["stations"] = json::array();
        sys["stations"].push_back({ { "name", root + k.suffix },
                                    { "role", k.role },
                                    { "size", k.size },
                                    { "faction", id },
                                    { "pos", pos } });
        // Theirs: held, and as safe as they keep it.
        for (json& e : region.systems)
            if (e.value("id", std::string()) == hosts[i])
            {
                e["owner"] = id;
                if (lawful)
                    e["security"] = std::max(e.value("security", 0.0), 0.35);
            }
        systems.push_back(hosts[i]);
    }
    if (systems.empty())
        return;

    // What they call the places they live in (#431 slice 3): a name in their culture per
    // system, drawn from that system's own key, unique in the region.
    json               names = json::object();
    const NameCulture* culture = Factions::Culture(ThemeName(theme));
    if (culture != nullptr)
    {
        std::set<std::string> taken;
        for (const json& s : systems)
            for (const Node& n : nodes)
                if (n.id == s.get<std::string>())
                {
                    const std::string name = CultureName(
                        *culture, Key(params.seed, NATIVES, 2, (uint64_t)n.index), taken);
                    if (!name.empty())
                    {
                        taken.insert(FoldName(name));
                        names[n.id] = name;
                    }
                }
    }

    const char* majors =
        theme == Theme::Clan ? "Hostile" : (theme == Theme::Traders ? "Friendly" : "Neutral");
    region.natives = { { "id", id },
                       { "name", name },
                       { "theme", ThemeName(theme) },
                       { "color", colour },
                       { "lawful", lawful },
                       { "temperament", NativeTemperament(theme) },
                       // What they think of the registry's parts, by role: the majors of the
                       // centres, and the pirates.
                       { "relations",
                         { { "majors", majors },
                           { "pirates", theme == Theme::Clan ? "Neutral" : "Hostile" } } },
                       { "home", systems.front() },
                       { "systems", systems } };
    region.natives["culture"] = ThemeName(theme);
    if (!names.empty())
        region.natives["names"] = names;
}

}  // namespace

std::string SystemId(int slot, int depth, int k)
{
    return "r" + std::to_string(slot) + "-" + std::to_string(depth) + "-" + std::to_string(k);
}

std::string SystemDesignation(int slot, int depth, int k)
{
    return Designation(slot, depth, k);
}

namespace
{
// "r<slot>-<depth>-<k>" taken apart; false for anything else.
bool ParseId(const std::string& id, int& slot, int& depth, int& k)
{
    if (id.size() < 6 || id[0] != 'r')
        return false;
    int    parts[3] = { 0, 0, 0 };
    size_t at = 1;
    for (int i = 0; i < 3; i++)
    {
        const size_t begin = at;
        while (at < id.size() && id[at] >= '0' && id[at] <= '9')
            at++;
        if (at == begin || at - begin > 6)
            return false;
        parts[i] = std::atoi(id.c_str() + begin);
        if (i < 2)
        {
            if (at >= id.size() || id[at] != '-')
                return false;
            at++;
        }
    }
    if (at != id.size())
        return false;
    slot = parts[0];
    depth = parts[1];
    k = parts[2];
    return true;
}
}  // namespace

nlohmann::json RegionToJson(const Region& region)
{
    json docs = json::object();
    for (const auto& kv : region.documents)
        docs[kv.first] = kv.second;
    json out = { { "slot", region.slot },     { "systems", region.systems },
                 { "links", region.links },   { "documents", docs },
                 { "entry", region.entryId }, { "wormhole", region.wormhole } };
    if (!region.homeId.empty())
        out["home"] = region.homeId;
    if (!region.heartId.empty())
        out["heart"] = region.heartId;
    if (!region.natives.empty())
        out["natives"] = region.natives;
    // What a pack wove in (S7), only where there is some.
    if (!region.pack.empty())
        out["pack"] = region.pack;
    if (!region.starts.empty())
        out["starts"] = region.starts;
    if (!region.crossings.empty())
        out["crossings"] = region.crossings;
    if (!region.factions.empty())
        out["factions"] = region.factions;
    if (!region.relations.empty())
        out["relations"] = region.relations;
    return out;
}

bool RegionFromJson(const nlohmann::json& doc, Region& out)
{
    if (!doc.is_object() || !doc.contains("slot") || !doc["slot"].is_number_integer() ||
        !doc.contains("systems") || !doc["systems"].is_array() || !doc.contains("links") ||
        !doc["links"].is_array() || !doc.contains("documents") || !doc["documents"].is_object() ||
        !doc.contains("entry") || !doc["entry"].is_string() || !doc.contains("wormhole") ||
        !doc["wormhole"].is_object())
        return false;
    Region r;
    r.slot = doc["slot"].get<int>();
    r.systems = doc["systems"];
    r.links = doc["links"];
    for (auto it = doc["documents"].begin(); it != doc["documents"].end(); ++it)
        r.documents[it.key()] = it.value();
    r.entryId = doc["entry"].get<std::string>();
    r.wormhole = doc["wormhole"];
    // Absent in a region kept before centres (#342 S2): one with a wormhole came from home.
    if (doc.contains("home") && doc["home"].is_string())
        r.homeId = doc["home"].get<std::string>();
    if (doc.contains("heart") && doc["heart"].is_string())
        r.heartId = doc["heart"].get<std::string>();
    // Absent in a region sealed before natives (#342 S5): nobody lives there, and nobody will.
    if (doc.contains("natives") && doc["natives"].is_object())
        r.natives = doc["natives"];
    // Absent in a region no pack had a part in (S7).
    if (doc.contains("pack") && doc["pack"].is_string())
        r.pack = doc["pack"].get<std::string>();
    if (doc.contains("starts") && doc["starts"].is_array())
        for (const json& s : doc["starts"])
            if (s.is_string())
                r.starts.push_back(s.get<std::string>());
    for (const char* key : { "crossings", "factions", "relations" })
        if (doc.contains(key) && doc[key].is_array())
            (key[0] == 'c' ? r.crossings : key[0] == 'f' ? r.factions : r.relations) = doc[key];
    out = std::move(r);
    return true;
}

bool ReadSealedRegion(const std::string& text, uint64_t seed, Region& out)
{
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object() || j.value("schema", 0) != SEAL_SCHEMA ||
        !j.contains("seed") || !j["seed"].is_number_unsigned() ||
        j["seed"].get<uint64_t>() != seed || !j.contains("region"))
        return false;
    return RegionFromJson(j["region"], out);
}

nlohmann::json EdgeGatePos(uint64_t key, const nlohmann::json* system, double dx, double dy)
{
    Rng                     rng(key);
    const std::vector<Band> bands = system != nullptr ? BandsOf(*system) : std::vector<Band>{};
    std::vector<Disc>       taken;
    // Clear of the gates it already has, as well as of its planets' paths.
    if (system != nullptr && system->contains("gates") && (*system)["gates"].is_array())
        for (const json& g : (*system)["gates"])
            if (g.is_object() && g.contains("pos") && g["pos"].is_array() && g["pos"].size() == 2 &&
                g["pos"][0].is_number() && g["pos"][1].is_number())
                taken.push_back({ g["pos"][0].get<double>(), g["pos"][1].get<double>(),
                                  g.value("size", 900.0) });
    const double l = std::sqrt(dx * dx + dy * dy);
    dx = l > 0.0 ? dx / l : 1.0;
    dy = l > 0.0 ? dy / l : 0.0;
    json pos;
    if (!Place(rng, 880000.0, 940000.0, 900.0, bands, taken, pos, dx, dy))
        Place(rng, 880000.0, 960000.0, 900.0, bands, taken, pos);
    PlaceAnyway(pos, dx, dy, 950000.0);
    return pos;
}

int DepthOf(const std::string& id)
{
    int slot, depth, k;
    return ParseId(id, slot, depth, k) ? depth : 0;
}

int SlotOf(const std::string& id)
{
    int slot, depth, k;
    return ParseId(id, slot, depth, k) ? slot : -1;
}

FactionId RegisterNatives(const nlohmann::json& natives)
{
    if (!natives.is_object() || !natives.contains("id") || !natives["id"].is_string() ||
        natives["id"].get<std::string>().empty())
        return Factions::Unaffiliated();
    FactionDef d;
    d.id = natives["id"].get<std::string>();
    FactionId known;
    if (Factions::Find(d.id, known))
        return known;
    d.name = natives.value("name", d.id);
    d.kind = "Native";
    d.lawful = natives.value("lawful", true);
    // The culture they name in, when this registry has it (#431 slice 3).
    const std::string culture = natives.value("culture", std::string());
    if (Factions::Culture(culture) != nullptr)
        d.culture = culture;
    const json c = natives.value("color", json());
    if (c.is_array() && c.size() >= 3 && c[0].is_number() && c[1].is_number() && c[2].is_number())
        d.color = Color{ (unsigned char)std::max(0, std::min(255, c[0].get<int>())),
                         (unsigned char)std::max(0, std::min(255, c[1].get<int>())),
                         (unsigned char)std::max(0, std::min(255, c[2].get<int>())), 255 };
    const json t = natives.value("temperament", json::object());
    if (t.is_object())
    {
        Temperament& m = d.temperament;
        m.appetite = t.value("appetite", m.appetite);
        m.capacity = t.value("capacity", m.capacity);
        m.growth = t.value("growth", m.growth);
        m.curiosity = t.value("curiosity", m.curiosity);
        const json v = t.value("values", json::object());
        if (v.is_object())
        {
            m.traffic = v.value("traffic", m.traffic);
            m.ore = v.value("ore", m.ore);
            m.salvage = v.value("salvage", m.salvage);
            m.unclaimed = v.value("unclaimed", m.unclaimed);
        }
    }
    const FactionId f = Factions::Add(d);
    // What they think of the others, by the part each plays: every major, and the pirates.
    const json rel = natives.value("relations", json::object());
    Stance     majors = Stance::Neutral, pirates = Stance::Hostile;
    if (rel.is_object())
    {
        Factions::ParseStance(rel.value("majors", std::string("Neutral")), majors);
        Factions::ParseStance(rel.value("pirates", std::string("Hostile")), pirates);
    }
    for (FactionId other : Factions::All())
        if (other == Factions::Pirates())
            Factions::SetRelation(f, other, pirates);
        else if (Factions::Kind(other) == "Major")
            Factions::SetRelation(f, other, majors);
    return f;
}

// What nobody has found yet (#346): placed after the natives, who have not found it either --
// their stations go where they know of ore, and these are placed around them. Hidden until a
// pilot's scan resolves it, by the archetype's `signature`.
void PlaceHidden(const RegionParams& params, const std::vector<Node>& nodes, Region& region)
{
    auto      stream = [&](Find f) { return Rng(Key(params.seed, FINDS, (uint64_t)f)); };
    const int more = 8 * Richness(params);
    // Rich belts: one in most regions, two or three in some, deep rather than near the
    // wormhole -- crystal, given up twice as fast.
    {
        Rng       rng = stream(RICH_BELT);
        const int count = 1 + (rng.Chance(45 + more) ? 1 : 0) + (rng.Chance(15 + more) ? 1 : 0);
        std::set<std::string> hosts;
        for (int i = 0; i < count; i++)
            if (const Node* n = PickHost(rng, nodes, region, 2))
            {
                if (!hosts.insert(n->id).second)
                    continue;  // one to a system: a second roll there is a miss
                PlaceFind(rng, region.documents[n->id], "asteroidFields", 260000.0, 840000.0,
                          { { "name", n->designation + " Seam" },
                            { "size", 3500 },
                            { "resource", "Crystal" },
                            { "ore", 1200 + 150 * n->depth },
                            { "archetype", "field.rich" } });
            }
    }
}

Region GenerateRegion(const RegionParams& worldParams)
{
    // Every stream below starts from the slot's own key (#342): two slots of one world are as
    // unrelated as two worlds, and neither depends on whether the other has been made.
    RegionParams params = worldParams;
    params.seed = Key(worldParams.seed, (uint64_t)worldParams.slot, REGION);
    const bool centre = params.kind == SlotKind::Centre;

    Region region;
    region.slot = params.slot;
    std::vector<Node> nodes;
    if (centre)
    {
        // A centre opens from nowhere: its heart sits on its slot's point of the map.
        nodes = CentreTopology(params, region.links);
        double cx, cy;
        SlotMapPoint(worldParams.seed, params.slot, cx, cy);
        PlaceCentreOnMap(params, cx, cy, nodes);
        region.heartId = nodes.front().id;
        params.homeId.clear();
    }
    else
    {
        nodes = Topology(params, region.links);
        PlaceOnMap(params, nodes);
        region.homeId = params.homeId;
    }
    region.entryId = nodes.front().id;

    const Node home{ params.homeId, params.homeId, 0, -1, params.homeMap.x, params.homeMap.y };
    int        deepest = 1;
    for (const Node& n : nodes)
        deepest = std::max(deepest, n.depth);

    for (const Node& n : nodes)
    {
        Rng    rng(Key(params.seed, (uint64_t)n.index + 1000, MAP));
        double security;
        bool   pirates = false;
        if (centre)
        {
            // Policed: 1.0 at the heart, about half that at the edge (#342 S2).
            const double fall = deepest > 1 ? 0.5 * (n.depth - 1) / (deepest - 1) : 0.0;
            security =
                n.depth == 1
                    ? 1.0
                    : std::min(1.0,
                               std::max(0.5, Round(1.0 - fall + rng.Range(-2, 2) * 0.01, 0.01)));
        }
        else
        {
            // Safer near home, lawless at the edge; the frontier is nobody's -- and the
            // further it is from a centre, the less law it starts with (#342 S2).
            const double base = 0.45 - 0.12 * (params.hops - 1);
            security =
                std::max(0.0, Round(base - 0.1 * (n.depth - 1) + rng.Range(-5, 5) * 0.01, 0.01));
            pirates = n.depth >= 3 && rng.Chance(30 + 5 * Richness(params));
        }
        region.systems.push_back(
            { { "id", n.id },
              { "name", n.designation },
              { "map", json::array({ (int64_t)n.mapX, (int64_t)n.mapY }) },
              { "security", security },
              { "owner", centre ? params.owner : (pirates ? params.pirates : std::string()) } });
        region.documents[n.id] =
            GenerateSystem(params, n, nodes, region.links, centre ? nullptr : &home);
    }
    if (centre)
        return region;  // settled long ago: whatever was rare here is in a museum
    PlaceFinds(params, nodes, region);
    PlaceNatives(params, nodes, region);
    PlaceHidden(params, nodes, region);

    // The wormhole's mouth in the home system: at the edge, facing the region, out of the
    // path of the home system's own planets.
    {
        Rng                     rng(Key(params.seed, WORMHOLE));
        const std::vector<Band> bands =
            params.homeSystem != nullptr ? BandsOf(*params.homeSystem) : std::vector<Band>{};
        std::vector<Disc> taken;
        double            dx = nodes.front().mapX - home.mapX, dy = nodes.front().mapY - home.mapY;
        const double      l = std::sqrt(dx * dx + dy * dy);
        dx = l > 0.0 ? dx / l : 1.0;
        dy = l > 0.0 ? dy / l : 0.0;
        json pos;
        if (!Place(rng, 880000.0, 940000.0, 900.0, bands, taken, pos, dx, dy))
            Place(rng, 880000.0, 960000.0, 900.0, bands, taken, pos);
        PlaceAnyway(pos, dx, dy, 950000.0);
        region.wormhole = { { "name", "Wormhole" },
                            { "pos", pos },
                            { "size", 900 },
                            { "destination", region.entryId } };
    }
    return region;
}

RegionParams SlotParams(const GalaxySkeleton& skeleton, int slot)
{
    RegionParams p;
    p.seed = skeleton.seed;
    p.slot = slot;
    if (slot >= 0 && slot < (int)skeleton.slots.size())
    {
        const Slot& s = skeleton.slots[(size_t)slot];
        p.kind = s.kind;
        p.hops = s.hops;
        p.owner = s.owner;
    }
    p.pirates = skeleton.pirates;
    p.unaffiliated = skeleton.unaffiliated;
    return p;
}

std::string MouthSystem(const Region& from, double towardX, double towardY)
{
    // The edge of the region that faces the new one. Among equals, the lower id: the index
    // is in generation order, and so is this.
    int depth = 0;
    for (const json& s : from.systems)
        depth = std::max(depth, DepthOf(s.value("id", std::string())));
    std::string best;
    double      bestD = 0.0;
    for (const json& s : from.systems)
    {
        const std::string id = s.value("id", std::string());
        if (DepthOf(id) != depth || !s.contains("map") || !s["map"].is_array())
            continue;
        const double dx = s["map"][0].get<double>() - towardX;
        const double dy = s["map"][1].get<double>() - towardY;
        const double d = dx * dx + dy * dy;
        if (best.empty() || d < bestD)
        {
            best = id;
            bestD = d;
        }
    }
    return best;
}

void HomeFrom(RegionParams& params, const GalaxySkeleton& skeleton, const Region& from,
              const std::string& homeId, const nlohmann::json& homeDoc)
{
    params.homeId = homeId;
    for (const json& s : from.systems)
        if (s.value("id", std::string()) == homeId && s.contains("map") && s["map"].is_array())
            params.homeMap = { s["map"][0].get<float>(), s["map"][1].get<float>() };
    double hx, hy, sx, sy;
    SlotMapPoint(skeleton.seed, from.slot, hx, hy);
    SlotMapPoint(skeleton.seed, params.slot, sx, sy);
    params.bearing = { (float)(sx - hx), (float)(sy - hy) };
    params.homeSystem = homeDoc.is_object() ? &homeDoc : nullptr;
}

std::vector<Region> GenerateWorld(const GalaxySkeleton&               skeleton,
                                  const std::function<void(Region&)>& pin)
{
    std::vector<Region> out;
    for (const Opening& o : InitialOpenings(skeleton))
    {
        RegionParams params = SlotParams(skeleton, o.slot);
        json         homeDoc;
        if (o.from >= 0)
        {
            const Region* from = nullptr;
            for (const Region& r : out)
                if (r.slot == o.from)
                    from = &r;
            if (from == nullptr)
                continue;
            double tx, ty;
            SlotMapPoint(skeleton.seed, o.slot, tx, ty);
            const std::string homeId = MouthSystem(*from, tx, ty);
            const auto        doc = from->documents.find(homeId);
            if (doc != from->documents.end())
                homeDoc = doc->second;
            HomeFrom(params, skeleton, *from, homeId, homeDoc);
        }
        out.push_back(GenerateRegion(params));
        if (pin)
            pin(out.back());
    }
    return out;
}

}  // namespace Gen
