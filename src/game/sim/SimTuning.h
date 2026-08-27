#pragma once

// Server tuning values that more than one of Simulation's translation units needs (#17).
//
// Only constants that genuinely cross a file boundary belong here; one used in a single
// unit stays next to the rule it tunes, where it is easier to read. Anything that varies
// per kind of object -- a dock's range, an asteroid's extraction rate -- is not a constant
// at all and belongs in data/archetypes.json (#34).
namespace Sim
{

// Margin on a field's radius within which the mining laser reaches. Two rules read it:
// StepPlayerMining, which extracts, and the Mine standing order, which decides how close
// to fly first. If they disagreed, an order would park the ship just out of range and
// then mine nothing, forever.
inline constexpr float MINING_RANGE = 40.0f;

}  // namespace Sim
