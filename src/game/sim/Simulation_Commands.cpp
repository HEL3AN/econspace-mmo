// The server's verb table (#354): what a pilot's tick and a pilot's actions ask of the world,
// and who does it.
//
// A Command is the client's input for one tick -- the held controls -- and an Action is a verb
// asked for once. Until #354 the host loop held this table and nothing tested it; it is a pair
// of methods of Simulation now, so a test can send a verb to a session and read the answer.
//
// One translation unit of Simulation (#17).

#include "sim/Simulation.h"

#include "core/Faction.h"
#include "entities/Entity.h"

#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace
{
// Who may ask for a verb, as it was before there were actions: some are asked for anywhere
// (a refusal says why), some only docked (the station is the counter), some only in flight
// (the ship is what acts).
enum class Where
{
    Anywhere,
    Docked,
    InFlight
};

template <class T> constexpr Where WhereOf()
{
    using namespace Proto::Act;
    if constexpr (std::is_same_v<T, Sell> || std::is_same_v<T, Buy> ||
                  std::is_same_v<T, RentHangar> || std::is_same_v<T, RequestAssets> ||
                  std::is_same_v<T, Deliver> || std::is_same_v<T, SwitchShip> ||
                  std::is_same_v<T, BuyShip> || std::is_same_v<T, InsureShip> ||
                  std::is_same_v<T, PayBounty> || std::is_same_v<T, AcceptOffer> ||
                  std::is_same_v<T, CompleteMission>)
        return Where::Docked;
    else if constexpr (std::is_same_v<T, Jump> || std::is_same_v<T, Loot> ||
                       std::is_same_v<T, Scan> || std::is_same_v<T, Deploy> ||
                       std::is_same_v<T, Dismantle> || std::is_same_v<T, SetDefence> ||
                       std::is_same_v<T, Lock> || std::is_same_v<T, Unlock> ||
                       std::is_same_v<T, SetActive> || std::is_same_v<T, Fire>)
        return Where::InFlight;
    else
        return Where::Anywhere;
}

// What one action did: whether the session changed system (its layout must be sent again), and
// the acknowledgements a sale or purchase adds.
struct Doer
{
    Simulation&                   sim;
    ClientSession&                s;
    std::vector<Proto::TradeAck>& acks;
    bool                          changed = false;

    void operator()(const Proto::Act::Dock&)
    {
        if (!s.IsDocked())
            sim.StepPlayerDock(s, *sim.SystemOf(s));
    }
    void operator()(const Proto::Act::Undock&) { sim.StepPlayerUndock(s); }

    // Debug (F1): credit money to the server account -- only on a server started with --dev
    // (#341). Anyone who could open a socket used to be able to print money.
    void operator()(const Proto::Act::DebugMoney&)
    {
        if (!sim.GrantDebugMoney(s))
            s.RecordEvent(Ev::Kind::Notice, "Debug money refused: this server is not in dev mode");
    }

    // Naming a system as its discoverer (#145). Not a station verb: the map offers it in
    // flight, and filed under the docked ones it was dropped without a word for everyone not
    // at a station (#308). The server says why it refuses, in the journal; an accepted name
    // reaches everyone in the galaxy index.
    void operator()(const Proto::Act::NameSystem& a)
    {
        if (!a.name.empty())
            sim.NameSystem(s, a.systemId.empty() ? s.systemId : a.systemId, a.name);
    }
    // A licence to build here (#431 slice 6): docked at the holding faction's station, or in a
    // corporation's system anywhere -- so not a docked verb either; a refusal says why.
    void operator()(const Proto::Act::BuyLicence&) { sim.BuyLicence(s); }
    // Charts sold to the faction docked at (#346, #431 slice 16): asked for in flight, it is
    // refused with a reason, so not under the docked verbs either.
    void operator()(const Proto::Act::SellChart& a)
    {
        if (!a.what.empty())
            sim.SellCharts(s, a.what);
    }

    // A refit (#279 fitting). Not under the docked verbs: asked for in flight, it is refused
    // with a reason rather than dropped without a word.
    void operator()(const Proto::Act::Refit& a) { sim.RefitShip(s, a.lines); }
    // The design library (#279 step 3): saved, renamed and deleted anywhere; applied as a
    // refit is, which says why when it is not at a shipyard.
    void operator()(const Proto::Act::SaveDesign& a)
    {
        if (!a.name.empty())
            sim.SaveDesign(s, a.name, a.lines);
    }
    void operator()(const Proto::Act::RenameDesign& a)
    {
        if (!a.name.empty())
            sim.RenameDesign(s, a.name, a.to);
    }
    void operator()(const Proto::Act::DeleteDesign& a)
    {
        if (!a.name.empty())
            sim.DeleteDesign(s, a.name);
    }
    void operator()(const Proto::Act::ApplyDesign& a)
    {
        if (!a.name.empty())
            sim.ApplyDesign(s, a.name);
    }
    void operator()(const Proto::Act::SetLivery& a)  // (#516)
    {
        if (!a.livery.empty())
            sim.SetLivery(s, a.livery);
    }

    // Goods between containers (#472), docked or in space -- a can is in space: what moved
    // shows in the snapshot, a refusal in the journal.
    void operator()(const Proto::Act::Move& a)
    {
        if (a.type >= 0 && a.amount > 0)
            sim.MoveGoods(s, a.from, a.to, a.type, a.amount);
    }

    // Station trade/hangar: sell, refit, buy, pay off bounty. Account effects
    // (money/skill/reputation/bounty) are applied by the core in account_ (server-authoritative).
    void operator()(const Proto::Act::Sell& a)
    {
        if (a.type < 0 || a.amount <= 0)
            return;
        const Simulation::PlayerSellResult r =
            sim.StepPlayerSell(s, *sim.SystemOf(s), a.type, a.amount, a.from);
        if (r.sold > 0)
            acks.push_back(Proto::TradeAck{ a.type, r.sold, r.gross, r.revenue, 0, 0.0, r.fee });
    }
    void operator()(const Proto::Act::Buy& a)  // a refusal says why, in the journal (#343)
    {
        if (a.type < 0 || a.amount <= 0)
            return;
        const Simulation::PlayerBuyResult r =
            sim.StepPlayerBuy(s, *sim.SystemOf(s), a.type, a.amount, a.to);
        if (r.bought > 0)
            acks.push_back(Proto::TradeAck{ a.type, 0, 0.0, 0.0, r.bought, r.cost, r.fee });
    }
    void operator()(const Proto::Act::RentHangar& a)  // a hangar's rented room (#472)
    {
        sim.RentHangar(s, a.amount);
    }
    void operator()(const Proto::Act::RequestAssets&) { s.assetsDue = true; }  // (#472 slice 9)
    void operator()(const Proto::Act::Deliver& a)  // goods fetched out of a hostile station
    {
        if (!a.from.empty())
            sim.RequestDelivery(s, a.from, a.type, a.amount);  // (#472 slice 6)
    }
    void operator()(const Proto::Act::SwitchShip& a) { sim.SwitchShip(s, a.ship); }  // (#5)
    void operator()(const Proto::Act::BuyShip& a) { sim.BuyShip(s, a.ship); }
    void operator()(const Proto::Act::InsureShip& a) { sim.InsureShip(s, a.ship); }  // (#344)
    void operator()(const Proto::Act::PayBounty& a)
    {
        FactionId to;
        if (!a.faction.empty() && Factions::Find(a.faction, to))
            sim.PayBounty(s, to);
    }
    void operator()(const Proto::Act::AcceptOffer& a) { sim.AcceptMission(s, a.offer); }
    void operator()(const Proto::Act::CompleteMission& a) { sim.CompleteMission(s, a.mission); }

    // A standing order arriving over the wire (#72). It is an intent like any other: the
    // server validates it and owns the execution. Docked, only an undock can run; anything
    // else is refused with the reason, so an agent learns to undock first rather than wait on
    // an order that never started.
    void operator()(const Proto::Act::GiveOrder& a)
    {
        if (a.order.kind == Orders::Kind::None)
            return;
        sim.GiveOrder(s, a.order);
        if (s.IsDocked() && a.order.kind != Orders::Kind::Undock)
            sim.AbortOrder(s, "docked; undock first");
    }
    void operator()(const Proto::Act::AbortOrder&) { sim.AbortOrder(s, "aborted by client"); }

    void operator()(const Proto::Act::Jump& a)
    {
        if (a.gateId == 0)
            return;
        const std::string dest = sim.JumpGateDestIfNear(s, *sim.SystemOf(s), a.gateId);
        if (!dest.empty())
        {
            sim.EnterThrough(s, a.gateId, dest);  // arrives at the gate back
            changed = true;
            return;
        }
        // Too heavy for an unstable mouth (#342 S4): said, since nothing else would show it.
        for (const auto& e : sim.SystemOf(s)->entities)
            if (e->GetId() == a.gateId)
            {
                const std::string barred = sim.MouthProblem(s, *e);
                if (!barred.empty())
                    s.RecordEvent(Ev::Kind::Notice, "Cannot jump: " + barred);
            }
    }
    void operator()(const Proto::Act::Loot& a)
    {
        if (a.id != 0)
            sim.StepPlayerLoot(s, *sim.SystemOf(s), a.id);
    }
    // A pass of the scanner (#346); the answer is in the journal either way.
    void operator()(const Proto::Act::Scan&) { sim.StartScan(s); }
    // A site, laid down from space (#39); the answer is in the journal either way.
    void operator()(const Proto::Act::Deploy& a)
    {
        if (!a.blueprint.empty())
            sim.Deploy(s, a.blueprint, a.at, a.name);
    }
    // ...and taken apart by its owner, the same way.
    void operator()(const Proto::Act::Dismantle& a)
    {
        if (a.id != 0)
            sim.Dismantle(s, a.id);
    }
    // ...and told whom to fire at (#340).
    void operator()(const Proto::Act::SetDefence& a)
    {
        if (a.id != 0)
            sim.SetDefence(s, a.id, a.defence);
    }
    // A direct trade with a pilot docked here (#473): not a docked verb either, so that a
    // cancel still reaches a pilot who has just left and a refusal says why.
    void operator()(const Proto::Act::Deal& a) { sim.ApplyDeal(s, a); }
    // An exchange contract (#473): posting and taking are done docked at its station, and a
    // cancel is from anywhere; the refusals say which.
    void operator()(const Proto::Act::Board& a) { sim.ApplyBoard(s, a); }
    // Targeting and fire (#619). A lock is asked for and answered in the snapshot's locks (or a
    // Notice saying why not); the guns are on or off and fire only at a held active lock.
    void operator()(const Proto::Act::Lock& a) { sim.LockTarget(s, a.id); }
    void operator()(const Proto::Act::Unlock& a) { sim.Unlock(s, a.id); }
    void operator()(const Proto::Act::SetActive& a) { sim.SetActiveLock(s, a.id); }
    void operator()(const Proto::Act::Fire& a) { s.weaponOn = a.on; }
};
}  // namespace

