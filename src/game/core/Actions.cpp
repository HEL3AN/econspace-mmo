#include "core/Actions.h"

#include "economy/Resource.h"

#include <cmath>
#include <cstdio>

namespace Actions
{

namespace
{
std::string Number(const char* format, float v)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), format, v);
    return buf;
}

float Distance(Vector2 a, Vector2 b)
{
    const float dx = a.x - b.x, dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

Action Make(Verb verb, std::string label, float distance = 0.0f)
{
    Action a;
    a.verb = verb;
    a.label = std::move(label);
    a.distance = distance;
    a.tool = ToolFor(verb);
    return a;
}
}  // namespace

const char* ToolFor(Verb verb)
{
    switch (verb)
    {
        case Verb::Approach:
        case Verb::Warp:
        case Verb::FlyHere:
        case Verb::WarpHere: return "move_to";
        case Verb::Orbit:
        case Verb::Keep:
        case Verb::Follow: return "hold_station";
        case Verb::Dock: return "dock";
        case Verb::Mine: return "mine";
        case Verb::Jump: return "travel_to_system";
        case Verb::Build: return "deploy";
        // The interface's own: they change what a window shows, not the world. An agent
        // names a thing by its id and a range by a number, and needs neither.
        case Verb::Select:
        case Verb::SetRange: return "";
        // Not yet a tool: an agent cannot fight or salvage. Listed in the test, so adding
        // the tool is the only way off the list.
        case Verb::Attack:
        case Verb::Investigate: return "";
    }
    return "";
}

int HoldMode(Verb verb)
{
    switch (verb)
    {
        case Verb::Orbit: return 3;
        case Verb::Keep: return 4;
        case Verb::Follow: return 5;
        default: return 0;
    }
}

float DefaultRange(float size)
{
    return size * HOLD_RANGES[1];
}

std::vector<Action> For(const Target& t, Vector2 from)
{
    std::vector<Action> out;
    out.push_back(Make(Verb::Select, "Target"));
    out.push_back(Make(Verb::Approach, "Approach", t.size + 70.0f));

    // The two standing behaviours, and the one that matches velocity. Every fight is flown
    // as one of these. Only for a thing with an id: a hold follows its target by id.
    const float base = t.size;
    if (t.id != 0)
    {
        for (float mult : HOLD_RANGES)
            out.push_back(Make(Verb::Orbit, Number("Orbit at %.0f", base * mult), base * mult));
        // Named for what it does (#309): hold this distance from it, wherever it goes.
        out.push_back(
            Make(Verb::Keep, Number("Hold %.0f from it", DefaultRange(base)), DefaultRange(base)));
        // Matching its velocity as well as its distance: the one that stays with a station
        // going round a planet rather than arriving behind it (#298).
        out.push_back(
            Make(Verb::Follow, Number("Follow at %.0f", DefaultRange(base)), DefaultRange(base)));
        // Any distance, not just the presets: the selected-item window's range control.
        out.push_back(Make(Verb::SetRange, "Hold at a range...", DefaultRange(base)));
    }

    // A warp only when it is far enough to be worth one. The drop distance is offered rather
    // than assumed: arriving on top of a station and arriving far enough out to look at it
    // first are different intentions.
    if (Distance(t.pos, from) > WARP_MIN)
    {
        out.push_back(Make(Verb::Warp, "Warp to", t.size + 70.0f));
        out.push_back(Make(Verb::Warp, Number("Warp to, %.0f out", base * HOLD_RANGES[2]),
                           base * HOLD_RANGES[2]));
    }

    // Listed exhaustively rather than with a `default:` so that a new kind of object -- the
    // point of #44 -- cannot quietly ship with an empty menu.
    switch (t.kind)
    {
        case EntityKind::Station: out.push_back(Make(Verb::Dock, "Dock")); break;
        case EntityKind::Field: out.push_back(Make(Verb::Mine, "Mine here")); break;
        case EntityKind::Npc: out.push_back(Make(Verb::Attack, "Attack")); break;
        case EntityKind::Derelict:
            if (!t.looted)
                out.push_back(Make(Verb::Investigate, "Investigate"));
            break;
        case EntityKind::Gate:
            out.push_back(Make(Verb::Jump, t.destinationName.empty()
                                               ? std::string("Jump")
                                               : "Jump to " + t.destinationName));
            break;
        // Nothing to do with one yet but go there; taking one down is the next slice (#39).
        case EntityKind::Structure: break;
        // Scenery and the player's own ship: going there is all there is to do with them.
        case EntityKind::Star:
        case EntityKind::Planet:
        case EntityKind::Nebula:
        case EntityKind::PlayerShip:
        case EntityKind::Unknown: break;
    }
    return out;
}

std::vector<Action> ForPoint(Vector2 point, Vector2 from, const std::vector<Blueprint>& blueprints)
{
    std::vector<Action> out;
    out.push_back(Make(Verb::FlyHere, "Fly here", 18.0f));
    const float d = Distance(point, from);
    if (d > WARP_MIN)
        out.push_back(Make(Verb::WarpHere, "Warp here", 60.0f));

    // Every blueprint whose reach the point is within, with what it costs. Only the reach is
    // checked here, so the menu does not offer what could never work; the rest -- the hold,
    // the room, the caps -- the server decides and says in the journal.
    for (const Blueprint& bp : blueprints)
    {
        if (d > bp.reach)
            continue;
        std::string cost;
        for (const auto& c : bp.cost)
            cost +=
                (cost.empty() ? "" : ", ") + std::to_string(c.second) + " " + ResourceName(c.first);
        Action a = Make(Verb::Build, "Build " + bp.name + " (" + cost + ")");
        a.blueprint = bp.id;
        out.push_back(a);
    }
    return out;
}

}  // namespace Actions
