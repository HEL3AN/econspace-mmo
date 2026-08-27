// The standing-order executor (#26): one translation unit of Simulation (#17).

#include "sim/Simulation.h"
#include "sim/ClientSession.h"
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

int Simulation::GiveOrder(ClientSession& s, const Orders::Order& o)
{
    s.order = o;
    s.orderId = ++s.nextOrderId;
    s.orderStatus = Orders::Status::Running;
    s.orderDetail.clear();
    s.orderNavIssued = false;
    return s.orderId;
}

void Simulation::AbortOrder(ClientSession& s, const std::string& why)
{
    if (s.orderStatus != Orders::Status::Running)
        return;
    s.orderStatus = Orders::Status::Failed;
    s.orderDetail = why;
    s.RecordEvent(Ev::Kind::OrderFailed, std::string(Orders::KindName(s.order.kind)) + ": " + why);
    if (s.ship)
    {
        s.ship->DisengageAutopilot();
        s.ship->CancelWarp();
        s.ship->SetMiningOn(false);
    }
}

void Simulation::StepPlayerOrder(ClientSession& s, SystemState& st, float dt)
{
    if (s.orderStatus != Orders::Status::Running || !s.ship)
        return;

    auto finish = [&s](Orders::Status status, const std::string& detail)
    {
        s.orderStatus = status;
        s.orderDetail = detail;
        if (s.ship)
        {
            s.ship->DisengageAutopilot();
            s.ship->SetMiningOn(false);
        }
        // The journal entry is the point: it is what an agent sleeps on rather than
        // polling the world to find out whether its order is done.
        s.RecordEvent(status == Orders::Status::Done ? Ev::Kind::OrderDone : Ev::Kind::OrderFailed,
                      std::string(Orders::KindName(s.order.kind)) + ": " + detail);
    };

    // Give up while there is still a ship to give up with. An agent may be thirty seconds
    // from its next decision; an order that keeps flying a dying ship into whatever is
    // killing it makes agent play a lottery.
    if (s.ship->GetMaxHull() > 0.0f &&
        s.ship->GetHull() / s.ship->GetMaxHull() < Orders::ABORT_HULL_FRACTION)
    {
        AbortOrder(s, "hull critical");
        return;
    }

    // Undock needs no approach and no ship step.
    if (s.order.kind == Orders::Kind::Undock)
    {
        StepPlayerUndock(s);
        finish(Orders::Status::Done, "undocked");
        return;
    }

    // A route is a sequence of jumps. The path is recomputed on arrival in each system
    // rather than planned once and followed blindly: the galaxy drifts underneath a long
    // journey, and a route that was safe when it was planned may not be by the third hop.
    if (s.order.kind == Orders::Kind::Route)
    {
        if (s.systemId == s.order.destSystem)
        {
            finish(Orders::Status::Done, "arrived in " + s.order.destSystem);
            return;
        }
        std::vector<std::string> path =
            PlanRoute(s.systemId, s.order.destSystem, s.order.avoidDanger);
        if (path.size() < 2)
        {
            finish(Orders::Status::Failed, "no route to " + s.order.destSystem);
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

        float gateDist = DistTo(*s.ship, gate->GetPosition());
        if (gateDist > gate->GetSize() + 100.0f)
        {
            if (!s.orderNavIssued)
            {
                Proto::Command nav;
                nav.navMode = 2;  // warp: gates are far apart and an agent is paying for time
                nav.navTarget = gate->GetPosition();
                nav.navStopDist = gate->GetSize() + 60.0f;
                Sim::StepPlayerShip(*s.ship, nav, 1.0f, dt);
                s.orderNavIssued = true;
                return;
            }
            Sim::StepPlayerShip(*s.ship, Proto::Command{}, 1.0f, dt);
            return;
        }

        // At the gate: jump, then let the next tick plan from where we land.
        s.ship->DisengageAutopilot();
        std::string dest = JumpGateDestIfNear(s, st, gate->GetId());
        if (dest.empty())
        {
            Sim::StepPlayerShip(*s.ship, Proto::Command{}, 1.0f, dt);
            return;  // still closing the last few units
        }
        ServerEnterSystem(s, dest, s.systemId);
        s.RecordEvent(Ev::Kind::Jumped, "Jumped to " + dest);
        s.orderNavIssued = false;
        return;
    }

    // Everything else acts on a place: an object by id, or a bare point.
    Entity* target = s.order.targetId != 0 ? FindById(st, s.order.targetId) : nullptr;
    if (s.order.targetId != 0 && target == nullptr)
    {
        finish(Orders::Status::Failed, "target is not in this system");
        return;
    }
    Vector2 dest = target != nullptr ? target->GetPosition() : s.order.point;
    float   size = target != nullptr ? target->GetSize() : 0.0f;

    // How close this order needs to get before it can act.
    float arrive = s.order.stopDist;
    if (s.order.kind == Orders::Kind::Dock)
    {
        // The dock's own range, asked of the dock, rather than a second copy of the
        // number: StepPlayerDock admits at exactly this distance, so a constant here
        // would park the ship just outside the door the moment a player-built dock
        // declares a different one (#44).
        const Archetype* a = target != nullptr ? target->GetArchetype() : nullptr;
        arrive = size + (a != nullptr ? a->dockRange : 0.0f);
    }
    else if (s.order.kind == Orders::Kind::Mine)
        arrive = size + Sim::MINING_RANGE;

    float dist = DistTo(*s.ship, dest);

    // While out of range, fly there. The nav order is issued once: re-issuing it every
    // tick would restart the warp spin-up forever.
    if (dist > arrive)
    {
        if (!s.orderNavIssued)
        {
            Proto::Command nav;
            nav.navMode = s.order.useWarp ? 2 : 1;
            nav.navTarget = dest;
            nav.navStopDist = arrive * 0.8f;  // aim inside the range, not at its edge
            Sim::StepPlayerShip(*s.ship, nav, 1.0f, dt);
            s.orderNavIssued = true;
            return;
        }
        Sim::StepPlayerShip(*s.ship, Proto::Command{}, 1.0f, dt);
        return;
    }

    // In range: the approach is over, whatever happens next.
    s.orderNavIssued = false;
    s.ship->DisengageAutopilot();

    switch (s.order.kind)
    {
        case Orders::Kind::MoveTo:
            Sim::StepPlayerShip(*s.ship, Proto::Command{}, 1.0f, dt);
            finish(Orders::Status::Done, "arrived");
            return;

        case Orders::Kind::Dock:
        {
            if (s.IsDocked())
            {
                finish(Orders::Status::Done, "docked");
                return;
            }
            if (StepPlayerDock(s, st) == 0)
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
            if (s.ship->GetCargoUsed() >= s.ship->GetCargoCapacity())
            {
                finish(Orders::Status::Done, "hold full");
                return;
            }
            s.ship->SetMiningOn(true);
            Sim::StepPlayerShip(*s.ship, Proto::Command{}, 1.0f, dt);
            PlayerMiningResult r =
                StepPlayerMining(s, st, s.account.GetSkills().GetBonus(SkillType::Mining), dt);
            if (r.fieldId == 0)
            {
                // In range but nothing came out: the field is exhausted.
                finish(Orders::Status::Done, "field is exhausted");
                return;
            }
            if (!s.order.untilFull)
                finish(Orders::Status::Done, "mined");
            return;
        }

        default: break;
    }

    finish(Orders::Status::Failed, "unsupported order");
}
