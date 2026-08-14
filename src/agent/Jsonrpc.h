#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <map>
#include <string>

// JSON-RPC 2.0 over stdio — the transport MCP speaks for a local server.
//
// Hand-written rather than taken from an SDK, and the reason is worth stating: the
// alternative was a TypeScript or Python bridge, which would have to reimplement
// Protocol.cpp and create a second source of truth for the wire format. This project
// avoids that everywhere else, so it avoids it here too. The cost is this file, which is
// small because the surface is small: read a line, dispatch on "method", write a line.
//
// One rule matters more than the rest: stdout carries JSON-RPC and nothing else. Any log,
// warning or stray printf on stdout corrupts the stream and the client sees a protocol
// error rather than the message that caused it. Logging goes to stderr.
namespace Rpc
{

using Json = nlohmann::json;

// A method handler. Returns the `result` value; throw Error to answer with an error.
using Handler = std::function<Json(const Json& params)>;

// Thrown by a handler to produce a JSON-RPC error response instead of a result.
struct Error
{
    int         code = -32603;  // internal error unless the thrower says otherwise
    std::string message;
};

// Standard JSON-RPC codes, the only ones this server produces.
constexpr int PARSE_ERROR = -32700;
constexpr int INVALID_REQUEST = -32600;
constexpr int METHOD_NOT_FOUND = -32601;
constexpr int INVALID_PARAMS = -32602;
constexpr int INTERNAL_ERROR = -32603;

class Server
{
public:
    void On(const std::string& method, Handler h) { handlers_[method] = std::move(h); }

    // Handles one incoming line. Returns the response to write back, or an empty string
    // for a notification (no id), which by the spec must not be answered.
    std::string HandleLine(const std::string& line);

    // Reads stdin to end of stream, answering each line. Returns when the client closes
    // the pipe, which is how an MCP client says goodbye.
    void Run();

private:
    std::map<std::string, Handler> handlers_;
};

// Writes a diagnostic to stderr. Never use printf here: see the note above.
void Log(const std::string& text);

}  // namespace Rpc