bool Simulation::ApplyAction(ClientSession& s, const Proto::Action& a,
                             std::vector<Proto::TradeAck>& acks)
{
    Doer doer{ *this, s, acks };
    std::visit(
        [&](const auto& verb)
        {
            using T = std::decay_t<decltype(verb)>;
            // At a station the ship neither moves nor fires, and in space there is no counter
            // to trade at: the other side's verbs are not asked of it.
            const bool docked = s.IsDocked();
            if ((WhereOf<T>() == Where::Docked && !docked) ||
                (WhereOf<T>() == Where::InFlight && docked))
                return;
            doer(verb);
        },
        a);
    return doer.changed;
}

// One client input = ONE player tick. This is critical for client and server
// prediction to match: the client predicts one StepPlayerShip per input, and the
// server must step the same way (otherwise the ack numbers diverge from the step count
// and the ship "jerks"). Applies movement/combat/mining for command c.
void Simulation::ApplyCommand(ClientSession& s, const Proto::Command& c, float dt, FireMap& fires)
{
    // While docked we do not step the player's physics (the ship is carried by the station,
    // #298), and it neither moves nor fires.
    if (s.IsDocked())
        return;

    // Manual control takes the ship back. This is the same rule the autopilot follows,
    // and it is what lets a human grab the stick from an agent mid-order.
    if (s.HasRunningOrder() && (c.thrust || c.brake || c.turn != 0.0f || c.navMode != 0))
        AbortOrder(s, "manual control");

    StepPlayerShip(s, c, 1.0f, Simulation::CommandDt(s, dt));

    // Player fire: on a shot — a beam event into the snapshot (client draws it blue). What a
    // shot or a kill costs or earns the account is applied in StepPlayerFire.
    Simulation::PlayerCombatEvents ev;
    s.selectedId = c.targetId;          // what it is told about in full, wherever it is (#349)
    const int fireAt = s.HeldTarget();  // a gun fires at a held lock, never at a selection (#619)
    if (StepPlayerFire(s, *SystemOf(s), fireAt, dt, &ev))
    {
        FireEvent fe;
        fe.from = ev.shotFrom;
        fe.to = ev.shotTo;
        fe.shooterFaction = Factions::Unaffiliated();
        fe.shooterSessionId = s.id;  // "mine" for this player and nobody else
        fe.targetEntityId = fireAt;
        fe.quality = ev.quality;
        fe.targetSessionId = ev.victimSession;  // "at me" for a pilot shot at (#431 slice 10)
        fires[s.systemId].push_back(fe);
    }
    StepPlayerMining(s, *SystemOf(s), s.account.GetSkills().GetBonus(SkillType::Mining), dt);
}
