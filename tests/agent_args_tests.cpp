// A tool call's arguments held to the tool's own schema (#425): `dock` with no station_id was
// accepted, read the id as 0 and flew the ship at the point (0, 0) -- into the star.
#include <doctest/doctest.h>

#include "agent/Jsonrpc.h"

#include <string>

namespace
{
// What econagent's `dock` declares, and a tool with an optional number and flag beside it.
Rpc::Json DockSchema()
{
    return Rpc::Json{ { "type", "object" },
                      { "properties",
                        { { "station_id", { { "type", "number" } } },
                          { "warp", { { "type", "boolean" } } },
                          { "via", { { "type", "array" } } },
                          { "resource", { { "type", "string" } } } } },
                      { "required", { "station_id" } } };
}

// The message CheckArguments refuses with, or empty if it lets the arguments through.
std::string Refusal(const Rpc::Json& args)
{
    try
    {
        Rpc::CheckArguments(DockSchema(), args);
    }
    catch (const Rpc::Error& e)
    {
        CHECK(e.code == Rpc::INVALID_PARAMS);
        return e.message;
    }
    return std::string();
}
}  // namespace

TEST_CASE("a tool call missing what its schema requires is refused, not read as zero (#425)")
{
    CHECK(Refusal(Rpc::Json::object()) == "station_id is required");
    CHECK(Refusal(Rpc::Json{ { "station_id", nullptr } }) == "station_id is required");
    CHECK(Refusal(Rpc::Json{ { "warp", true } }) == "station_id is required");
    CHECK(Refusal(Rpc::Json::array()) == "arguments must be an object");
}

TEST_CASE("a number or a flag given as something else is refused by name (#425)")
{
    CHECK(Refusal(Rpc::Json{ { "station_id", "the hub" } }) == "station_id must be a number");
    CHECK(Refusal(Rpc::Json{ { "station_id", 4 }, { "warp", "yes" } }) ==
          "warp must be true or false");
    CHECK(Refusal(Rpc::Json{ { "station_id", 4 }, { "via", "S-12" } }) == "via must be a list");
}

TEST_CASE("well-formed arguments pass, and a string field may carry a number (#425)")
{
    CHECK(Refusal(Rpc::Json{ { "station_id", 44 } }).empty());
    CHECK(Refusal(Rpc::Json{ { "station_id", 44.0 }, { "warp", false } }).empty());
    // A few tools take a resource or a ship by name or by number on purpose (#171), and say
    // so in their own refusal; the check leaves a string property's type to them.
    CHECK(Refusal(Rpc::Json{ { "station_id", 44 }, { "resource", 2 } }).empty());
    // Something the schema does not declare is the tool's business, not a refusal.
    CHECK(Refusal(Rpc::Json{ { "station_id", 44 }, { "extra", "x" } }).empty());
}
