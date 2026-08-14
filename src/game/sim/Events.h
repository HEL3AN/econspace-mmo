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
    Notice,        // something worth saying, with no other handling
    OrderDone,     // a standing order finished as asked
    OrderFailed,   // a standing order gave up; text says why
    Docked,        //
    Undocked,      //
    Jumped,        // arrived in another system
    CargoFull,     // the hold filled up
    UnderAttack,   // something is shooting at us
    ShipDestroyed  // we died
};

struct Event
{
    int         seq = 0;  // monotonic per session; ask for "since seq" to catch up
    Kind        kind = Kind::Notice;
    std::string text;  // human-readable; never the thing code decides on
};

const char* KindName(Kind k);

}  // namespace Ev
