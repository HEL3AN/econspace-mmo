#pragma once

#include <map>
#include <string>
#include <vector>

// The world's own events that leave an ore field behind (#617, documents/mining_and_fields.md),
// read from data/events.json: a registry like factions.json, where a kind is data and the code
// names none. A kind says what it needs in a system, what it leaves and how much of the region's
// ore bank it spends, how long the field lives and what the news says. Strict, as every content
// file is (#191): an unknown field, or an archetype that is not a field, is a load error.
namespace FieldEvents
{
struct Kind
{
    std::string id;
    std::string name;
    double      weight = 1.0;
    std::string needs;  // "planet": an Impact; "belt": a Collision
    // What it is made of, by ore id and weight; empty for a kind that moves a belt's own ore.
    std::map<std::string, double> ores;
    double                        shareLo = 1.0, shareHi = 1.0;  // of the bank, when it spends it
    double                        moves = 0.0;  // of a belt's ore, when it moves some
    double                        hoursLo = 1.0, hoursHi = 1.0;  // its life
    double                        sizeLo = 500.0, sizeHi = 500.0;
    // A satellite of the planet it struck: its distance in planet radii, and its speed round
    // it in radians a second.
    double      orbitRadius = 0.0, orbitSpeed = 0.0;
    std::string field;  // what the field is called
    // Large ones are seen by everyone; small ones are a signature, found by a scan (#346).
    std::string publicArchetype, signatureArchetype;
    std::string line;  // for the news: {planet} and {system}
};

// Reads the file after the archetypes, which it is checked against. False, with Error(), and
// nothing changed when it does not read.
bool                     Load(const std::string& path);
const std::string&       Error();
const std::vector<Kind>& Kinds();
const Kind*              Find(const std::string& id);
// Their ids, each after a space, for a message.
std::string Names();
// What the file hashed to, so a world can say which events it was made with.
const std::string& Hash();
// For a test: kinds of its own, without a file.
void Set(const std::vector<Kind>& kinds);
}  // namespace FieldEvents
