#pragma once

#include "sim/Protocol.h"

#include <cstddef>
#include <deque>
#include <vector>

// Where the client draws everything but its own ship (M4e-2, entity interpolation per
// Gambetta), with no window in it (#358).
//
// The server describes the world about twenty times a second, and the snapshots arrive
// bunched and stretched by the network. So nothing is drawn where the newest snapshot says:
// it is drawn a little in the past, between the two snapshots around that moment, each
// stamped with the server's time rather than the time it arrived. The own ship is not here;
// it runs on Prediction.
//
// Its input is snapshots as they arrive; its output is a pose per object for a render
// time. That is the half of the netcode a test can drive against a Simulation, as
// Prediction is the other.
class Interpolation
{
public:
    // How far behind the newest snapshot the buffer reaches, and its most snapshots ever.
    static constexpr double KEEP_SECONDS = 0.5;
    static constexpr size_t MAX_SNAPSHOTS = 120;

    struct Pose
    {
        Vector2 pos{ 0.0f, 0.0f };
        float   heading = 0.0f;
    };

    // A snapshot arrived, stamped with the server's time. Every one goes in -- also one
    // that arrived in the same frame as a newer one and is otherwise dropped: each is a
    // sample of where things were.
    void Add(double serverTime, std::vector<Proto::EntitySnapshot> entities);
    // Once a frame, after what arrived: in server time order (one that came late takes its
    // place), and no more than KEEP_SECONDS behind the newest -- but never fewer than two.
    void Settle();
    // Another system: its history is no use here.
    void   Clear() { snaps_.clear(); }
    size_t Size() const { return snaps_.size(); }

    // The two snapshots around a render time: the last at or before it and the first after.
    // Found once a frame and asked for every object.
    struct Bracket
    {
        double                                    t = 0.0;
        const std::vector<Proto::EntitySnapshot>* before = nullptr;
        double                                    beforeT = 0.0;
        const std::vector<Proto::EntitySnapshot>* after = nullptr;
        double                                    afterT = 0.0;
    };
    Bracket Around(double renderTime) const;

    // Where object `id` is drawn: between the two snapshots when both have it, else as the
    // one that does. False when neither has it -- an object just created, which the caller
    // takes from the newest snapshot.
    static bool PoseOf(const Bracket& b, int id, Pose& out);

    // How object `id` moves, from the two newest snapshots that have it: what the server
    // said it did, not a derivative of the interpolation, which steps at every snapshot. A
    // follow flies it (#298). Zero with fewer than two.
    Vector2 VelocityOf(int id) const;

private:
    struct Snap
    {
        double                             t = 0.0;
        std::vector<Proto::EntitySnapshot> entities;
    };
    std::deque<Snap> snaps_;
};
