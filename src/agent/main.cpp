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
// Run:  econagent connect <host> <port> <name> <secret>
// Wire it to a client:
//   claude mcp add econspace -- <path>/econagent.exe connect 127.0.0.1 50800

#include "agent/Jsonrpc.h"
#include "agent/Session.h"

#include "core/Actions.h"  // WARP_MIN: when a move is worth a warp
#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "core/WorldLoader.h"
#include "economy/Resource.h"
#include "entities/ShipType.h"
#include "missions/MissionSystem.h"
#include "sim/Names.h"
#include "sim/Orders.h"

#include "raylib.h"

#include <atomic>
#include <cctype>
#include <cmath>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <thread>

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

Agent::Session g_session;

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
    if (!g_session.ByeReason().empty())
        throw Rpc::Error{ Rpc::INTERNAL_ERROR,
                          "the server ended this session: " + g_session.ByeReason() };
    if (!g_session.Alive())
        throw Rpc::Error{ Rpc::INTERNAL_ERROR,
                          "connection to the server is closed: " + g_session.CloseReason() };
}

double NumberOr(const Rpc::Json& args, const char* key, double fallback)
{
    return args.contains(key) && args[key].is_number() ? args[key].get<double>() : fallback;
}

// The last journal entry handed to the model. Separate from the session's own cursor,
// which only tracks what has been received (#113).
int g_reportedEventSeq = 0;

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
    for (const WorldLoader::SystemInfo& si : g_session.Universe().systems)
    {
        std::string line = "  " + si.id + "  " + si.name;
        if (!si.charted)  // a gate leads there; nobody has been (#144)
            line += "   uncharted: nobody has been there yet";
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
    for (const WorldLoader::SystemLink& l : g_session.Universe().links)
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
          "travel_to_system to a neighbour and compare. There is no buy tool yet, so the "
          "cargo to trade is cargo you mine: mine where a resource is plentiful and sell "
          "it where it is dear. Prefer avoid_danger on the route when the hold is full — "
          "cargo lost is worse than time lost." },
        { "scout", "Visit each system and report what is there",
          "Scout the galaxy. For every system in the galaxy resource, travel_to_system to "
          "it, observe, and note the stations, asteroid fields and how much traffic and "
          "hostility you see. Report a short summary per system at the end. Do not pick "
          "fights; if a system looks dangerous, say so and move on." },
        { "contract_work", "Take jobs from a station's board and earn from them",
          "Work the job boards. Dock at a station and call missions to read its board. Take "
          "work you can actually do with accept_mission: a mining job needs ore you can mine "
          "nearby, a delivery needs only the trip, a bounty means fighting pirates -- judge "
          "that from observe before taking one. Do the work, then dock where the job says and "
          "complete_mission. If a faction has a bounty on you, pay_bounty before its ships "
          "find you. When the hangar offers a ship that suits the work better and you can "
          "afford it, buy_ship." },
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
    // Built key by key, on purpose. `Rpc::Json(props)` on a list of pairs gives an *array* of
    // [name, schema] pairs, not an object -- which is not JSON Schema, so every tool shipped
    // an input schema a strict MCP client could not read the argument names from. It went
    // unnoticed until the reference generator (#172) tried to read the schemas itself.
    Rpc::Json properties = Rpc::Json::object();
    for (const auto& kv : props)
        properties[kv.first] = kv.second;
    Rpc::Json schema{ { "type", "object" }, { "properties", properties } };
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

// Which resource a tool call means. By name, because that is what observe shows: the
// description used to say "index, as shown by observe" while observe showed names, so a
// bot had no way to learn what to pass (#171). Matched exactly, ignoring case, and refused
// otherwise -- ResourceFromName quietly turns any unknown name into Iron, and a bot asking
// to sell Gold must not sell its iron instead. A number is still accepted for scripts
// written against the old description.
int ResourceArg(const Rpc::Json& args)
{
    if (!args.contains("resource"))
        throw Rpc::Error{ Rpc::INVALID_PARAMS, "resource is required" };
    const Rpc::Json& r = args["resource"];
    if (r.is_number())
        return r.get<int>();
    if (!r.is_string())
        throw Rpc::Error{ Rpc::INVALID_PARAMS, "resource is a name, e.g. 'Iron'" };

    auto lower = [](std::string s)
    {
        for (char& ch : s)
            ch = (char)std::tolower((unsigned char)ch);
        return s;
    };
    const std::string want = lower(r.get<std::string>());
    const auto&       types = AllResourceTypes();
    std::string       known;
    for (size_t i = 0; i < types.size(); i++)
    {
        if (lower(ResourceName(types[i])) == want)
            return (int)i;
        known += (known.empty() ? "" : ", ") + ResourceName(types[i]);
    }
    throw Rpc::Error{ Rpc::INVALID_PARAMS,
                      "no resource called '" + r.get<std::string>() + "'; known: " + known };
}

// --- Station business (#109) --------------------------------------------------
// Buying, switching, paying and handing in are not orders: there is no status to watch. So
// each of these checks first what the agent can see for itself -- the same snapshot a
// player's station screen reads -- and names the reason, sends the command, waits for the
// server to acknowledge it, and then reports what actually changed. The server stays the
// judge, and since #219 it says why it declined, in a journal Notice that arrives in the
// same snapshot as the acknowledgement; a refusal quotes it.

Obs::View CurrentView()
{
    Obs::View v;
    v.snapshot = g_session.HasSnapshot() ? &g_session.Snapshot() : nullptr;
    v.layout = &g_session.Layout();
    v.universe = &g_session.Universe();
    v.galaxy = &g_session.Galaxy();
    return v;
}

void RequireDocked(const char* what)
{
    if (!g_session.Snapshot().player.docked)
        throw Rpc::Error{ Rpc::INVALID_PARAMS, std::string("not docked; ") + what +
                                                   " happens at a station -- dock first" };
}

