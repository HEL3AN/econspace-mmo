#include "core/Goal.h"

namespace Goal
{

namespace
{
constexpr const char* DOCK_AT = "dock at ";

// "dock at Vela Hub (#12)" is what a mission still needs once its work is done, said for an
// agent; a player is told the station's name, and the pointer shows where it is.
bool NeedsDocking(const Mission& m)
{
    return m.needs.rfind(DOCK_AT, 0) == 0;
}

// Where a mission goes back to, by name when it is in this system.
std::string Destination(const Mission& m)
{
    return m.handInName.empty() ? std::string("its station, in another system (map, G)")
                                : m.handInName;
}

std::string Quoted(const std::string& title)
{
    return "\"" + title + "\"";
}

Prompt Make(Step step, std::string text, int pointAt = 0, bool missionsWindow = false)
{
    Prompt p;
    p.step = step;
    p.text = std::move(text);
    p.pointAt = pointAt;
    p.missionsWindow = missionsWindow;
    return p;
}
}  // namespace

Prompt Next(const State& s)
{
    // A finished mission comes first wherever the player is: it is money waiting.
    const Mission* ready = nullptr;
    for (const Mission& m : s.active)
        if (m.ready && ready == nullptr)
            ready = &m;
    // ...and a plan's job is paid by wire (#345): handed in from wherever the ship is.
    for (const Mission& m : s.active)
        if (m.ready && m.anywhere)
            return Make(Step::HandIn, "Hand in " + Quoted(m.title) + " in the Missions window (J)",
                        0, true);

    if (s.docked)
    {
        for (const Mission& m : s.active)
            if (m.ready && m.handInId == s.dockedStationId)
                return Make(Step::HandIn,
                            "Hand in " + Quoted(m.title) + " in the Missions window (J)", 0, true);
        if (s.active.empty() && s.offers > 0 && s.roomForMore)
            return Make(Step::TakeMission, "Take a mission from the board (Missions, J)", 0, true);
        if (ready != nullptr)
            return Make(Step::Undock,
                        "Undock and take " + Quoted(ready->title) + " to " + Destination(*ready));
        if (!s.active.empty())
        {
            const Mission& m = s.active.front();
            if (NeedsDocking(m))
                return Make(Step::Undock, "Undock and fly to " + Destination(m));
            return Make(Step::Undock, "Undock and " + m.needs);
        }
        return Make(Step::Undock, "No work on this board: undock and try another station");
    }

    if (ready != nullptr)
    {
        if (!ready->handInName.empty())
            return Make(Step::FlyTo,
                        "Fly to " + ready->handInName + " and dock to hand in " +
                            Quoted(ready->title),
                        ready->handInId);
        return Make(Step::FarHandIn, "Take " + Quoted(ready->title) + " to " + Destination(*ready));
    }
    if (!s.active.empty())
    {
        const Mission& m = s.active.front();
        if (NeedsDocking(m))
        {
            if (!m.handInName.empty())
                return Make(Step::FlyTo, "Fly to " + m.handInName + " and dock", m.handInId);
            return Make(Step::Work, Quoted(m.title) + ": fly to " + Destination(m));
        }
        // A plan's job points at what it is about when that is here: a supply job's site, an
        // escort's hauler (#430).
        return Make(Step::Work, Quoted(m.title) + ": " + m.needs,
                    m.anywhere && !m.handInName.empty() ? m.handInId : 0);
    }
    if (s.nearestStationId != 0)
        return Make(Step::DockForWork, "Dock at " + s.nearestStationName + " to pick up work",
                    s.nearestStationId);
    return Make(Step::FindStation, "No station here: open the map (G) and jump toward one");
}

bool Shown(bool setting, int handedIn)
{
    return setting && handedIn < LEARNED_AFTER;
}

}  // namespace Goal
