// The server's verb table (#354): what one client Command asks of the world, and who does it.
//
// A Command is the client's input for one tick -- held controls and the one-shot verbs that
// ride along with them. Until #354 the host loop held this table and nothing tested it; it is
// a method of Simulation now, so a test can send a verb to a session and read the answer.
//
// One translation unit of Simulation (#17).

#include "sim/Simulation.h"

#include "core/Faction.h"
#include "entities/Entity.h"

#include <string>
#include <vector>

namespace
{

// A standing order arriving over the wire (#72). It is an intent like any other: the server
// validates it and owns the execution. Docked, only an undock can run; anything else is refused
// with the reason, so an agent learns to undock first rather than wait on an order that never
// started.
void TakeOrder(Simulation& sim, ClientSession& s, const Proto::Command& c)
{
    if (c.abortOrder)
        sim.AbortOrder(s, "aborted by client");
    if (c.orderKind == 0)
        return;
    Orders::Order o;
    o.kind = (Orders::Kind)c.orderKind;
    o.targetId = c.orderTarget;
    o.point = c.orderPoint;
    o.stopDist = c.orderStopDist;
    o.useWarp = c.orderWarp;
    o.untilFull = c.orderUntilFull;
    o.destSystem = c.orderDestSystem;
    o.waypoints = c.orderWaypoints;
    o.preference = c.orderPreference;
    o.blueprint = c.orderBlueprint;
    o.name = c.orderName;
    sim.GiveOrder(s, o);
    if (s.IsDocked() && o.kind != Orders::Kind::Undock)
        sim.AbortOrder(s, "docked; undock first");
}
}  // namespace

