#pragma once

#include "sim/PlayerStep.h"
#include "sim/Protocol.h"

#include <cstddef>
#include <functional>
#include <vector>

class Ship;

// The client's prediction of its own ship (M4e, per Gambetta), with no window in it (#358).
//
// The authoritative ship lives on the server. The client steps a copy of it with the same
// Sim::StepPlayerShip the server uses, one command per fixed tick, and keeps every command
// until a snapshot acknowledges it. Each snapshot then resets the copy to the server's
// state and replays the commands the server has not seen yet on top of it -- so the ship
// is where the server will put it, without snapping back to where it was.
//
// Its inputs are the acknowledged server state and the pending commands; its output is the
// ship it is given. That is the half of the netcode a test can drive against a Simulation
// with the same command stream, which is the invariant CLAUDE.md says breaks silently.
class Prediction
{
public:
    // Where the object the ship is holding station on is, as this side sees it, or null.
    // Asked once per step, because a replayed command can start or release a hold.
    using HoldFn = std::function<const Sim::HoldTarget*()>;

    // More than this and the server has stalled; the oldest command is forgotten.
    static constexpr size_t MAX_PENDING = 256;

    // Numbers the command, keeps it until it is acknowledged and steps the ship by it.
    // Returns the numbered command: that is the one to send.
    const Proto::Command& Predict(Ship& ship, const Proto::Command& cmd, const HoldFn& hold);

    // A snapshot arrived: forget what it acknowledges, put the ship where the server says
    // it is -- flying the hull the server says it flies -- and replay the rest on top.
    void Reconcile(Ship& ship, const Proto::PlayerView& view, const HoldFn& hold);

    size_t       Pending() const { return pending_.size(); }
    unsigned int LastSeq() const { return seq_; }

private:
    unsigned int                seq_ = 0;
    std::vector<Proto::Command> pending_;
};
