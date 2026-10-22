// Everything the server does on behalf of one player: the verbs (fire, mine, loot, sell,
// dock, jump, refit, buy), the account behind them, and the mission board.
//
// One translation unit of Simulation (#17). It is also the preparation for #3: what a
// per-connection session would own is collected here first, instead of being scattered
// through the same file as the world and the NPCs.

#include "sim/Simulation.h"
#include "sim/ClientSession.h"
#include "sim/PlayerStep.h"
#include "sim/SaveFile.h"

#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Orbits.h"
#include "core/World.h"
#include "economy/Resource.h"
#include "entities/AsteroidField.h"
#include "entities/Combatant.h"
#include "entities/Derelict.h"
#include "entities/JumpGate.h"
#include "entities/NpcShip.h"
#include "entities/Ship.h"
#include "entities/ShipType.h"
#include "entities/Station.h"
#include "entities/Structure.h"
#include "core/Fitting.h"
#include "raymath.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <set>

namespace
{
// Player combat (server logic). The weapon's range is not here: it is in
// Sim::PLAYER_WEAPON_RANGE, shared because the client draws the targeting circle from it.
// Every other reach -- docking, mining, salvage, a gate -- belongs to the object being
// reached and comes from its archetype (#34). What a shot takes off is the ship's: its
// design's guns under a player (#279 step 5, ShipStats::damage).
constexpr float PLAYER_FIRE_INTERVAL = 0.5f;  // cooldown between player shots

// A station as a save names it (#227): the system it is in, its name, and which of that
// name it is there. An entity id comes from a counter that interleaves every system's
// stations with the NPCs hydrated before them, so it means the same station only within
// one server run. Null for an id that is no station.
nlohmann::json StationRefOf(const std::map<std::string, SystemState>& systems, int id)
{
    if (id == 0)
        return nullptr;
    for (const auto& kv : systems)
    {
        const auto& all = kv.second.entities;
        for (size_t i = 0; i < all.size(); i++)
        {
            if (all[i]->GetId() != id || all[i]->GetKind() != EntityKind::Station)
                continue;
            // Counted among stations of the same name only, so one added elsewhere in the
            // system does not change which is meant.
            int nth = 0;
            for (size_t k = 0; k < i; k++)
                if (all[k]->GetKind() == EntityKind::Station &&
                    all[k]->GetName() == all[i]->GetName())
                    nth++;
            return { { "system", kv.first }, { "station", all[i]->GetName() }, { "nth", nth } };
        }
    }
    return nullptr;
}

// The other direction, on load: the id that station has in this run, or 0 when the galaxy
// no longer has it.
int StationIdOf(const std::map<std::string, SystemState>& systems, const nlohmann::json& ref)
{
    if (!ref.is_object())
        return 0;
    const auto it = systems.find(ref.value("system", std::string()));
    if (it == systems.end())
        return 0;
    const std::string name = ref.value("station", std::string());
    int               nth = ref.value("nth", 0);
    for (const auto& e : it->second.entities)
        if (e->GetKind() == EntityKind::Station && e->GetName() == name && nth-- == 0)
            return e->GetId();
    return 0;
}
}  // namespace

// Credits as a pilot reads them.
static std::string Credits(double cr)
{
    return std::to_string((long long)std::llround(cr)) + " cr";
}

namespace
{
// A station a pilot may wake at: one that would let them dock (StepPlayerDock's rule).
bool TakesPilot(const ClientSession& s, const Entity& e)
{
    return e.GetKind() == EntityKind::Station && e.Has(Component::Dockable) &&
           Factions::TierOf(s.account.GetReputation(static_cast<const Station&>(e).GetFaction())) !=
               RepTier::Hated;
}

// How large the wreck of a player's ship is: about a generated battle's.
constexpr float SHIP_WRECK_SIZE = 40.0f;
}  // namespace

int Simulation::HomeOf(const ClientSession& s, std::string& systemId) const
{
    if (s.homeStationId != 0)
        for (const auto& kv : systems_)
            for (const auto& e : kv.second.entities)
                if (e->GetId() == s.homeStationId && TakesPilot(s, *e))
                {
                    systemId = kv.first;
                    return e->GetId();
                }
    for (const StartPoint& sp : starts_)
        if (const SystemState* st = SystemById(sp.systemId))
            for (const auto& e : st->entities)
                if (e->GetName() == sp.station && TakesPilot(s, *e))
                {
                    systemId = sp.systemId;
                    return e->GetId();
                }
    return 0;
}

double Simulation::ValueOf(const ClientSession& s, int catalogIndex, const SystemState& st) const
{
    const std::vector<ShipType>& catalog = GetShipCatalog();
    if (catalogIndex < 0 || catalogIndex >= (int)catalog.size())
        return 0.0;
    const ShipType&         t = catalog[catalogIndex];
    const Ships::Catalogue& ships = Archetypes::ShipCatalogue();
    const Ships::Design*    stock = ships.FindDesign(t.stats.design);
    const Ships::Design*    flown = designs_.Find(s.DesignOf(catalogIndex));
    if (stock == nullptr)
        return t.price;
    std::vector<float> ore;
    for (ResourceType rt : AllResourceTypes())
        ore.push_back((float)st.market.GetPrice(rt));
    return ShipValue(ships, t.price, *stock, flown != nullptr ? *flown : *stock, ore);
}

void Simulation::ServerRespawnPlayer(ClientSession& s)
{
    if (!s.ship)
        return;
    const std::vector<ShipType>& catalog = GetShipCatalog();
    const int                    hull = s.currentShip;
    const bool                   known = hull >= 0 && hull < (int)catalog.size();
    const std::string            hullName = known ? catalog[hull].name : std::string("ship");
    const std::string            starter = catalog.empty() ? std::string("ship") : catalog[0].name;
    SystemState*                 here = SystemOf(s);
    const std::string            where = SystemName(s.systemId);

    // What went with it: the ship as fitted, at this system's prices, and the hold.
    const double value = here != nullptr ? ValueOf(s, hull, *here) : 0.0;
    int          cargo = 0;
    for (ResourceType rt : AllResourceTypes())
        cargo += s.ship->GetCargoAmount(rt);

    // The policy pays and is spent.
    const double cover = s.CoverOf(hull);
    const double paid = cover > 0.0 ? std::floor(InsurancePayout(cover, value)) : 0.0;
    s.insurance.erase(hull);
    if (paid > 0.0)
        s.account.AddMoney(paid);

    // Whatever it was doing is over.
    AbortOrder(s, "ship destroyed");

    // A wreck where it fell, holding a share of what was lost: whoever gets there first
    // searches it, as a structure's (Simulation_Build.cpp). For an hour, so a busy fight does
    // not leave the system full of them. The ship's share is credits; the hold's is the goods
    // themselves (#343) -- the same share of each, as a hauler's wreck holds its cargo. A wreck
    // holds one good: the largest stack stays in it, and every other is a pod beside it.
    if (here != nullptr)
    {
        const double                              share = Blueprints::Rules().wreckShare;
        const double                              worth = std::floor(value * share);
        std::vector<std::pair<int, ResourceType>> kept;  // what survives, largest first
        for (ResourceType rt : AllResourceTypes())
        {
            const int n = (int)std::floor(s.ship->GetCargoAmount(rt) * share);
            if (n > 0)
                kept.push_back({ n, rt });
        }
        std::stable_sort(kept.begin(), kept.end(),
                         [](const auto& a, const auto& b) { return a.first > b.first; });
        if (worth >= 1.0 || !kept.empty())
        {
            const std::string pilot = s.ship->GetPilotName();
            const std::string name =
                "Wreck of " + (pilot.empty() ? std::string("a ") : pilot + "'s ") + hullName;
            const Vector2 at = s.ship->GetPosition();
            for (size_t i = 0; i < std::max<size_t>(1, kept.size()); i++)
            {
                auto wreck = std::make_unique<Derelict>(
                    Vector2{ at.x + 2.0f * SHIP_WRECK_SIZE * (float)i, at.y }, SHIP_WRECK_SIZE,
                    i == 0 ? name : "Cargo of " + name.substr(9), i == 0 ? worth : 0.0);
                if (i < kept.size())
                    wreck->SetCargo(kept[i].second, kept[i].first);
                wreck->SetExpiresAt(time_ + SHIP_WRECK_SECONDS);
                AddStatic(s.systemId, std::move(wreck), std::string());
            }
        }
    }

    // The hull leaves the hangar; the starter never does, and is the stock one again.
    if (known && hull != 0)
        s.ownedShips.erase(std::remove(s.ownedShips.begin(), s.ownedShips.end(), hull),
                           s.ownedShips.end());
    s.shipDesigns.erase(hull);
    if (!s.Owns(0))
        s.ownedShips.insert(s.ownedShips.begin(), 0);
    s.currentShip = 0;
    if (!catalog.empty())
    {
        ShipStats stats = catalog[0].stats;
        ShipStatsFor(s.DesignOf(0), stats);
        s.ship->Refit(stats);
    }
    s.ship->ClearCargo();
    s.ship->Repair();
    s.ship->DisengageAutopilot();
    s.ship->CancelWarp();
    s.ship->ReleaseHold();
    s.ship->SetMiningOn(false);
    s.ship->Stop();
    s.weaponOn = false;
    s.missions.ClearOffers();

    // Awake at home, docked, in whatever system that is.
    std::string homeSystem;
    const int   home = HomeOf(s, homeSystem);
    std::string wokeAt;
    if (home != 0)
    {
        SystemState& dest = systems_[homeSystem];
        if (!IsHot(dest))
            Orbits::Place(dest.entities, time_);  // where the clock has the station now
        s.systemId = homeSystem;
        Learn(s, homeSystem);  // where they wake is on their map (#431)
        Warm(dest);
        for (const auto& e : dest.entities)
            if (e->GetId() == home)
            {
                s.dockedStationId = home;
                s.dockBearing = { 0.0f, 1.0f };
                s.ship->Teleport(Sim::DockBerth(e->GetPosition(), e->GetSize(), s.dockBearing));
                wokeAt = e->GetName() + ", " + SystemName(homeSystem);
                break;
            }
        GenerateDockOffers(s);
    }
    else
        // Nowhere would take them: beside a station here that would not shoot (#224).
        s.ship->Teleport(SafeArrival(s.systemId, &s));

    ClientSession::ShipLoss& loss = s.lastLoss;
    loss.seq++;
    loss.hull = hullName;
    loss.system = where;
    loss.value = value;
    loss.paid = paid;
    loss.cargo = cargo;
    loss.wokeAt = wokeAt;

    std::string text = "Ship lost: your " + hullName + " was destroyed in " + where;
    if (value > 0.0)
        text += ", worth " + Credits(value);
    if (cargo > 0)
        text += ", with " + std::to_string(cargo) + " units of cargo";
    if (paid > 0.0)
        text += "; insurance paid " + Credits(paid);
    else if (value > 0.0)
        text += "; it was not insured";
    text += wokeAt.empty() ? ". You are in your " + starter
                           : ". You wake at " + wokeAt + " in your " + starter;
    s.RecordEvent(Ev::Kind::ShipDestroyed, text);
}

