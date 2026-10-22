#pragma once

#include "core/Faction.h"

#include <cstdint>
#include <set>
#include <string>

// Names in a people's own tongue (#431 slice 3). A native culture names the systems its
// people live in, and a major faction names the empty systems it is the first to chart; both
// speak a NameCulture from factions.json's `cultures`. The generator's rules hold here as in
// Region.cpp (Rng.h says why): randomness only from Gen::Rng keyed by what is named, no
// distributions, no unordered containers -- a native name is sealed with its region, and a
// charted name must come out the same on every server that charts the same system.
namespace Gen
{

// FNV-1a of a text: a key from an id, the same on every platform (std::hash is not).
uint64_t TextKey(const std::string& text);

// A name folded for comparison: ASCII lower case. Names are unique regardless of case (N3).
std::string FoldName(const std::string& name);

// True when a generated name contains a word nobody should find a system called -- a short
// list, matched in the folded name with its spaces, hyphens and apostrophes taken out. A
// generator throws such a name away and draws again; an admin's own filter is slice 4's.
bool NameBlocked(const std::string& name);

// A name in `culture`, drawn from `key`: the shape a player's name must have (3 to 24
// characters, a letter first; Names::ValidSystemName), never blocked, never one of `taken`
// (folded names). Empty only when every draw failed, which a culture of any size never does.
std::string CultureName(const NameCulture& culture, uint64_t key,
                        const std::set<std::string>& taken = {});

// What `faction` calls an empty system it is the first to chart (decided after the rounds,
// 1): a name in its culture, drawn from (world seed, system id, faction), so every server
// that charts the same system with the same faction names it alike. Never one of `taken`
// (folded names -- the world's, for uniqueness). Empty for a faction with no culture, which
// leaves the designation. Slice 2's ChartBy calls it.
std::string ChartedName(FactionId faction, uint64_t worldSeed, const std::string& systemId,
                        const std::set<std::string>& taken = {});

}  // namespace Gen
