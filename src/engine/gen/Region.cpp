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

bool Clear(double x, double y, double size, const std::vector<Band>& bands,
           const std::vector<Disc>& taken)
{
    const double r = std::sqrt(x * x + y * y);
    for (const Band& b : bands)
        if (std::fabs(r - b.radius) < b.size + size + CLEARANCE)
            return false;
    for (const Disc& d : taken)
    {
        const double dx = x - d.x, dy = y - d.y;
        const double need = d.size + size + 20000.0;
        if (dx * dx + dy * dy < need * need)
            return false;
    }
    return true;
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
                                { "orbitSpeed", rng.Range(280, 520) },
                                { "angle", rng.Range(0, 65535) * (TWO_PI / 65536.0) },
                                { "deposit", DepositOf(type) } });
            bands.push_back({ r, (double)size });
            r += size + rng.Range(70, 150) * 1000.0;
        }
        sys["planets"] = planets;
    }

    std::vector<Disc> taken;

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
        for (int i = 0; i < count; i++)
        {
            const int   roll = rng.Range(0, 99);
            const char* res = roll < 15 + 12 * n.depth ? "Crystal" : (roll < 60 ? "Iron" : "Ice");
            if (character == Character::Frozen)
                res = roll < 75 ? "Ice" : "Crystal";
            const int size = rng.Range(50, 75) * 100;
            json      pos;
            if (!Place(rng, 260000.0, 840000.0, size, bands, taken, pos))
                continue;
            belts.push_back(
                { { "name", n.designation + " Belt " + std::string(1, (char)('A' + i)) },
                  { "pos", pos },
                  { "size", size },
                  { "resource", res },
                  { "ore", rng.Range(200, 400) + 40 * n.depth +
                               (character == Character::BeltCluster ? 100 : 0) } });
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
            const int chance = character == Character::Barren ? 15 : 25 + 12 * n.depth;
            const int count = rng.Chance(chance) ? rng.Range(1, 2) : 0;
            for (int i = 0; i < count; i++)
            {
                const int size = rng.Range(30, 50);
                json      pos;
                if (!Place(rng, 200000.0, 850000.0, size, bands, taken, pos))
                    continue;
                wrecks.push_back({ { "name", NAMES[rng.Range(0, 4)] },
                                   { "pos", pos },
                                   { "size", size },
                                   { "reward", 1000 + 500 * n.depth + rng.Range(0, 8) * 100 } });
            }
        }
        sys["derelicts"] = wrecks;
    }

    sys["stations"] = json::array();  // nobody has built anything out here yet
    return sys;
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
