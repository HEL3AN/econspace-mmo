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
// 3: the world carries what players changed in each system's static layer (#38). Older
//    files have none and load as the world the data describes. The bump is what keeps a
//    version 2 build from reading a newer file, ignoring the changes, and writing the world
//    back without them -- every structure built and every wreck searched, quietly undone.
// 4: an added object may be a structure (#39), written under "structures" with its time
//    line. A version 3 build would not rebuild one, drop it with a warning, and write the
//    world back without it -- every site and beacon gone after one checkpoint.
// 5: the world carries what each faction knows, the surveys under way and the history
//    (#295). A version 4 build would load the world without them and write it back so,
//    and every faction would forget what it had found. An older file loads with factions
//    that know their holdings and what is next door, as a new world's do.
inline constexpr int WORLD_VERSION = 5;
// 2: a ship's position is in a system a million units across (#159). An older one is
//    read for everything else, and the ship is placed beside a station.
// 3: a mission names its stations by system and station rather than by entity id, which
//    holds for one server run only (#227). An older one keeps its ids, as it always did.
inline constexpr int ACCOUNT_VERSION = 3;

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
