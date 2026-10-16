#pragma once

#include "raylib.h"
#include <nlohmann/json_fwd.hpp>
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

// The game's factions (#355). Stations and NPC ships belong to them; the player has a
// reputation with each. They are data: a registry read from data/factions.json, to which a
// generated region's natives and an admin's pack add their own at runtime (#342). Nothing
// in the code names one -- the few parts the world gives a faction to play (who owns what
// nobody owns, who the pirates are, whose police patrol a lawless system, who trades
// everywhere) are `roles` in
// the same file.
//
// A FactionId is a handle: an index into this process's registry, valid for as long as the
// process runs and meaningless outside it. A save and the wire name a faction by its string
// id (`Factions::Id`), never by the index -- the order of a file, or of what was generated
// first, is not something another process shares.
class FactionId
{
public:
    // The unaffiliated faction: what owns what nobody owns (the first in the registry).
    constexpr FactionId() = default;
    static constexpr FactionId FromIndex(int index) { return FactionId((uint16_t)index); }

    constexpr int Index() const { return index_; }

    constexpr bool operator==(FactionId o) const { return index_ == o.index_; }
    constexpr bool operator!=(FactionId o) const { return index_ != o.index_; }
    constexpr bool operator<(FactionId o) const { return index_ < o.index_; }

private:
    constexpr explicit FactionId(uint16_t index) : index_(index) {}
    uint16_t index_ = 0;
};

// How a faction acts on the galaxy (#231): the same machinery for every faction, with only
// this differing, so a new faction is data. A faction reaches into a neighbouring system
// when what it values there outweighs the risk by its own appetite for risk.
struct Temperament
{
    float appetite = 0.0f;  // how bold: 0 never reaches out; 1 takes an even bet
    float capacity = 4.0f;  // the strength (ships) one of its systems sustains
    float growth = 0.1f;    // how fast its strength in a system it holds recovers, per period
    // What it is after, as weights on what a system offers (each 0..1).
    float traffic = 0.0f;    // ships and trade passing through: prey, or customers
    float ore = 0.0f;        // belts to mine
    float salvage = 0.0f;    // wrecks, ruins, finds
    float unclaimed = 0.0f;  // a system nobody holds
    // What finding out is worth to it (#295): the value of surveying a system it knows
    // nothing about. 0 never looks further than it can see.
    float curiosity = 0.0f;
};

// Ships a system has more of than it keeps leave for a neighbour that lacks them (#295): the
// `migration` section of factions.json. For each role, at most one ship leaves a system per
// that many seconds (0: that role never leaves); `travel` is how long the journey takes from
// the moment it sets out, and `align` the longest a real ship, where somebody is to see it,
// takes to warp to the gate before it is gone -- it goes through as soon as it is there.
struct Migration
{
    float trader = 240.0f;
    float miner = 360.0f;
    float patrol = 300.0f;
    float pirate = 300.0f;
    float travel = 90.0f;
    float align = 40.0f;
};

// Stance between two factions (for combat/hostility logic).
enum class Stance
{
    War,
    Hostile,
    Neutral,
    Friendly,
    Ally
};

// Player's reputation tier with a faction (by numeric thresholds from factions.json).
enum class RepTier
{
    Hated,
    Hostile,
    Neutral,
    Liked,
    Allied
};

// One faction as the registry holds it: an entry of factions.json, or one added at runtime.
struct FactionDef
{
    std::string id;    // stable: what saves, the wire and other data files call it
    std::string name;  // what a player reads
    Color       color = Color{ 200, 200, 200, 255 };
    bool        lawful = true;
    std::string kind;  // "Independent", "Major", "Pirate", ... -- descriptive
    Temperament temperament;
};

std::string FactionName(FactionId faction);
Color       FactionColor(FactionId faction);

