#pragma once

#include "raylib.h"
#include <nlohmann/json.hpp>

// Where a system sits on the galaxy map, as universe.json spells it.
//
// Shared because both halves of galaxy mode need it: the map view places nodes and
// hit-tests clicks with it (Editor_Galaxy), and the mutations read it back when they
// rebuild the index or wire up a gate (Editor_Universe). A missing or malformed "map"
// is the origin rather than an error -- an unplaced system shows up at the centre where
// it can be dragged somewhere sensible.
Vector2 MapPos(const nlohmann::json& sys);