bool Simulation::InsureShip(ClientSession& s, int catalogIndex)
{
    const std::vector<ShipType>& catalog = GetShipCatalog();
    if (catalogIndex < 0 || catalogIndex >= (int)catalog.size())
        return false;
    const std::string name = catalog[catalogIndex].name;
    auto              refuse = [&](const std::string& why)
    {
        s.RecordEvent(Ev::Kind::Notice, "Insurance refused: " + why);
        return false;
    };
    const SystemState* st = SystemOf(s);
    if (!s.IsDocked() || st == nullptr)
        return refuse("insurance is bought docked at a station");
    if (!s.Owns(catalogIndex))
        return refuse("you do not own a " + name);
    const double value = ValueOf(s, catalogIndex, *st);
    const double cover = s.CoverOf(catalogIndex);
    if (value <= 0.0)
        return refuse("a stock " + name + " is free and needs no insurance");
    if (cover >= value)
        return refuse("the " + name + " is covered for " + Credits(cover) + " already");
    const double premium = std::ceil(InsurancePremium(value, cover));
    if (!s.account.CanAfford(premium))
        return refuse("it costs " + Credits(premium) + " and you have " +
                      Credits(s.account.GetMoney()));
    s.account.AddMoney(-premium);
    s.insurance[catalogIndex] = value;
    s.RecordEvent(Ev::Kind::Notice, "Insured the " + name + " for " + Credits(value) + " (pays " +
                                        Credits(InsurancePayout(value, value)) + " if lost) for " +
                                        Credits(premium));
    return true;
}

// --- Player as a server agent (M4d-2b) ---

ClientSession& Simulation::CreateSession(const std::string& systemId, Vector2 pos,
                                         const ShipStats& stats)
{
    const int      id = ++sessionIdCounter_;
    ClientSession& s = sessions_[id];
    s.id = id;
    s.ship = std::make_unique<Ship>(pos, stats);
    // From the same counter as stations and NPCs, so a player's ship can be named in a
    // snapshot and selected like anything else without colliding with the world (#4).
    s.ship->SetId(NextAgentId());
    s.systemId = systemId;
    s.ownedShips = { 0 };  // everyone starts owning the starter (#5)
    s.currentShip = 0;
    s.shipDesigns.clear();  // flying its stock design (#279 fitting)
    // Home, until they dock at a shipyard, is the trade hub of the start they began at (#344).
    for (const StartPoint& sp : starts_)
        if (sp.systemId == systemId)
            if (const SystemState* st = SystemById(systemId))
                for (const auto& e : st->entities)
                    if (e->GetKind() == EntityKind::Station && e->GetName() == sp.station)
                    {
                        s.homeStationId = e->GetId();
                        break;
                    }
    if (SystemState* st = SystemById(systemId))
        Warm(*st);  // a player is here now (#295)
    return s;
}

void Simulation::DestroySession(int id)
{
    sessions_.erase(id);
}

ClientSession* Simulation::Session(int id)
{
    auto it = sessions_.find(id);
    return it == sessions_.end() ? nullptr : &it->second;
}

const ClientSession* Simulation::Session(int id) const
{
    auto it = sessions_.find(id);
    return it == sessions_.end() ? nullptr : &it->second;
}

bool Simulation::BeginLinger(ClientSession& s)
{
    if (!s.ship || s.IsDocked())
        return false;
    s.lingerLeft = LINGER_SECONDS;
    return true;
}

std::vector<int> Simulation::StepLinger(float dt)
{
    std::vector<int> over;
    for (auto& kv : sessions_)
    {
        ClientSession& s = kv.second;
        if (!s.Lingering())
            continue;
        s.lingerLeft -= dt;
        if (s.lingerLeft <= 0.0f)
        {
            s.lingerLeft = 0.0f;  // still lingering until the host ends it
            over.push_back(s.id);
        }
    }
    return over;
}

bool Simulation::InCover(const ClientSession& s) const
{
    const SystemState* st = SystemOf(s);
    if (st == nullptr || !s.ship)
        return false;
    const Vector2 pp = s.ship->GetPosition();
    for (const auto& e : st->entities)
    {
        if (!e->Has(Component::Hazard) || !e->GetArchetype()->hazardHidesShips)
            continue;
        // radius 0 in the archetype means "as large as the object itself".
        const float r =
            e->GetArchetype()->hazardRadius > 0.0f ? e->GetArchetype()->hazardRadius : e->GetSize();
        const float dx = pp.x - e->GetPosition().x;
        const float dy = pp.y - e->GetPosition().y;
        if (dx * dx + dy * dy <= r * r)
            return true;
    }
    return false;
}

bool Simulation::GrantDebugMoney(ClientSession& s)
{
    if (!devCommands_)
        return false;
    s.account.AddMoney(1000.0);
    return true;
}

void Simulation::StepPlayerShip(ClientSession& s, const Proto::Command& cmd, float pilotBonus,
                                float dt)
{
    if (!s.ship)
        return;

    // A standing hold follows something that moves, so the step has to be told where that
    // something is now (#157). The server resolves it from the live entity; the client
    // resolves the same id from its own proxy.
    // Its velocity is the one StepPlayerAttachment measured on the last world tick (#298).
    Sim::HoldTarget        hold;
    const Sim::HoldTarget* holdPtr = nullptr;
    const int              holdId = s.ship->GetHoldTargetId();
    if (s.ship->GetHoldMode() != HoldMode::None && holdId != 0)
    {
        if (const SystemState* st = SystemOf(s))
            for (const auto& e : st->entities)
                if (e->GetId() == holdId)
                {
                    hold.pos = e->GetPosition();
                    if (s.holdTrackId == holdId)
                        hold.vel = s.holdTrackVel;
                    holdPtr = &hold;
                    break;
                }
        // A target that is gone -- destroyed, or in another system now -- releases the
        // hold rather than leaving the ship steering at a stale point forever.
        if (holdPtr == nullptr)
            s.ship->ReleaseHold();
    }

    // The step itself is shared with the client's prediction (sim/PlayerStep.h) — one
    // implementation, so predicted and authoritative movement cannot drift apart.
    Sim::StepPlayerShip(*s.ship, cmd, pilotBonus, dt, holdPtr);
}

bool Simulation::StepPlayerFire(ClientSession& s, SystemState& st, int targetId, float dt,
                                PlayerCombatEvents* ev)
{
    if (s.fireTimer > 0.0f)
        s.fireTimer -= dt;
    if (!s.weaponOn || !s.ship || targetId == 0)
        return false;

    // The target, by id, in the player's system: a ship, or something somebody built (#39).
    Entity* found = nullptr;
    for (auto& e : st.entities)
        if (e->GetId() == targetId)
        {
            found = e.get();
            break;
        }
    if (found == nullptr ||
        (found->GetKind() != EntityKind::Npc && found->GetKind() != EntityKind::Structure))
        return false;
    if (found->GetKind() == EntityKind::Npc && !static_cast<NpcShip*>(found)->IsAlive())
        return false;
    if (HiddenFrom(s, st, *found))
        return false;  // what a pilot has not found, they cannot aim at (#346, #415)

    const Vector2 shipPos = s.ship->GetPosition();
    const Vector2 at = found->GetPosition();
    const float   dx = at.x - shipPos.x;
    const float   dy = at.y - shipPos.y;
    // A structure is hit at its edge: an outpost is far larger than any ship.
    const float reach =
        PLAYER_WEAPON_RANGE + (found->GetKind() == EntityKind::Structure ? found->GetSize() : 0.0f);
    if (std::sqrt(dx * dx + dy * dy) > reach || s.fireTimer > 0.0f)
        return false;
    s.fireTimer = PLAYER_FIRE_INTERVAL;
    if (ev != nullptr)
    {
        ev->shotFrom = shipPos;
        ev->shotTo = at;
    }

    // A structure: its rules are construction's (Simulation_Build.cpp), and what the shot
    // costs the shooter is decided by whose law it stands under, not by its owner's colour.
    if (found->GetKind() == EntityKind::Structure)
    {
        FactionId law = Factions::Unaffiliated();
        if (IsCrime(st, s, *found, law) && ev != nullptr)
        {
            ev->hitLawful = true;
            ev->hitFaction = law;
        }
        if (DamageStructure(st, targetId, s.ship->GetDamage(), &s) && ev != nullptr &&
            ev->hitLawful)
        {
            ev->killedLawful = true;
            ev->killedFaction = law;
        }
        return true;
    }

    NpcShip* target = static_cast<NpcShip*>(found);
    target->TakeDamage(s.ship->GetDamage());

    // Account effects: attacking a lawful target is a crime; a kill is mission credit
    // (pirate) or a serious crime (lawful). Applied to the session's account, which is the
    // only one there is. `ev` is filled as well, because the client draws the consequences
    // -- a beam, a flash, a notice -- from facts rather than by recomputing them.
    FactionId  tf = target->GetFaction();
    const bool crime = IsCrime(st, s, *target, tf);
    if (crime)
    {
        if (ev != nullptr)
        {
            ev->hitLawful = true;
            ev->hitFaction = tf;
        }
        ChargeAttack(s, tf, false);
        CallPolice(st, s, tf);  // ...and the law comes, from real forces (#431 slice 9)
    }
    if (!target->IsAlive())
    {
        CreditJobKill(s, *target);  // a besieger, for a break-the-siege job (#345)
        if (target->IsPirate())
        {
            if (ev != nullptr)
                ev->killedPirate = true;
            // Mission credit for the pirate, into this session's mission log.
            s.missions.OnPirateKilled();
        }
        else if (crime)
        {
            if (ev != nullptr)
            {
                ev->killedLawful = true;
                ev->killedFaction = tf;
            }
            ChargeAttack(s, tf, true);
        }
    }
    return true;
}

void Simulation::ChargeAttack(ClientSession& s, FactionId victim, bool killed)
{
    if (killed)
    {
        s.account.AddReputation(victim, -Crime::KILL_STANDING);
        s.account.AddBounty(victim, Crime::KILL_BOUNTY);
        return;
    }
    s.account.AddReputation(victim, -Crime::HIT_STANDING);
    s.account.AddBounty(victim, Crime::HIT_BOUNTY);
}

