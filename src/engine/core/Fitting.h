#pragma once

#include "core/ShipDesign.h"

#include <string>
#include <vector>

// Refitting a ship (#279 fitting, step 2): a player changes the fit of the hull they fly and
// the ship flies the design that makes. One set of rules, read by the server that charges for
// it, the hangar's Fit tab that shows it and the agent's preview_fit that quotes it -- three
// copies would be how a button says one price and the account is charged another (#109).
namespace Ships
{
// What may be fitted where: one row per socket kind a position of the hull offers.
struct SocketGroup
{
    std::string              in;  // "bow", "mid", "stern"
    std::string              on;  // the socket kind
    int                      capacity = 0;
    int                      used = 0;  // by the fit being looked at
    std::vector<std::string> modules;   // every module whose look names this kind
};

// The hull's sockets, as `fit` uses them, in bow-to-stern order.
std::vector<SocketGroup> Sockets(const Catalogue& c, const Design& hull,
                                 const std::vector<FitLine>& fit);

// A fit, in one order and with each module/position/socket once, so the same fit is always
// written the same way -- and so filed under the same id.
std::vector<FitLine> Normalized(std::vector<FitLine> fit);

// The design a player's `fit` on `hull` makes. The hull's sections are the hull's: only the
// fit changes. Beyond what every design keeps (Validate), a player's fit
// - sets no `scale`: a module is the size its look says, so nobody fits a gun the size of
//   the ship; a line that is the hull's own keeps the hull's drawing of it,
// - leaves the ship a drive, since a ship that cannot move cannot leave the station.
// A fit that is the hull's own makes the hull's stock design itself, id and all. False,
// with the reason in words a player reads, for a fit that is refused.
bool PlayerFit(const Catalogue& c, const Design& hull, const std::vector<FitLine>& fit, Design& out,
               std::string& error);

// What a station with a shipyard charges to go from one fit to another, in credits, before
// standing: every module added at what its parts cost in ore here (`orePrices`, in
// ResourceType order) times the yard's `markup`, less a share of the same for every module
// taken out -- the share dismantling refunds. Negative when the refit pays the player back.
double                 RefitPrice(const Catalogue& c, const Design& from, const Design& to,
                                  const std::vector<float>& orePrices, float markup);
inline constexpr float REFIT_REFUND = 0.5f;

// A design's fit as a player writes it: in one order and with no scale. What applying a
// saved design sends, and what the Fit tab draws up from the design a ship flies.
std::vector<FitLine> PlayerLines(const Design& d);

// The named design library (#279 fitting, step 3). A player keeps designs under names of
// their own; the name is the account's and the design is filed by its content, so two
// pilots' "Miner" are two names for whatever each of them saved. Bounded, because every
// entry keeps a design alive in the server's store (#41).
inline constexpr int    LIBRARY_MAX = 24;
inline constexpr size_t DESIGN_NAME_MAX = 32;
// A name a design may be saved under: 1 to DESIGN_NAME_MAX printable characters, with no
// space at either end. False, with the reason in a pilot's words, for one that is not.
bool ValidDesignName(const std::string& name, std::string& error);
}  // namespace Ships
