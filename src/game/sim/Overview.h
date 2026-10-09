#pragma once

#include "sim/Protocol.h"
#include <functional>
#include <string>
#include <vector>

// The overview as a list a player flies by (#157).
//
// In EVE the overview is the instrument: a player picks a thing from a list and chooses what
// to do about it, rather than hunting for it on screen. That matters more as a system grows
// forty times larger (#159) -- at a million units almost nothing you need is on screen. This
// is the half of it with no drawing in it: which rows, in what order. The client draws what
// it returns; a test can hold everything it decides.
namespace Overview
{

// The tabs. Grouped by what a player is choosing between, not by class: a gate and a station
// are both somewhere to go, a star and a belt are both features of the place.
enum class Filter
{
    All,
    Destinations,  // stations and gates -- somewhere to dock or to leave by
    Ships,         // NPCs and other players
    Features,      // stars, planets, belts, nebulae, wrecks
    Hostile        // anything that would shoot at you
};

enum class Sort
{
    Distance,
    Name,
    Kind
};

const char*                Label(Filter f);
const std::vector<Filter>& AllFilters();

struct Row
{
    const Proto::EntitySnapshot* entity = nullptr;
    float                        distance = 0.0f;
    bool                         hostile = false;
    std::string                  kind;  // what the row says the thing is
    std::string                  name;
};

// Whether a thing would shoot at the viewer. Passed in rather than computed here, because the
// answer depends on the viewer's standing and the client and the agent already have one.
using IsHostile = std::function<bool(const Proto::EntitySnapshot&)>;

// The rows for one tab, in order. Hostile rows come first under any sort, because they are
// the reason to change plan -- the same rule the agent's text projection follows.
std::vector<Row> Build(const std::vector<Proto::EntitySnapshot>& entities, Vector2 from,
                       Filter filter, Sort sort, const IsHostile& hostile);

// The word a row uses for a kind of thing. Shared with nothing on purpose: this is what a
// player reads in a column, and it may say "pilot" where the agent's projection says it too.
const char* KindWord(const Proto::EntitySnapshot& e);

}  // namespace Overview
