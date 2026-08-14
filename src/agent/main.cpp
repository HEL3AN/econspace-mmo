// econagent — EconSpace as an MCP server.
//
// Two hats at once: an MCP server on stdio for a language model, and an ordinary TCP game
// client to econserver. Nothing about the game half is privileged — it speaks the same
// Command/Snapshot protocol a human client speaks, so an agent is a player rather than a
// side channel.
//
// It is written in C++ and lives in this repository for one reason: a bridge in another
// language would have to reimplement Protocol.cpp, and a second implementation of the wire
// format is a second source of truth. Linking netproto means the format cannot drift.
//
// Run:  econagent connect <host> [port]
// Wire it to a client:
//   claude mcp add econspace -- <path>/econagent.exe connect 127.0.0.1 50800

#include "agent/Jsonrpc.h"
#include "agent/Session.h"

#include "core/Faction.h"
#include "core/WorldLoader.h"
#include "sim/Orders.h"

#include "raylib.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace
{

// raylib logs to stdout, and stdout is the JSON-RPC channel. One "INFO: WorldLoader..."
// line is enough to make the client see a parse error instead of the handshake, so every
// trace goes to stderr instead. This is not hypothetical: it happened the first time this
// was run end to end.
void TraceToStderr(int level, const char* text, va_list args)
{
    const char* label = level >= 5 ? "ERROR" : (level == 4 ? "WARN" : "INFO");
    std::fprintf(stderr, "raylib %s: ", label);
    std::vfprintf(stderr, text, args);
    std::fputc('\n', stderr);
}

Agent::Session        g_session;
WorldLoader::Universe g_universe;

// MCP wants tool results as a list of content blocks. Everything here answers with text,
// because text is what a model reads and what a human debugging this can read too.
Rpc::Json TextResult(const std::string& text)
{
    return Rpc::Json{ { "content",
                        Rpc::Json::array({ Rpc::Json{ { "type", "text" }, { "text", text } } }) } };
}

void RequireLive()
{
    g_session.Pump();
    if (!g_session.ProtocolError().empty())
        throw Rpc::Error{ Rpc::INTERNAL_ERROR, "protocol mismatch: " + g_session.ProtocolError() };
    if (!g_session.Alive())
        throw Rpc::Error{ Rpc::INTERNAL_ERROR, "connection to the server is closed" };
}

double NumberOr(const Rpc::Json& args, const char* key, double fallback)
{
    return args.contains(key) && args[key].is_number() ? args[key].get<double>() : fallback;
}

// Sends an order and reports what the server made of it. The wait is short on purpose:
// long enough for the server to acknowledge, not long enough to be mistaken for the order
// itself finishing -- that is what wait_for_event is for.
std::string GiveOrder(const Proto::Command& cmd, const std::string& what)
{
    const int before = g_session.Snapshot().player.orderId;
    g_session.Send(cmd);
    g_session.WaitUntil([&] { return g_session.Snapshot().player.orderId != before; }, 2.0);

    const Proto::PlayerView& p = g_session.Snapshot().player;
    if (p.orderId == before)
        return what + ": sent, but the server has not acknowledged it yet";
    if (p.orderStatus == (int)Orders::Status::Failed)
        return what + ": refused — " + p.orderDetail;
    return what + ": accepted (order " + std::to_string(p.orderId) +
           "). It runs on the server; use wait_for_event to sleep until it finishes.";
}

Proto::Command OrderCommand(Orders::Kind kind)
{
    Proto::Command c;
    c.orderKind = (int)kind;
    return c;
}

// --- Tool registry ----------------------------------------------------------
// Each entry is a name, a description a model reads to choose between them, a JSON Schema
// for the arguments, and the implementation.
struct Tool
{
    const char*                                  name;
    const char*                                  description;
    Rpc::Json                                    schema;
    std::function<std::string(const Rpc::Json&)> run;
};

Rpc::Json Obj(std::initializer_list<std::pair<const std::string, Rpc::Json>> props,
              std::vector<std::string>                                       required = {})
{
    Rpc::Json schema{ { "type", "object" }, { "properties", Rpc::Json(props) } };
    if (!required.empty())
        schema["required"] = required;
    return schema;
}

Rpc::Json Num(const char* desc)
{
    return Rpc::Json{ { "type", "number" }, { "description", desc } };
}
Rpc::Json Str(const char* desc)
{
    return Rpc::Json{ { "type", "string" }, { "description", desc } };
}
Rpc::Json Bool(const char* desc)
{
    return Rpc::Json{ { "type", "boolean" }, { "description", desc } };
}

std::vector<Tool> BuildTools()
{
    std::vector<Tool> tools;

    tools.push_back(
        { "observe",
          "Look at the world: the ship, the system, what is nearby, hostiles, "
          "and recent events. Start here, and call it again after anything "
          "changes. Use detail='full' only when deciding something.",
          Obj({ { "detail", Str("'brief' (default) or 'full'") } }), [](const Rpc::Json& args)
          {
              RequireLive();
              const std::string d =
                  args.value("detail", std::string("brief")) == "full" ? "full" : "brief";
              return g_session.Describe(d == "full" ? Obs::Detail::Full : Obs::Detail::Brief,
                                        &g_universe);
          } });

    tools.push_back({ "move_to",
                      "Fly to an object by id, or to a point. Finishes when the ship "
                      "arrives. Use warp for anything far away.",
                      Obj({ { "target_id", Num("object id from observe") },
                            { "x", Num("destination x, if no target_id") },
                            { "y", Num("destination y, if no target_id") },
                            { "stop_distance", Num("how close to stop (default 150)") },
                            { "warp", Bool("warp instead of cruising") } }),
                      [](const Rpc::Json& args)
                      {
                          RequireLive();
                          Proto::Command c = OrderCommand(Orders::Kind::MoveTo);
                          c.orderTarget = (int)NumberOr(args, "target_id", 0);
                          c.orderPoint = { (float)NumberOr(args, "x", 0.0),
                                           (float)NumberOr(args, "y", 0.0) };
                          c.orderStopDist = (float)NumberOr(args, "stop_distance", 150.0);
                          c.orderWarp = args.value("warp", false);
                          return GiveOrder(c, "move_to");
                      } });

    tools.push_back({ "dock",
                      "Approach a station and dock with it. The server may refuse on "
                      "reputation; the order then fails and says so.",
                      Obj({ { "station_id", Num("station id from observe") } }, { "station_id" }),
                      [](const Rpc::Json& args)
                      {
                          RequireLive();
                          Proto::Command c = OrderCommand(Orders::Kind::Dock);
                          c.orderTarget = (int)NumberOr(args, "station_id", 0);
                          return GiveOrder(c, "dock");
                      } });

    tools.push_back({ "undock", "Leave the station.", Obj({}), [](const Rpc::Json&)
                      {
                          RequireLive();
                          return GiveOrder(OrderCommand(Orders::Kind::Undock), "undock");
                      } });

    tools.push_back({ "mine",
                      "Approach an asteroid field and mine it. With until_full, keeps "
                      "going until the hold is full or the field is exhausted.",
                      Obj({ { "field_id", Num("field id from observe") },
                            { "until_full", Bool("keep mining until the hold is full") } },
                          { "field_id" }),
                      [](const Rpc::Json& args)
                      {
                          RequireLive();
                          Proto::Command c = OrderCommand(Orders::Kind::Mine);
                          c.orderTarget = (int)NumberOr(args, "field_id", 0);
                          c.orderUntilFull = args.value("until_full", true);
                          return GiveOrder(c, "mine");
                      } });

    tools.push_back({ "travel_to_system",
                      "Travel to another star system, jumping gate by gate. One order for "
                      "the whole journey however many hops it takes.",
                      Obj({ { "system", Str("system id, e.g. 'reach'") },
                            { "avoid_danger", Bool("prefer safer systems over the short way") } },
                          { "system" }),
                      [](const Rpc::Json& args)
                      {
                          RequireLive();
                          Proto::Command c = OrderCommand(Orders::Kind::Route);
                          c.orderDestSystem = args.value("system", std::string());
                          c.orderWarp = true;
                          if (c.orderDestSystem.empty())
                              throw Rpc::Error{ Rpc::INVALID_PARAMS, "system is required" };
                          return GiveOrder(c, "travel_to_system");
                      } });

    tools.push_back({ "abort_order", "Stop whatever the ship is currently doing.", Obj({}),
                      [](const Rpc::Json&)
                      {
                          RequireLive();
                          Proto::Command c;
                          c.abortOrder = true;
                          g_session.Send(c);
                          return std::string("abort sent");
                      } });

    tools.push_back(
        { "wait_for_event",
          "Sleep until something happens, then return what happened. This is how to wait "
          "out a flight or a mining run without burning turns re-observing. Returns "
          "immediately if events are already pending.",
          Obj({ { "timeout_seconds", Num("give up after this long (default 60, max 300)") } }),
          [](const Rpc::Json& args)
          {
              RequireLive();
              const int since = g_session.LastEventSeq();
              double    timeout = NumberOr(args, "timeout_seconds", 60.0);
              if (timeout > 300.0)
                  timeout = 300.0;  // an agent should not be able to hold a session forever
              g_session.WaitUntil([&] { return g_session.LastEventSeq() > since; }, timeout);

              std::vector<Ev::Event> fresh = g_session.EventsSince(since);
              if (fresh.empty())
                  return std::string("nothing happened within the timeout");
              std::string out;
              for (const Ev::Event& e : fresh)
                  out += "[" + std::to_string(e.seq) + "] " + Ev::KindName(e.kind) + ": " + e.text +
                         "\n";
              return out;
          } });

    tools.push_back({ "sell_cargo",
                      "Sell cargo at the station you are docked at. Sells everything of "
                      "that resource unless an amount is given.",
                      Obj({ { "resource", Num("resource index, as shown by observe") },
                            { "amount", Num("how much (default: all of it)") } },
                          { "resource" }),
                      [](const Rpc::Json& args)
                      {
                          RequireLive();
                          if (!g_session.Snapshot().player.docked)
                              throw Rpc::Error{ Rpc::INVALID_PARAMS,
                                                "not docked; dock at a station first" };
                          Proto::Command c;
                          c.sellType = (int)NumberOr(args, "resource", -1);
                          c.sellAmount = (int)NumberOr(args, "amount", 100000.0);
                          g_session.Send(c);
                          g_session.WaitUntil([] { return false; }, 0.5);  // let the ack land
                          return std::string("sell sent; call observe to see the result");
                      } });

    return tools;
}

}  // namespace

