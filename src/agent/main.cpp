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

// The galaxy as a table: where the systems are relative to each other, how dangerous each
// one is and who holds it. An agent planning a trade run needs this and it does not change
// minute to minute, which is exactly what makes it a resource rather than a tool.
std::string DescribeGalaxy()
{
    std::string out = "GALAXY\n";
    for (const WorldLoader::SystemInfo& si : g_universe.systems)
    {
        std::string line = "  " + si.id + "  " + si.name;
        for (const Proto::GalaxySystemStat& g : g_session.Galaxy().systems)
            if (g.id == si.id)
            {
                char buf[160];
                std::snprintf(buf, sizeof(buf),
                              "   security %.2f  pirates %d  prosperity %.0f%%  held by %s",
                              g.security, g.pirates, g.prosperity * 100.0f,
                              FactionName(g.controller).c_str());
                line += buf;
                break;
            }
        if (si.id == g_session.Snapshot().systemId)
            line += "   <- you are here";
        out += line + "\n";
    }

    out += "GATES\n";
    for (const WorldLoader::SystemLink& l : g_universe.links)
        out += "  " + l.a + " <-> " + l.b + "\n";

    if (!g_session.Galaxy().events.empty())
    {
        out += "NEWS\n";
        for (const std::string& e : g_session.Galaxy().events)
            out += "  " + e + "\n";
    }
    return out;
}

// Ready-made plans, offered by the client for a user to pick.
struct Prompt
{
    const char* name;
    const char* description;
    const char* text;
};

