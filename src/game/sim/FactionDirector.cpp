#include "sim/FactionDirector.h"

#include <algorithm>

std::optional<int> PlanBook::PopDue(double now)
{
    if (due_.empty() || due_.begin()->first > now)
        return std::nullopt;
    const int id = due_.begin()->second;
    due_.erase(due_.begin());
    return id;
}

Plan* PlanBook::Find(int id)
{
    const auto it = plans_.find(id);
    return it == plans_.end() ? nullptr : &it->second;
}

const Plan* PlanBook::Find(int id) const
{
    const auto it = plans_.find(id);
    return it == plans_.end() ? nullptr : &it->second;
}

void PlanBook::Clear()
{
    plans_.clear();
    due_.clear();
    nextId_ = 1;
}

void Chronicle::Record(double time, const std::string& kind, std::optional<FactionId> faction,
                       const std::string& system, const std::string& text)
{
    ChronicleEntry e;
    e.seq = ++seq_;
    e.time = time;
    e.kind = kind;
    e.faction = faction;
    e.system = system;
    e.text = text;
    entries_.push_back(std::move(e));
    if (entries_.size() > KEEP)
        entries_.erase(entries_.begin(), entries_.begin() + (long)(entries_.size() - KEEP));
}

void Chronicle::Restore(std::vector<ChronicleEntry> entries, long long seq)
{
    entries_ = std::move(entries);
    seq_ = seq;
    for (const ChronicleEntry& e : entries_)
        seq_ = std::max(seq_, e.seq);
}

void Chronicle::Clear()
{
    entries_.clear();
    seq_ = 0;
}

void FactionDirector::Clear()
{
    minds_.Clear();
    seeded_ = false;
    plans_.Clear();
    chronicle_.Clear();
}
