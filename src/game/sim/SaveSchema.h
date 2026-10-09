#pragma once

// Versions for the files the server keeps, and what a load can conclude about one (#20).
//
// Decoding is permissive per field on purpose -- see Protocol.h -- and for a save that is
// exactly the wrong policy. A file written by a build that stored something differently
// loads "successfully" with defaults quietly filled in, and the player's progress degrades
// without anything failing. The version is what turns that into a decision.
namespace Save
{

// Bump when the meaning of a field changes or one is removed. Adding a field that older
// readers can ignore, and newer readers can default, does not need a bump -- that is what
// the permissive reader is for.
// 2: the world carries the seed and the rules' version of the region beyond the wormhole
//    (#140). A version 1 file has neither, and is read as the same galaxy with a region
//    made from a new seed.
inline constexpr int WORLD_VERSION = 2;
// 2: a ship's position is in a system a million units across (#159). An older one is
//    read for everything else, and the ship is placed beside a station.
inline constexpr int ACCOUNT_VERSION = 2;

// A file with no version at all: everything written before this existed. Those files are
// a strict subset of version 1, so they are read as-is rather than migrated.
inline constexpr int UNVERSIONED = 0;

enum class Result
{
    Ok,       // read, and the version is one this build understands
    Missing,  // no such file -- a new galaxy, or a player who has never played
    Corrupt,  // unreadable or not the shape a save has
    TooNew    // written by a later build; NOT read, and must not be written over
};

}  // namespace Save
