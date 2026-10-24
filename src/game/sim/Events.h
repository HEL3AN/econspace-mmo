#pragma once

#include <string>

// The server's journal of things that happened to the player.
//
// It replaces a plain queue of notification strings, for two reasons.
//
// Sequence numbers. A client asks for "everything after N" and cannot miss an entry
// because a frame was dropped or two snapshots were coalesced. That is what lets an agent
// sleep until something happens instead of polling `observe` in a loop and paying a
// language model for every look.
//
// Kinds. "Docked" and "hull critical" want different reactions, and matching on prose is
// how that goes wrong. The text stays for humans; the kind is what code branches on.
namespace Ev
{

enum class Kind
{
    Notice,             // something worth saying, with no other handling
    OrderDone,          // a standing order finished as asked
    OrderFailed,        // a standing order gave up; text says why
    Docked,             //
    Undocked,           //
    Jumped,             // arrived in another system
    CargoFull,          // the hold filled up
    UnderAttack,        // something is shooting at us
    ShipDestroyed,      // we died
    Built,              // a structure of ours was finished (#39)
    Scanned,            // a pass of the scanner finished (#346); text says what it found
    StructureAttacked,  // a structure of ours, or our corporation's, was attacked (#431 C2)
    Count               // not a kind: how many there are, so a loop over them misses none (#487)
};

struct Event
{
    int         seq = 0;  // monotonic per session; ask for "since seq" to catch up
    Kind        kind = Kind::Notice;
    std::string text;  // human-readable; never the thing code decides on
};

const char* KindName(Kind k);
// The kind KindName names; false for a name it does not know (a save from a later build).
bool KindFrom(const std::string& name, Kind& out);

}  // namespace Ev