// One client input = ONE player tick. This is critical for client and server
// prediction to match: the client predicts one StepPlayerShip per input, and the
// server must step the same way (otherwise the ack numbers diverge from the step count
// and the ship "jerks"). Applies movement/jump/loot/combat/mining for command c.
// Returns true if the system changed.
bool Simulation::ApplyCommand(ClientSession& s, const Proto::Command& c, float dt,
                              std::vector<Proto::TradeAck>& acks, FireMap& fires)
{
    bool changed = false;

    // Dock/undock (server-authoritative). While docked we do not step the player's
    // physics (the ship is carried by the station, #298), but we still handle trade/undock.
    if (c.dock && !s.IsDocked())
        StepPlayerDock(s, *SystemOf(s));
    if (c.undock)
        StepPlayerUndock(s);

    // Debug (F1): credit money to the server account -- only on a server started with --dev
    // (#341). Anyone who could open a socket used to be able to print money.
    if (c.debugMoney && !GrantDebugMoney(s))
        s.RecordEvent(Ev::Kind::Notice, "Debug money refused: this server is not in dev mode");

    // Naming the system the ship is in (#145), as its discoverer. Not a station verb: the
    // map offers it in flight, and filed under the docked ones it was dropped without a
    // word for everyone not at a station (#308). The server says why it refuses, in the
    // journal; an accepted name reaches everyone in the galaxy index.
    if (!c.nameSystem.empty())
        NameSystem(s, c.nameSystemId.empty() ? s.systemId : c.nameSystemId, c.nameSystem);
    // A licence to build here (#431 slice 6): docked at the holding faction's station, or in a
    // corporation's system anywhere -- so not a docked verb either; a refusal says why.
    if (c.buyLicence)
        BuyLicence(s);
    // Charts sold to the faction docked at (#346, #431 slice 16): asked for in flight, it is
    // refused with a reason, so not under the docked verbs either.
    if (!c.sellChart.empty())
        SellCharts(s, c.sellChart);

    // A refit (#279 fitting). Not under the docked verbs below: asked for in flight, it is
    // refused with a reason rather than dropped without a word.
    if (c.refit)
        RefitShip(s, c.refitLines);
    // The design library (#279 step 3): saved, renamed and deleted anywhere; applied as a
    // refit is, which says why when it is not at a shipyard.
    if (!c.saveDesign.empty())
        SaveDesign(s, c.saveDesign, c.saveLines);
    if (!c.renameDesign.empty())
        RenameDesign(s, c.renameDesign, c.renameTo);
    if (!c.deleteDesign.empty())
        DeleteDesign(s, c.deleteDesign);
    if (!c.applyDesign.empty())
        ApplyDesign(s, c.applyDesign);
    if (!c.setLivery.empty())  // (#516)
        SetLivery(s, c.setLivery);

    // Goods between containers (#472), docked or in space -- a can is in space: what moved
    // shows in the snapshot, a refusal in the journal.
    if (c.moveType >= 0 && c.moveAmount > 0)
        MoveGoods(s, c.moveFrom, c.moveTo, c.moveType, c.moveAmount);

    // Station trade/hangar: sell, refit, buy, pay off bounty. Account effects
    // (money/skill/reputation/bounty) are applied by the core in account_ (server-authoritative).
    if (s.IsDocked())
    {
        if (c.sellType >= 0 && c.sellAmount > 0)
        {
            Simulation::PlayerSellResult r =
                StepPlayerSell(s, *SystemOf(s), c.sellType, c.sellAmount, c.sellFrom);
            if (r.sold > 0)
                acks.push_back(
                    Proto::TradeAck{ c.sellType, r.sold, r.gross, r.revenue, 0, 0.0, r.fee });
        }
        if (c.buyType >= 0 && c.buyAmount > 0)  // a refusal says why, in the journal (#343)
        {
            const Simulation::PlayerBuyResult r =
                StepPlayerBuy(s, *SystemOf(s), c.buyType, c.buyAmount, c.buyTo);
            if (r.bought > 0)
                acks.push_back(Proto::TradeAck{ c.buyType, 0, 0.0, 0.0, r.bought, r.cost, r.fee });
        }
        if (c.rentHangar >= 0)  // a hangar's rented room (#472)
            RentHangar(s, c.rentHangar);
        if (c.requestAssets)  // (#472 slice 9)
            s.assetsDue = true;
        if (!c.deliverFrom.empty())  // goods fetched out of a hostile station (#472 slice 6)
            RequestDelivery(s, c.deliverFrom, c.deliverType, c.deliverAmount);
        if (c.storeType >= 0 && c.storeAmount > 0)
            StoreCargo(s, c.storeType, c.storeAmount);
        if (c.takeType >= 0 && c.takeAmount > 0)
            TakeCargo(s, c.takeType, c.takeAmount);
        if (c.refitShip >= 0)
            SwitchShip(s, c.refitShip);  // only to a ship this account owns (#5)
        if (c.buyShip >= 0)
            BuyShip(s, c.buyShip);  // purchase (deducts money)
        if (c.insureShip >= 0)
            InsureShip(s, c.insureShip);  // a policy on an owned hull (#344)
        FactionId bountyTo;
        if (!c.payBountyFaction.empty() && Factions::Find(c.payBountyFaction, bountyTo))
            PayBounty(s, bountyTo);  // clear bounty
        if (c.acceptOffer >= 0)
            AcceptMission(s, c.acceptOffer);  // accept a mission from the board
        if (c.completeMission >= 0)
            CompleteMission(s, c.completeMission);  // hand in a mission (reward to account)
        // An agent leaves a station by an order, and asks for its next one from there. Dropped
        // with the rest of the flight path, its undock never ran: an agent that docked stayed.
        TakeOrder(*this, s, c);
        return changed;  // at a station the ship neither moves nor fires
    }

    // Manual control takes the ship back. This is the same rule the autopilot follows,
    // and it is what lets a human grab the stick from an agent mid-order.
    if (s.HasRunningOrder() && (c.thrust || c.brake || c.turn != 0.0f || c.navMode != 0))
        AbortOrder(s, "manual control");

    TakeOrder(*this, s, c);

    if (c.toggleWeapon)
        s.ToggleWeapon();

    StepPlayerShip(s, c, 1.0f, Simulation::CommandDt(s, dt));
    if (c.scan)  // a pass of the scanner (#346); the answer is in the journal either way
        StartScan(s);

    if (c.jumpGateId != 0)
    {
        std::string dest = JumpGateDestIfNear(s, *SystemOf(s), c.jumpGateId);
        if (!dest.empty())
        {
            EnterThrough(s, c.jumpGateId, dest);  // arrives at the gate back
            changed = true;
        }
        else
            // Too heavy for an unstable mouth (#342 S4): said, since nothing else would show it.
            for (const auto& e : SystemOf(s)->entities)
                if (e->GetId() == c.jumpGateId)
                {
                    const std::string barred = MouthProblem(s, *e);
                    if (!barred.empty())
                        s.RecordEvent(Ev::Kind::Notice, "Cannot jump: " + barred);
                }
    }
    if (c.lootId != 0)
        StepPlayerLoot(s, *SystemOf(s), c.lootId);
    // A site, laid down from space (#39); the answer is in the journal either way.
    if (!c.deploy.empty())
        Deploy(s, c.deploy, c.deployPos, c.deployName);
    // ...and taken apart by its owner, the same way.
    if (c.dismantleId != 0)
        Dismantle(s, c.dismantleId);
    // ...and told whom to fire at (#340).
    if (c.defenceId != 0)
        SetDefence(s, c.defenceId, c.defence);

    // Player fire: on a shot — a beam event into the snapshot (client draws it blue). What a
    // shot or a kill costs or earns the account is applied in StepPlayerFire.
    Simulation::PlayerCombatEvents ev;
    s.selectedId = c.targetId;  // what it is told about in full, wherever it is (#349)
    if (StepPlayerFire(s, *SystemOf(s), c.targetId, dt, &ev))
    {
        FireEvent fe;
        fe.from = ev.shotFrom;
        fe.to = ev.shotTo;
        fe.shooterFaction = Factions::Unaffiliated();
        fe.shooterSessionId = s.id;  // "mine" for this player and nobody else
        fe.targetEntityId = c.targetId;
        fe.targetSessionId = ev.victimSession;  // "at me" for a pilot shot at (#431 slice 10)
        fires[s.systemId].push_back(fe);
    }
    StepPlayerMining(s, *SystemOf(s), s.account.GetSkills().GetBonus(SkillType::Mining), dt);
    return changed;
}
