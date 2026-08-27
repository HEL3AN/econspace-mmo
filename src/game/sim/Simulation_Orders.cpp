// The standing-order executor (#26): one translation unit of Simulation (#17).

#include "sim/Simulation.h"
#include "sim/PlayerStep.h"
#include "sim/SimTuning.h"

#include "core/Archetype.h"
#include "entities/Entity.h"
#include "entities/JumpGate.h"
#include "entities/Ship.h"

#include <cmath>

// --- Standing orders (#26) --------------------------------------------------
//
// The executor decides what this tick's command should be and drives the ship through
// Sim::StepPlayerShip -- the same function a client command goes through. It never moves
// the ship itself, so there is exactly one implementation of "how a ship moves" and an
// ordered ship behaves identically to a flown one.

namespace
{
Entity* FindById(SystemState& st, int id)
{
    for (auto& e : st.entities)
        if (e->GetId() == id)
            return e.get();
    return nullptr;
}

float DistTo(const Ship& s, Vector2 p)
{
    float dx = p.x - s.GetPosition().x;
    float dy = p.y - s.GetPosition().y;
    return std::sqrt(dx * dx + dy * dy);
}
}  // namespace

int Simulation::GiveOrder(const Orders::Order& o)
{
    order_ = o;
    orderId_ = ++nextOrderId_;
    orderStatus_ = Orders::Status::Running;
    orderDetail_.clear();
    orderNavIssued_ = false;
    return orderId_;
}

void Simulation::AbortOrder(const std::string& why)
{
    if (orderStatus_ != Orders::Status::Running)
        return;
    orderStatus_ = Orders::Status::Failed;
    orderDetail_ = why;
    RecordEvent(Ev::Kind::OrderFailed, std::string(Orders::KindName(order_.kind)) + ": " + why);
    if (player_)
    {
        player_->DisengageAutopilot();
        player_->CancelWarp();
        player_->SetMiningOn(false);
    }
}

