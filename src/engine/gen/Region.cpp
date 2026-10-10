#include "gen/Region.h"
#include "gen/Rng.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

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
};

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
};

std::string Designation(int depth, int k)
{
    return "W-" + std::to_string(depth) + "." + std::to_string(k);
}

std::string IdFor(int depth, int k)
{
    return "w" + std::to_string(depth) + "-" + std::to_string(k);
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
    nodes.push_back({ IdFor(1, 1), Designation(1, 1), 1, 0 });
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
            Node         n{ IdFor(depth, k), Designation(depth, k), depth, (int)nodes.size() };
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
    const double dx = DIRS[d][0], dy = DIRS[d][1];
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
    json           sys;

    Character character;
    {
        Rng rng(Key(params.seed, key, CHARACTER));
        character = PickCharacter(rng, n.depth);
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
                          (character == Character::BeltCluster ? 100 : 0);
            belts.push_back(belt);
        }
        sys["asteroidFields"] = belts;
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
        const int          chance = character == Character::Barren ? 20 : 4 + 4 * n.depth;
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

    sys["stations"] = json::array();  // nobody has built anything out here yet
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
    for (const char* group : { "gates", "asteroidFields", "derelicts" })
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

void PlaceFinds(const RegionParams& params, const std::vector<Node>& nodes, Region& region)
{
    enum Find : uint64_t
    {
        LEVIATHAN = 1,
        MOTHERLODE,
        HULK,
        ROGUE,
        ANCIENT_GATE,
    };
    auto stream = [&](Find f) { return Rng(Key(params.seed, FINDS, (uint64_t)f)); };

    // Something to salvage, big.
    {
        static const char* NAMES[] = { "The Grey Leviathan", "The Sleeper", "Old Colossus",
                                       "The Drowned Giant" };
        Rng                rng = stream(LEVIATHAN);
        if (rng.Chance(45))
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
        if (rng.Chance(55))
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
        if (rng.Chance(45))
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
        if (rng.Chance(55))
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
        const Node* deep = rng.Chance(30) ? PickHost(rng, nodes, region, 4) : nullptr;
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

}  // namespace

int DepthOf(const std::string& id)
{
    if (id.size() < 2 || id[0] != 'w')
        return 0;
    return std::atoi(id.c_str() + 1);
}

Region GenerateRegion(const RegionParams& params)
{
    Region            region;
    std::vector<Node> nodes = Topology(params, region.links);
    PlaceOnMap(params, nodes);
    region.entryId = nodes.front().id;

    const Node home{ params.homeId, params.homeId, 0, -1, params.homeMap.x, params.homeMap.y };

    for (const Node& n : nodes)
    {
        // Safer near home, lawless at the edge; the frontier is nobody's.
        Rng          rng(Key(params.seed, (uint64_t)n.index + 1000, MAP));
        const double security =
            std::max(0.0, Round(0.45 - 0.1 * (n.depth - 1) + rng.Range(-5, 5) * 0.01, 0.01));
        const bool pirates = n.depth >= 3 && rng.Chance(30);
        region.systems.push_back({ { "id", n.id },
                                   { "name", n.designation },
                                   { "map", json::array({ (int64_t)n.mapX, (int64_t)n.mapY }) },
                                   { "security", security },
                                   { "owner", pirates ? "Pirates" : "" } });
        region.documents[n.id] = GenerateSystem(params, n, nodes, region.links, &home);
    }
    PlaceFinds(params, nodes, region);

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

}  // namespace Gen