// An index argument, checked against how many there are, so a model that miscounted is told
// what the valid numbers are rather than that nothing happened.
int IndexArg(const Rpc::Json& args, const char* key, size_t count, const char* listedBy)
{
    if (!args.contains(key) || !args[key].is_number_integer())
        throw Rpc::Error{ Rpc::INVALID_PARAMS,
                          std::string(key) + " is required: the number " + listedBy + " shows" };
    const int i = args[key].get<int>();
    if (count == 0)
        throw Rpc::Error{ Rpc::INVALID_PARAMS, std::string("there is nothing to choose from; ") +
                                                   "call " + listedBy + " to see why" };
    if (i < 0 || i >= (int)count)
        throw Rpc::Error{ Rpc::INVALID_PARAMS, std::string(key) + " must be 0.." +
                                                   std::to_string((int)count - 1) +
                                                   ", as numbered by " + listedBy };
    return i;
}

std::string Lower(std::string s)
{
    for (char& ch : s)
        ch = (char)std::tolower((unsigned char)ch);
    return s;
}

// A ship by catalog name ("Hauler") or by its number in the hangar.
int ShipArg(const Rpc::Json& args)
{
    const std::vector<ShipType>& catalog = GetShipCatalog();
    if (args.contains("ship") && args["ship"].is_number_integer())
    {
        const int i = args["ship"].get<int>();
        if (i >= 0 && i < (int)catalog.size())
            return i;
    }
    else if (args.contains("ship") && args["ship"].is_string())
    {
        const std::string want = Lower(args["ship"].get<std::string>());
        for (size_t i = 0; i < catalog.size(); i++)
            if (Lower(catalog[i].name) == want)
                return (int)i;
    }
    std::string known;
    for (const ShipType& t : catalog)
        known += (known.empty() ? "" : ", ") + t.name;
    throw Rpc::Error{ Rpc::INVALID_PARAMS, "ship is a name from hangar; known: " + known };
}

bool OwnsShip(int index)
{
    for (int o : g_session.Snapshot().player.ownedShips)
        if (o == index)
            return true;
    return false;
}

// The faction a bounty is paid to: the one named, or the owner of this station.
FactionId FactionArg(const Rpc::Json& args)
{
    if (!args.contains("faction"))
        return Obs::DockedFaction(CurrentView());
    if (!args["faction"].is_string())
        throw Rpc::Error{ Rpc::INVALID_PARAMS, "faction is a name, e.g. 'Syndicate'" };
    const std::string want = Lower(args["faction"].get<std::string>());
    std::string       known;
    for (int i = 0; i < 4; i++)
    {
        const std::string name = FactionName((FactionId)i);
        if (Lower(name) == want)
            return (FactionId)i;
        known += (known.empty() ? "" : ", ") + name;
    }
    throw Rpc::Error{ Rpc::INVALID_PARAMS, "no faction called '" +
                                               args["faction"].get<std::string>() +
                                               "'; known: " + known };
}

// Sends a station command and returns false, with the reason in `why`, if the server never
// acknowledged it. Two seconds is forty snapshots; a missing acknowledgement means the
// connection is in trouble, not that the server is thinking.
bool Confirm(const Proto::Command& c, std::string& why)
{
    if (g_session.SendAndConfirm(c, 2.0))
        return true;
    RequireLive();  // a dead connection is the likelier story; say that if it is
    why = "sent, but the server has not acknowledged it yet; call observe before retrying";
    return false;
}

// A refusal, in the server's words when it gave any: the newest Notice journalled after
// `seq`, which was the journal's end when the command was sent. `fallback` covers a server
// that said nothing.
std::string Refused(const char* tool, int seq, const std::string& fallback)
{
    std::string said;
    for (const Ev::Event& e : g_session.EventsSince(seq))
        if (e.kind == Ev::Kind::Notice)
            said = e.text;
    return std::string(tool) + ": " + (said.empty() ? fallback : "refused -- " + said);
}

