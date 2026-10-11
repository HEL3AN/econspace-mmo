#pragma once

#include "core/Faction.h"

#include <nlohmann/json_fwd.hpp>
#include <string>
#include <vector>

// A player's armed structure's rules of engagement (#340): whom its owner lets it fire at,
// like a structure's settings in EVE. A faction's own outpost has none -- it keeps its
// faction's doctrine and fires at the faction's enemies.
//
// The rules are the owner's word, so whatever they make the battery do is the owner's act:
// firing first at someone innocent in a system a lawful faction holds is the owner's crime,
// by the same table as a shot from the owner's own guns (#41). Only a hot system steps its
// batteries at all (#295), so none of this matters where nobody is.
enum class Engage
{
    Hold,       // never fires, even back
    Return,     // fires back at whoever shot it lately (the default, as before #340)
    Enemies,    // ...and first at factions the owner's standing is below the threshold with,
                // and at players wanted by a lawful faction
    Strangers,  // ...and first at anyone who is not the owner and not on the allow-list
};

struct Defence
{
    // An allow-list is a list of names: it is cut to this many, each at most NAME_MAX long.
    static constexpr int   LIST_MAX = 32;
    static constexpr int   NAME_MAX = 24;
    static constexpr float THRESHOLD_MIN = -100.0f;
    static constexpr float THRESHOLD_MAX = 100.0f;

    Engage engage = Engage::Return;
    // Enemies: a faction whose standing with the owner is below this is fired on. The
    // default is where the hostile tier begins in the data as shipped.
    float threshold = -10.0f;
    // Strangers: who it holds fire for -- account names, and faction ids.
    std::vector<std::string> players;
    std::vector<std::string> factions;

    bool IsDefault() const;
    bool operator==(const Defence& o) const;
    bool operator!=(const Defence& o) const { return !(*this == o); }

    bool Allows(const std::string& player) const;  // on the allow-list as a player
    bool Allows(FactionId faction) const;          // ...as a faction
};

const char* EngageName(Engage e);   // "hold", "return", "enemies", "strangers"
const char* EngageLabel(Engage e);  // what a player reads: "Hold fire", ...
bool        EngageFrom(const std::string& name, Engage& out);

// Why these rules cannot be taken as they are -- a name too long, a list too long, a faction
// that does not exist, a threshold out of range -- or empty when they can.
std::string DefenceProblem(const Defence& d);

// The rules as JSON, the same on the wire and in a save: {"engage", "threshold", "players",
// "factions"}, each left out at its default. Reading is strict: a field of the wrong type, an
// unknown mode or an unknown field is false (#191), and `out` is then left as it was.
nlohmann::json DefenceJson(const Defence& d);
bool           DefenceFrom(const nlohmann::json& j, Defence& out);

// What attacking something of a lawful faction's costs the attacker with that faction (#41):
// per hit, and more for the kill. One table for a ship's guns, a structure's and the hits
// on a structure, so the rules-of-engagement window can say what a mode will cost.
namespace Crime
{
inline constexpr float  HIT_STANDING = 0.4f;
inline constexpr double HIT_BOUNTY = 5.0;
inline constexpr float  KILL_STANDING = 3.0f;
inline constexpr double KILL_BOUNTY = 50.0;
}  // namespace Crime

// One line on what `engage` will cost the owner in a system whose law is `law` (`lawful`
// false: nobody's law holds there): what firing first at an innocent costs, or that it
// costs nothing. What a player reads beside the mode, and what set_defence answers.
std::string DefenceCost(Engage engage, bool lawful, FactionId law);

// The allow-list as one line a player types: names separated by commas. A name that is a
// faction's id or its name (in any case) is that faction; anything else is a player.
std::string AllowListText(const Defence& d);
void        ReadAllowList(const std::string& text, Defence& d);
