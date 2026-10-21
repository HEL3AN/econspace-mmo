#include "core/Navigation.h"

#include "sim/Orders.h"

#include <nlohmann/json.hpp>

#include <algorithm>

bool Navigation::Replan(const WorldLoader::Universe& known, const std::string& from, double mass,
                        double now, std::string& reason, const std::string& destination,
                        const std::vector<std::string>& waypoints, Route::Preference preference)
{
    std::vector<std::string> left = waypoints;
    while (!left.empty() && left.front() == from)
        left.erase(left.begin());
    std::vector<std::string> stops = left;
    stops.push_back(destination);
    Route::Result r = Route::FindVia(known, from, stops, mass, now, preference);
    reason = r.reason;
    if (r.path.empty())
        return false;  // a request that fails leaves the itinerary as it was
    destination_ = destination;
    waypoints_ = std::move(left);
    preference_ = preference;
    path_ = std::move(r.path);
    reached_ = 0;
    status_ = path_.size() == 1 ? Status::Arrived : Status::Planned;
    detail_.clear();
    observedSystem_ = from;
    orderId_ = 0;
    return true;
}

bool Navigation::SetDestination(const WorldLoader::Universe& known, const std::string& from,
                                const std::string& to, double mass, double now, std::string& reason)
{
    // As EVE's "set destination": a new destination starts a new itinerary.
    return Replan(known, from, mass, now, reason, to, {}, preference_);
}

bool Navigation::AddWaypoint(const WorldLoader::Universe& known, const std::string& from,
                             const std::string& via, double mass, double now, std::string& reason)
{
    if (destination_.empty())  // the first stop of an empty itinerary is its destination
        return SetDestination(known, from, via, mass, now, reason);
    if (waypoints_.size() >= Proto::ROUTE_WAYPOINTS_MAX)
    {
        reason =
            "A route has room for " + std::to_string(Proto::ROUTE_WAYPOINTS_MAX) + " waypoints.";
        return false;
    }
    // Before the destination: the destination stays where the journey ends.
    std::vector<std::string> wps = waypoints_;
    wps.push_back(via);
    return Replan(known, from, mass, now, reason, destination_, wps, preference_);
}

bool Navigation::RemoveWaypoint(const WorldLoader::Universe& known, const std::string& from,
                                size_t index, double mass, double now, std::string& reason)
{
    if (index >= waypoints_.size())
        return false;
    std::vector<std::string> wps = waypoints_;
    wps.erase(wps.begin() + (long)index);
    return Replan(known, from, mass, now, reason, destination_, wps, preference_);
}

bool Navigation::SetPreference(const WorldLoader::Universe& known, const std::string& from,
                               Route::Preference p, double mass, double now, std::string& reason)
{
    if (destination_.empty())
    {
        preference_ = p;  // kept for the next route
        return true;
    }
    return Replan(known, from, mass, now, reason, destination_, waypoints_, p);
}

void Navigation::Clear()
{
    std::vector<std::string> keep = std::move(favorites_);
    const Route::Preference  p = preference_;
    *this = Navigation();
    favorites_ = std::move(keep);
    preference_ = p;
}

bool Navigation::Refresh(const WorldLoader::Universe& known, const std::string& from, double mass,
                         double now, std::string& reason)
{
    if (destination_.empty())
        return false;
    std::vector<std::string> left = waypoints_;
    while (!left.empty() && left.front() == from)
        left.erase(left.begin());
    std::vector<std::string> stops = left;
    stops.push_back(destination_);
    Route::Result r = Route::FindVia(known, from, stops, mass, now, preference_);
    reason = r.reason;
    if (r.path.empty())
    {
        status_ = Status::Blocked;
        detail_ = reason;
        return false;
    }
    // What was travelled stays on the list: up to where the ship left the plan.
    std::vector<std::string> travelled;
    if (!path_.empty())
    {
        const size_t upto = std::min(reached_, path_.size() - 1);
        travelled.assign(path_.begin(), path_.begin() + (long)upto);
        if (path_[upto] != from)
            travelled.push_back(path_[upto]);
    }
    waypoints_ = std::move(left);
    path_ = travelled;
    path_.insert(path_.end(), r.path.begin(), r.path.end());
    reached_ = travelled.size();
    observedSystem_ = from;
    if (status_ == Status::Blocked)
    {
        status_ = Status::Planned;
        detail_.clear();
    }
    return true;
}

void Navigation::Begin(int previousOrder)
{
    if (path_.empty() || reached_ + 1 >= path_.size())
        return;
    orderAfter_ = previousOrder;
    orderId_ = 0;
    status_ = Status::Starting;
    detail_ = "Waiting for the autopilot.";
}

void Navigation::Pause(const std::string& reason)
{
    if (path_.empty())
        return;
    status_ = Status::Paused;
    detail_ = reason;
}