Simulation::PlayerMiningResult Simulation::StepPlayerMining(ClientSession& s, SystemState& st,
                                                            float miningBonus, float dt)
{
    PlayerMiningResult r;
    if (!s.ship || !s.ship->IsMiningOn() || s.ship->GetCargoUsed() >= s.ship->GetCargoCapacity())
        return r;

    Vector2 sp = s.ship->GetPosition();
    for (auto& e : st.entities)
    {
        // Asks what can be mined, and from how far, rather than what is an AsteroidField
        // (#34). Reading the ore itself still needs the class: how much is left is
        // per-instance state, and moving that off the class is the last step of #34, not
        // this one.
        if (!e->Has(Component::Mineable))
            continue;
        AsteroidField* field =
            e->GetKind() == EntityKind::Field ? static_cast<AsteroidField*>(e.get()) : nullptr;
        if (field == nullptr || !field->HasOre())
            continue;

        float dx = field->GetPosition().x - sp.x;
        float dy = field->GetPosition().y - sp.y;
        if (std::sqrt(dx * dx + dy * dy) > field->GetSize() + e->GetArchetype()->extractRange)
            continue;

        r.fieldId = field->GetId();

        // The ship sets the pace, the deposit how rich it is (#193), and the mining skill
        // (passed as a multiplier) speeds both up.
        float rate = s.ship->GetStats().miningRate * e->GetArchetype()->extractRate * miningBonus;
        s.miningProgress += rate * dt;
        while (s.miningProgress >= 1.0f)
        {
            s.miningProgress -= 1.0f;
            int got = field->Extract(1);
            if (got <= 0 || !s.ship->AddCargo(field->GetResource(), got))
                break;
            r.minedUnits += got;
        }
        break;
    }
    // Mining xp into this session's account. The client sees the new total in the next
    // snapshot rather than crediting a copy of its own.
    if (r.minedUnits > 0)
        s.account.GetSkills().AddXp(SkillType::Mining, 3.0f * (float)r.minedUnits);
    return r;
}

std::string Simulation::JumpGateDestIfNear(const ClientSession& s, SystemState& st,
                                           int gateId) const
{
    if (!s.ship || gateId == 0)
        return std::string();
    for (const auto& e : st.entities)
        if (e->GetId() == gateId)
        {
            // Anything that links somewhere, at whatever distance it says it works from
            // (#34). Where it leads stays on the instance -- two gates of one kind lead
            // to different places, which is why that is not archetype data.
            if (!e->Has(Component::JumpLink))
                return std::string();
            const JumpGate* g = static_cast<const JumpGate*>(e.get());
            Vector2         sp = s.ship->GetPosition();
            float           dx = g->GetPosition().x - sp.x;
            float           dy = g->GetPosition().y - sp.y;
            if (std::sqrt(dx * dx + dy * dy) > g->GetSize() + e->GetArchetype()->jumpRange)
                return std::string();
            // An unstable mouth lets through only small ships (#342 S4), and so does a roaming
            // hole of its class (S9) -- which a pilot who has not found it cannot use at all,
            // whatever id their client names (#346).
            if (!MouthProblem(s, *e).empty() || HiddenFrom(s, st, *e))
                return std::string();
            return g->GetDestination();
        }
    return std::string();
}

double Simulation::StepPlayerLoot(ClientSession& s, SystemState& st, int derelictId)
{
    if (!s.ship || derelictId == 0)
        return 0.0;
    for (auto& e : st.entities)
        if (e->GetId() == derelictId)
        {
            // Salvageable, from the distance it declares (#34). What it pays out and
            // whether it has been taken already are per-instance.
            if (!e->Has(Component::Salvageable))
                return 0.0;
            Derelict* dr =
                e->GetKind() == EntityKind::Derelict ? static_cast<Derelict*>(e.get()) : nullptr;
            if (dr == nullptr || dr->IsLooted())
                return 0.0;
            Vector2 sp = s.ship->GetPosition();
            float   dx = dr->GetPosition().x - sp.x;
            float   dy = dr->GetPosition().y - sp.y;
            if (std::sqrt(dx * dx + dy * dy) > dr->GetSize() + e->GetArchetype()->salvageRange)
                return 0.0;
            // What a hauler carried (#343): as much as the hold has room for. What does not fit
            // stays for the next search, and the wreck is searched once it is empty.
            if (dr->GetCargoAmount() > 0)
            {
                const int room = s.ship->GetCargoCapacity() - s.ship->GetCargoUsed();
                const int took = std::min(room, dr->GetCargoAmount());
                if (took <= 0)
                {
                    s.RecordEvent(Ev::Kind::Notice, "The hold is full: the wreck's " +
                                                        ResourceName(dr->GetCargo()) +
                                                        " stays where it is");
                    return 0.0;
                }
                s.ship->AddCargo(dr->GetCargo(), took);
                dr->SetCargo(dr->GetCargo(), dr->GetCargoAmount() - took);
                s.RecordEvent(Ev::Kind::Notice, "Took " + std::to_string(took) + " " +
                                                    ResourceName(dr->GetCargo()) +
                                                    " from the wreck");
                if (dr->GetCargoAmount() > 0)
                {
                    MarkStaticChanged(st, derelictId);
                    return 0.0;
                }
            }
            dr->SetLooted();
            // Everyone else in the system sees it searched now, not on their next visit
            // -- and nobody is offered a wreck the server will refuse (#38).
            MarkStaticChanged(st, derelictId);
            double reward = dr->GetReward();
            s.account.AddMoney(reward);
            return reward;
        }
    return 0.0;
}

// Whether the session is docked somewhere that trades, and whose it is. Every station has a
// market today, so this refuses nothing yet -- it starts refusing the moment a player builds
// a dock that is not also a market (#44), which is the point of the component being real
// (#34). `feeTo` is the faction a trade's fee goes to (#343): the station's, or the faction
// that built the structure; false for a market a player built, whose fee leaves the economy
// until owners are paid (#44).
static bool DockedMarket(const ClientSession& s, const SystemState& st, FactionId& owner,
                         bool* feeTo = nullptr)
{
    owner = Factions::Unaffiliated();
    if (feeTo != nullptr)
        *feeTo = false;
    for (const auto& e : st.entities)
        if (e->GetId() == s.dockedStationId)
        {
            bool faction = false;
            if (e->GetKind() == EntityKind::Station)
            {
                owner = static_cast<const Station&>(*e).GetFaction();
                faction = true;
            }
            else if (!(faction = Outposts::FactionOf(e->GetOwner(), owner)))
                owner = Factions::Unaffiliated();
            if (feeTo != nullptr)
                *feeTo = faction;
            return e->Has(Component::Market);
        }
    return false;
}

Simulation::PlayerSellResult Simulation::StepPlayerSell(ClientSession& s, SystemState& st,
                                                        int resourceType, int amount)
{
    PlayerSellResult r;
    FactionId        sf;
    bool             feeTo = false;
    if (!s.ship || resourceType < 0 || resourceType >= Goods::Count() ||
        !DockedMarket(s, st, sf, &feeTo))
        return r;

    const ResourceType type = ResourceType::FromIndex(resourceType);
    const int          have = s.ship->GetCargoAmount(type);
    const int          sold = amount < have ? amount : have;
    if (sold <= 0)
        return r;
    // What the units fetch at the bid, each lowering the price of the next (#343) -- the ask
    // read before the sale moves the stock, for the cap below.
    const double ask = st.market.Ask(type);
    r.gross = st.market.Sell(type, sold);
    s.ship->RemoveCargo(type, sold);
    r.sold = sold;

    // The trading skill and the station's owner's opinion raise what it pays -- never above
    // what it asked a moment ago, so a round trip at one market always loses (#343).
    const float  sellMul = SellPriceMultiplier(Factions::TierOf(s.account.GetReputation(sf)));
    const double paid = std::min(
        r.gross * s.account.GetSkills().GetBonus(SkillType::Trading) * sellMul, ask * sold);
    // Everybody pays the fee, human or agent (#348), and it is the market's owner's (#343).
    r.fee = paid * Goods::Rules().fee;
    if (feeTo)
        minds_[sf].treasury += r.fee;
    const double revenue = paid - r.fee;
    s.account.AddMoney(revenue);
    s.account.GetSkills().AddXp(SkillType::Trading, (float)(revenue * 0.05));
    s.account.AddReputation(sf, (float)(revenue * 0.002));
    r.revenue = revenue;
    return r;
}

Simulation::PlayerBuyResult Simulation::StepPlayerBuy(ClientSession& s, SystemState& st,
                                                      int resourceType, int amount)
{
    PlayerBuyResult r;
    FactionId       sf;
    bool            feeTo = false;
    if (!s.ship || amount <= 0 || resourceType < 0 || resourceType >= Goods::Count())
        return r;
    const ResourceType type = ResourceType::FromIndex(resourceType);
    const std::string  name = ResourceName(type);
    if (!DockedMarket(s, st, sf, &feeTo))
    {
        s.RecordEvent(Ev::Kind::Notice, "Buying " + name + " refused: no market here");
        return r;
    }
    // A station that thinks badly of a pilot charges more; one that likes them does not
    // charge less -- the spread is the market's living (#343).
    const double markup =
        1.0 / std::min(1.0f, SellPriceMultiplier(Factions::TierOf(s.account.GetReputation(sf))));
    const double fee = Goods::Rules().fee;
    // As many as the stock, the hold and the money allow: counted down one at a time, since
    // every unit bought raises the price of the next.
    const int room = s.ship->GetCargoCapacity() - s.ship->GetCargoUsed();
    const int stock = (int)st.market.Stock(type);
    int       n = std::min({ amount, room, stock });
    while (n > 0 && st.market.QuoteBuy(type, n) * markup * (1.0 + fee) > s.account.GetMoney())
        n--;
    if (n <= 0)
    {
        const char* why = room <= 0    ? "the hold is full"
                          : stock <= 0 ? "the market has none"
                                       : "not enough money for one";
        s.RecordEvent(Ev::Kind::Notice, "Buying " + name + " refused: " + why);
        return r;
    }
    int          bought = 0;
    const double price = st.market.Buy(type, n, bought) * markup;
    r.fee = price * fee;
    r.cost = price + r.fee;
    if (feeTo)
        minds_[sf].treasury += r.fee;
    r.bought = bought;
    s.account.AddMoney(-r.cost);
    s.ship->AddCargo(type, bought);
    s.account.GetSkills().AddXp(SkillType::Trading, (float)(price * 0.01));
    return r;
}

