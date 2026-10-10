// Everything the server does on behalf of one player: the verbs (fire, mine, loot, sell,
// dock, jump, refit, buy), the account behind them, and the mission board.
//
// One translation unit of Simulation (#17). It is also the preparation for #3: what a
// per-connection session would own is collected here first, instead of being scattered
// through the same file as the world and the NPCs.

#include "sim/Simulation.h"
#include "sim/ClientSession.h"
#include "sim/PlayerStep.h"

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
#include "raymath.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>

namespace
{
// Player combat (server logic). The weapon's range is not here: it is in
// Sim::PLAYER_WEAPON_RANGE, shared because the client draws the targeting circle from it.
// Every other reach -- docking, mining, salvage, a gate -- belongs to the object being
// reached and comes from its archetype (#34).
constexpr float PLAYER_WEAPON_DAMAGE = 16.0f;  // player shot damage
constexpr float PLAYER_FIRE_INTERVAL = 0.5f;   // cooldown between player shots

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

void Simulation::ServerRespawnPlayer(ClientSession& s)
{
    if (!s.ship)
        return;
    s.RecordEvent(Ev::Kind::ShipDestroyed, "Ship destroyed; respawned with cargo lost");
    s.ship->Teleport(SafeArrival(s.systemId, &s));
    s.ship->Repair();
    s.ship->ClearCargo();
    s.ship->DisengageAutopilot();
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

    // Target — an NPC of the active system by id.
    NpcShip* target = nullptr;
    for (auto& e : st.entities)
        if (e->GetId() == targetId)
        {
            target = e->GetKind() == EntityKind::Npc ? static_cast<NpcShip*>(e.get()) : nullptr;
            break;
        }
    if (target == nullptr || !target->IsAlive())
        return false;

    Vector2 shipPos = s.ship->GetPosition();
    float   dx = target->GetPosition().x - shipPos.x;
    float   dy = target->GetPosition().y - shipPos.y;
    if (std::sqrt(dx * dx + dy * dy) > PLAYER_WEAPON_RANGE || s.fireTimer > 0.0f)
        return false;

    target->TakeDamage(PLAYER_WEAPON_DAMAGE);
    s.fireTimer = PLAYER_FIRE_INTERVAL;

    if (ev != nullptr)
    {
        ev->shotFrom = shipPos;
        ev->shotTo = target->GetPosition();
    }

    // Account effects: attacking a lawful target is a crime; a kill is mission credit
    // (pirate) or a serious crime (lawful). Applied to the session's account, which is the
    // only one there is. `ev` is filled as well, because the client draws the consequences
    // -- a beam, a flash, a notice -- from facts rather than by recomputing them.
    FactionId tf = target->GetFaction();
    if (Factions::IsLawful(tf))
    {
        if (ev != nullptr)
        {
            ev->hitLawful = true;
            ev->hitFaction = tf;
        }
        s.account.AddReputation(tf, -0.4f);
        s.account.AddBounty(tf, 5.0);
    }
    if (!target->IsAlive())
    {
        if (target->IsPirate())
        {
            if (ev != nullptr)
                ev->killedPirate = true;
            // Mission credit for the pirate, into this session's mission log.
            s.missions.OnPirateKilled();
        }
        else if (Factions::IsLawful(tf))
        {
            if (ev != nullptr)
            {
                ev->killedLawful = true;
                ev->killedFaction = tf;
            }
            s.account.AddReputation(tf, -3.0f);
            s.account.AddBounty(tf, 50.0);
        }
    }
    return true;
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

Simulation::PlayerSellResult Simulation::StepPlayerSell(ClientSession& s, SystemState& st,
                                                        int resourceType, int amount)
{
    PlayerSellResult r;
    if (!s.ship)
        return r;
    // Selling needs somewhere that trades. Every station has a market today, so this
    // refuses nothing yet -- it starts refusing the moment a player builds a dock that is
    // not also a market (#44), which is the point of the component being real (#34).
    bool trades = false;
    for (const auto& e : st.entities)
        if (e->GetId() == s.dockedStationId && e->Has(Component::Market))
        {
            trades = true;
            break;
        }
    if (!trades)
        return r;

    ResourceType type = (ResourceType)resourceType;
    int          have = s.ship->GetCargoAmount(type);
    int          sold = amount < have ? amount : have;
    if (sold <= 0)
        return r;
    r.gross = st.market.Sell(type, sold);  // revenue at the current price + price sag
    s.ship->RemoveCargo(type, sold);
    r.sold = sold;

    // Net revenue into this session's account: trading-skill multiplier and the
    // reputation of the station's faction, found by the id docking recorded.
    FactionId sf = FactionId::Independent;
    for (auto& e : st.entities)
        if (e->GetId() == s.dockedStationId)
        {
            if (Station* station =
                    e->GetKind() == EntityKind::Station ? static_cast<Station*>(e.get()) : nullptr)
                sf = station->GetFaction();
            break;
        }
    const float sellMul = SellPriceMultiplier(Factions::TierOf(s.account.GetReputation(sf)));
    double      revenue = r.gross * s.account.GetSkills().GetBonus(SkillType::Trading) * sellMul;
    s.account.AddMoney(revenue);
    s.account.GetSkills().AddXp(SkillType::Trading, (float)(revenue * 0.05));
    s.account.AddReputation(sf, (float)(revenue * 0.002));
    r.revenue = revenue;
    return r;
}

// A hull whose hold is smaller than what the ship carries is refused, not refitted (#219).
// Refit swaps stats and leaves the cargo alone, so without this a full Hauler became a Scout
// carrying four times its capacity. Refusing keeps "a hold never holds more than it can"
// true without deciding for the player which ore to throw away.
static bool HoldFits(ClientSession& s, const ShipType& t, const std::string& verb)
{
    const int used = s.ship->GetCargoUsed();
    if (used <= t.stats.cargoCapacity)
        return true;
    s.RecordEvent(Ev::Kind::Notice, verb + " refused: the hold carries " + std::to_string(used) +
                                        " and a " + t.name + " holds " +
                                        std::to_string(t.stats.cargoCapacity) +
                                        "; sell or hand in cargo first");
    return false;
}

static std::string Credits(double cr)
{
    return std::to_string((long long)std::llround(cr)) + " cr";
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
    if (!HoldFits(s, t, "Switch to " + t.name))
        return false;
    s.ship->Refit(t.stats);
    s.currentShip = catalogIndex;
    s.RecordEvent(Ev::Kind::Notice, "Now flying the " + t.name);
    return true;
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
    if (!HoldFits(s, t, "Purchase of a " + t.name))
        return false;

    // Price multiplier by the docked station's faction reputation.
    FactionId sf = FactionId::Independent;
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
    s.ship->Refit(t.stats);
    s.currentShip = catalogIndex;
    s.RecordEvent(Ev::Kind::Notice,
                  "Bought a " + t.name + " for " + Credits(price) + "; now flying it");
    return true;
}

bool Simulation::AccountHostileToFaction(const ClientSession& s, FactionId f) const
{
    if (f == FactionId::Pirates)
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
}

bool Simulation::MissionCompletableNow(const ClientSession& s, const Mission& m) const
{
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
    // Asks what is dockable rather than what is a Station (#34). The moment a player can
    // build a dock (#44) it joins this pass by declaring the component, without this
    // function being touched.
    for (auto& e : st.entities)
    {
        if (!e->Has(Component::Dockable))
            continue;
        float dx = e->GetPosition().x - pp.x;
        float dy = e->GetPosition().y - pp.y;
        if (std::sqrt(dx * dx + dy * dy) > e->GetSize() + e->GetArchetype()->dockRange)
            continue;

        // Reputation admittance (M4f-4): at the Hated tier the owning station refuses;
        // Hostile still admits. Reputation is authoritative here (s.account) — the player
        // is told via the snapshot. Who owns a dock is still Station-specific state, so
        // this one narrowing stays until ownership becomes a component too (#41).
        if (e->GetKind() == EntityKind::Station)
        {
            FactionId sf = static_cast<Station*>(e.get())->GetFaction();
            if (Factions::TierOf(s.account.GetReputation(sf)) == RepTier::Hated)
            {
                s.RecordEvent(Ev::Kind::Notice, "Docking denied: hostile reputation");
                return 0;
            }
        }
        s.ship->DisengageAutopilot();
        s.ship->ReleaseHold();
        s.ship->Stop();
        s.dockedStationId = e->GetId();
        // Berthed on the side it came in from, and from now on carried there (#298).
        s.dockBearing = Sim::DockBearing(e->GetPosition(), pp);
        s.ship->Teleport(Sim::DockBerth(e->GetPosition(), e->GetSize(), s.dockBearing));
        s.RecordEvent(Ev::Kind::Docked, "Docked at " + e->GetName());
        GenerateDockOffers(s);  // fresh station mission board (M4f-2)
        return s.dockedStationId;
    }
    return 0;
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

    float heading = s.ship->GetHeading();
    s.ship->Teleport(ArrivalFrom(destId, fromId, &s, &heading));
    s.ship->SetHeading(heading);
    s.ship->DisengageAutopilot();
    s.ship->CancelWarp();
    s.dockedStationId = 0;     // the jump releases the docking
    s.missions.ClearOffers();  // clear the board of the station we left; active missions
                               // survive the jump (they address stations by id) — otherwise
                               // Bounty/Delivery into another system would be uncompletable

    // The first ship into a system beyond the wormhole (#143): from now on it is part of
    // the contested world, and that it was reached at all is news.
    SystemAggregate& agg = systems_[destId].agg;
    if (!agg.visited)
    {
        agg.visited = true;
        agg.discoverer = s.ship ? s.ship->GetPilotName() : std::string();
        chartsChanged_ = true;
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
    // distance that no hostile defensive station covers.
    auto underFire = [&](Vector2 at)
    {
        if (sys == nullptr)
            return false;
        for (const auto& e : sys->entities)
            if (e->GetKind() == EntityKind::Station && !takesUs(*e) && e->Has(Component::Defensive))
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

void Simulation::SaveAccount(const ClientSession& s, const std::string& path) const
{
    using nlohmann::json;
    json j;
    j["version"] = Save::ACCOUNT_VERSION;
    j["money"] = s.account.GetMoney();

    json rep = json::array();
    for (int i = 0; i < 4; i++)
        rep.push_back(s.account.GetReputation((FactionId)i));
    j["reputation"] = rep;

    json bounty = json::array();
    for (int i = 0; i < 4; i++)
        bounty.push_back(s.account.GetBounty((FactionId)i));
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
                cargo.push_back({ { "resource", ResourceName(r) }, { "amount", n } });
        j["cargo"] = cargo;
    }

    if (!s.authStored.empty())
        j["auth"] = { { "salt", s.authSalt }, { "stored", s.authStored } };

    j["ships"] = s.ownedShips;
    j["ship"] = s.currentShip;

    // A mission addresses its stations by id while the server runs -- that is what lets
    // one survive a jump -- but an id is good for one run only (#227). The save names them
    // the way the data does, by system and station, and the load looks the ids up again.
    json missions = json::array();
    for (const Mission& m : s.missions.Active())
        missions.push_back({ { "type", (int)m.type },
                             { "faction", (int)m.faction },
                             { "title", m.title },
                             { "description", m.description },
                             { "giver", StationRefOf(systems_, m.giverStationId) },
                             { "dest", StationRefOf(systems_, m.destStationId) },
                             { "resource", ResourceName(m.resource) },
                             { "target", m.targetCount },
                             { "progress", m.progress },
                             { "rewardMoney", m.rewardMoney },
                             { "rewardRep", m.rewardRep } });
    j["missions"] = missions;

    std::ofstream out(path);
    if (out.is_open())
        out << j.dump(2) << "\n";
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
    if (j.contains("reputation"))
        for (int i = 0; i < 4 && i < (int)j["reputation"].size(); i++)
            s.account.SetReputation((FactionId)i, (float)j["reputation"][i]);
    if (j.contains("bounty"))
        for (int i = 0; i < 4 && i < (int)j["bounty"].size(); i++)
            s.account.SetBounty((FactionId)i, (double)j["bounty"][i]);
    if (j.contains("skills"))
    {
        Skills& sk = s.account.GetSkills();
        sk.SetXp(SkillType::Piloting, (float)j["skills"].value("piloting", 0));
        sk.SetXp(SkillType::Mining, (float)j["skills"].value("mining", 0));
        sk.SetXp(SkillType::Trading, (float)j["skills"].value("trading", 0));
    }

    // Where they were (#49). A saved system that the galaxy no longer has -- an edited
    // universe, an older save -- leaves the player wherever the session was created
    // rather than in a system that does not exist.
    if (s.ship && j.contains("place") && j["place"].is_object())
    {
        const auto& pl = j["place"];
        std::string sys = pl.value("system", std::string());
        if (!sys.empty() && HasSystem(sys))
        {
            s.systemId = sys;
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
                if (const SystemState* state = SystemById(sys))
                    for (const auto& e : state->entities)
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
    if (j.contains("ships") && j["ships"].is_array())
    {
        s.ownedShips.clear();
        for (const auto& v : j["ships"])
            if (v.is_number_integer())
            {
                const int idx = v.get<int>();
                if (idx >= 0 && idx < (int)GetShipCatalog().size() && !s.Owns(idx))
                    s.ownedShips.push_back(idx);
            }
        if (s.ownedShips.empty())
            s.ownedShips.push_back(0);
    }
    const int wanted = j.value("ship", 0);
    s.currentShip = s.Owns(wanted) ? wanted : s.ownedShips.front();
    if (s.ship)
        s.ship->Refit(GetShipCatalog()[s.currentShip].stats);

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
            const int amount = c.value("amount", 0);
            if (amount > 0)
                s.ship->AddCargo(ResourceFromName(c.value("resource", std::string())), amount);
        }
    }

    if (j.contains("missions") && j["missions"].is_array())
    {
        std::vector<Mission> active;
        for (const auto& mj : j["missions"])
        {
            if (!mj.is_object())
                continue;
            Mission m;
            m.type = (MissionType)mj.value("type", 0);
            m.faction = (FactionId)mj.value("faction", 0);
            m.title = mj.value("title", std::string());
            m.description = mj.value("description", std::string());
            m.resource = ResourceFromName(mj.value("resource", std::string()));
            m.targetCount = mj.value("target", 0);
            m.progress = mj.value("progress", 0);
            m.rewardMoney = mj.value("rewardMoney", 0.0);
            m.rewardRep = mj.value("rewardRep", 0.0f);
            const json giver = mj.value("giver", json());
            const json dest = mj.value("dest", json());
            if (version >= 3)
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
    return Save::Result::Ok;
}
