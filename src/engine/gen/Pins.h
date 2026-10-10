#pragma once

#include "gen/Region.h"
#include <nlohmann/json.hpp>
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
//             generated one called that (with "remove": true, removes it); any other key
//             (star, stars, character) is set.
//             "replace": the document IS the system. Its gates are kept from the generated
//             one unless it lists its own, because they are the region's topology.
namespace Gen
{
// Applies every pin that matches `seed` to `region`, in file order. Each problem -- an
// unknown system, mode or field, a "replaces" with nothing to replace -- is one line in
// `problems`, and that pin (or that object) is not applied.
void ApplyPins(Region& region, const nlohmann::json& pins, uint64_t seed,
               std::vector<std::string>& problems);
}  // namespace Gen
