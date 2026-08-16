#pragma once

#include "entities/EntityKind.h"
#include "raylib.h"
#include <string>
#include <vector>

// What a kind of object IS, stated as data rather than implied by a C++ class (#34).
//
// The class hierarchy answers "what can this do?" by identity: a thing is dockable
// because it is a `Station`. That has two costs the project can no longer pay. A player
// cannot build a new kind of object (#44), because a new kind means a new class and a
// recompile. And an agent (#42) cannot reason about an object it has never been told
// about, because the capability lives in the type system rather than in anything it can
// read.
//
// An archetype turns both around. It names a kind of object, says how it looks, and
// lists the components it has. Simulation passes ask for components — "everything
// dockable within range" — so a newly invented object joins those passes by declaring a
// component, not by editing them.
//
// This is deliberately NOT a general entity-component system. Entities keep their
// classes for now; the archetype is the vocabulary the two directions above need, and
// the hierarchy is migrated onto it pass by pass.

// What an object can do. Each value is a capability some simulation pass looks for.
enum class Component
{
    Dockable,     // a ship can dock with it
    Mineable,     // holds a deposit that can be extracted
    Market,       // buys and sells resources
    Defensive,    // fires on hostiles within range
    Storage,      // holds cargo that is not aboard a ship
    JumpLink,     // connects this system to another
    Hazard,       // changes the conditions of anything inside it
    Salvageable,  // pays out once to whoever reaches it first
    Buildable     // a player can construct one (#44)
};

// The JSON key this component is written under, and the name it is reported by.
const char* ComponentName(Component c);

// All components, in declaration order — for iterating without hard-coding the list.
const std::vector<Component>& AllComponents();

// A set of components as a bitmask. Small enough to copy, cheap enough to ask in a
// per-tick loop, which is the whole reason the passes are allowed to query it.
class ComponentSet
{
public:
    bool Has(Component c) const { return (bits_ & Bit(c)) != 0u; }
    void Add(Component c) { bits_ |= Bit(c); }
    bool Empty() const { return bits_ == 0u; }

    unsigned int Bits() const { return bits_; }

private:
    static unsigned int Bit(Component c) { return 1u << static_cast<unsigned int>(c); }

    unsigned int bits_ = 0u;
};

// How an object looks, kept separate from who draws it (#35). A glyph backend, a text
// backend and the existing shape/sprite path all read the same three fields; none of
// them needs the object's C++ type.
// How a glyph occupies space. This is the grammar of the ASCII look (#36), and it is
// three values rather than one because a nebula three thousand units across is not the
// same kind of thing as a ship sixteen units across, and drawing both as one character
// scaled to fit makes the larger one unreadable.
enum class GlyphStyle
{
    Point,       // one character, sized from the object — a star, a planet, a station
    Region,      // an area: the character scattered around its extent — a nebula, a belt
    Directional  // a character turned to face the way the object is heading — a ship
};

struct Visual
{
    std::string glyph = "?";  // one character in the ASCII presentation
    std::string sprite;       // texture name for the sprite backend; empty means shapes only
    Color       color = { 255, 255, 255, 255 };
    int         layer = 0;  // draw order, lowest first
    GlyphStyle  style = GlyphStyle::Point;
};

// One entry of the registry.
struct Archetype
{
    std::string id;    // "station.trade_hub" — how world data refers to this
    std::string name;  // "Trade Hub" — what a player is shown
    EntityKind  kind = EntityKind::Unknown;

    Visual visual;
    float  defaultSize = 0.0f;  // used when the instance does not give its own

    // Where this archetype lives in a system file, and what it is called there.
    // `worldCategory` is the JSON array ("stations", "gates", …); `worldSubType` is the
    // value of that category's type key ("Military", "Ice"). An empty category means the
    // archetype is not something an editor or a player places — a star is one per system,
    // a ship is not scenery.
    //
    // This is a bridge, and it disappears when system files name archetypes by id
    // directly (#34). Until then it is what lets the editor palette come from the
    // registry rather than from a hard-coded list.
    std::string worldCategory;
    std::string worldSubType;

    bool Placeable() const { return !worldCategory.empty(); }

    ComponentSet components;

    // Component parameters, flat and typed. A property bag of string→double would have
    // been shorter to write and would have moved the guessing rather than removed it:
    // the point of putting object types in data is that what a thing can do stops being
    // something the reader has to infer.
    float  dockRange = 0.0f;          // Dockable: added to the object's radius
    float  extractRate = 0.0f;        // Mineable: units per second at skill 1
    float  weaponRange = 0.0f;        // Defensive
    float  weaponDamage = 0.0f;       // Defensive: per second
    float  storageCapacity = 0.0f;    // Storage
    float  hazardRadius = 0.0f;       // Hazard: 0 means "the object's own radius"
    bool   hazardHidesShips = false;  // Hazard: ships inside are invisible to NPCs
    double buildCost = 0.0;           // Buildable
    float  buildSeconds = 0.0f;       // Buildable

    bool Has(Component c) const { return components.Has(c); }
};

// The registry is process-wide and loaded once, like Factions. Every executable that
// touches the world needs it, and an archetype is immutable reference data — there is
// nothing per-instance to own it.
namespace Archetypes
{
// Reads data/archetypes.json. Returns false and leaves the previous contents in place
// if the file is missing or malformed; Error() then says what went wrong. Callers that
// cannot run without a world should treat that as fatal rather than limping on with an
// empty registry.
bool Load(const std::string& path);

// Null when no archetype has that id. Callers are expected to check: an unknown id in
// world data is a content error, and silently substituting a default would hide it.
const Archetype* Find(const std::string& id);

// Every archetype declaring the given component.
std::vector<const Archetype*> With(Component c);

// Every archetype of the given kind, in file order.
std::vector<const Archetype*> OfKind(EntityKind kind);

const std::vector<Archetype>& All();

// Why the last Load() failed. Empty after a successful load.
const std::string& Error();
}  // namespace Archetypes