int main(int argc, char** argv)
{
    // stdout is the JSON-RPC channel and nothing else may touch it; diagnostics go to
    // stderr. Unbuffered because a client is waiting on each line.
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetTraceLogCallback(TraceToStderr);

    if (argc < 3 || std::strcmp(argv[1], "connect") != 0)
    {
        std::fprintf(stderr,
                     "econagent — EconSpace as an MCP server.\n"
                     "  %s connect <host> [port]     port defaults to 50800\n"
                     "\n"
                     "Start a server first:  econserver host 50800\n",
                     argv[0]);
        return 2;
    }

    const std::string    host = argv[2];
    const unsigned short port = (argc >= 4) ? (unsigned short)std::atoi(argv[3]) : 50800;

    std::string dataDir = AGENT_DATA_DIR;
    Factions::Load(dataDir + "factions.json");
    // The galaxy index is local, exactly as it is for the game client: GalaxyState carries
    // per-system statistics but not names, map positions or links.
    g_universe = WorldLoader::LoadUniverse(dataDir + "universe.json");

    if (!g_session.Connect(host, port))
    {
        std::fprintf(stderr, "econagent: could not connect to %s:%u\n", host.c_str(), port);
        return 1;
    }
    Rpc::Log("econagent: connected to " + host + ":" + std::to_string(port));

    // Give the server a moment to send the opening layout and snapshot, so the first
    // observe has a world in it rather than "no world state yet".
    g_session.WaitUntil([] { return g_session.HasSnapshot(); }, 5.0);

    const std::vector<Tool> tools = BuildTools();
    Rpc::Server             rpc;

    rpc.On("initialize",
           [](const Rpc::Json&)
           {
               return Rpc::Json{ { "protocolVersion", "2024-11-05" },
                                 { "capabilities", { { "tools", Rpc::Json::object() } } },
                                 { "serverInfo",
                                   { { "name", "econspace" }, { "version", "0.1.0" } } } };
           });

    rpc.On("tools/list",
           [&tools](const Rpc::Json&)
           {
               Rpc::Json list = Rpc::Json::array();
               for (const Tool& t : tools)
                   list.push_back({ { "name", t.name },
                                    { "description", t.description },
                                    { "inputSchema", t.schema } });
               return Rpc::Json{ { "tools", list } };
           });

    rpc.On("tools/call",
           [&tools](const Rpc::Json& params)
           {
               const std::string name = params.value("name", std::string());
               const Rpc::Json   args =
                   params.contains("arguments") ? params["arguments"] : Rpc::Json::object();
               for (const Tool& t : tools)
                   if (name == t.name)
                       return TextResult(t.run(args));
               throw Rpc::Error{ Rpc::INVALID_PARAMS, "no such tool: " + name };
           });

    rpc.Run();
    Rpc::Log("econagent: client closed the connection, exiting");
    Net::Shutdown();
    return 0;
}
