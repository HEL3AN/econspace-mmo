#pragma once

#include "core/Faction.h"
#include "gen/Galaxy.h"
#include "sim/ClientSession.h"
#include "sim/Protocol.h"
#include "sim/ServerConfig.h"
#include "sim/Simulation.h"
#include "net/Transport.h"

#include <cstdint>
#include <string>
#include <vector>

// What the server's host loop (main_headless.cpp) lends the self-tests (#354): the same
// setup, the same world step and the same way of reading inputs as the real server runs.

// The authoritative simulation as the host sets it up: data loaded, galaxy made or restored
// from `worldPath`, every system materialized. An empty path keeps a run ephemeral.
void SetupHostSim(Simulation& sim, const std::string& dataDir,
                  const std::string& worldPath = std::string(), uint64_t newSeed = 1,
                  int centres = Gen::DEFAULT_CENTRES, int maxRegions = 0, int maxSystems = 0);

// A player at a start, the first one unless told which.
ClientSession& SetupHostPlayer(Simulation& sim, std::string start = std::string());

// The world's step: every system, the sessions' standing orders, and the maintenance.
void HostStepWorld(Simulation& sim, float dt, Simulation::FireMap& fires);

// Each input waiting on the transport is applied as one player tick (Simulation::ApplyCommand).
bool HostDrainInputs(ITransport& conn, Simulation& sim, ClientSession& s, float dt, int& lastSeq,
                     std::vector<Proto::TradeAck>& acks, Simulation::FireMap& fires);

// The layout a session is sent for a system, encoded.
std::string LayoutFor(const Simulation& sim, const std::string& systemId, const ClientSession& s);

// `econserver host`: the real loop.
int RunHost(const Host::ServerConfig& cfg, const std::string& worldDir);