// A hull whose hold is smaller than what the ship carries is refused, not refitted (#219).
// Refit swaps stats and leaves the cargo alone, so without this a full Hauler became a Scout
// carrying four times its capacity. Refusing keeps "a hold never holds more than it can"
// true without deciding for the player which ore to throw away.
static bool HoldFits(ClientSession& s, const std::string& name, const ShipStats& stats,
                     const std::string& verb)
{
    const int used = s.ship->GetCargoUsed();
    if (used <= stats.cargoCapacity)
        return true;
    s.RecordEvent(Ev::Kind::Notice, verb + " refused: the hold carries " + std::to_string(used) +
                                        " and a " + name + " holds " +
                                        std::to_string(stats.cargoCapacity) +
                                        "; sell or hand in cargo first");
    return false;
}

bool Simulation::SwitchShip(ClientSession& s, int catalogIndex)
{
    const std::vector<ShipType>& catalog = GetShipCatalog();
    if (!s.ship || catalogIndex < 0 || catalogIndex >= (int)catalog.size())
        return false;
    const ShipType& t = catalog[catalogIndex];
    if (!s.Owns(catalogIndex))
    {
        // Not theirs; the client asking nicely does not make it so.
        s.RecordEvent(Ev::Kind::Notice, "Switch to " + t.name + " refused: not in your hangar");
        return false;
    }
    if (catalogIndex == s.currentShip)
        return true;
    // The ship in that berth, as it was last fitted (#279 fitting).
    ShipStats stats = t.stats;
    ShipStatsFor(s.DesignOf(catalogIndex), stats);
    if (!HoldFits(s, t.name, stats, "Switch to " + t.name))
        return false;
    s.ship->Refit(stats);
    s.currentShip = catalogIndex;
    s.RecordEvent(Ev::Kind::Notice, "Now flying the " + t.name);
    return true;
}

bool Simulation::ShipStatsFor(const std::string& design, ShipStats& out) const
{
    const Ships::Design* d = designs_.Find(design);
    std::string          why;
    return d != nullptr && PlayerStats(*d, out, why);
}

void Simulation::FlyDesign(ClientSession& s, int hull, const std::string& design)
{
    s.shipDesigns[hull] = design;
    ShipStats stats;
    if (s.ship && hull == s.currentShip && ShipStatsFor(design, stats))
        s.ship->Refit(stats);
}

void Simulation::SaveDesigns(const std::string& path)
{
    std::set<std::string> live;
    for (const auto& [id, s] : sessions_)
    {
        for (const auto& [hull, design] : s.shipDesigns)
            live.insert(design);
        for (const auto& [name, design] : s.designLibrary)  // a saved design is kept (#279)
            live.insert(design);
    }
    designs_.Prune(live);
    designs_.Save(path);
}

Save::Result Simulation::LoadDesigns(const std::string& path)
{
    return designs_.Load(path);
}

std::vector<Proto::DesignDef> Simulation::DesignDefsFor(const Proto::Snapshot& snap,
                                                        std::set<std::string>& sent) const
{
    std::vector<Proto::DesignDef> out;
    auto                          need = [&](const std::string& id)
    {
        if (!designs_.Holds(id) || !sent.insert(id).second)
            return;
        Proto::DesignDef def;
        def.design = *designs_.Find(id);
        out.push_back(std::move(def));
    };
    for (const std::string& id : snap.player.shipDesigns)
        need(id);
    for (const Proto::SavedDesign& d : snap.player.library)  // what the Fit tab prices (#279)
        need(d.design);
    for (const Proto::EntitySnapshot& e : snap.entities)
        need(e.design);
    return out;
}

void Simulation::StepPlayerAccountTick(ClientSession& s, float dt)
{
    // Piloting xp accrues in flight (not docked); the wanted level decays slowly.
    if (!s.IsDocked())
        s.account.GetSkills().AddXp(SkillType::Piloting, 4.0f * dt);
    s.account.DecayBounty(1.0 * dt);
}

bool Simulation::PayBounty(ClientSession& s, FactionId faction)
{
    const double      b = s.account.GetBounty(faction);
    const std::string who = FactionName(faction);
    if (b <= 0.0)
    {
        s.RecordEvent(Ev::Kind::Notice, "Bounty not paid: " + who + " has no bounty on you");
        return false;
    }
    if (!s.account.CanAfford(b))
    {
        s.RecordEvent(Ev::Kind::Notice, "Bounty not paid: " + who + " wants " + Credits(b) +
                                            " and you have " + Credits(s.account.GetMoney()));
        return false;
    }
    s.account.AddMoney(-b);
    s.account.SetBounty(faction, 0.0);
    s.RecordEvent(Ev::Kind::Notice, "Bounty paid to " + who + ": record cleared");
    return true;
}

bool Simulation::BuyShip(ClientSession& s, int catalogIndex)
{
    const std::vector<ShipType>& catalog = GetShipCatalog();
    if (!s.ship || catalogIndex < 0 || catalogIndex >= (int)catalog.size())
        return false;
    const ShipType& t = catalog[catalogIndex];
    // A ship already in the hangar is switched to for nothing (SwitchShip). Buying it again
    // used to charge the full price a second time and hand back what the account already had.
    if (s.Owns(catalogIndex))
    {
        s.RecordEvent(Ev::Kind::Notice,
                      "Purchase refused: you already own a " + t.name + "; switch to it instead");
        return false;
    }
    // Checked before charging: a purchase that cannot be flown away is not a purchase.
    if (!HoldFits(s, t.name, t.stats, "Purchase of a " + t.name))
        return false;

    // Price multiplier by the docked station's faction reputation.
    FactionId sf = Factions::Unaffiliated();
    for (auto& e : SystemOf(s)->entities)
        if (e->GetId() == s.dockedStationId)
        {
            if (const Station* sta = e->GetKind() == EntityKind::Station
                                         ? static_cast<const Station*>(e.get())
                                         : nullptr)
                sf = sta->GetFaction();
            break;
        }
    const double price =
        t.price * ShipPriceMultiplier(Factions::TierOf(s.account.GetReputation(sf)));
    if (!s.account.CanAfford(price))
    {
        s.RecordEvent(Ev::Kind::Notice, "Purchase refused: a " + t.name + " costs " +
                                            Credits(price) + " here and you have " +
                                            Credits(s.account.GetMoney()));
        return false;
    }
    s.account.AddMoney(-price);
    s.ownedShips.push_back(catalogIndex);
    s.shipDesigns[catalogIndex] = t.stats.design;  // a new ship is the stock one
    s.ship->Refit(t.stats);
    s.currentShip = catalogIndex;
    s.RecordEvent(Ev::Kind::Notice,
                  "Bought a " + t.name + " for " + Credits(price) + "; now flying it");
    return true;
}

bool Simulation::RefitShip(ClientSession& s, const std::vector<Ships::FitLine>& fit)
{
    if (!s.ship)
        return false;
    const std::vector<ShipType>& catalog = GetShipCatalog();
    const int                    hull = s.currentShip;
    if (hull < 0 || hull >= (int)catalog.size())
        return false;
    const std::string name = catalog[hull].name;
    auto              refuse = [&](const std::string& why)
    {
        s.RecordEvent(Ev::Kind::Notice, "Refit refused: " + why);
        return false;
    };
    SystemState* st = SystemOf(s);
    if (!s.IsDocked() || st == nullptr)
        return refuse("dock at a station with a shipyard first");
    // A faction's station, or a player's finished structure grown into a shipyard (#432).
    const Entity* yard = nullptr;
    for (const auto& e : st->entities)
        if (e->GetId() == s.dockedStationId && (e->GetKind() == EntityKind::Station ||
                                                (e->GetKind() == EntityKind::Structure &&
                                                 !static_cast<const Structure&>(*e).IsBuilding())))
            yard = e.get();
    if (yard == nullptr || yard->GetArchetype() == nullptr ||
        !yard->GetArchetype()->Has(Component::Shipyard))
        return refuse("this station has no shipyard");

    const Ships::Catalogue& ships = Archetypes::ShipCatalogue();
    const Ships::Design*    stock = ships.FindDesign(catalog[hull].stats.design);
    const Ships::Design*    now = designs_.Find(s.DesignOf(hull));
    if (stock == nullptr || now == nullptr)
        return false;
    Ships::Design want;
    std::string   why;
    if (!Ships::PlayerFit(ships, *stock, fit, want, why))
        return refuse(why);
    if (want.id == now->id)
        return refuse("that is the fit the " + name + " has");
    ShipStats stats;
    if (!PlayerStats(want, stats, why))
        return refuse(why);
    if (!HoldFits(s, name, stats, "Refit of the " + name))
        return false;

    // At the system's ore prices, as the snapshot shows them, and by standing as a ship is.
    std::vector<float> ore;
    for (ResourceType rt : AllResourceTypes())
        ore.push_back((float)st->market.GetPrice(rt));
    double price = Ships::RefitPrice(ships, *now, want, ore, yard->GetArchetype()->refitMarkup);
    if (price > 0.0)
        price *= ShipPriceMultiplier(Factions::TierOf(s.account.GetReputation(
            yard->GetKind() == EntityKind::Station ? static_cast<const Station*>(yard)->GetFaction()
                                                   : Factions::Unaffiliated())));
    if (price > 0.0 && !s.account.CanAfford(price))
        return refuse("it costs " + Credits(price) + " here and you have " +
                      Credits(s.account.GetMoney()));

    std::string id = want.id;
    if (Ships::IsPlayerDesign(id))
    {
        id = designs_.File(want, why);
        if (id.empty())
            return refuse(why);
        designsChanged_ = true;
    }
    s.account.AddMoney(-price);
    FlyDesign(s, hull, id);
    s.RecordEvent(Ev::Kind::Notice,
                  "Refitted the " + name +
                      (price >= 0.0 ? " for " + Credits(price) : ", refunded " + Credits(-price)));
    return true;
}

