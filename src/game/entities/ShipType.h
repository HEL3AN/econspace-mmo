#pragma once

#include "core/Faction.h"
#include "core/ShipDesign.h"

#include <string>
#include <vector>

// Ship parameters. Change when switching to another ship. Every one of them is read off the
// ship's design (#279 step 5, Ships::Derive) -- the server flies the ship by them and the
// client predicts its own with the same numbers (Sim::StepPlayerShip), so they cannot come
// from anywhere else.
struct ShipStats
{
    float       thrustPower = 0.0f;  // engine thrust (stabilizer-off mode)
    float       turnSpeed = 0.0f;    // turn rate, rad/sec
    float       maxSpeed = 0.0f;     // top speed
    float       rcsAccel = 0.0f;     // stabilizer power
    int         cargoCapacity = 0;   // cargo hold capacity
    float       miningRate = 0.0f;   // mining rate, units/sec
    float       hull = 0.0f;         // structure: what a hit takes once the shields are down
    float       damage = 0.0f;       // what one shot of its guns takes off
    std::string design;              // what it is and what it looks like, in data/ships.json
};

// A ship type in the hangar: name, price, base stats.
struct ShipType
{
    std::string name;
    double      price = 0.0;
    ShipStats   stats;
};

// Catalog of available ships: the hangar of data/ships.json, each hull's stats derived from
// its design, so it is read once the archetypes are loaded (Archetypes::Load) and again
// whenever they are reloaded. The first one is the starter. An account owns hulls by their
// index here, which is their place in the file's hangar list.
const std::vector<ShipType>& GetShipCatalog();

// The hull in the catalog that is `design`, or null: what a client makes of the design the
// snapshot says another player's ship is.
const ShipType* ShipTypeOf(const std::string& design);

// What a design is under a player: Ships::Derive, with the player's hull and gun (#279 step
// 5). One function because the server flies a ship by these numbers and the client predicts
// its own with them; computed twice two ways, prediction would pull the ship back.
bool PlayerStats(const Ships::Design& d, ShipStats& out, std::string& error);

// The same for a design by id: a stock one, or one this process has been told about
// (Archetypes::AddDesign). False when it is neither.
bool StatsOfDesign(const std::string& design, ShipStats& out);

// The hangar hull a design is built on (#279 fitting): the entry whose design has the same
// frame and sections. A refit changes the fit and never the hull, so every ship a player
// flies has one; -1 for a design that is no hull's.
int HullOf(const std::string& design);

// What a station charges for a ship, as a multiple of the catalog price, given the buyer's
// standing with the station's owner. One function because three places need the same number
// (#109): the server charges it, the hangar screen shows it, and an agent quotes it before
// deciding whether it can afford one. Two copies of a price are how a button says one thing
// and the account is charged another.
float ShipPriceMultiplier(RepTier standing);

// What a station pays for cargo, as a multiple of its gross, given the seller's standing.
float SellPriceMultiplier(RepTier standing);

// How a station scales the money and reputation its missions offer, given the standing of
// the player reading its board.
float MissionRewardMultiplier(RepTier standing);

// The three tables move together and are monotonic in standing (#219): a worse tier is never
// a better deal. Hated once fell through to list price -- cheaper than Hostile -- and only
// the dock refusing Hated players hid it.

// --- Ship loss and insurance (#344) ---
// What a ship is worth: its hull's list price, plus its fit beyond the hull's stock fit at
// what a refit would charge for it with no yard markup -- modules at `orePrices` (in
// ResourceType order). Never below nothing. One function, because the server pays insurance
// by it, the hangar window quotes it and an agent reads it (#109).
double ShipValue(const Ships::Catalogue& c, double hullPrice, const Ships::Design& stock,
                 const Ships::Design& flown, const std::vector<float>& orePrices);

// A policy covers a hull for a value fixed when it is bought. Its premium is a share of the
// value covered, and only of what is not covered already: insuring again after a refit pays
// for the difference.
inline constexpr double INSURANCE_PREMIUM = 0.10;
// What a lost ship's policy pays: a share of its cover, and never of more than the ship was
// worth when it was lost -- a policy bought on a big fit does not pay for one taken off since.
inline constexpr double INSURANCE_PAYOUT = 0.70;
double                  InsurancePremium(double value, double cover);
double                  InsurancePayout(double cover, double valueAtLoss);