std::string Money(double cr)
{
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.0f cr", cr);
    return buf;
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
              return g_session.Describe(d == "full" ? Obs::Detail::Full : Obs::Detail::Brief);
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

    tools.push_back(
        { "hold_station",
          "Hold station on an object at a distance you choose, until another order replaces "
          "it or abort_order ends it. mode 'orbit' circles it, 'keep' holds that distance "
          "from it and moves only as much as that takes, 'follow' holds the distance and "
          "matches its velocity -- the one that stays with a station going round a planet, "
          "or with a moving ship. It "
          "flies there at sublight: for something far away, move_to with warp first.",
          Obj({ { "target_id", Num("object id from observe") },
                { "mode", Str("'orbit', 'keep' or 'follow'") },
                { "range", Num("distance to hold from its centre (default 500)") } },
              { "target_id", "mode" }),
          [](const Rpc::Json& args)
          {
              RequireLive();
              const std::string mode = Lower(args.value("mode", std::string()));
              Orders::Kind      kind = Orders::Kind::None;
              if (mode == "orbit")
                  kind = Orders::Kind::Orbit;
              else if (mode == "keep")
                  kind = Orders::Kind::Keep;
              else if (mode == "follow")
                  kind = Orders::Kind::Follow;
              else
                  throw Rpc::Error{ Rpc::INVALID_PARAMS, "mode is 'orbit', 'keep' or 'follow'" };
              const double range = NumberOr(args, "range", 500.0);
              if (!(range >= 1.0))
                  throw Rpc::Error{ Rpc::INVALID_PARAMS, "range is a distance of at least 1" };
              Proto::Command c = OrderCommand(kind);
              c.orderTarget = (int)NumberOr(args, "target_id", 0);
              c.orderStopDist = (float)range;
              return GiveOrder(c, "hold_station");
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
                          c.orderAvoidDanger = args.value("avoid_danger", false);
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
              // What the model has been shown, which is not the same as what has
              // arrived (#113). RequireLive pumps the socket, so reading the session's
              // cursor here would put every event that came in first behind it -- and an
              // order that finished quickly would be waited out and then reported as
              // "nothing happened".
              const int since = g_reportedEventSeq;
              RequireLive();
              double timeout = NumberOr(args, "timeout_seconds", 60.0);
              if (timeout > 300.0)
                  timeout = 300.0;  // an agent should not be able to hold a session forever
              g_session.WaitUntil([&] { return g_session.LastEventSeq() > since; }, timeout);

              std::vector<Ev::Event> fresh = g_session.EventsSince(since);
              if (fresh.empty())
                  return std::string("nothing happened within the timeout");
              g_reportedEventSeq = fresh.back().seq;
              std::string out;
              for (const Ev::Event& e : fresh)
                  out += "[" + std::to_string(e.seq) + "] " + Ev::KindName(e.kind) + ": " + e.text +
                         "\n";
              return out;
          } });

    tools.push_back({ "sell_cargo",
                      "Sell cargo at the station you are docked at. Sells everything of "
                      "that resource unless an amount is given.",
                      Obj({ { "resource", Str("resource name as observe lists it under "
                                              "CARGO, e.g. 'Iron'") },
                            { "amount", Num("how much (default: all of it)") } },
                          { "resource" }),
                      [](const Rpc::Json& args)
                      {
                          RequireLive();
                          if (!g_session.Snapshot().player.docked)
                              throw Rpc::Error{ Rpc::INVALID_PARAMS,
                                                "not docked; dock at a station first" };
                          Proto::Command c;
                          c.sellType = ResourceArg(args);
                          c.sellAmount = (int)NumberOr(args, "amount", 100000.0);
                          g_session.Send(c);
                          g_session.WaitUntil([] { return false; }, 0.5);  // let the ack land
                          return std::string("sell sent; call observe to see the result");
                      } });

    tools.push_back(
        { "missions",
          "The job board of the station you are docked at, and the missions you have taken: "
          "what each asks, what it pays, where it is handed in and what it still needs. The "
          "numbers are what accept_mission and complete_mission take.",
          Obj({}), [](const Rpc::Json&)
          {
              RequireLive();
              return Obs::DescribeMissions(CurrentView());
          } });

    static_assert(MissionSystem::MAX_ACTIVE == 5, "accept_mission's description quotes the cap");
    tools.push_back(
        { "accept_mission",
          "Take a job from the board of the station you are docked at. It joins your active "
          "missions and stays with you across systems until you hand it in. At most "
          "5 can be active at once.",
          Obj({ { "offer", Num("offer number from missions") } }, { "offer" }),
          [](const Rpc::Json& args)
          {
              RequireLive();
              RequireDocked("taking work");
              const Proto::Snapshot& before = g_session.Snapshot();
              const int i = IndexArg(args, "offer", before.missionOffers.size(), "missions");
              const Proto::MissionView taken = before.missionOffers[i];
              const size_t             activeBefore = before.missionActive.size();

              const int      seq = g_session.LastEventSeq();
              Proto::Command c;
              c.acceptOffer = i;
              std::string why;
              if (!Confirm(c, why))
                  return "accept_mission: " + why;
              const Proto::Snapshot& after = g_session.Snapshot();
              if (after.missionActive.size() <= activeBefore)
                  return Refused("accept_mission", seq,
                                 "the server did not take it; call missions to see the board "
                                 "as it is now");
              const Proto::MissionView& m = after.missionActive.back();
              return "accept_mission: taken -- " + m.title + ", " + Money(m.rewardMoney) +
                     ". It is active mission [" + std::to_string(after.missionActive.size() - 1) +
                     "]; it needs: " +
                     (m.completable ? std::string("nothing more, hand it in")
                                    : Obs::MissionNeeds(CurrentView(), m)) +
                     (taken.title == m.title ? "" : " (the board had changed)");
          } });

    tools.push_back(
        { "complete_mission",
          "Hand in an active mission at the station you are docked at, for its reward. A "
          "bounty or mining job goes back to the station that gave it, a delivery to its "
          "destination; mining hands over the ore from the hold.",
          Obj({ { "mission", Num("active mission number from missions") } }, { "mission" }),
          [](const Rpc::Json& args)
          {
              RequireLive();
              RequireDocked("handing in work");
              const Proto::Snapshot& before = g_session.Snapshot();
              const int i = IndexArg(args, "mission", before.missionActive.size(), "missions");
              const Proto::MissionView m = before.missionActive[i];
              if (!m.completable)
                  throw Rpc::Error{ Rpc::INVALID_PARAMS,
                                    "'" + m.title + "' cannot be handed in here yet; it needs: " +
                                        Obs::MissionNeeds(CurrentView(), m) };
              const double moneyBefore = before.player.money;
              const size_t activeBefore = before.missionActive.size();

              const int      seq = g_session.LastEventSeq();
              Proto::Command c;
              c.completeMission = i;
              std::string why;
              if (!Confirm(c, why))
                  return "complete_mission: " + why;
              const Proto::Snapshot& after = g_session.Snapshot();
              if (after.missionActive.size() >= activeBefore)
                  return Refused("complete_mission", seq,
                                 "the server did not accept the hand-in; call missions to see "
                                 "what it still needs");
              return "complete_mission: handed in '" + m.title + "' -- paid " +
                     Money(after.player.money - moneyBefore) + ", money now " +
                     Money(after.player.money);
          } });

    tools.push_back({ "hangar",
                      "The ships you own, the one you are flying, and what every other hull "
                      "costs at this station -- at the price you would actually pay, which "
                      "depends on your standing with the station's owner.",
                      Obj({}), [](const Rpc::Json&)
                      {
                          RequireLive();
                          return Obs::DescribeHangar(CurrentView());
                      } });

    tools.push_back(
        { "buy_ship",
          "Buy a ship at the station you are docked at and fly it from now on. Your old ship "
          "stays in the hangar; switch_ship goes back to it for free. Cargo capacity becomes "
          "the new hull's, so a hull too small for what you carry is refused.",
          Obj({ { "ship", Str("ship name as hangar lists it, e.g. 'Hauler'") } }, { "ship" }),
          [](const Rpc::Json& args)
          {
              RequireLive();
              RequireDocked("buying a ship");
              const int       i = ShipArg(args);
              const ShipType& t = GetShipCatalog()[i];
              if (OwnsShip(i))
                  throw Rpc::Error{ Rpc::INVALID_PARAMS,
                                    "you already own a " + t.name + "; use switch_ship" };
              const Proto::PlayerView& p = g_session.Snapshot().player;
              const FactionId          sf = Obs::DockedFaction(CurrentView());
              const float              standing =
                  (size_t)sf < p.reputation.size() ? p.reputation[(size_t)sf] : 0.0f;
              const double price = t.price * ShipPriceMultiplier(Factions::TierOf(standing));
              if (p.money < price)
                  throw Rpc::Error{ Rpc::INVALID_PARAMS, "a " + t.name + " costs " + Money(price) +
                                                             " here and you have " +
                                                             Money(p.money) };
              const double moneyBefore = p.money;

              const int      seq = g_session.LastEventSeq();
              Proto::Command c;
              c.buyShip = i;
              std::string why;
              if (!Confirm(c, why))
                  return "buy_ship: " + why;
              if (!OwnsShip(i))
                  return Refused("buy_ship", seq,
                                 "the server did not sell it; call hangar to see the price and "
                                 "your money as they are now");
              const Proto::PlayerView& after = g_session.Snapshot().player;
              return "buy_ship: bought a " + t.name + " for " + Money(moneyBefore - after.money) +
                     " and flying it -- cargo capacity " + std::to_string(t.stats.cargoCapacity) +
                     ", money now " + Money(after.money);
          } });

    tools.push_back(
        { "switch_ship",
          "Fly another ship you already own, at the station you are docked at. "
          "Free; hangar lists what you own. Refused if what you carry would not fit "
          "its hold.",
          Obj({ { "ship", Str("ship name as hangar lists it, e.g. 'Scout'") } }, { "ship" }),
          [](const Rpc::Json& args)
          {
              RequireLive();
              RequireDocked("switching ships");
              const int       i = ShipArg(args);
              const ShipType& t = GetShipCatalog()[i];
              if (g_session.Snapshot().player.shipIndex == i)
                  return "switch_ship: already flying the " + t.name;
              if (!OwnsShip(i))
                  throw Rpc::Error{ Rpc::INVALID_PARAMS,
                                    "you do not own a " + t.name + "; buy_ship first" };

              const int      seq = g_session.LastEventSeq();
              Proto::Command c;
              c.refitShip = i;
              std::string why;
              if (!Confirm(c, why))
                  return "switch_ship: " + why;
              if (g_session.Snapshot().player.shipIndex != i)
                  return Refused("switch_ship", seq,
                                 "the server did not switch; call hangar to see what it says "
                                 "you own");
              return "switch_ship: now flying the " + t.name + " -- cargo capacity " +
                     std::to_string(t.stats.cargoCapacity);
          } });

    tools.push_back(
        { "pay_bounty",
          "Pay off the bounty a faction has on you, at a station, so its ships stop hunting "
          "you. Costs the whole bounty. observe with detail='full' lists who wants you.",
          Obj({ { "faction", Str("faction name; default: the faction that owns this station") } }),
          [](const Rpc::Json& args)
          {
              RequireLive();
              RequireDocked("paying a bounty");
              const FactionId          f = FactionArg(args);
              const Proto::PlayerView& p = g_session.Snapshot().player;
              const double owed = (size_t)f < p.bounty.size() ? p.bounty[(size_t)f] : 0.0;
              if (owed <= 0.0)
              {
                  std::string wanted;
                  for (size_t i = 0; i < p.bounty.size(); i++)
                      if (p.bounty[i] > 0.0)
                          wanted += (wanted.empty() ? "" : ", ") + FactionName((FactionId)i) + " " +
                                    Money(p.bounty[i]);
                  throw Rpc::Error{ Rpc::INVALID_PARAMS,
                                    FactionName(f) + " has no bounty on you" +
                                        (wanted.empty() ? std::string("; nobody does")
                                                        : "; wanted by: " + wanted) };
              }
              if (p.money < owed)
                  throw Rpc::Error{ Rpc::INVALID_PARAMS, "the bounty is " + Money(owed) +
                                                             " and you have " + Money(p.money) };
              const double moneyBefore = p.money;

              const int      seq = g_session.LastEventSeq();
              Proto::Command c;
              c.payBountyFaction = (int)f;
              std::string why;
              if (!Confirm(c, why))
                  return "pay_bounty: " + why;
              const Proto::PlayerView& after = g_session.Snapshot().player;
              const double left = (size_t)f < after.bounty.size() ? after.bounty[(size_t)f] : 0.0;
              if (left > 0.0)
                  return Refused("pay_bounty", seq,
                                 "the server did not take the payment; call observe with "
                                 "detail='full' to see the bounty now");
              return "pay_bounty: paid " + Money(moneyBefore - after.money) + " to " +
                     FactionName(f) + "; no longer wanted by them. Money now " + Money(after.money);
          } });

    tools.push_back(
        { "name_system",
          "Name the system you are in. Beyond the wormhole a system has only a designation "
          "(W-3.2) until whoever got there first names it -- once, for everyone. 3 to 24 "
          "characters, starting with a letter; letters, digits, spaces, ' and -; not a name "
          "another system has.",
          Obj({ { "name", Str("the name, e.g. 'Haven'") } }, { "name" }), [](const Rpc::Json& args)
          {
              RequireLive();
              const std::string name = args.contains("name") && args["name"].is_string()
                                           ? args["name"].get<std::string>()
                                           : std::string();
              std::string       why;
              if (!Names::ValidSystemName(name, why))
                  throw Rpc::Error{ Rpc::INVALID_PARAMS, why };
              const std::string here = g_session.Snapshot().systemId;
              auto              named = [&]
              {
                  for (const auto& si : g_session.Universe().systems)
                      if (si.id == here)
                          return si.name == name;
                  return false;
              };
              const int      seq = g_session.LastEventSeq();
              Proto::Command c;
              c.nameSystem = name;
              if (!Confirm(c, why))
                  return "name_system: " + why;
              // The new name reaches everyone in the galaxy index the server resends.
              g_session.WaitUntil(named, 3.0);
              if (!named())
                  return Refused("name_system", seq, "the server did not name it");
              return "name_system: " + here + " is now " + name + ", for everyone";
          } });

    tools.push_back(
        { "blueprints",
          "What you can build in space (#39): each blueprint's id, what it costs from the "
          "hold, how long the site takes, how long the result stands, and how close to your "
          "ship it must go. Marks the ones your hold can pay for now.",
          Obj({}), [](const Rpc::Json&)
          {
              RequireLive();
              if (Blueprints::All().empty())
                  return std::string("blueprints: none are known to this bridge");
              const Proto::PlayerView&  p = g_session.Snapshot().player;
              std::vector<ResourceType> types = AllResourceTypes();
              auto                      held = [&](ResourceType r)
              {
                  for (size_t i = 0; i < types.size() && i < p.cargoByType.size(); i++)
                      if (types[i] == r)
                          return p.cargoByType[i];
                  return 0;
              };
              std::string out = "BLUEPRINTS\n";
              for (const Blueprint& bp : Blueprints::All())
              {
                  std::string cost;
                  bool        affordable = true;
                  for (const auto& c : bp.cost)
                  {
                      cost += (cost.empty() ? "" : ", ") + std::to_string(c.second) + " " +
                              ResourceName(c.first);
                      affordable = affordable && held(c.first) >= c.second;
                  }
                  char line[256];
                  std::snprintf(line, sizeof(line),
                                "  %-8s %-14s %s; builds in %.0fs; stands %s; within %.0fu of "
                                "the ship%s\n",
                                bp.id.c_str(), bp.name.c_str(), cost.c_str(), bp.buildSeconds,
                                bp.lifetime > 0.0f
                                    ? (std::to_string((int)(bp.lifetime / 60.0f)) + " min").c_str()
                                    : "until removed",
                                bp.reach, affordable ? "  CAN AFFORD" : "");
                  out += line;
              }
              if (Blueprints::PerAccount() > 0)
                  out += "At most " + std::to_string(Blueprints::PerAccount()) +
                         " structures standing per account.\n";
              return out;
          } });

    tools.push_back(
        { "deploy",
          "Lay down a construction site from a blueprint, near your ship, out of the hold. It "
          "takes the cost at once, finishes by itself after the build time -- wait_for_event "
          "wakes on 'built' -- and everyone in the system sees it. Refused, with the reason, "
          "if the hold is short, the place is too close to something or in a planet's path, "
          "or you are docked or warping. blueprints lists what can be built.",
          Obj({ { "blueprint", Str("blueprint id from blueprints, e.g. 'beacon'") },
                { "name", Str("what to call it (default: the blueprint's name); 3 to 24 "
                              "characters, letters, digits, spaces, ' and -") },
                { "x", Num("where, world x (default: where the ship is)") },
                { "y", Num("where, world y (default: where the ship is)") } },
              { "blueprint" }),
          [](const Rpc::Json& args)
          {
              RequireLive();
              const std::string id = args.contains("blueprint") && args["blueprint"].is_string()
                                         ? args["blueprint"].get<std::string>()
                                         : std::string();
              const Blueprint*  bp = Blueprints::Find(id);
              if (bp == nullptr)
              {
                  std::string known;
                  for (const Blueprint& b : Blueprints::All())
                      known += (known.empty() ? "" : ", ") + b.id;
                  throw Rpc::Error{ Rpc::INVALID_PARAMS,
                                    "no blueprint '" + id + "'; known: " + known };
              }
              const std::string name = args.contains("name") && args["name"].is_string()
                                           ? args["name"].get<std::string>()
                                           : std::string();
              std::string       why;
              if (!name.empty() && !Names::ValidSystemName(name, why))
                  throw Rpc::Error{ Rpc::INVALID_PARAMS, why };
              const Proto::PlayerView& p = g_session.Snapshot().player;
              if (p.docked)
                  throw Rpc::Error{ Rpc::INVALID_PARAMS, "docked; undock first" };
              const Vector2 at = { (float)NumberOr(args, "x", p.pos.x),
                                   (float)NumberOr(args, "y", p.pos.y) };
              const float   dx = at.x - p.pos.x, dy = at.y - p.pos.y;
              if (std::sqrt(dx * dx + dy * dy) > bp->reach)
                  throw Rpc::Error{ Rpc::INVALID_PARAMS,
                                    "too far from the ship; a " + bp->name + " goes within " +
                                        std::to_string((int)bp->reach) + "u of it" };

              // Ours and new: the server's answer is an object in the layout, sent as a delta
              // before the snapshot that acknowledges the command.
              std::set<int> before;
              for (const auto& kv : g_session.Layout())
                  before.insert(kv.first);
              auto laid = [&]() -> const Proto::EntityLayout*
              {
                  for (const auto& kv : g_session.Layout())
                      if (kv.second.kind == Proto::EntityKind::Structure &&
                          kv.second.owner == g_session.Account() && before.count(kv.first) == 0)
                          return &kv.second;
                  return nullptr;
              };
              const int      seq = g_session.LastEventSeq();
              Proto::Command c;
              c.deploy = bp->id;
              c.deployPos = at;
              c.deployName = name;
              if (!Confirm(c, why))
                  return "deploy: " + why;
              const Proto::EntityLayout* site = laid();
              if (site == nullptr)
                  return Refused("deploy", seq, "the server did not lay it down");
              char out[256];
              std::snprintf(out, sizeof(out),
                            "deploy: site #%d, %s, laid down at (%.0f, %.0f); built in %.0fs. "
                            "wait_for_event wakes on 'built'.",
                            site->id, site->name.c_str(), site->pos.x, site->pos.y,
                            site->completesAt - g_session.Snapshot().time);
              return std::string(out);
          } });

    tools.push_back(
        { "dismantle",
          "Take apart a site or structure you built, from within its blueprint's reach. What "
          "comes back goes into the hold: all of the cost for a site just laid down, falling to "
          "half for a finished structure, and less for whatever has been shot off. Refused, "
          "with the reason, if it is not yours, you are too far, docked or warping, or the "
          "hold has no room for what it returns.",
          Obj({ { "structure_id", Num("structure id from observe") } }, { "structure_id" }),
          [](const Rpc::Json& args)
          {
              RequireLive();
              const int  id = (int)NumberOr(args, "structure_id", 0);
              const auto it = g_session.Layout().find(id);
              if (it == g_session.Layout().end() || it->second.kind != Proto::EntityKind::Structure)
                  throw Rpc::Error{ Rpc::INVALID_PARAMS,
                                    "no structure #" + std::to_string(id) + " in this system" };
              if (it->second.owner != g_session.Account())
                  throw Rpc::Error{ Rpc::INVALID_PARAMS,
                                    it->second.name +
                                        " is not yours; attack takes down another's" };
              const std::string name = it->second.name;
              const int         seq = g_session.LastEventSeq();
              Proto::Command    c;
              c.dismantleId = id;
              std::string why;
              if (!Confirm(c, why))
                  return "dismantle: " + why;
              // The answer is in the journal either way: what came back, or why not.
              for (const Ev::Event& e : g_session.EventsSince(seq))
                  if (e.kind == Ev::Kind::Notice && e.text.rfind("Dismantled ", 0) == 0)
                      return "dismantle: " + e.text;
              return Refused("dismantle", seq, "the server did not take " + name + " apart");
          } });

    tools.push_back(
        { "attack",
          "Close to weapon range of a ship or a structure and fire until it is destroyed. "
          "Finishes when it is gone; aborts like any order if the hull gets critical. A "
          "destroyed structure leaves a wreck where it fits. Shooting what a lawful faction "
          "owns, or a player's structure in a system a lawful faction holds, is a crime: "
          "reputation and bounty with that faction, per hit and more for the kill. Your own "
          "structures are dismantled, not attacked.",
          Obj({ { "target_id", Num("ship or structure id from observe") } }, { "target_id" }),
          [](const Rpc::Json& args)
          {
              RequireLive();
              const int  id = (int)NumberOr(args, "target_id", 0);
              const auto it = g_session.Layout().find(id);
              if (it != g_session.Layout().end() &&
                  it->second.kind == Proto::EntityKind::Structure &&
                  it->second.owner == g_session.Account())
                  throw Rpc::Error{ Rpc::INVALID_PARAMS,
                                    it->second.name + " is yours; dismantle it instead" };
              Proto::Command c = OrderCommand(Orders::Kind::Attack);
              c.orderTarget = id;
              return GiveOrder(c, "attack");
          } });

    tools.push_back(
        { "salvage",
          "Search a wreck (a derelict observe marks 'lootable') and take what it pays. A wreck "
          "is searched once, by whoever gets there first, and everyone sees it searched "
          "after. Within its reach this searches it now and says what it paid. Out of reach "
          "it flies there instead (a move_to order, warping when far): wait_for_event until "
          "it arrives, then call salvage again.",
          Obj({ { "derelict_id", Num("derelict id from observe") } }, { "derelict_id" }),
          [](const Rpc::Json& args)
          {
              RequireLive();
              const int  id = (int)NumberOr(args, "derelict_id", 0);
              const auto it = g_session.Layout().find(id);
              // What can be salvaged is a component (#34), as the server decides it; a
              // derelict's own state is whether it has been taken already.
              const Archetype* type =
                  it == g_session.Layout().end() ? nullptr : Archetypes::Find(it->second.archetype);
              if (it == g_session.Layout().end() ||
                  it->second.kind != Proto::EntityKind::Derelict ||
                  (type != nullptr && !type->Has(Component::Salvageable)))
                  throw Rpc::Error{ Rpc::INVALID_PARAMS,
                                    "no wreck #" + std::to_string(id) + " in this system" };
              const Proto::EntityLayout& wreck = it->second;
              if (wreck.looted)
                  throw Rpc::Error{ Rpc::INVALID_PARAMS,
                                    wreck.name + " has been searched already; nothing is left" };
              const Proto::PlayerView& p = g_session.Snapshot().player;
              if (p.docked)
                  throw Rpc::Error{ Rpc::INVALID_PARAMS, "docked; undock first" };

              const float reach = wreck.size + (type != nullptr ? type->salvageRange : 0.0f);
              const float dx = wreck.pos.x - p.pos.x, dy = wreck.pos.y - p.pos.y;
              const float dist = std::sqrt(dx * dx + dy * dy);
              if (dist > reach)
              {
                  // Out of reach: the same thing the client's Investigate does -- go there
                  // first. The stop is well inside the reach, so arriving is enough.
                  Proto::Command c = OrderCommand(Orders::Kind::MoveTo);
                  c.orderTarget = id;
                  c.orderStopDist = wreck.size + (reach - wreck.size) * 0.5f;
                  c.orderWarp = dist > Actions::WARP_MIN;  // as far as the menu offers a warp
                  char head[160];
                  std::snprintf(head, sizeof(head),
                                "salvage: %s is %.0f away and the reach is %.0f; flying there. ",
                                wreck.name.c_str(), dist, reach);
                  return head + GiveOrder(c, "move_to") + " Then call salvage again.";
              }

              const std::string name = wreck.name;  // the layout may change under a pump
              const double      before = p.money;
              Proto::Command    c;
              c.lootId = id;
              std::string why;
              if (!Confirm(c, why))
                  return "salvage: " + why;
              // The answer is the money and the wreck's state, both in the snapshot that
              // acknowledged the command: the server applies it before it builds the next.
              const double paid = g_session.Snapshot().player.money - before;
              const auto   now = g_session.Layout().find(id);
              if (now != g_session.Layout().end() && now->second.looted)
                  return "salvage: searched " + name + ", +" + Money(paid);
              return "salvage: the server did not let you search " + name +
                     " -- someone may have got there first, or the ship drifted out of reach; "
                     "call observe";
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
        {
            // Over MCP an Rpc::Error becomes an error reply; here there is nobody to reply
            // to, and letting it escape ends the process with std::terminate instead of a
            // failed check. A refused login is exactly this case (#106).
            try
            {
                return t.run(args);
            }
            catch (const Rpc::Error& e)
            {
                return "error: " + e.message;
            }
        }
    return "no such tool: " + name;
}

// --- What the server offers, as data ------------------------------------
// The answers to tools/list, resources/list and prompts/list, built in one place. The RPC
// handlers return them, and so does `econagent describe`, which is what the reference in
// docs/agents/ is generated from (#172) -- so a tool that changes changes its documentation
// in the same build, and CI fails if the committed copy says otherwise.

Rpc::Json ToolsList(const std::vector<Tool>& tools)
{
    Rpc::Json list = Rpc::Json::array();
    for (const Tool& t : tools)
        list.push_back(
            { { "name", t.name }, { "description", t.description }, { "inputSchema", t.schema } });
    return list;
}

Rpc::Json ResourcesList()
{
    return Rpc::Json::array(
        { Rpc::Json{ { "uri", "econspace://system" },
                     { "name", "Current system" },
                     { "description", "Everything visible where the ship is, in full" },
                     { "mimeType", "text/plain" } },
          Rpc::Json{ { "uri", "econspace://galaxy" },
                     { "name", "Galaxy" },
                     { "description",
                       "Every system, its security, controller and gate links, plus recent "
                       "galactic news" },
                     { "mimeType", "text/plain" } } });
}

Rpc::Json PromptsList(bool withText)
{
    Rpc::Json list = Rpc::Json::array();
    for (const auto& p : Prompts())
    {
        Rpc::Json e{ { "name", p.name }, { "description", p.description } };
        if (withText)
            e["text"] = p.text;
        list.push_back(e);
    }
    return list;
}

// What the selftest is doing right now, for the watchdog to name if it never finishes.
std::atomic<const char*> g_phase{ "starting" };

// A self-test that can hang is a broken self-test: in CI it holds the job until somebody
// cancels it, and the log it leaves says nothing about where it stopped. Every wait in the
// script has its own timeout, so this should never fire -- it is there for what those
// cannot cover (a call that blocks inside the OS, a process that will not exit), and when
// it does fire it says what it was waiting for and exits non-zero.
//
// The budget is the sum of the script's own waits with room to spare: connect 10 s, login
// 10 s, first snapshot 5 s, two order acknowledgements of 2 s, the flight of 120 s, then
// docking (60 s) and five station calls of about 2 s each.
constexpr double SELFTEST_BUDGET_SECONDS = 270.0;  // under CI's own 300 s bound

void StartSelftestWatchdog()
{
    std::thread(
        []
        {
            std::this_thread::sleep_for(std::chrono::duration<double>(SELFTEST_BUDGET_SECONDS));
            // Read without a lock on purpose: this is a last word before exiting, and a
            // torn read of a status string is better than a watchdog that can deadlock.
            std::fprintf(stderr, "Agent selftest: FAIL -- still %s after %.0f s, giving up\n",
                         g_phase.load(), SELFTEST_BUDGET_SECONDS);
            std::fflush(stderr);
            std::_Exit(3);
        })
        .detach();
}

// Scripted agent: the same tools an LLM calls, driven by a fixed sequence.
//
// This is what keeps the agent seam honest in CI. Testing it with a real model would cost
// money, need a key and give a different answer every run; testing it with the tools
// called directly proves the part that can actually break -- the bridge, the orders, the
// journal and the round trip through the server.
// Run: econagent selftest <host> <port> <name> <secret>   (a server must already be listening)
int Selftest(const std::vector<Tool>& tools)
{
    auto note = [](const char* what, bool ok)
    { std::fprintf(stderr, "  %-22s %s\n", what, ok ? "OK" : "FAIL"); };

    // 1) The world is visible at all.
    g_phase = "observing";
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
        g_phase = "giving a move_to order";
        std::string reply = RunTool(
            tools, "move_to",
            Rpc::Json{ { "target_id", stationId }, { "warp", true }, { "stop_distance", 400 } });
        ordered = reply.find("accepted") != std::string::npos;
        note("move_to accepted", ordered);

        if (ordered)
        {
            g_phase = "waiting for the order to complete";
            std::string ev =
                RunTool(tools, "wait_for_event", Rpc::Json{ { "timeout_seconds", 120 } });
            arrived = ev.find("order_done") != std::string::npos;
            note("order completed", arrived);
        }
    }

    // 4) An order naming something absent must be refused, not silently swallowed.
    g_phase = "waiting for a bad order to be refused";
    const int   orderBefore = g_session.Snapshot().player.orderId;
    std::string bogus = RunTool(tools, "dock", Rpc::Json{ { "station_id", 999999 } });
    bool        refused = bogus.find("refused") != std::string::npos ||
                          bogus.find("not in this system") != std::string::npos;
    // The tool waits two seconds for the server, on purpose; a loaded CI runner with two
    // agents and a server on it can take longer (seen on Windows). What is checked here is
    // the server's answer, not how fast it came, so wait for the answer itself.
    if (!refused)
    {
        g_session.WaitUntil([&] { return g_session.Snapshot().player.orderId != orderBefore; },
                            20.0);
        const Proto::PlayerView& p = g_session.Snapshot().player;
        refused = p.orderId != orderBefore && p.orderStatus == (int)Orders::Status::Failed;
    }
    note("bad target refused", refused);

    // 5) Station business (#109): dock, take a job, and be told plainly why a purchase and a
    // payment cannot happen. A new account has 500 cr and no bounty, so both refusals are
    // certain -- and a refusal that names its reason is the behaviour being tested.
    bool docked = false, listed = false, accepted = false, unaffordable = false, noBounty = false;
    if (arrived)
    {
        g_phase = "docking";
        std::string reply = RunTool(tools, "dock", Rpc::Json{ { "station_id", stationId } });
        if (reply.find("accepted") != std::string::npos)
            g_session.WaitUntil([] { return g_session.Snapshot().player.docked; }, 60.0);
        docked = g_session.Snapshot().player.docked;
        note("docked", docked);
    }
    if (docked)
    {
        g_phase = "reading the job board";
        g_session.WaitUntil([] { return !g_session.Snapshot().missionOffers.empty(); }, 2.0);
        std::string board = RunTool(tools, "missions", Rpc::Json::object());
        listed = board.find("OFFERS at") != std::string::npos;
        note("missions lists a board", listed);

        g_phase = "accepting a mission";
        std::string take = RunTool(tools, "accept_mission", Rpc::Json{ { "offer", 0 } });
        accepted = take.find("taken") != std::string::npos;
        note("accept_mission", accepted);

        g_phase = "asking the hangar";
        std::string hangar = RunTool(tools, "hangar", Rpc::Json::object());
        std::string buy = RunTool(tools, "buy_ship", Rpc::Json{ { "ship", "Miner" } });
        unaffordable =
            hangar.find("FLYING") != std::string::npos &&
            (buy.find("costs") != std::string::npos || buy.find("bought") != std::string::npos);
        note("hangar and buy_ship", unaffordable);

        g_phase = "paying a bounty nobody has set";
        std::string pay = RunTool(tools, "pay_bounty", Rpc::Json::object());
        noBounty =
            pay.find("no bounty") != std::string::npos || pay.find("paid") != std::string::npos;
        note("pay_bounty", noBounty);
    }

    // 6) Construction (#39): what can be built is listed, and a site is never laid down from
    // inside a station -- the tool says so before the server has to.
    g_phase = "reading the blueprints";
    const std::string bps = RunTool(tools, "blueprints", Rpc::Json::object());
    const std::string fromDock = RunTool(tools, "deploy", Rpc::Json{ { "blueprint", "beacon" } });
    // ...and nothing is taken apart that is not there to take (#39).
    const std::string nothing =
        RunTool(tools, "dismantle", Rpc::Json{ { "structure_id", 999999 } });
    const bool building = bps.find("beacon") != std::string::npos &&
                          (!docked || fromDock.find("undock first") != std::string::npos) &&
                          nothing.find("no structure") != std::string::npos;
    note("blueprints, deploy and dismantle", building);

    // 7) Salvage (#297): a wreck that is not there is named as such, and one that is gets
    // searched or approached -- whichever its distance calls for. Which wrecks a system has
    // depends on the region, so the second half runs only where there is one.
    g_phase = "salvaging";
    bool salvage =
        RunTool(tools, "salvage", Rpc::Json{ { "derelict_id", 999999 } }).find("no wreck") !=
        std::string::npos;
    for (const auto& kv : g_session.Layout())
        if (kv.second.kind == Proto::EntityKind::Derelict && !kv.second.looted)
        {
            const std::string r =
                RunTool(tools, "salvage", Rpc::Json{ { "derelict_id", kv.first } });
            salvage =
                salvage && (r.find(docked ? "undock first" : "salvage:") != std::string::npos);
            break;
        }
    note("salvage", salvage);

    const bool ok = observed && stationId != 0 && ordered && arrived && refused && docked &&
                    listed && accepted && unaffordable && noBounty && building && salvage;
    // A failure that is really a lost connection should say so, rather than leave a list of
    // FAILs to be read as five separate bugs.
    if (!ok && !g_session.ByeReason().empty())
        std::fprintf(stderr, "  the server ended the session: %s\n", g_session.ByeReason().c_str());
    else if (!ok && !g_session.Alive())
        std::fprintf(stderr, "  the connection closed: %s\n", g_session.CloseReason().c_str());
    std::fprintf(stderr, "Agent selftest: %s\n", ok ? "PASS" : "FAIL");
    g_phase = "exiting";
    return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv)
{
    // stdout is the JSON-RPC channel and nothing else may touch it; diagnostics go to
    // stderr. Unbuffered because a client is waiting on each line.
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetTraceLogCallback(TraceToStderr);

    // What this server offers, as JSON, without connecting to anything (#172). It is the
    // source the reference in docs/agents/ is generated from.
    if (argc >= 2 && std::strcmp(argv[1], "describe") == 0)
    {
        const std::vector<Tool> tools = BuildTools();
        const Rpc::Json         all{ { "protocolVersion", "2024-11-05" },
                                     { "tools", ToolsList(tools) },
                                     { "resources", ResourcesList() },
                                     { "prompts", PromptsList(true) } };
        std::fputs(all.dump(2).c_str(), stdout);
        std::fputs("\n", stdout);
        return 0;
    }

    const bool selftest = argc >= 3 && std::strcmp(argv[1], "selftest") == 0;
    if (!selftest && (argc < 3 || std::strcmp(argv[1], "connect") != 0))
    {
        std::fprintf(stderr,
                     "econagent — EconSpace as an MCP server.\n"
                     "  %s connect <host> <port> <name> <secret>   serve MCP on stdio\n"
                     "  %s selftest <host> <port> <name> <secret>  scripted run\n"
                     "  %s describe                                 the tools, resources and "
                     "prompts, as JSON\n"
                     "\n"
                     "Start a server first:  econserver host 50800\n",
                     argv[0], argv[0], argv[0]);
        return 2;
    }

    const std::string    host = argv[2];
    const unsigned short port = (argc >= 4) ? (unsigned short)std::atoi(argv[3]) : 50800;
    // The account this agent flies under. Its own by default: an agent and the human who
    // started it are two players, and sharing one account would have them share a ship.
    // Who this agent plays as, and what proves it (#3, #106). Both or neither: naming an
    // account without its secret is a login nobody can check, and quietly falling back to
    // a well-known one would hand that account to anyone who read this file.
    if (argc == 5)
    {
        std::fprintf(stderr, "econagent: an account name needs its secret too.\n");
        return 2;
    }
    const std::string account = (argc >= 5) ? argv[4] : "agent";
    const std::string secret = (argc >= 6) ? argv[5] : "agent-local";

    std::string dataDir = AGENT_DATA_DIR;
    Factions::Load(dataDir + "factions.json");
    // Read locally too: describing an object to a model means saying what it can do, and
    // that lives in the archetype rather than in the snapshot.
    if (!Archetypes::Load(dataDir + "archetypes.json"))
        Rpc::Log("econagent: " + Archetypes::Error());
    if (!Blueprints::Load(dataDir + "blueprints.json"))  // what deploy can lay down (#39)
        Rpc::Log("econagent: " + Blueprints::Error());
    // The galaxy index is NOT read here: the server sends it at login, exactly as it does
    // to the game client (#206), and the session keeps it.

    if (selftest)
    {
        StartSelftestWatchdog();
        g_phase = "connecting and logging in";
    }
    if (!g_session.Connect(host, port, account, secret))
    {
        std::fprintf(stderr, "econagent: could not connect to %s:%u\n", host.c_str(), port);
        return 1;
    }
    Rpc::Log("econagent: connected to " + host + ":" + std::to_string(port));

    // Give the server a moment to send the opening layout and snapshot, so the first
    // observe has a world in it rather than "no world state yet".
    g_phase = "waiting for the first snapshot";
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
    rpc.On("resources/list",
           [](const Rpc::Json&) { return Rpc::Json{ { "resources", ResourcesList() } }; });

    rpc.On("resources/read",
           [](const Rpc::Json& params)
           {
               const std::string uri = params.value("uri", std::string());
               std::string       text;
               if (uri == "econspace://system")
               {
                   RequireLive();
                   text = g_session.Describe(Obs::Detail::Full);
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
           [](const Rpc::Json&) { return Rpc::Json{ { "prompts", PromptsList(false) } }; });

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
           [&tools](const Rpc::Json&) { return Rpc::Json{ { "tools", ToolsList(tools) } }; });

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