void Simulation::StepPlayerOrder(SystemState& st, float dt)
{
    if (orderStatus_ != Orders::Status::Running || !player_)
        return;

    auto finish = [this](Orders::Status s, const std::string& detail)
    {
        orderStatus_ = s;
        orderDetail_ = detail;
        if (player_)
        {
            player_->DisengageAutopilot();
            player_->SetMiningOn(false);
        }
        // The journal entry is the point: it is what an agent sleeps on rather than
        // polling the world to find out whether its order is done.
        RecordEvent(s == Orders::Status::Done ? Ev::Kind::OrderDone : Ev::Kind::OrderFailed,
                    std::string(Orders::KindName(order_.kind)) + ": " + detail);
    };

    // Give up while there is still a ship to give up with. An agent may be thirty seconds
    // from its next decision; an order that keeps flying a dying ship into whatever is
    // killing it makes agent play a lottery.
    if (player_->GetMaxHull() > 0.0f &&
        player_->GetHull() / player_->GetMaxHull() < Orders::ABORT_HULL_FRACTION)
    {
        AbortOrder("hull critical");
        return;
    }

    // Undock needs no approach and no ship step.
    if (order_.kind == Orders::Kind::Undock)
    {
        StepPlayerUndock();
        finish(Orders::Status::Done, "undocked");
        return;
    }

    // A route is a sequence of jumps. The path is recomputed on arrival in each system
    // rather than planned once and followed blindly: the galaxy drifts underneath a long
    // journey, and a route that was safe when it was planned may not be by the third hop.
    if (order_.kind == Orders::Kind::Route)
    {
        if (activeId_ == order_.destSystem)
        {
            finish(Orders::Status::Done, "arrived in " + order_.destSystem);
            return;
        }
        std::vector<std::string> path = PlanRoute(activeId_, order_.destSystem, order_.avoidDanger);
        if (path.size() < 2)
        {
            finish(Orders::Status::Failed, "no route to " + order_.destSystem);
            return;
        }
        const std::string& nextHop = path[1];

        // The gate for this hop. Gates carry their destination, so the hop names the gate.
        JumpGate* gate = nullptr;
        for (auto& e : st.entities)
        {
            JumpGate* g =
                e->GetKind() == EntityKind::Gate ? static_cast<JumpGate*>(e.get()) : nullptr;
            if (g != nullptr && g->GetDestination() == nextHop)
            {
                gate = g;
                break;
            }
        }
        if (gate == nullptr)
        {
            finish(Orders::Status::Failed, "no gate to " + nextHop + " in this system");
            return;
        }

        float gateDist = DistTo(*player_, gate->GetPosition());
        if (gateDist > gate->GetSize() + 100.0f)
        {
            if (!orderNavIssued_)
            {
                Proto::Command nav;
                nav.navMode = 2;  // warp: gates are far apart and an agent is paying for time
                nav.navTarget = gate->GetPosition();
                nav.navStopDist = gate->GetSize() + 60.0f;
                Sim::StepPlayerShip(*player_, nav, 1.0f, dt);
                orderNavIssued_ = true;
                return;
            }
            Sim::StepPlayerShip(*player_, Proto::Command{}, 1.0f, dt);
            return;
        }

        // At the gate: jump, then let the next tick plan from where we land.
        player_->DisengageAutopilot();
        std::string dest = JumpGateDestIfNear(st, gate->GetId());
        if (dest.empty())
        {
            Sim::StepPlayerShip(*player_, Proto::Command{}, 1.0f, dt);
            return;  // still closing the last few units
        }
        ServerEnterSystem(dest, activeId_);
        RecordEvent(Ev::Kind::Jumped, "Jumped to " + dest);
        orderNavIssued_ = false;
        return;
    }

    // Everything else acts on a place: an object by id, or a bare point.
    Entity* target = order_.targetId != 0 ? FindById(st, order_.targetId) : nullptr;
    if (order_.targetId != 0 && target == nullptr)
    {
        finish(Orders::Status::Failed, "target is not in this system");
        return;
    }
    Vector2 dest = target != nullptr ? target->GetPosition() : order_.point;
    float   size = target != nullptr ? target->GetSize() : 0.0f;

    // How close this order needs to get before it can act.
    float arrive = order_.stopDist;
    if (order_.kind == Orders::Kind::Dock)
    {
        // The dock's own range, asked of the dock, rather than a second copy of the
        // number: StepPlayerDock admits at exactly this distance, so a constant here
        // would park the ship just outside the door the moment a player-built dock
        // declares a different one (#44).
        const Archetype* a = target != nullptr ? target->GetArchetype() : nullptr;
        arrive = size + (a != nullptr ? a->dockRange : 0.0f);
    }
    else if (order_.kind == Orders::Kind::Mine)
        arrive = size + Sim::MINING_RANGE;

    float dist = DistTo(*player_, dest);

    // While out of range, fly there. The nav order is issued once: re-issuing it every
    // tick would restart the warp spin-up forever.
    if (dist > arrive)
    {
        if (!orderNavIssued_)
        {
            Proto::Command nav;
            nav.navMode = order_.useWarp ? 2 : 1;
            nav.navTarget = dest;
            nav.navStopDist = arrive * 0.8f;  // aim inside the range, not at its edge
            Sim::StepPlayerShip(*player_, nav, 1.0f, dt);
            orderNavIssued_ = true;
            return;
        }
        Sim::StepPlayerShip(*player_, Proto::Command{}, 1.0f, dt);
        return;
    }

    // In range: the approach is over, whatever happens next.
    orderNavIssued_ = false;
    player_->DisengageAutopilot();

    switch (order_.kind)
    {
        case Orders::Kind::MoveTo:
            Sim::StepPlayerShip(*player_, Proto::Command{}, 1.0f, dt);
            finish(Orders::Status::Done, "arrived");
            return;

        case Orders::Kind::Dock:
        {
            if (IsPlayerDocked())
            {
                finish(Orders::Status::Done, "docked");
                return;
            }
            if (StepPlayerDock(st) == 0)
            {
                // In range and still refused: reputation, or warping. Either way the order
                // cannot finish, and the reason is already queued for the player.
                finish(Orders::Status::Failed, "docking refused");
                return;
            }
            finish(Orders::Status::Done, "docked");
            return;
        }

        case Orders::Kind::Mine:
        {
            if (player_->GetCargoUsed() >= player_->GetCargoCapacity())
            {
                finish(Orders::Status::Done, "hold full");
                return;
            }
            player_->SetMiningOn(true);
            Sim::StepPlayerShip(*player_, Proto::Command{}, 1.0f, dt);
            PlayerMiningResult r =
                StepPlayerMining(st, account_.GetSkills().GetBonus(SkillType::Mining), dt);
            if (r.fieldId == 0)
            {
                // In range but nothing came out: the field is exhausted.
                finish(Orders::Status::Done, "field is exhausted");
                return;
            }
            if (!order_.untilFull)
                finish(Orders::Status::Done, "mined");
            return;
        }

        default: break;
    }

    finish(Orders::Status::Failed, "unsupported order");
}
