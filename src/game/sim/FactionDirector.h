#pragma once

#include "core/Faction.h"
#include "sim/FactionMind.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

// What the factions' macro layer owns (#353): each faction's mind, the plans it has set in
// motion with the queue of when they come due, and the chronicle of what happened. Everything
// here is data and its invariants; none of it reads the galaxy, so it needs no context --
// `Simulation` decides, and tells the director what was decided.

// The plans under way, by id (#295), and when each is due. A plan is always put in the book
// and, if it ends at a time rather than being stepped (a convoy is), scheduled; whatever moves
// its time schedules it again. Only the due queue is derived: it is not saved and is remade
// from the plans a save holds.
class PlanBook
{
public:
    using Map = std::map<int, Plan>;

    // The next unused id.
    int NewId() { return nextId_++; }
    // After a load: ids below this are taken. Never lowers it.
    void ReserveId(int id) { nextId_ = std::max(nextId_, id); }
    void SetNextId(int id) { nextId_ = id; }
    int  NextId() const { return nextId_; }

    // Keeps `p` under its id, replacing what was there, and returns the kept copy.
    Plan& Put(const Plan& p) { return plans_[p.id] = p; }
    // Puts `p` on the due queue at its `dueAt`.
    void Schedule(const Plan& p) { due_.insert({ p.dueAt, p.id }); }
    // Takes `p` off the queue as it was scheduled; true if it was there.
    bool Unschedule(const Plan& p) { return due_.erase({ p.dueAt, p.id }) > 0; }

    // The id of the plan due soonest, and off the queue, if it is due by `now`. Its entry may
    // name a plan that has gone since: look it up.
    std::optional<int> PopDue(double now);
    bool               NothingDue() const { return due_.empty(); }

    // The plan by id, or null.
    Plan*       Find(int id);
    const Plan* Find(int id) const;
    bool        Has(int id) const { return plans_.count(id) != 0; }
    // Forgets a plan. Its due entry, if any, finds nothing when it comes up.
    void          Erase(int id) { plans_.erase(id); }
    Map::iterator Erase(Map::iterator it) { return plans_.erase(it); }
    // A plan by id, made empty if there is none (a job's plan that was never kept).
    Plan& operator[](int id) { return plans_[id]; }

    // Every plan, in id order.
    Map&       All() { return plans_; }
    const Map& All() const { return plans_; }

    void Clear();

private:
    Map                              plans_;
    std::set<std::pair<double, int>> due_;
    int                              nextId_ = 1;
};

// The world's history as it happens (#295): the newest entries, numbered. Saved.
class Chronicle
{
public:
    static constexpr size_t KEEP = 200;

    // Writes an entry at `time`, numbering it, and drops the oldest past KEEP.
    void Record(double time, const std::string& kind, std::optional<FactionId> faction,
                const std::string& system, const std::string& text);
    const std::vector<ChronicleEntry>& Entries() const { return entries_; }
    long long                          Seq() const { return seq_; }
    // For a load: entries as they were saved, and the sequence they had reached.
    void Restore(std::vector<ChronicleEntry> entries, long long seq);
    void Clear();

private:
    std::vector<ChronicleEntry> entries_;
    long long                   seq_ = 0;
};

class FactionDirector
{
public:
    // What a faction knows and holds (#295). Created on first use, as a PerFaction is.
    FactionMind&       Mind(FactionId f) { return minds_[f]; }
    const FactionMind& Mind(FactionId f) const { return minds_[f]; }
    // Whether the minds have been seeded with their holdings; an older save seeds on first step.
    bool Seeded() const { return seeded_; }
    void SetSeeded(bool v) { seeded_ = v; }

    PlanBook&        Plans() { return plans_; }
    const PlanBook&  Plans() const { return plans_; }
    Chronicle&       History() { return chronicle_; }
    const Chronicle& History() const { return chronicle_; }

    // A new world: nothing known, nothing planned, nothing remembered.
    void Clear();

private:
    PerFaction<FactionMind> minds_;
    bool                    seeded_ = false;
    PlanBook                plans_;
    Chronicle               chronicle_;
};
