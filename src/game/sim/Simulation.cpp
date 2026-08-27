// Simulation's spine: lifetime, the world RNG and the event journal.
//
// The rules themselves live in the sibling translation units of the same class (#17):
// Simulation_World, Simulation_Agents, Simulation_Player, Simulation_Orders and
// Simulation_Snapshot. Splitting the file did not split the class -- Simulation.h is
// still the one place its surface is declared.

#include "sim/Simulation.h"

#include "core/World.h"
#include "entities/Ship.h"

// Constructor/destructor — out of line: the unique_ptr<Ship> member needs Ship's full
// type at the point where the destructor is generated (here Ship.h is already included).
Simulation::Simulation() = default;
Simulation::~Simulation() = default;

void Simulation::LoadUniverse(const std::string& path)
{
    universe_ = WorldLoader::LoadUniverse(path);
}

int Simulation::RandRange(int lo, int hi)
{
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    if (hi <= lo)
        return lo;
    return lo + (int)(rng_ % (unsigned)(hi - lo + 1));
}

float Simulation::Rand01()
{
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return (rng_ & 0xFFFFFFu) / (float)0x1000000u;
}

void Simulation::PushEvent(const std::string& msg)
{
    events_.push_back(msg);
    if (events_.size() > 8)
        events_.erase(events_.begin());  // keep the last 8
}

// --- Event journal (#29) -----------------------------------------------------

void Simulation::RecordEvent(Ev::Kind kind, const std::string& text)
{
    // Keep the last stretch of history only. An agent that has fallen further behind
    // than this has lost its place regardless and needs a fresh observation, so holding
    // older entries would cost memory to serve nobody.
    constexpr size_t JOURNAL_CAP = 256;

    Ev::Event e;
    e.seq = ++nextEventSeq_;
    e.kind = kind;
    e.text = text;
    journal_.push_back(std::move(e));
    if (journal_.size() > JOURNAL_CAP)
        journal_.erase(journal_.begin(), journal_.begin() + (journal_.size() - JOURNAL_CAP));
}

std::vector<Ev::Event> Simulation::EventsSince(int seq) const
{
    std::vector<Ev::Event> out;
    for (const Ev::Event& e : journal_)
        if (e.seq > seq)
            out.push_back(e);
    return out;
}