bool Navigation::Observe(const Proto::Snapshot& snapshot, const WorldLoader::Universe& known,
                         double mass)
{
    if (path_.empty() || snapshot.systemId.empty())
        return false;
    const nlohmann::json before = ToJson();
    const std::string&   here = snapshot.systemId;
    if (here != observedSystem_)
    {
        // A waypoint reached is passed, as the server's order passes it.
        while (!waypoints_.empty() && waypoints_.front() == here)
            waypoints_.erase(waypoints_.begin());
        // Reaching a system is the server's fact, whatever is left of the way on: the next
        // place on the plan it matches, or -- off the plan -- the rest planned from here.
        auto at = path_.end();
        if (reached_ + 1 < path_.size())
            at = std::find(path_.begin() + (long)reached_ + 1, path_.end(), here);
        if (at != path_.end())
            reached_ = (size_t)(at - path_.begin());
        else
        {
            std::string why;
            Refresh(known, here, mass, snapshot.time, why);
        }
        observedSystem_ = here;
    }
    const Proto::PlayerView& p = snapshot.player;
    if (Active() && p.orderId > orderAfter_)
    {
        if (orderId_ == 0 && p.orderKind == (int)Orders::Kind::Route)
            orderId_ = p.orderId;
        if (p.orderId != orderId_ || p.orderKind != (int)Orders::Kind::Route)
            Pause("Another order took the ship. The route is kept.");
        else if (p.orderStatus == (int)Orders::Status::Running)
        {
            status_ = Status::Travelling;
            detail_ = "The autopilot is flying the route.";
        }
        else if (p.orderStatus == (int)Orders::Status::Failed)
        {
            status_ = p.orderDetail == "manual control" || p.orderDetail == "aborted by client"
                          ? Status::Paused
                          : Status::Blocked;
            detail_ = p.orderDetail;
        }
        else if (p.orderStatus == (int)Orders::Status::Done)
        {
            status_ = Status::Arrived;
            detail_ = p.orderDetail;
        }
    }
    if (waypoints_.empty() && here == destination_)
    {
        reached_ = path_.size() - 1;
        status_ = Status::Arrived;
        detail_ = "Destination reached.";
    }
    return before != ToJson();
}

const char* Navigation::Label() const
{
    switch (status_)
    {
        case Status::Empty: return "No route";
        case Status::Planned: return "Ready";
        case Status::Starting: return "Starting";
        case Status::Travelling: return "Autopilot on";
        case Status::Paused: return "Paused";
        case Status::Arrived: return "Arrived";
        case Status::Blocked: return "Route blocked";
    }
    return "No route";
}

bool Navigation::Favorite(const std::string& id) const
{
    return std::find(favorites_.begin(), favorites_.end(), id) != favorites_.end();
}

void Navigation::ToggleFavorite(const std::string& id)
{
    auto it = std::find(favorites_.begin(), favorites_.end(), id);
    if (it != favorites_.end())
        favorites_.erase(it);
    else if (!id.empty())
        favorites_.push_back(id);
}

nlohmann::json Navigation::ToJson() const
{
    return { { "destination", destination_ },
             { "waypoints", waypoints_ },
             { "preference", Route::PreferenceName(preference_) },
             { "path", path_ },
             { "reached", reached_ },
             { "status", (int)status_ },
             { "detail", detail_ },
             { "favorites", favorites_ } };
}

bool Navigation::FromJson(const nlohmann::json& value)
{
    if (!value.is_object())
        return false;
    try
    {
        Navigation next;
        next.destination_ = value.value("destination", std::string());
        next.waypoints_ = value.value("waypoints", std::vector<std::string>{});
        next.path_ = value.value("path", std::vector<std::string>{});
        next.favorites_ = value.value("favorites", std::vector<std::string>{});
        next.reached_ = value.value("reached", size_t{ 0 });
        next.detail_ = value.value("detail", std::string());
        const int state = value.value("status", 0);
        if (!Route::PreferenceFromName(value.value("preference", std::string("shortest")),
                                       next.preference_) ||
            state < 0 || state > (int)Status::Blocked ||
            next.waypoints_.size() > Proto::ROUTE_WAYPOINTS_MAX ||
            (!next.path_.empty() && next.reached_ >= next.path_.size()) ||
            (next.path_.empty() != next.destination_.empty()))
            return false;
        next.status_ = next.path_.empty() ? Status::Empty : (Status)state;
        if (!next.path_.empty())
            next.observedSystem_ = next.path_[next.reached_];
        // A journey is never resumed by itself: the pilot says when.
        if (next.Active())
            next.Pause("Paused when the game closed. Resume when ready.");
        *this = std::move(next);
        return true;
    }
    catch (const nlohmann::json::exception&)
    {
        return false;
    }
}
