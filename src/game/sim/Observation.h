#pragma once

#include "core/WorldLoader.h"
#include "sim/Protocol.h"

#include <map>
#include <string>

// Projection of what the client knows into compact text.
//
// This exists for the agent seam (#42): a language model cannot read a Proto::Snapshot —
// every entity in the system, full-precision floats, re-sent sixty times a second — and
// every token of noise is paid for. So the same state is projected into a few dozen lines
// that a model (or a human staring at a terminal) can act on directly.
//
// Two rules shape everything here:
//
//   * It projects ONLY what the client legitimately receives. The input is a snapshot, a
//     layout and the local galaxy index — the same three things the renderer uses. An
//     agent must not be able to see further than a player can.
//   * It is free of raylib's render layer and of Simulation, so econagent can build it
//     headless and link nothing it does not need.
namespace Obs
{

// How much to spend. Brief is what an agent reads on a normal turn; Full is for when it
// is deciding something and wants the whole picture.
enum class Detail
{
    Brief,
    Full
};

// Everything the projection is allowed to look at.
struct View
{
    const Proto::Snapshot*                    snapshot = nullptr;
    const std::map<int, Proto::EntityLayout>* layout = nullptr;    // statics by id, may be null
    const WorldLoader::Universe*              universe = nullptr;  // system names, may be null
    const Proto::GalaxyState*                 galaxy = nullptr;    // map stats, may be null
};

// Renders the situation report. Never throws; missing optional inputs simply mean fewer
// lines rather than an error, because an agent asking "where am I" during a system change
// should get a partial answer, not a failure.
std::string Describe(const View& view, Detail detail);

// The station's job board and the missions under way (#109), in full: what each asks, what
// it pays, where it is handed in and whether it can be right now. Numbered as the snapshot
// numbers them, because those numbers are what accept_mission and complete_mission take.
std::string DescribeMissions(const View& view);

// What this account owns, which ship it flies, and what each other hull would cost here
// (#109) -- at the price this player would be charged, which depends on its standing with
// the station's owner, rather than the catalog's.
std::string DescribeHangar(const View& view);

// Who owns the station the player is docked at, from the layout. Independent when not
// docked or the layout does not say -- which is also what the server assumes.
FactionId DockedFaction(const View& view);

// What a mission still needs before it can be handed in, in one phrase ("destroy 2 more
// pirates", "dock at Vale Station"). Empty when it can be handed in now. Kept beside the
// mission list so an agent refused a hand-in is told the same thing the list says.
std::string MissionNeeds(const View& view, const Proto::MissionView& m);

// Compass point for a world-space offset ("N", "NE", ...). Exposed because the tests pin
// the convention: +y is down in world coordinates, so north is -y, matching what the
// player sees on screen rather than what the raw numbers suggest.
std::string Compass(float dx, float dy);

}  // namespace Obs