bool Simulation::SaveDesign(ClientSession& s, const std::string& name,
                            const std::vector<Ships::FitLine>& fit)
{
    auto refuse = [&](const std::string& why)
    {
        s.RecordEvent(Ev::Kind::Notice, "Design not saved: " + why);
        return false;
    };
    const std::vector<ShipType>& catalog = GetShipCatalog();
    const int                    hull = s.currentShip;
    if (!s.ship || hull < 0 || hull >= (int)catalog.size())
        return false;
    std::string why;
    if (!Ships::ValidDesignName(name, why))
        return refuse(why);
    const bool replaces = s.designLibrary.count(name) != 0;
    if (!replaces && (int)s.designLibrary.size() >= Ships::LIBRARY_MAX)
        return refuse("the library holds " + std::to_string(Ships::LIBRARY_MAX) +
                      " designs; delete one first");
    const Ships::Catalogue& ships = Archetypes::ShipCatalogue();
    const Ships::Design*    stock = ships.FindDesign(catalog[hull].stats.design);
    if (stock == nullptr)
        return false;
    // The same rules as a refit: a design that could not be flown is not one to keep.
    Ships::Design want;
    if (!Ships::PlayerFit(ships, *stock, fit, want, why))
        return refuse(why);
    std::string id = want.id;
    if (Ships::IsPlayerDesign(id))
    {
        id = designs_.File(want, why);
        if (id.empty())
            return refuse(why);
        designsChanged_ = true;
    }
    s.designLibrary[name] = id;
    s.RecordEvent(Ev::Kind::Notice, (replaces ? "Saved over design '" : "Saved design '") + name +
                                        "' for the " + catalog[hull].name);
    return true;
}

bool Simulation::ApplyDesign(ClientSession& s, const std::string& name)
{
    auto refuse = [&](const std::string& why)
    {
        s.RecordEvent(Ev::Kind::Notice, "Refit refused: " + why);
        return false;
    };
    const auto it = s.designLibrary.find(name);
    if (it == s.designLibrary.end())
        return refuse("there is no design called '" + name + "' in your library");
    const Ships::Design* d = designs_.Find(it->second);
    if (d == nullptr)
        return refuse("design '" + name + "' is no longer on file");
    const int                    hull = HullOf(it->second);
    const std::vector<ShipType>& catalog = GetShipCatalog();
    if (hull < 0 || hull >= (int)catalog.size())
        return refuse("design '" + name + "' is for no hull in the hangar");
    if (hull != s.currentShip)
        return refuse("design '" + name + "' is for the " + catalog[hull].name +
                      "; switch to one first");
    // Its fit, as a player would send it: one price and one set of rules for both (#109).
    return RefitShip(s, Ships::PlayerLines(*d));
}

bool Simulation::RenameDesign(ClientSession& s, const std::string& from, const std::string& to)
{
    auto refuse = [&](const std::string& why)
    {
        s.RecordEvent(Ev::Kind::Notice, "Design not renamed: " + why);
        return false;
    };
    const auto it = s.designLibrary.find(from);
    if (it == s.designLibrary.end())
        return refuse("there is no design called '" + from + "' in your library");
    std::string why;
    if (!Ships::ValidDesignName(to, why))
        return refuse(why);
    if (to == from)
        return true;
    if (s.designLibrary.count(to) != 0)
        return refuse("you already have a design called '" + to + "'");
    const std::string id = it->second;
    s.designLibrary.erase(it);
    s.designLibrary[to] = id;
    s.RecordEvent(Ev::Kind::Notice, "Renamed design '" + from + "' to '" + to + "'");
    return true;
}

bool Simulation::DeleteDesign(ClientSession& s, const std::string& name)
{
    if (s.designLibrary.erase(name) == 0)
    {
        s.RecordEvent(Ev::Kind::Notice, "Design not deleted: there is no design called '" + name +
                                            "' in your library");
        return false;
    }
    s.RecordEvent(Ev::Kind::Notice, "Deleted design '" + name + "'");
    return true;
}

bool Simulation::AccountHostileToFaction(const ClientSession& s, FactionId f) const
{
    if (f == Factions::Pirates())
        return true;
    if (s.account.IsWanted(f))
        return true;
    RepTier t = Factions::TierOf(s.account.GetReputation(f));
    return t == RepTier::Hostile || t == RepTier::Hated;
}

void Simulation::GenerateDockOffers(ClientSession& s)
{
    Station*              giver = nullptr;
    std::vector<Station*> all;
    for (auto& e : SystemOf(s)->entities)
        if (Station* sta =
                e->GetKind() == EntityKind::Station ? static_cast<Station*>(e.get()) : nullptr)
        {
            all.push_back(sta);
            if (sta->GetId() == s.dockedStationId)
                giver = sta;
        }
    if (giver == nullptr)
        return;

    // Reward multiplier by the station faction's reputation.
    const float repMul =
        MissionRewardMultiplier(Factions::TierOf(s.account.GetReputation(giver->GetFaction())));
    s.missions.GenerateOffers(giver, all, repMul);
    // The work the faction's plans want done goes first (#345): it is what the world around
    // the station is actually doing.
    std::vector<Mission> board = JobsPostedAt(s, giver->GetId());
    for (const Mission& m : s.missions.Offers())
        board.push_back(m);
    s.missions.SetOffers(std::move(board));
}

bool Simulation::MissionCompletableNow(const ClientSession& s, const Mission& m) const
{
    if (IsPlanJob(m.type))
        return PlanJobReadyNow(s, m);  // anywhere: the faction pays by wire (#345)
    if (s.dockedStationId == 0 || !s.ship)
        return false;
    switch (m.type)
    {
        case MissionType::Bounty:
            return m.giverStationId == s.dockedStationId && m.progress >= m.targetCount;
        case MissionType::Mining:
            return m.giverStationId == s.dockedStationId &&
                   s.ship->GetCargoAmount(m.resource) >= m.targetCount;
        case MissionType::Delivery: return m.destStationId == s.dockedStationId;
        case MissionType::Contract: return ContractReadyNow(s, m);
        case MissionType::Supply:
        case MissionType::Escort:
        case MissionType::Siege:
        case MissionType::Besiege: break;  // a plan's, above
    }
    return false;
}

bool Simulation::AcceptMission(ClientSession& s, int offerIndex)
{
    const std::vector<Mission>& offers = s.missions.Offers();
    if (offerIndex < 0 || offerIndex >= (int)offers.size())
    {
        s.RecordEvent(Ev::Kind::Notice, "Mission not taken: that offer is no longer on the board");
        return false;
    }
    const std::string what = offers[offerIndex].title + ": " + offers[offerIndex].description;
    if ((int)s.missions.Active().size() >= MissionSystem::MAX_ACTIVE)
    {
        s.RecordEvent(Ev::Kind::Notice, "Mission not taken: already carrying " +
                                            std::to_string(MissionSystem::MAX_ACTIVE) +
                                            " missions, the most one pilot may; hand one in first");
        return false;
    }
    // A market's contract is one pilot's (#343): off every board once taken.
    if (offers[offerIndex].type == MissionType::Contract && !TakeContract(s, offers[offerIndex]))
        return false;
    if (!s.missions.Accept(offerIndex))
        return false;
    s.RecordEvent(Ev::Kind::Notice, "Mission taken -- " + what);
    return true;
}

bool Simulation::CompleteMission(ClientSession& s, int activeIndex)
{
    std::vector<Mission>& active = s.missions.Active();
    if (activeIndex < 0 || activeIndex >= (int)active.size())
    {
        s.RecordEvent(Ev::Kind::Notice, "Hand-in refused: no such active mission");
        return false;
    }
    if (IsPlanJob(active[activeIndex].type))
        return CompleteJob(s, activeIndex);
    if (active[activeIndex].type == MissionType::Contract)
        return CompleteContract(s, activeIndex);
    const Mission&    m = active[activeIndex];
    const std::string what = m.title + ": " + m.description;
    if (!MissionCompletableNow(s, m))
    {
        s.RecordEvent(Ev::Kind::Notice,
                      "Hand-in refused -- " + what + " is not done, or is not handed in here");
        return false;
    }

    if (m.type == MissionType::Mining && s.ship)
        s.ship->RemoveCargo(m.resource, m.targetCount);
    const double reward = m.rewardMoney;
    s.account.AddMoney(m.rewardMoney);
    s.account.AddReputation(m.faction, m.rewardRep);
    active.erase(active.begin() + activeIndex);  // m dangles from here on
    s.RecordEvent(Ev::Kind::Notice, "Mission complete -- " + what + ", paid " + Credits(reward));
    return true;
}

int Simulation::StepPlayerDock(ClientSession& s, SystemState& st)
{
    if (!s.ship || s.ship->IsWarping())
        return 0;

    Vector2 pp = s.ship->GetPosition();
    // Asks what is dockable rather than what is a Station (#34): a player's outpost (#433)
    // joins this pass by declaring the component. A dock that refuses this pilot is passed
    // over for another in reach, and said only when none takes them.
    std::string refused;
    for (auto& e : st.entities)
    {
        if (!e->Has(Component::Dockable))
            continue;
        float dx = e->GetPosition().x - pp.x;
        float dy = e->GetPosition().y - pp.y;
        if (std::sqrt(dx * dx + dy * dy) > e->GetSize() + e->GetArchetype()->dockRange)
            continue;
        const std::string why = DockRefusal(s, *e);
        if (!why.empty())
        {
            if (refused.empty())
                refused = why;
            continue;
        }
        s.ship->DisengageAutopilot();
        s.ship->ReleaseHold();
        s.ship->Stop();
        s.dockedStationId = e->GetId();
        // Berthed on the side it came in from, and from now on carried there (#298).
        s.dockBearing = Sim::DockBearing(e->GetPosition(), pp);
        s.ship->Teleport(Sim::DockBerth(e->GetPosition(), e->GetSize(), s.dockBearing));
        s.RecordEvent(Ev::Kind::Docked, "Docked at " + e->GetName());
        // A shipyard is where a hull is issued, so it is where a pilot wakes when their ship
        // is lost (#344): the last one they docked at. A station's: a player's shipyard
        // (#432) refits, but issues no hull to wake in.
        if (e->GetKind() == EntityKind::Station && e->GetArchetype() != nullptr &&
            e->GetArchetype()->Has(Component::Shipyard) && s.homeStationId != e->GetId())
        {
            s.homeStationId = e->GetId();
            s.RecordEvent(Ev::Kind::Notice, e->GetName() +
                                                " is your home now: if your ship is lost, you "
                                                "wake here");
        }
        GenerateDockOffers(s);  // fresh station mission board (M4f-2)
        return s.dockedStationId;
    }
    if (!refused.empty())
        s.RecordEvent(Ev::Kind::Notice, "Docking denied: " + refused);
    return 0;
}

