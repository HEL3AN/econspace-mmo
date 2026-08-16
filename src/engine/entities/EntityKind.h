#pragma once

// What a world object is, as data rather than as a C++ type.
//
// Today "what is this?" is answered by `dynamic_cast` — 50-odd sites, many of them in
// per-tick loops that rescan every entity once per subsystem. That is the cheap part of
// the problem. The expensive part is that a new kind of object means a new class, edits to
// every simulation pass, and a recompile, which is precisely what stops a player from
// building anything (#44) and stops the world from being described by data (#43).
//
// This enum is the shared vocabulary those two need. It lives in `engine` rather than
// beside the protocol because `engine` may not depend on `game`, and both sides need it:
// the world model to say what an object is, and the wire to carry it. Having one enum
// instead of two removes the translation between an RTTI probe and an int.
enum class EntityKind
{
    Unknown,
    Star,
    Planet,
    Station,
    Field,
    Gate,
    Nebula,
    Derelict,
    Npc,
    PlayerShip
};
