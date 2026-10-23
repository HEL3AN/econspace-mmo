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

void CheckArguments(const Json& schema, const Json& args)
{
    if (!args.is_object())
        throw Error{ INVALID_PARAMS, "arguments must be an object" };
    if (schema.contains("required") && schema["required"].is_array())
        for (const Json& key : schema["required"])
        {
            const std::string name = key.get<std::string>();
            if (!args.contains(name) || args[name].is_null())
                throw Error{ INVALID_PARAMS, name + " is required" };
        }
    if (!schema.contains("properties") || !schema["properties"].is_object())
        return;
    const Json& props = schema["properties"];
    for (auto it = args.begin(); it != args.end(); ++it)
    {
        const auto p = props.find(it.key());
        if (p == props.end() || !p->is_object() || it->is_null())
            continue;
        const std::string type = p->value("type", std::string());
        if (type == "number" && !it->is_number())
            throw Error{ INVALID_PARAMS, it.key() + " must be a number" };
        if (type == "boolean" && !it->is_boolean())
            throw Error{ INVALID_PARAMS, it.key() + " must be true or false" };
        if (type == "array" && !it->is_array())
            throw Error{ INVALID_PARAMS, it.key() + " must be a list" };
    }
}

void Server::Run()
{
    std::string line;
    while (std::getline(std::cin, line))
    {
        if (line.empty())
            continue;
        std::string response;
        if (lock_ != nullptr)
        {
            std::lock_guard<std::mutex> hold(*lock_);
            response = HandleLine(line);
        }
        else
        {
            response = HandleLine(line);
        }
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