// Faction registry: data and relations from JSON.
namespace Factions
{
// Reads factions.json and replaces the registry with it. Strict (#191, #357): a missing or
// unreadable file, an unknown field, a faction named twice, a relation or role naming a
// faction that is not there, or a stance that is not one -- each is an error, and the
// registry is left as it was. Before any load the registry holds the four factions of the
// original game with default temperaments, so a tool or a test that loads nothing still
// has a world to work with.
bool               Load(const std::string& path);
const std::string& Error();  // why the last Load failed

// Reads one entry of factions.json's `factions` as Load does, as strictly: an admin pack
// (#342 S7) writes its factions in the same format. False with `error` saying why.
bool ReadDef(const nlohmann::json& entry, FactionDef& out, std::string& error);

// Adds a faction at runtime (#342: a generated region's natives, a pack's factions) and
// returns its handle; an id already there returns that one, unchanged. A new faction is
// neutral to everyone but the pirates, as an unlisted pair in the file is.
FactionId Add(const FactionDef& def);

int  Count();
bool Find(const std::string& id, FactionId& out);  // by string id; false if there is none
// Everything the registry holds about one faction.
const FactionDef& Def(FactionId f);
// Sets the stance between two factions, both ways: what a faction added at runtime (#342 S5)
// thinks of the others, and what a client is told the server's registry says.
void SetRelation(FactionId a, FactionId b, Stance stance);
// A stance by the name factions.json writes it with ("War" .. "Ally"), and back.
const char* StanceName(Stance s);
bool        ParseStance(const std::string& s, Stance& out);

// Every faction in registry order, for `for (FactionId f : Factions::All())`. The order is
// the order of the file, then of what was added -- the same on every run, which is what a
// deterministic world steps by.
class Range
{
public:
    class Iterator
    {
    public:
        explicit Iterator(int i) : i_(i) {}
        FactionId operator*() const { return FactionId::FromIndex(i_); }
        Iterator& operator++()
        {
            ++i_;
            return *this;
        }
        bool operator!=(const Iterator& o) const { return i_ != o.i_; }

    private:
        int i_;
    };
    Iterator begin() const { return Iterator(0); }
    Iterator end() const { return Iterator(Count()); }
};
inline Range All()
{
    return Range();
}

// The parts the world gives a faction to play (`roles` in factions.json).
FactionId Unaffiliated();   // owns what nobody owns; the default handle
FactionId Pirates();        // spawns as the pirates, and is what unrest feeds
FactionId DefaultPolice();  // patrols a system whose own holder is lawless
FactionId Traders();        // trades on every lane, beside the system's own and the unaffiliated

// The faction a save from before #355 meant by index `i` -- those wrote the compiled order
// Independent, TradersGuild, Syndicate, Pirates. False when it is out of range or the
// registry no longer has that faction.
bool FromLegacyIndex(int i, FactionId& out);

const std::string& Id(FactionId f);  // string id (as used in station data and saves)
bool               IsLawful(FactionId f);
const std::string& Kind(FactionId f);
// The ids of the factions of kind "Major", in registry order: who owns a civilisation
// centre (#342). Never the pirates, whatever their kind: they have no centre.
std::vector<std::string> MajorIds();
Stance                   Relation(FactionId a, FactionId b);
const Temperament&       TemperamentOf(FactionId f);
const Migration&         MigrationRules();

// Player reputation tiers.
RepTier     TierOf(float rep);
std::string TierName(RepTier t);
Color       TierColor(RepTier t);
}  // namespace Factions

// A value for each faction (#355), indexed by its handle: sized by the registry, and growing
// with it, so a faction added at runtime reads as `T{}` everywhere until something is
// written for it. Iterate with `Factions::All()`.
template <class T> class PerFaction
{
public:
    T& operator[](FactionId f)
    {
        if ((size_t)f.Index() >= values_.size())
            values_.resize((size_t)std::max(f.Index() + 1, Factions::Count()));
        return values_[(size_t)f.Index()];
    }
    const T& operator[](FactionId f) const
    {
        static const T none{};
        return (size_t)f.Index() < values_.size() ? values_[(size_t)f.Index()] : none;
    }
    // Every faction back to T{}.
    void Clear() { values_.clear(); }
    bool operator==(const PerFaction& o) const
    {
        for (FactionId f : Factions::All())
            if (!((*this)[f] == o[f]))
                return false;
        return true;
    }

private:
    std::vector<T> values_;
};
