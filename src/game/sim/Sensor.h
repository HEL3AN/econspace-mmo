#pragma once

#include "core/Faction.h"
#include "render/Scene.h"
#include "sim/Protocol.h"
#include <functional>
#include <vector>

// The sensor screen (#123): the system around the ship projected onto a fixed grid of
// characters, where a cell holds whatever is standing in it.
//
// This is where glyphs are genuinely good. A grid is a readout, not a picture -- nothing in
// the flight is quantised to it, it is only how the instrument shows what it sees -- so it
// stays honest at any range, where a glyph stretched to an object's size in the world did
// not. And because it is an instrument, colour here means allegiance as *this* player sees
// it (#117): the same station is green on one screen and red on another, which is right for
// a reading and wrong for an object.
//
// The half with no drawing in it lives here, so a test can hold what lands where and what
// colour it reads as. The client draws the Picture.
namespace Sensor
{

// Whose a thing is, as the viewer sees it. `Unowned` is a fact about the thing rather than
// a reading: a star or a belt has no side to be on.
enum class Allegiance
{
    Own,       // the viewer's own ship
    Friendly,  // a faction that likes the viewer
    Neutral,   // everyone else with a side -- other pilots included, until they have standings
    Hostile,   // would shoot at the viewer
    Unowned    // stars, planets, belts, clouds, wrecks, gates
};

// The viewer's standing, as the client already knows it: whether a faction would shoot,
// and how it regards them otherwise.
struct Standing
{
    std::function<bool(FactionId)>    hostile;
    std::function<RepTier(FactionId)> tier;
};

// How one thing in the snapshot reads to this viewer.
Allegiance Classify(const Proto::EntitySnapshot& e, const Standing& viewer);

// The instrument's colours. Hostile is the radar's and the overview's red, so one reading
// is one colour in every instrument.
Color       ColorOf(Allegiance a);
const char* Word(Allegiance a);

// What a glyph stands for, for the legend: the class of thing, not its name.
const char* KindWord(EntityKind k);

// The ranges the screen steps through, in world units across the grid: from a ship's own
// surroundings out to a whole system, which is two million across (#159).
const std::vector<float>& Ranges();
float                     DefaultRange();
// `steps` ranges further out (positive) or in (negative) from `current`, clamped to the ends.
float StepRange(float current, int steps);

struct Cell
{
    char       glyph = ' ';
    int        id = 0;  // 0: empty, or the viewer's own ship
    Allegiance allegiance = Allegiance::Unowned;
};

struct LegendEntry
{
    char        glyph;
    const char* kind;
};

struct Picture
{
    int               width = 0;
    int               height = 0;
    float             span = 0.0f;  // world units across the grid
    std::vector<Cell> cells;        // row-major
    // One line per glyph on the screen, in the order first seen. Only what is in view: a
    // key to characters that are not there is noise.
    std::vector<LegendEntry> legend;

    const Cell& At(int x, int y) const;
    float       UnitsPerCell() const { return span / (float)(width < 1 ? 1 : width); }
};

// How each id in the scene reads; the client answers it from the snapshot by Classify.
using AllegianceOf = std::function<Allegiance(int id)>;

// Projects `items` onto a `width` x `height` grid `span` units across, centred on `own` --
// the viewer's ship, which always occupies the centre cell. Items outside the grid are
// dropped rather than piled on its edge (GridBackend's rule), and where two share a cell the
// higher layer wins (Present's rule). The world colour of an item is never read.
Picture Scan(std::vector<Render::Item> items, const Render::Item& own, int width, int height,
             float span, const AllegianceOf& allegianceOf);

}  // namespace Sensor