const std::vector<Prompt>& Prompts()
{
    static const std::vector<Prompt> prompts = {
        { "mining_run", "Fill the hold from an asteroid field and sell it at a station",
          "Run a mining trip. Start with observe. Find an asteroid field and mine it with "
          "until_full set, then wait_for_event until that order finishes. Then dock at a "
          "station and sell what you mined. If something turns hostile while you are "
          "mining, break off and dock instead — the ore is not worth the ship." },
        { "trade_run", "Find a price difference between two systems and work it",
          "Look for a profitable trade. Read the galaxy resource to see which systems "
          "exist and how dangerous they are. Dock somewhere, note the market prices, then "
          "travel_to_system to a neighbour and compare. Buy where a resource is cheap and "
          "sell where it is dear. Prefer avoid_danger on the route when the hold is full — "
          "cargo lost is worse than time lost." },
        { "scout", "Visit each system and report what is there",
          "Scout the galaxy. For every system in the galaxy resource, travel_to_system to "
          "it, observe, and note the stations, asteroid fields and how much traffic and "
          "hostility you see. Report a short summary per system at the end. Do not pick "
          "fights; if a system looks dangerous, say so and move on." },
        { "patrol", "Hold a system and deal with hostiles you can handle",
          "Patrol the system you are in. Observe regularly. If hostiles appear, judge "
          "whether you can take them from their hull and numbers, and disengage if not — "
          "an order gives up below a quarter hull, but do not rely on that as a plan. "
          "Dock to repair when you are hurt." },
    };
    return prompts;
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

namespace
{

std::string RunTool(const std::vector<Tool>& tools, const std::string& name, const Rpc::Json& args)
{
    for (const Tool& t : tools)
        if (name == t.name)
            return t.run(args);
    return "no such tool: " + name;
}

// Scripted agent: the same tools an LLM calls, driven by a fixed sequence.
//
// This is what keeps the agent seam honest in CI. Testing it with a real model would cost
// money, need a key and give a different answer every run; testing it with the tools
// called directly proves the part that can actually break -- the bridge, the orders, the
// journal and the round trip through the server.
// Run: econagent selftest <host> [port]   (a server must already be listening)
int Selftest(const std::vector<Tool>& tools)
{
    auto note = [](const char* what, bool ok)
    { std::fprintf(stderr, "  %-22s %s\n", what, ok ? "OK" : "FAIL"); };

    // 1) The world is visible at all.
    std::string world = RunTool(tools, "observe", Rpc::Json::object());
    bool        observed =
        world.find("SHIP") != std::string::npos && world.find("SYSTEM") != std::string::npos;
    note("observe", observed);

    // 2) Find something to fly to. Whatever station the report lists first will do; the
    // point is that ids from observe are the ids orders accept.
    int    stationId = 0;
    size_t at = world.find(" station ");
    if (at != std::string::npos)
    {
        size_t hash = world.rfind('#', at);
        if (hash != std::string::npos)
            stationId = std::atoi(world.c_str() + hash + 1);
    }
    note("station id from observe", stationId != 0);

    // 3) Give an order and let the server fly it.
    bool ordered = false, arrived = false;
    if (stationId != 0)
    {
        std::string reply = RunTool(
            tools, "move_to",
            Rpc::Json{ { "target_id", stationId }, { "warp", true }, { "stop_distance", 400 } });
        ordered = reply.find("accepted") != std::string::npos;
        note("move_to accepted", ordered);

        if (ordered)
        {
            std::string ev =
                RunTool(tools, "wait_for_event", Rpc::Json{ { "timeout_seconds", 120 } });
            arrived = ev.find("order_done") != std::string::npos;
            note("order completed", arrived);
        }
    }

    // 4) An order naming something absent must be refused, not silently swallowed.
    std::string bogus = RunTool(tools, "dock", Rpc::Json{ { "station_id", 999999 } });
    bool        refused = bogus.find("refused") != std::string::npos ||
                          bogus.find("not in this system") != std::string::npos;
    note("bad target refused", refused);

    const bool ok = observed && stationId != 0 && ordered && arrived && refused;
    std::fprintf(stderr, "Agent selftest: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv)
{
    // stdout is the JSON-RPC channel and nothing else may touch it; diagnostics go to
    // stderr. Unbuffered because a client is waiting on each line.
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetTraceLogCallback(TraceToStderr);

    const bool selftest = argc >= 3 && std::strcmp(argv[1], "selftest") == 0;
    if (!selftest && (argc < 3 || std::strcmp(argv[1], "connect") != 0))
    {
        std::fprintf(stderr,
                     "econagent — EconSpace as an MCP server.\n"
                     "  %s connect <host> [port] [name]   serve MCP on stdio\n"
                     "  %s selftest <host> [port] [name]  scripted run, no model needed\n"
                     "\n"
                     "Start a server first:  econserver host 50800\n",
                     argv[0], argv[0]);
        return 2;
    }

    const std::string    host = argv[2];
    const unsigned short port = (argc >= 4) ? (unsigned short)std::atoi(argv[3]) : 50800;
    // The account this agent flies under. Its own by default: an agent and the human who
    // started it are two players, and sharing one account would have them share a ship.
    const std::string account = (argc >= 5) ? argv[4] : "agent";

    std::string dataDir = AGENT_DATA_DIR;
    Factions::Load(dataDir + "factions.json");
    // Read locally too: describing an object to a model means saying what it can do, and
    // that lives in the archetype rather than in the snapshot.
    if (!Archetypes::Load(dataDir + "archetypes.json"))
        Rpc::Log("econagent: " + Archetypes::Error());
    // The galaxy index is local, exactly as it is for the game client: GalaxyState carries
    // per-system statistics but not names, map positions or links.
    g_universe = WorldLoader::LoadUniverse(dataDir + "universe.json");

    if (!g_session.Connect(host, port, account))
    {
        std::fprintf(stderr, "econagent: could not connect to %s:%u\n", host.c_str(), port);
        return 1;
    }
    Rpc::Log("econagent: connected to " + host + ":" + std::to_string(port));

    // Give the server a moment to send the opening layout and snapshot, so the first
    // observe has a world in it rather than "no world state yet".
    g_session.WaitUntil([] { return g_session.HasSnapshot(); }, 5.0);

    const std::vector<Tool> tools = BuildTools();
    if (selftest)
        return Selftest(tools);

    Rpc::Server rpc;

    rpc.On("initialize",
           [](const Rpc::Json&)
           {
               return Rpc::Json{ { "protocolVersion", "2024-11-05" },
                                 { "capabilities",
                                   { { "tools", Rpc::Json::object() },
                                     { "resources", Rpc::Json::object() },
                                     { "prompts", Rpc::Json::object() } } },
                                 { "serverInfo",
                                   { { "name", "econspace" }, { "version", "0.1.0" } } } };
           });

    // --- Resources ----------------------------------------------------------
    // Pull-based, unlike tools: a long briefing costs tokens only when the agent decides
    // it needs one. The system map is the same projection observe returns, which is the
    // point -- there is one description of the world, not one per consumer.
    rpc.On(
        "resources/list",
        [](const Rpc::Json&)
        {
            return Rpc::Json{
                { "resources",
                  Rpc::Json::array({ Rpc::Json{ { "uri", "econspace://system" },
                                                { "name", "Current system" },
                                                { "description",
                                                  "Everything visible where the ship is, in full" },
                                                { "mimeType", "text/plain" } },
                                     Rpc::Json{ { "uri", "econspace://galaxy" },
                                                { "name", "Galaxy" },
                                                { "description",
                                                  "Every system, its security, controller and gate "
                                                  "links, plus recent galactic news" },
                                                { "mimeType", "text/plain" } } }) }
            };
        });

    rpc.On("resources/read",
           [](const Rpc::Json& params)
           {
               const std::string uri = params.value("uri", std::string());
               std::string       text;
               if (uri == "econspace://system")
               {
                   RequireLive();
                   text = g_session.Describe(Obs::Detail::Full, &g_universe);
               }
               else if (uri == "econspace://galaxy")
               {
                   RequireLive();
                   text = DescribeGalaxy();
               }
               else
               {
                   throw Rpc::Error{ Rpc::INVALID_PARAMS, "no such resource: " + uri };
               }
               return Rpc::Json{ { "contents",
                                   Rpc::Json::array({ Rpc::Json{ { "uri", uri },
                                                                 { "mimeType", "text/plain" },
                                                                 { "text", text } } }) } };
           });

    // --- Prompts ------------------------------------------------------------
    // Canned plans a user picks from their client. They double as the honest test of the
    // tool surface: if a prompt here cannot be carried out with the tools above, the tools
    // are incomplete.
    rpc.On("prompts/list",
           [](const Rpc::Json&)
           {
               Rpc::Json list = Rpc::Json::array();
               for (const auto& p : Prompts())
                   list.push_back({ { "name", p.name }, { "description", p.description } });
               return Rpc::Json{ { "prompts", list } };
           });

    rpc.On("prompts/get",
           [](const Rpc::Json& params)
           {
               const std::string name = params.value("name", std::string());
               for (const auto& p : Prompts())
                   if (name == p.name)
                       return Rpc::Json{
                           { "description", p.description },
                           { "messages",
                             Rpc::Json::array({ Rpc::Json{
                                 { "role", "user" },
                                 { "content", { { "type", "text" }, { "text", p.text } } } } }) }
                       };
               throw Rpc::Error{ Rpc::INVALID_PARAMS, "no such prompt: " + name };
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
