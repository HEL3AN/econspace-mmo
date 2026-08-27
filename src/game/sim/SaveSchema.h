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
inline constexpr int WORLD_VERSION = 1;
inline constexpr int ACCOUNT_VERSION = 1;

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
