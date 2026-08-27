#include "sim/ClientSession.h"

#include "entities/Ship.h"

// Out of line for the same reason Simulation's destructor is: the unique_ptr<Ship>
// member needs Ship's full type where the destructor is generated.
ClientSession::~ClientSession() = default;
ClientSession::ClientSession() = default;
ClientSession::ClientSession(ClientSession&&) noexcept = default;
ClientSession& ClientSession::operator=(ClientSession&&) noexcept = default;

void ClientSession::RecordEvent(Ev::Kind kind, const std::string& text)
{
    // Keep the last stretch of history only. An agent that has fallen further behind
    // than this has lost its place regardless and needs a fresh observation, so holding
    // older entries would cost memory to serve nobody.
    constexpr size_t JOURNAL_CAP = 256;

    Ev::Event e;
    e.seq = ++nextEventSeq;
    e.kind = kind;
    e.text = text;
    journal.push_back(std::move(e));
    if (journal.size() > JOURNAL_CAP)
        journal.erase(journal.begin(), journal.begin() + (journal.size() - JOURNAL_CAP));
}

std::vector<Ev::Event> ClientSession::EventsSince(int seq) const
{
    std::vector<Ev::Event> out;
    for (const Ev::Event& e : journal)
        if (e.seq > seq)
            out.push_back(e);
    return out;
}
