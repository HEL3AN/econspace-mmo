#pragma once

#include "gen/Region.h"
#include <nlohmann/json.hpp>
#include <map>
#include <string>
#include <vector>

// Hand-written exceptions on top of a generated region (#147): the generated layer is the
// default, not a monopoly. A starting point, a set piece, anything a mission depends on is
// a pin. They are applied AFTER generation, always -- a pin that silently lost to the
// generator would be a bug nobody could see -- and a pin that cannot apply says so.
//
// data/pins.json:
//   { "pins": [ { "system": "w1-1", "seed": 7, "mode": "merge", "document": { ... } } ] }
//
//   system    the generated system's id; required.
//   seed      only for the region this seed makes; absent, for every seed.
//   mode      "merge" (default): keep the generated system and add to it -- an array
//             (planets, stations, asteroidFields, nebulae, derelicts, gates) appends its
//             objects, and an object naming "replaces": "<name>" takes the place of the
//             one called that (with "remove": true, removes it). "replaces": N names the
//             N-th object instead, which is how a nameless one -- a generated planet -- is
//             reached. Both are read against the list as it stood before this pin. Any
//             other key (star, stars, character) is set.
//             "replace": the document IS the system. Its gates are kept from the generated
//             one unless it lists its own, because they are the region's topology.
namespace Gen
{
// Applies every pin that matches `seed` to `region`, in file order. Each problem -- an
// unknown system, mode or field, a "replaces" with nothing to replace -- is one line in
// `problems`, and that pin (or that object) is not applied.
void ApplyPins(Region& region, const nlohmann::json& pins, uint64_t seed,
               std::vector<std::string>& problems);

// ---- The editor's side (#237): a system opened for editing, saved as its difference ----
//
// For each list of a system document, where each of its objects came from: the index of
// the object it was in the document the editing started from, or -1 for one added since.
// That is what lets a difference say "this one, moved" instead of "one removed, one added"
// -- and it is the only way to say it about a planet, which has no name.
using Origins = std::map<std::string, std::vector<int>>;

// Every object comes from itself.
Origins IdentityOrigins(const nlohmann::json& doc);

// A system opened for editing. `base` is what the generator made of it with every pin the
// editor does not own on top; `edited` adds the pins it does own -- those for exactly
// this system and this seed, which saving rewrites as one.
struct EditedSystem
{
    nlohmann::json           base;
    nlohmann::json           edited;
    Origins                  origins;  // edited's objects, in base
    std::vector<std::string> problems;
};

// Whether the editor writes this pin: it names `system` and exactly `seed`.
bool OwnedPin(const nlohmann::json& pin, const std::string& system, uint64_t seed);

// Opens `system` of a freshly generated (unpinned) region. False if it has no such system.
bool OpenForEditing(const Region& generated, const nlohmann::json& pins, uint64_t seed,
                    const std::string& system, EditedSystem& out);

// The pin that makes `base` into `edited`: a "merge" when the difference can be said as
// one (objects added, removed or changed, keys set), a "replace" when it cannot (a key
// taken away). Null when there is no difference -- nothing to pin.
nlohmann::json PinFor(const std::string& system, uint64_t seed, const nlohmann::json& base,
                      const nlohmann::json& edited, const Origins& origins);

// Puts `pin` into the pins file in place of every pin the editor owns for that system and
// seed (or only removes those, for a null pin). Pins written by hand are left alone.
void StorePin(nlohmann::json& pinsFile, const std::string& system, uint64_t seed,
              const nlohmann::json& pin);
}  // namespace Gen
