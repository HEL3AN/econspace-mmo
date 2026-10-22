#pragma once

#include "core/ShipDesign.h"
#include "core/WorldLoader.h"
#include "entities/ShipType.h"
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

// --- Fitting (#279) ---------------------------------------------------------------------
// The design the ship being flown has: its refit if it has one, else its hull's stock
// design. Null before the first snapshot.
const Ships::Design* CurrentDesign(const View& view);
// The stock design of the hull being flown: what a refit may change the fit of.
const Ships::Design* CurrentHull(const View& view);
// Docked at a station whose archetype declares a shipyard, and its markup if so.
bool AtShipyard(const View& view, float* markup = nullptr);

// What a fit would make of the ship being flown, here, before anything is sent: the same
// rules (Ships::PlayerFit), the same price (Ships::RefitPrice at this system's ore prices
// and the station's markup, by standing) and the same hold and money checks the server
// makes. The Fit tab draws it and preview_fit reads it out.
struct FitQuote
{
    bool          ok = false;       // the server would take it
    bool          changed = false;  // it is not the fit the ship has
    std::string   problem;          // why not, in a pilot's words; empty when ok
    std::string   hull;             // the hull's name
    Ships::Design design;           // what it makes
    ShipStats     before, after;
    double        price = 0.0;  // credits; negative pays the pilot back
};
FitQuote QuoteFit(const View& view, const std::vector<Ships::FitLine>& fit);

// What the agent's `fitting` tool says: the hull, its sockets and what fills them, the
// modules that go on each and what one costs here.
std::string DescribeFitting(const View& view);
// What `preview_fit` says: a quote, before and after.
std::string DescribeQuote(const FitQuote& q);

// The design library (#279 fitting, step 3): each saved design as the Fit tab lists it and
// the agent's `designs` reads it out -- the hull it is for, its numbers, and, for the hull
// being flown, the quote for applying it (QuoteFit of its fit: the server's price and rules).
struct SavedDesignRow
{
    std::string name;  // the player's name for it
    std::string id;    // the design's
    std::string hull;  // the hull's name; empty for a design this client cannot read
    int         hullIndex = -1;
    ShipStats   stats;          // as it would fly
    bool        known = false;  // its definition is here, so the numbers mean something
    bool        flown = false;  // the ship being flown is this design now
    bool        thisHull = false;
    FitQuote    quote;  // applying it now; meaningful only for thisHull
};
std::vector<SavedDesignRow> SavedDesigns(const View& view);
// What `designs` says.
std::string DescribeDesigns(const View& view);

// --- Ship loss and insurance (#344) ------------------------------------------------------
// What insuring an owned hull would come to here, before anything is sent: its value as it is
// fitted (ShipValue at this system's ore prices), its cover now, the premium and what a policy
// pays -- and why the server would refuse, by the server's own checks. The hangar draws it and
// the agent's hangar and insure_ship read it out.
struct InsuranceQuote
{
    bool        owned = false;
    std::string hull;           // the hull's name
    double      value = 0.0;    // what it is worth as fitted, here
    double      cover = 0.0;    // what its policy covers now; 0 -- not insured
    double      premium = 0.0;  // what insuring it for `value` costs
    double      payout = 0.0;   // what a policy for `value` pays if it is lost
    std::string problem;        // why the server would refuse; empty -- it would take it
};
InsuranceQuote QuoteInsurance(const View& view, int hull);
// The last ship lost, in one line; empty when none has been.
std::string DescribeLoss(const Proto::LossView& loss);
// The scanner (#346): a pass under way, and the signatures of the system the scans have
// touched -- unresolved by code and how much is resolved, resolved by what each is and its
// id. Then the roaming holes this pilot knows, with where they lead, when they close and
// which ships they let through (#342 S9). Empty when there is nothing to say.
std::string DescribeScan(const View& view);

// Whose charts put a system on the map and who reached it first (#431), in one phrase the
// map, observe and the galaxy resource all say alike: "charted by Syndicate", "found by
// Vega", or both when a pilot was there before a faction. Empty for a centre's own space.
std::string ChartsOf(const WorldLoader::SystemInfo& si);

// Who owns the station the player is docked at, from the layout. Independent when not
// docked or the layout does not say -- which is also what the server assumes.
FactionId DockedFaction(const View& view);

// What a mission still needs before it can be handed in, in one phrase ("destroy 2 more
// pirates", "dock at Vale Station"). Empty when it can be handed in now. Kept beside the
// mission list so an agent refused a hand-in is told the same thing the list says.
std::string MissionNeeds(const View& view, const Proto::MissionView& m);

// How long a plan's job has left (#345), by the snapshot's clock: "due in 12 min", "due in
// 40 s", "overdue". Empty for a mission with no time on it.
std::string JobDue(const View& view, const Proto::MissionView& m);

// An escort job's convoy as it is now (#430): "waiting at Vela", "en route in Vela -> Kor",
// "with convoy" when its hauler is in this system with the ship; empty when the job is not an
// escort or its convoy has no way yet. `arrow` is the client's arrow glyph or the agent's "->".
std::string ConvoyStatus(const Proto::MissionView& m, const char* arrow);

// Which part of one of this pilot's escort jobs' convoys a ship is (#430): its hauler, a
// warship escorting it, or neither. The instruments mark them; the world view does not (#117).
enum class ConvoyPart
{
    None,
    Hauler,
    Escort,
};
ConvoyPart ConvoyPartOf(const Proto::Snapshot& snap, int entityId);

// Compass point for a world-space offset ("N", "NE", ...). Exposed because the tests pin
// the convention: +y is down in world coordinates, so north is -y, matching what the
// player sees on screen rather than what the raw numbers suggest.
std::string Compass(float dx, float dy);

}  // namespace Obs
