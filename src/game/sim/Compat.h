#pragma once

#include <map>
#include <string>
#include <vector>

// Whether a client may play on a server: the same build, reading the same data (#400).
//
// PROTO_VERSION says two peers read each other's messages. It does not say they agree on
// what the messages are about: a client whose ships.json is a day older than the server's
// predicts its own ship with another hull's thrust, prices a refit differently and lists
// goods the market does not have -- connected, with no error, and wrong. So a Hello carries
// the client's build number and a hash of each data file both sides read, and the server
// refuses a mismatch with a Bye that names what differs and which build to get.
namespace Compat
{

// The data files a client and a server must agree on: what the client predicts, prices or
// lists from. Art the client alone reads (materials, modules, look, shaders, themes) is left
// out, and so is what the server alone reads (world_rules, pins): a difference there decides
// nothing on the client, and refusing over it would only make a retheme a forced update.
const std::vector<std::string>& SharedFiles();

// Each shared file by name, with a short hash of its content (line endings ignored, so a
// checkout with CRLF matches one with LF). A missing file is listed with an empty hash.
std::map<std::string, std::string> DataHashes(const std::string& dataDir);

// What a server running `serverBuild` with `serverData` tells a client of `clientBuild` with
// `clientData`, or empty when the client may come in. The build is compared first and
// exactly: a different number is a different game whatever its data says.
std::string Refusal(const std::string&                        serverBuild,
                    const std::map<std::string, std::string>& serverData,
                    const std::string&                        clientBuild,
                    const std::map<std::string, std::string>& clientData);

}  // namespace Compat