std::string Simulation::DockRefusal(const ClientSession& s, const Entity& e) const
{
    // Reputation admittance (M4f-4): at the Hated tier a faction's dock refuses -- a station's,
    // or a structure a faction built; Hostile still admits. Reputation is authoritative here
    // (s.account), and the player is told via the snapshot.
    FactionId  f = Factions::Unaffiliated();
    const bool faction = e.GetKind() == EntityKind::Station
                             ? (f = static_cast<const Station&>(e).GetFaction(), true)
                             : Outposts::FactionOf(e.GetOwner(), f);
    // A structure without a market (#433) is a private dock: its owner's, and the owner's
    // corporation's (#347). A faction's outpost takes no pilot at all; only a market is open.
    const std::string& owner = e.GetOwner();
    if (e.GetKind() == EntityKind::Structure && !owner.empty() && !e.Has(Component::Market))
    {
        if (!faction && !s.accountName.empty() &&
            (owner == s.accountName || corps_.SameCorp(owner, s.accountName)))
            return std::string();
        return e.GetName() + (faction ? " is " + FactionName(f) + "'s" : " is " + owner + "'s") +
               ", and docks only its owner's ships";
    }
    if (faction && Factions::TierOf(s.account.GetReputation(f)) == RepTier::Hated)
        return "hostile reputation";
    return std::string();
}

void Simulation::StepPlayerUndock(ClientSession& s)
{
    if (s.dockedStationId == 0)
        return;
    // Out at the berth beside where the station is now, at rest -- not wherever the ship
    // was when it docked, which an orbiting station left long ago (#298).
    if (s.ship)
        if (const SystemState* st = SystemOf(s))
            for (const auto& e : st->entities)
                if (e->GetId() == s.dockedStationId)
                {
                    s.ship->Teleport(Sim::DockBerth(e->GetPosition(), e->GetSize(), s.dockBearing));
                    break;
                }
    s.RecordEvent(Ev::Kind::Undocked, "Undocked");
    s.dockedStationId = 0;
}

void Simulation::StepPlayerAttachment(ClientSession& s, float dt)
{
    if (!s.ship || dt <= 0.0f)
        return;
    const SystemState* st = SystemOf(s);
    if (st == nullptr)
        return;

    if (s.IsDocked())
    {
        for (const auto& e : st->entities)
            if (e->GetId() == s.dockedStationId)
            {
                const Vector2 berth = Sim::DockBerth(e->GetPosition(), e->GetSize(), s.dockBearing);
                const Vector2 was = s.ship->GetPosition();
                s.ship->Carry(berth, { (berth.x - was.x) / dt, (berth.y - was.y) / dt });
                break;
            }
        return;
    }

    // Measured here, once a world tick, rather than in the player step: commands come in
    // bursts, and two steps inside one tick would see the target standing still.
    const int     id = s.ship->GetHoldMode() != HoldMode::None ? s.ship->GetHoldTargetId() : 0;
    const Entity* target = nullptr;
    if (id != 0)
        for (const auto& e : st->entities)
            if (e->GetId() == id)
            {
                target = e.get();
                break;
            }
    if (target == nullptr)
    {
        s.holdTrackId = 0;
        s.holdTrackVel = { 0.0f, 0.0f };
        return;
    }
    const Vector2 now = target->GetPosition();
    if (s.holdTrackId == id)
        s.holdTrackVel = { (now.x - s.holdTrackPos.x) / dt, (now.y - s.holdTrackPos.y) / dt };
    else
        s.holdTrackVel = { 0.0f, 0.0f };  // first sight: one tick before it can be measured
    s.holdTrackId = id;
    s.holdTrackPos = now;
}

void Simulation::ServerEnterSystem(ClientSession& s, const std::string& destId, std::string fromId)
{
    // fromId is taken by value: every caller passes s.systemId, which the next line
    // overwrites. Held by reference it read as the destination, no gate there leads to
    // itself, and every jump ended beside a station or in open space instead (#310).
    if (!HasSystem(destId))
        return;
    s.systemId = destId;
    if (!s.ship)
        return;

    // A cold system's bodies were placed at its last coarse pass; where the ship arrives
    // is reckoned from where they are now. Its ships are made once the arrival is known, so
    // none is dropped on top of the player (#295).
    SystemState& dest = systems_[destId];
    if (!IsHot(dest))
        Orbits::Place(dest.entities, time_);
    float heading = s.ship->GetHeading();
    s.ship->Teleport(ArrivalFrom(destId, fromId, &s, &heading));
    s.ship->SetHeading(heading);
    s.ship->DisengageAutopilot();
    s.ship->CancelWarp();
    s.dockedStationId = 0;     // the jump releases the docking
    s.missions.ClearOffers();  // clear the board of the station we left; active missions
                               // survive the jump (they address stations by id) — otherwise
                               // Bounty/Delivery into another system would be uncompletable
    Warm(dest);

    // On the pilot's own charts from now on (#431 K1) -- and their corporation's -- but on
    // nobody else's: it becomes public only when a faction charts it.
    Learn(s, destId);
    // The first ship into a system beyond the wormhole (#143): that it was reached at all
    // is news, and its discoverer may name it (#145).
    SystemAggregate& agg = dest.agg;
    if (!agg.publicChart && agg.discoverer.empty())
    {
        agg.discoverer = s.ship ? s.ship->GetPilotName() : std::string();
        chartsChanged_ = true;  // world state: written at once
        PushEvent("First ship into " + SystemName(destId));
        s.RecordEvent(Ev::Kind::Notice, "First ship ever into " + SystemName(destId));
    }
}

Vector2 Simulation::ArrivalFrom(const std::string& destId, const std::string& fromId,
                                const ClientSession* who, float* heading) const
{
    // Beside the gate that leads back to where the ship came from -- the wormhole, for the
    // link between home and the region, is such a gate too -- on the side towards the middle
    // of the system, clear of the gate, and facing away from it. Gates do not move, so the
    // position is the one in the layout every client has.
    const SystemState* sys = SystemById(destId);
    if (sys != nullptr && !fromId.empty())
        for (const auto& e : sys->entities)
            if (e->GetKind() == EntityKind::Gate &&
                static_cast<const JumpGate&>(*e).GetDestination() == fromId)
            {
                const Vector2 gp = e->GetPosition();
                const float   d = std::sqrt(gp.x * gp.x + gp.y * gp.y);
                // Towards the centre; a gate at the very centre is left along +y.
                const Vector2 dir =
                    d > 1.0f ? Vector2{ -gp.x / d, -gp.y / d } : Vector2{ 0.0f, 1.0f };
                const float off = e->GetSize() + ARRIVAL_CLEARANCE;
                if (heading != nullptr)
                    *heading = std::atan2(dir.y, dir.x);
                return { gp.x + dir.x * off, gp.y + dir.y * off };
            }
    return SafeArrival(destId, who);
}

Vector2 Simulation::SafeArrival(const std::string& systemId, const ClientSession* who) const
{
    const SystemState* sys = SystemById(systemId);
    // A station whose owner counts this player an enemy refuses them and, if it is armed,
    // shoots (#193): arriving beside it is respawning into fire (#224).
    auto takesUs = [&](const Entity& e)
    {
        return who == nullptr ||
               !AccountHostileToFaction(*who, static_cast<const Station&>(e).GetFaction());
    };
    if (sys != nullptr)
        for (const auto& e : sys->entities)
            if (e->GetKind() == EntityKind::Station && takesUs(*e))
            {
                const Archetype* a = e->GetArchetype();
                const float      reach = a != nullptr ? a->dockRange : 0.0f;
                const Vector2    p = e->GetPosition();
                return { p.x, p.y + e->GetSize() + reach * 0.5f };
            }
    // No station -- a system beyond the wormhole has none (#140): clear of the star, which
    // is a hundred and fifty thousand units across, not a fixed distance that is inside it.
    float clear = World::SYSTEM_RADIUS * 0.1f;
    if (sys != nullptr)
        for (const auto& e : sys->entities)
            if (e->GetKind() == EntityKind::Star)
            {
                // A binary's stars stand either side of the middle (#142): clear of the
                // furthest edge of any of them.
                const Vector2 s = e->GetPosition();
                clear = std::max(clear, sqrtf(s.x * s.x + s.y * s.y) + e->GetSize() + 60000.0f);
            }
    // And out of reach of every gun that would fire: the first of eight bearings at that
    // distance that no hostile defensive station, or armed faction outpost (#39), covers.
    auto armedAgainstUs = [&](const Entity& e)
    {
        if (e.GetKind() == EntityKind::Station)
            return !takesUs(e);
        FactionId f;
        return e.GetKind() == EntityKind::Structure && who != nullptr &&
               !static_cast<const Structure&>(e).IsBuilding() &&
               Outposts::FactionOf(e.GetOwner(), f) && AccountHostileToFaction(*who, f);
    };
    auto underFire = [&](Vector2 at)
    {
        if (sys == nullptr)
            return false;
        for (const auto& e : sys->entities)
            if (e->Has(Component::Defensive) && armedAgainstUs(*e))
            {
                const Archetype* a = e->GetArchetype();
                const float      reach = e->GetSize() + (a != nullptr ? a->weaponRange : 0.0f);
                if (Vector2Distance(at, e->GetPosition()) < reach + 20000.0f)
                    return true;
            }
        return false;
    };
    static const Vector2 BEARINGS[] = { { 0.0f, 1.0f },       { 1.0f, 0.0f },
                                        { 0.0f, -1.0f },      { -1.0f, 0.0f },
                                        { 0.707f, 0.707f },   { 0.707f, -0.707f },
                                        { -0.707f, -0.707f }, { -0.707f, 0.707f } };
    for (const Vector2& b : BEARINGS)
    {
        const Vector2 at = { b.x * clear, b.y * clear };
        if (!underFire(at))
            return at;
    }
    return { 0.0f, clear };
}

