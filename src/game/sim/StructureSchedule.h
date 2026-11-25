#pragma once

#include <optional>
#include <set>
#include <string>
#include <tuple>

class Entity;

// When each structure next has something happen to it (#295, #353): it is finished, or its
// time is up, a wreck's end (#41), and the end of a field with a life (#617). Entries are (instant,
// entity id, system). The step pops what is due and touches nothing else. Not saved: remade when
// the static layers are built, from the time lines the structures carry. An entry whose entity has
// gone is skipped by whoever pops it.
class StructureSchedule
{
public:
    struct Due
    {
        int         id = 0;
        std::string systemId;
    };

    // Puts an entity's coming instants on the queue; anything but a structure or a wreck is
    // ignored. Whatever changes a time line after it is added schedules it again.
    void Add(const std::string& systemId, const Entity& e);
    // The soonest entry due by `now`, taken off the queue.
    std::optional<Due> PopDue(double now);
    void               Clear() { due_.clear(); }

private:
    std::set<std::tuple<double, int, std::string>> due_;
};
