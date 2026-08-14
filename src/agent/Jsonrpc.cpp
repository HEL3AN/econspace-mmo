#include "agent/Jsonrpc.h"

#include <cstdio>
#include <iostream>

namespace Rpc
{

void Log(const std::string& text)
{
    std::fprintf(stderr, "%s\n", text.c_str());
}

namespace
{

Json ErrorResponse(const Json& id, int code, const std::string& message)
{
    return Json{ { "jsonrpc", "2.0" },
                 { "id", id.is_null() ? Json(nullptr) : id },
                 { "error", { { "code", code }, { "message", message } } } };
}

}  // namespace

std::string Server::HandleLine(const std::string& line)
{
    Json req = Json::parse(line, nullptr, false);
    if (req.is_discarded() || !req.is_object())
        return ErrorResponse(nullptr, PARSE_ERROR, "invalid JSON").dump();

    // A request without an id is a notification: handle it, answer nothing. Answering one
    // is a protocol violation, and clients differ in how loudly they complain.
    const bool notification = !req.contains("id") || req["id"].is_null();
    Json       id = notification ? Json(nullptr) : req["id"];

    if (!req.contains("method") || !req["method"].is_string())
        return notification ? std::string()
                            : ErrorResponse(id, INVALID_REQUEST, "missing method").dump();

    const std::string method = req["method"].get<std::string>();
    auto              it = handlers_.find(method);
    if (it == handlers_.end())
    {
        // Unknown notifications are normal -- clients send lifecycle ones a server may not
        // care about -- so staying quiet is correct rather than lenient.
        return notification
                   ? std::string()
                   : ErrorResponse(id, METHOD_NOT_FOUND, "no such method: " + method).dump();
    }

    const Json params = req.contains("params") ? req["params"] : Json::object();
    try
    {
        Json result = it->second(params);
        if (notification)
            return std::string();
        return Json{ { "jsonrpc", "2.0" }, { "id", id }, { "result", result } }.dump();
    }
    catch (const Error& e)
    {
        return notification ? std::string() : ErrorResponse(id, e.code, e.message).dump();
    }
    catch (const std::exception& e)
    {
        // A handler throwing anything else is a bug here, not a client mistake. Report it
        // as an internal error rather than letting it kill the process mid-session.
        return notification ? std::string() : ErrorResponse(id, INTERNAL_ERROR, e.what()).dump();
    }
}

void Server::Run()
{
    std::string line;
    while (std::getline(std::cin, line))
    {
        if (line.empty())
            continue;
        std::string response = HandleLine(line);
        if (response.empty())
            continue;
        // Unbuffered and flushed per message: an MCP client is waiting on this line, and a
        // buffered write looks exactly like a hung server.
        std::fwrite(response.data(), 1, response.size(), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
    }
}

}  // namespace Rpc