bool Simulation::SaveAccount(const ClientSession& s, const std::string& path)
{
    using nlohmann::json;
    json j;
    j["version"] = Save::ACCOUNT_VERSION;
    // The world it was saved in (#392), so another world does not read its place as one of
    // its own. A field an older build ignores, so no version bump (#20).
    if (!worldId_.empty())
        j["world"] = worldId_;
    j["money"] = s.account.GetMoney();

    // By faction id (#355): an index is this process's registry order, which a later one --
    // with a region's natives added -- does not share.
    json rep = json::object(), bounty = json::object();
    for (FactionId f : Factions::All())
    {
        rep[Factions::Id(f)] = s.account.GetReputation(f);
        bounty[Factions::Id(f)] = s.account.GetBounty(f);
    }
    j["reputation"] = rep;
    j["bounty"] = bounty;

    const Skills& sk = s.account.GetSkills();
    j["skills"] = { { "piloting", sk.GetXp(SkillType::Piloting) },
                    { "mining", sk.GetXp(SkillType::Mining) },
                    { "trading", sk.GetXp(SkillType::Trading) } };

    // Where the player was and what they were carrying (#49). Until sessions existed the
    // one player never went away, so losing this on disconnect was invisible; with a
    // session per connection it is the first thing a returning player notices.
    if (s.ship)
    {
        j["place"] = { { "system", s.systemId },
                       { "pos", json::array({ s.ship->GetPosition().x, s.ship->GetPosition().y }) },
                       { "heading", s.ship->GetHeading() } };
        // And where that was relative to the nearest body, because bodies move (#210): a
        // ship left beside a station that orbits a planet came back further from it every
        // time, since the station had gone on round and the ship had not (#258).
        if (const SystemState* sys = SystemById(s.systemId))
        {
            const Vector2 at = s.ship->GetPosition();
            const Entity* best = nullptr;
            float         bestD = NEAR_BODY_RANGE;
            for (const auto& e : sys->entities)
            {
                const EntityKind k = e->GetKind();
                if (k == EntityKind::Npc || k == EntityKind::PlayerShip || k == EntityKind::Star)
                    continue;
                const float d = Vector2Distance(at, e->GetPosition()) - e->GetSize();
                if (d < bestD)
                {
                    bestD = d;
                    best = e.get();
                }
            }
            if (best != nullptr)
            {
                // Named by kind, name and which of that name it is: planets share theirs.
                int nth = 0;
                for (const auto& e : sys->entities)
                {
                    if (e.get() == best)
                        break;
                    if (e->GetKind() == best->GetKind() && e->GetName() == best->GetName())
                        nth++;
                }
                const Vector2 o = { at.x - best->GetPosition().x, at.y - best->GetPosition().y };
                j["place"]["near"] = { { "kind", (int)best->GetKind() },
                                       { "name", best->GetName() },
                                       { "nth", nth },
                                       { "offset", json::array({ o.x, o.y }) } };
            }
        }

        json cargo = json::array();
        for (ResourceType r : AllResourceTypes())
            if (int n = s.ship->GetCargoAmount(r))
                cargo.push_back({ { "resource", Goods::Id(r) }, { "amount", n } });
        j["cargo"] = cargo;
    }

    if (!s.authStored.empty())
        j["auth"] = { { "salt", s.authSalt }, { "stored", s.authStored } };

    // Each ship as the design it flies (#279 fitting). The design itself is in designs.json,
    // which is told this account names it, so it is kept for as long as the file says so.
    json                  ships = json::array();
    std::set<std::string> named;
    for (int hull : s.ownedShips)
    {
        const std::string design = s.DesignOf(hull);
        ships.push_back({ { "hull", hull }, { "design", design } });
        named.insert(design);
    }
    j["ships"] = ships;
    j["ship"] = s.currentShip;
    // Insurance and home (#344): fields an older build ignores, so no version bump (#20).
    json insurance = json::array();
    for (const auto& [hull, cover] : s.insurance)
        if (s.Owns(hull))
            insurance.push_back({ { "hull", hull }, { "cover", cover } });
    j["insurance"] = insurance;
    j["home"] = StationRefOf(systems_, s.homeStationId);
    // The library (#279 fitting, step 3): names of the account's, designs by id, kept in
    // designs.json for as long as a save names them. A field an older build ignores, so no
    // version bump (#20).
    json library = json::array();
    for (const auto& [name, design] : s.designLibrary)
    {
        library.push_back({ { "name", name }, { "design", design } });
        named.insert(design);
    }
    j["designs"] = library;
    // What this pilot has found (#346), by key, and the systems on their own charts (#431),
    // as "system:<id>". A field an older build ignores, so no version bump (#20); and an
    // older build reading it keeps no "system:" key -- it is no system's signature.
    j["scanned"] = json(s.known);
    if (!s.accountName.empty())
        designs_.SetUsers(s.accountName, named);

    // A mission addresses its stations by id while the server runs -- that is what lets
    // one survive a jump -- but an id is good for one run only (#227). The save names them
    // the way the data does, by system and station, and the load looks the ids up again.
    json missions = json::array();
    for (const Mission& m : s.missions.Active())
    {
        missions.push_back({ { "type", (int)m.type },
                             { "faction", Factions::Id(m.faction) },
                             { "title", m.title },
                             { "description", m.description },
                             { "giver", StationRefOf(systems_, m.giverStationId) },
                             { "dest", StationRefOf(systems_, m.destStationId) },
                             { "resource", Goods::Id(m.resource) },
                             { "target", m.targetCount },
                             { "progress", m.progress },
                             { "rewardMoney", m.rewardMoney },
                             { "rewardRep", m.rewardRep } });
        // A plan's job (#345): which plan -- its id is the world's and saved with it -- where,
        // and by when; an escort's convoy and whether the pilot has been with it.
        if (IsPlanJob(m.type))
        {
            json& mj = missions.back();
            mj["plan"] = m.planId;
            mj["system"] = m.systemId;
            mj["where"] = m.systemName;
            mj["deadline"] = m.deadline;
            mj["stage"] = m.stage;
            mj["rode"] = m.rode;
            if (IsSiegeJob(m.type))
                mj["against"] = Factions::Id(m.against);  // the other side (#407)
        }
        // A market's contract (#343): which -- the world keeps it -- where, and by when.
        if (m.type == MissionType::Contract)
        {
            json& mj = missions.back();
            mj["contract"] = m.contractId;
            mj["system"] = m.systemId;
            mj["where"] = m.systemName;
            mj["deadline"] = m.deadline;
        }
    }
    j["missions"] = missions;
    // How far the corporation's journal has been read (#431 C2). Additions an older build
    // ignores. The pending part of the journal is given at login, so nothing is left of it.
    if (!s.corpJournalOf.empty())
        j["corpJournal"] = { { "corp", s.corpJournalOf }, { "seen", s.corpJournalSeen } };

    // Atomically, with an older copy kept (#341): a truncated account file used to read as a
    // name nobody had taken, and the first person to log in under it claimed it.
    return Save::WriteFileAtomic(path, j.dump(2) + "\n", ACCOUNT_BACKUPS);
}

Save::Result Simulation::AppendPendingJournal(const std::string&            path,
                                              const std::vector<Ev::Event>& events)
{
    using nlohmann::json;
    std::ifstream in(path);
    if (!in.is_open())
        return Save::Result::Missing;
    json j = json::parse(in, nullptr, false);
    in.close();
    if (j.is_discarded() || !j.is_object())
        return Save::Result::Corrupt;
    if (j.value("version", Save::UNVERSIONED) > Save::ACCOUNT_VERSION)
        return Save::Result::TooNew;  // never written over (#20)
    json journal = j.value("journal", json::array());
    if (!journal.is_array())
        journal = json::array();
    for (const Ev::Event& e : events)
        journal.push_back({ { "kind", Ev::KindName(e.kind) }, { "text", e.text } });
    while (journal.size() > MAX_PENDING_JOURNAL)
        journal.erase(journal.begin());
    j["journal"] = journal;
    return Save::WriteFileAtomic(path, j.dump(2) + "\n", ACCOUNT_BACKUPS) ? Save::Result::Ok
                                                                          : Save::Result::Corrupt;
}

