#pragma once

#include "core/Route.h"
#include "core/WorldLoader.h"
#include "sim/Protocol.h"

#include <nlohmann/json_fwd.hpp>
#include <string>
#include <vector>

// The pilot's itinerary on the galaxy map (#432): a destination, the systems to pass on the
// way, how to weigh the hops, and the favourites -- a UI preference of this machine's, kept
// per server and pilot beside the executable, never in the account. The path is planned with
// Route::FindVia over the index the client holds, the same search the server's route order
// makes over the same index, so what the map draws is the way the ship flies.
//
// The itinerary is the pilot's whether or not an order is flying it: stopping the order, or
// taking the stick, keeps both the travelled part and what is left. Restored at login, it is
// paused -- a journey begins only when the pilot says so.
class Navigation
{
public:
    enum class Status
    {
        Empty,       // no destination
        Planned,     // a route, not flown
        Starting,    // asked for, the server has not said yet
        Travelling,  // the server's route order is flying it
        Paused,      // stopped, by the pilot or by another order
        Arrived,
        Blocked  // no way there now; the detail says why
    };

    // The itinerary itself. Each plans the path again from `from`; false (and the reason)
    // when there is no way, and then nothing about the itinerary has changed.
    bool SetDestination(const WorldLoader::Universe& known, const std::string& from,
                        const std::string& to, double mass, double now, std::string& reason);
    bool AddWaypoint(const WorldLoader::Universe& known, const std::string& from,
                     const std::string& via, double mass, double now, std::string& reason);
    bool RemoveWaypoint(const WorldLoader::Universe& known, const std::string& from, size_t index,
                        double mass, double now, std::string& reason);
    bool SetPreference(const WorldLoader::Universe& known, const std::string& from,
                       Route::Preference p, double mass, double now, std::string& reason);
    void Clear();
    // Plans what is left from where the ship is now, keeping what was travelled.
    bool Refresh(const WorldLoader::Universe& known, const std::string& from, double mass,
                 double now, std::string& reason);

    // The order was sent; `previousOrder` is the order id the snapshot showed before it, so
    // the next order id is this one.
    void Begin(int previousOrder);
    void Pause(const std::string& reason);
    // A snapshot: where the ship is and how the order is going. True if the itinerary changed
    // (it is worth saving).
    bool Observe(const Proto::Snapshot& snapshot, const WorldLoader::Universe& known, double mass);

    const std::string&              Destination() const { return destination_; }
    const std::vector<std::string>& Waypoints() const { return waypoints_; }
    Route::Preference               Preference() const { return preference_; }
    const std::vector<std::string>& Path() const { return path_; }
    size_t                          Reached() const { return reached_; }
    Status                          State() const { return status_; }
    const std::string&              Detail() const { return detail_; }
    bool Active() const { return status_ == Status::Starting || status_ == Status::Travelling; }
    const char* Label() const;

    bool                            Favorite(const std::string& id) const;
    void                            ToggleFavorite(const std::string& id);
    const std::vector<std::string>& Favorites() const { return favorites_; }

    nlohmann::json ToJson() const;
    // False, and nothing changed, for something that is not an itinerary.
    bool FromJson(const nlohmann::json& value);

private:
    bool Replan(const WorldLoader::Universe& known, const std::string& from, double mass,
                double now, std::string& reason, const std::string& destination,
                const std::vector<std::string>& waypoints, Route::Preference preference);

    std::string              destination_;
    std::vector<std::string> waypoints_;
    Route::Preference        preference_ = Route::Preference::Shortest;
    std::vector<std::string> path_;
    std::vector<std::string> favorites_;
    size_t                   reached_ = 0;
    Status                   status_ = Status::Empty;
    std::string              detail_;
    std::string              observedSystem_;
    int                      orderAfter_ = 0;
    int                      orderId_ = 0;
};
