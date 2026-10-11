#pragma once

#include "doctest/doctest.h"

#include "core/Faction.h"

// The factions of data/factions.json -- which are also the registry before any load -- by
// their ids (#355). The code never names a faction; a test may, because it is a test of
// that data. A faction that is not there fails the test rather than quietly becoming the
// unaffiliated one.
namespace Fac
{
inline FactionId Named(const char* id)
{
    FactionId f;
    REQUIRE(Factions::Find(id, f));
    return f;
}
inline FactionId Independent()
{
    return Named("Independent");
}
inline FactionId TradersGuild()
{
    return Named("TradersGuild");
}
inline FactionId Syndicate()
{
    return Named("Syndicate");
}
inline FactionId Pirates()
{
    return Named("Pirates");
}
}  // namespace Fac