Save::Result Simulation::LoadAccount(ClientSession& s, const std::string& path)
{
    using nlohmann::json;
    std::ifstream in(path);
    if (!in.is_open())
        return Save::Result::Missing;
    json j = json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.is_object())
        return Save::Result::Corrupt;
    // Refused, not read: a later build may store money or cargo differently, and loading
    // it here would hand the player a plausible-looking wrong account -- then save it.
    const int version = j.value("version", Save::UNVERSIONED);
    if (version > Save::ACCOUNT_VERSION)
        return Save::Result::TooNew;

    s.account.SetMoney(j.value("money", 500.0));
    // A version 3 file and older lists them by the compiled index; a newer one by id.
    const auto perFaction = [&](const char* key, const auto& set)
    {
        const json v = j.value(key, json());
        FactionId  f;
        if (v.is_array())
            for (size_t i = 0; i < v.size(); i++)
            {
                if (v[i].is_number() && Factions::FromLegacyIndex((int)i, f))
                    set(f, v[i].get<double>());
            }
        else if (v.is_object())
            for (auto it = v.begin(); it != v.end(); ++it)
                if (it->is_number() && Factions::Find(it.key(), f))
                    set(f, it->get<double>());
    };
    perFaction("reputation", [&](FactionId f, double x) { s.account.SetReputation(f, (float)x); });
    perFaction("bounty", [&](FactionId f, double x) { s.account.SetBounty(f, x); });
    if (j.contains("skills"))
    {
        Skills& sk = s.account.GetSkills();
        sk.SetXp(SkillType::Piloting, (float)j["skills"].value("piloting", 0));
        sk.SetXp(SkillType::Mining, (float)j["skills"].value("mining", 0));
        sk.SetXp(SkillType::Trading, (float)j["skills"].value("trading", 0));
    }

    // An account from another world (#392) keeps what is the pilot's -- money, skills, ships
    // -- and leaves behind what was the other world's: where they were and the missions its
    // stations gave. Its system ids would name systems here, just not the same ones.
    const json stamp = j.value("world", json());
    const bool foreign = ForeignWorld(stamp.is_string() ? stamp.get<std::string>() : std::string());
    if (foreign)
        s.RecordEvent(Ev::Kind::Notice,
                      "You were last in another world; your place and missions there are gone");
    // What happened while they were away (#431 C2): the pending part of their journal, which
    // the host kept in this file, and what their corporation's journal has had since.
    if (!foreign && j.contains("journal") && j["journal"].is_array())
        for (const json& ej : j["journal"])
        {
            Ev::Kind kind;
            if (ej.is_object() && ej.contains("text") && ej["text"].is_string() &&
                Ev::KindFrom(ej.value("kind", std::string()), kind))
                s.RecordEvent(kind, ej["text"].get<std::string>());
        }
    const json cj = j.value("corpJournal", json());
    s.corpJournalOf = cj.is_object() ? cj.value("corp", std::string()) : std::string();
    s.corpJournalSeen = cj.is_object() ? cj.value("seen", 0) : 0;
    ReadCorpJournal(s);

    // What they found (#346): this world's, and only what is still there -- a hole that has
    // closed since is no longer a way anywhere.
    s.known.clear();
    if (!foreign && j.contains("scanned") && j["scanned"].is_array())
        for (const json& k : j["scanned"])
        {
            if (!k.is_string())
                continue;
            const std::string  key = k.get<std::string>();
            const bool         hole = key.rfind("hole:", 0) == 0;
            const RoamingHole* h = hole ? HoleById(key.substr(5)) : nullptr;
            // A system on their charts (#431) that is on everyone's now needs no key.
            const std::string chart = ChartedSystemOf(key);
            if (!chart.empty() ? HasSystem(chart) && !OnPublicCharts(chart)
                : hole         ? (h != nullptr && h->closesAt > time_)
                               : HasSystem(key.substr(0, key.find('/'))))
                s.known.insert(key);
        }

    // Where they were (#49). A saved system that the galaxy no longer has -- an edited
    // universe, an older save -- leaves the player wherever the session was created
    // rather than in a system that does not exist.
    if (s.ship && !foreign && j.contains("place") && j["place"].is_object())
    {
        const auto& pl = j["place"];
        std::string sys = pl.value("system", std::string());
        if (!sys.empty() && HasSystem(sys))
        {
            s.systemId = sys;
            // The bodies of a cold system where they are now, not at its last coarse pass
            // (#295); its ships are made on the next tick, around the player once placed.
            SystemState& state = systems_[sys];
            if (!IsHot(state))
                Orbits::Place(state.entities, time_);
            // A position from before #159 is in a system forty times smaller, where every
            // body has since moved and grown; kept, it would put the ship inside a star.
            if (version >= 2 && pl.contains("pos") && pl["pos"].is_array() && pl["pos"].size() >= 2)
                s.ship->Teleport({ (float)pl["pos"][0], (float)pl["pos"][1] });
            else
                s.ship->Teleport(SafeArrival(sys, &s));
            // Beside the body it was left beside, wherever that body has got to (#258).
            if (version >= 2 && pl.contains("near") && pl["near"].is_object())
            {
                const json& nr = pl["near"];
                const int   kind = nr.value("kind", -1);
                const auto  name = nr.value("name", std::string());
                int         nth = nr.value("nth", 0);
                for (const auto& e : state.entities)
                    if ((int)e->GetKind() == kind && e->GetName() == name && nth-- == 0)
                    {
                        const json& o = nr["offset"];
                        if (o.is_array() && o.size() >= 2)
                            s.ship->Teleport({ e->GetPosition().x + (float)o[0],
                                               e->GetPosition().y + (float)o[1] });
                        break;
                    }
            }
            s.ship->SetHeading((float)pl.value("heading", 0.0));
        }
    }

    if (j.contains("auth") && j["auth"].is_object())
    {
        s.authSalt = j["auth"].value("salt", std::string());
        s.authStored = j["auth"].value("stored", std::string());
    }

    // The hangar (#5). An account written before this existed has no list, and defaults
    // to the starter -- which is what such an account was, since nothing else was kept.
    // No schema bump for that reason: an older reader ignores these keys and a newer one
    // defaults them, which is the case the version deliberately does not cover (#20).
    //
    // Since version 5 each ship names the design it flies (#279 fitting); before, a ship was
    // its hull index and flew the stock design, which is what it is read as.
    if (j.contains("ships") && j["ships"].is_array())
    {
        s.ownedShips.clear();
        s.shipDesigns.clear();
        const int hulls = (int)GetShipCatalog().size();
        for (const auto& v : j["ships"])
        {
            int         idx = -1;
            std::string design;
            if (version < 5 && v.is_number_integer())
                idx = v.get<int>();
            else if (version >= 5 && v.is_object())
            {
                idx = v.value("hull", -1);
                design = v.value("design", std::string());
            }
            if (idx < 0 || idx >= hulls || s.Owns(idx))
                continue;
            s.ownedShips.push_back(idx);
            const std::string stock = GetShipCatalog()[idx].stats.design;
            // A design the server no longer has -- or one that is not this hull's -- is
            // not flown as something else: the ship is its hull's stock design again, and
            // the player is told.
            if (design.empty() || design == stock)
                continue;
            ShipStats stats;
            if (designs_.Find(design) == nullptr || HullOf(design) != idx ||
                !ShipStatsFor(design, stats))
            {
                s.RecordEvent(Ev::Kind::Notice, "Your " + GetShipCatalog()[idx].name +
                                                    "'s fit is no longer valid; it is back to "
                                                    "its stock fit");
                continue;
            }
            s.shipDesigns[idx] = design;
        }
        if (s.ownedShips.empty())
            s.ownedShips.push_back(0);
    }
    // The saved designs (#279 fitting, step 3). One the server no longer has, or that is for
    // no hull any more, is dropped with a word, as a ship's is.
    s.designLibrary.clear();
    if (j.contains("designs") && j["designs"].is_array())
        for (const auto& v : j["designs"])
        {
            if (!v.is_object() || (int)s.designLibrary.size() >= Ships::LIBRARY_MAX)
                continue;
            const std::string name = v.value("name", std::string());
            const std::string design = v.value("design", std::string());
            std::string       why;
            if (!Ships::ValidDesignName(name, why) || s.designLibrary.count(name) != 0)
                continue;
            if (designs_.Find(design) == nullptr || HullOf(design) < 0)
            {
                s.RecordEvent(Ev::Kind::Notice,
                              "Saved design '" + name + "' is no longer valid and was removed");
                continue;
            }
            s.designLibrary[name] = design;
        }

    // Insurance and home (#344). A policy on a hull the account no longer has is dropped; a
    // home the galaxy no longer has leaves the one the session began with (CreateSession).
    s.insurance.clear();
    if (j.contains("insurance") && j["insurance"].is_array())
        for (const auto& v : j["insurance"])
        {
            if (!v.is_object())
                continue;
            const int    hull = v.value("hull", -1);
            const double cover = v.value("cover", 0.0);
            if (s.Owns(hull) && cover > 0.0)
                s.insurance[hull] = cover;
        }
    if (!foreign)
        if (const int home = StationIdOf(systems_, j.value("home", json())))
            s.homeStationId = home;

    const int wanted = j.value("ship", 0);
    s.currentShip = s.Owns(wanted) ? wanted : s.ownedShips.front();
    if (s.ship)
    {
        ShipStats stats = GetShipCatalog()[s.currentShip].stats;
        ShipStatsFor(s.DesignOf(s.currentShip), stats);
        s.ship->Refit(stats);
    }

    // Cargo is refilled through AddCargo rather than written in, so a hold that shrank
    // between sessions -- a smaller ship, a changed catalog -- drops the overflow instead
    // of carrying more than it can.
    if (s.ship && j.contains("cargo") && j["cargo"].is_array())
    {
        s.ship->ClearCargo();
        for (const auto& c : j["cargo"])
        {
            if (!c.is_object())
                continue;
            // A good this server's registry does not have is left behind rather than read as
            // another one (#343).
            const int    amount = c.value("amount", 0);
            ResourceType good;
            if (amount > 0 && Goods::Find(c.value("resource", std::string()), good))
                s.ship->AddCargo(good, amount);
        }
    }

    if (!foreign && j.contains("missions") && j["missions"].is_array())
    {
        std::vector<Mission> active;
        for (const auto& mj : j["missions"])
        {
            if (!mj.is_object())
                continue;
            Mission m;
            m.type = (MissionType)mj.value("type", 0);
            const json fj = mj.value("faction", json());
            if (fj.is_number_integer())
                Factions::FromLegacyIndex(fj.get<int>(), m.faction);  // version 3 and older
            else if (fj.is_string())
                Factions::Find(fj.get<std::string>(), m.faction);
            m.title = mj.value("title", std::string());
            m.description = mj.value("description", std::string());
            m.resource = ResourceFromName(mj.value("resource", std::string()));
            m.targetCount = mj.value("target", 0);
            m.progress = mj.value("progress", 0);
            m.rewardMoney = mj.value("rewardMoney", 0.0);
            m.rewardRep = mj.value("rewardRep", 0.0f);
            const json giver = mj.value("giver", json());
            const json dest = mj.value("dest", json());
            if (IsPlanJob(m.type))
            {
                // A plan's job is handed in anywhere: what it needs is its plan, not a station.
                m.planId = mj.value("plan", 0);
                m.systemId = mj.value("system", std::string());
                m.systemName = mj.value("where", m.systemId);
                m.deadline = mj.value("deadline", 0.0);
                m.stage = mj.value("stage", 0);
                m.rode = mj.value("rode", false);
                if (mj.contains("against") && mj["against"].is_string())
                    Factions::Find(mj["against"].get<std::string>(), m.against);
                m.giverStationId = StationIdOf(systems_, giver);
                if (plans_.count(m.planId) == 0 && m.progress < m.targetCount)
                {
                    s.RecordEvent(Ev::Kind::Notice,
                                  "Job dropped -- " + m.title + ": what it was for is over");
                    continue;
                }
            }
            else if (m.type == MissionType::Contract)
            {
                // Its station and what is owed are the world's contract's; one the world no
                // longer has -- over while the pilot was away -- is gone.
                m.contractId = mj.value("contract", 0);
                m.systemId = mj.value("system", std::string());
                m.systemName = mj.value("where", m.systemId);
                m.deadline = mj.value("deadline", 0.0);
                m.giverStationId = StationIdOf(systems_, giver);
                m.destStationId = StationIdOf(systems_, dest);
                const auto c = contracts_.find(m.contractId);
                if (c == contracts_.end() || !c->second.taken || m.destStationId == 0)
                {
                    s.RecordEvent(Ev::Kind::Notice,
                                  "Contract dropped -- " + m.title + ": it is over");
                    continue;
                }
            }
            else if (version >= 3)
            {
                m.giverStationId = StationIdOf(systems_, giver);
                m.destStationId = StationIdOf(systems_, dest);
                // A mission handed in at a station the galaxy no longer has could never be
                // completed, and would hold one of the player's slots for good.
                const int handIn =
                    m.type == MissionType::Delivery ? m.destStationId : m.giverStationId;
                if (handIn == 0)
                {
                    s.RecordEvent(Ev::Kind::Notice, "Mission dropped -- " + m.title +
                                                        ": its station is no longer there");
                    continue;
                }
            }
            else
            {
                // Before #227 the ids themselves were written. They are the best there is,
                // and right whenever the world has materialised in the same order since.
                m.giverStationId = giver.is_number_integer() ? giver.get<int>() : 0;
                m.destStationId = dest.is_number_integer() ? dest.get<int>() : 0;
            }
            active.push_back(std::move(m));
        }
        // The offer board is not restored: it belongs to the station the player was
        // docked at and is regenerated on the next dock.
        s.missions.SetMirror({}, std::move(active));
    }
    // The systems on their own charts (#431), once their place is known.
    LoadKnown(s);
    return Save::Result::Ok;
}
