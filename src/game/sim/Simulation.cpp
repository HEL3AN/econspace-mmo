#include "sim/Simulation.h"
#include "sim/PlayerStep.h"

#include "core/World.h"
#include "economy/Resource.h"
#include "entities/AsteroidField.h"
#include "entities/Combatant.h"
#include "entities/Derelict.h"
#include "entities/JumpGate.h"
#include "entities/Nebula.h"
#include "entities/NpcShip.h"
#include "entities/Planet.h"
#include "entities/Ship.h"
#include "entities/ShipType.h"
#include "entities/Star.h"
#include "entities/Station.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>

namespace
{
// NPC combat parameters (server combat/AI simulation).
constexpr float PIRATE_WEAPON_RANGE = 230.0f;  // NPC fire range
constexpr float PIRATE_WEAPON_DAMAGE = 7.0f;   // damage to the player
constexpr float NPC_WEAPON_DAMAGE = 6.0f;      // NPC-vs-NPC damage
constexpr float NPC_AGGRO_RANGE = 3500.0f;     // AI target detection radius
constexpr float NPC_THREAT_RANGE = 2000.0f;    // distance at which peaceful ones flee

// Player combat/mining (server logic; weapon range — in Simulation::PLAYER_WEAPON_RANGE).
constexpr float PLAYER_WEAPON_DAMAGE = 16.0f;  // player shot damage
constexpr float PLAYER_FIRE_INTERVAL = 0.5f;   // cooldown between player shots
constexpr float MINING_RANGE = 40.0f;          // margin on the field radius for mining

// Spawn director. "Pressure": losses in a role raise the suppression of its spawn,
// which then slowly recovers — the player/battles really change the population.
constexpr float SUPPRESS_PER_KILL = 0.30f;        // suppression gain per 1 loss
constexpr float SUPPRESS_DECAY = 0.97f;           // suppression falloff per coarse step (~2 s)
constexpr float DANGER_SEC = 0.35f;               // below this the system is "unsafe" (ambushes)
constexpr float SPAWN_MIN_PLAYER_DIST = 2600.0f;  // do not spawn closer to the player
constexpr float MAINT_STEP = 2.0f;                // coarse world maintenance period

float ClampF(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// Accumulate spawn suppression from losses (in [0,1]).
void AccumSuppress(float& sup, float losses)
{
    if (losses > 0.0f)
        sup = std::min(1.0f, sup + losses * SUPPRESS_PER_KILL);
}

float Dist(Vector2 a, Vector2 b)
{
    return sqrtf((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y));
}

// Live NPCs of a system.
std::vector<NpcShip*> AliveShips(SystemState& st)
{
    std::vector<NpcShip*> ships;
    for (auto& e : st.entities)
        if (NpcShip* n = dynamic_cast<NpcShip*>(e.get()))
            if (n->IsAlive())
                ships.push_back(n);
    return ships;
}
}  // namespace

bool Simulation::NpcHostileToNpc(const NpcShip* a, const NpcShip* b)
{
    Stance s = Factions::Relation(a->GetFaction(), b->GetFaction());
    return s == Stance::Hostile || s == Stance::War;
}

void Simulation::StepNpcAi(SystemState& st, Combatant* player,
                           const std::function<bool(const NpcShip*)>& hostileToPlayer)
{
    std::vector<NpcShip*> ships = AliveShips(st);
    for (NpcShip* n : ships)
    {
        Vector2 npos = n->GetPosition();
        if (n->IsCombatant())
        {
            // Nearest hostile target within the detection radius.
            float   bestD = NPC_AGGRO_RANGE;
            Vector2 bestPos{};
            bool    found = false;
            if (player != nullptr && hostileToPlayer && hostileToPlayer(n))
            {
                float d = Dist(player->GetPosition(), npos);
                if (d < bestD)
                {
                    bestD = d;
                    bestPos = player->GetPosition();
                    found = true;
                }
            }
            for (NpcShip* o : ships)
            {
                if (o == n || !NpcHostileToNpc(n, o))
                    continue;
                float d = Dist(o->GetPosition(), npos);
                if (d < bestD)
                {
                    bestD = d;
                    bestPos = o->GetPosition();
                    found = true;
                }
            }

            if (!found)
                n->StandDown();
            else if (n->GetHull() < n->GetMaxHull() * 0.25f)
                n->FleeFrom(bestPos);  // heavily damaged — retreats
            else
                n->Engage(bestPos);
        }
        else
        {
            // Peaceful: flees from the nearest combat enemy within the threat zone.
            float   td = NPC_THREAT_RANGE;
            Vector2 threat{};
            bool    threatened = false;
            for (NpcShip* o : ships)
            {
                if (o == n || !o->IsCombatant() || !NpcHostileToNpc(o, n))
                    continue;
                float d = Dist(o->GetPosition(), npos);
                if (d < td)
                {
                    td = d;
                    threat = o->GetPosition();
                    threatened = true;
                }
            }
            if (threatened)
                n->FleeFrom(threat);
            else
                n->StandDown();
        }
    }
}

void Simulation::StepNpcCombat(SystemState& st, Combatant* player, bool playerHidden,
                               const std::function<bool(const NpcShip*)>& hostileToPlayer,
                               std::vector<FireEvent>*                    fires)
{
    std::vector<NpcShip*> ships = AliveShips(st);
    for (NpcShip* npc : ships)
    {
        if (!npc->IsAlive() || !npc->IsCombatant() || !npc->ReadyToFire())
            continue;

        Vector2    npos = npc->GetPosition();
        float      bestD = PIRATE_WEAPON_RANGE;
        Combatant* target = nullptr;
        bool       targetIsPlayer = false;

        auto consider = [&](Combatant* c, bool isPlayer)
        {
            float d = Dist(c->GetPosition(), npos);
            if (d <= bestD)
            {
                bestD = d;
                target = c;
                targetIsPlayer = isPlayer;
            }
        };

        if (player != nullptr && !playerHidden && hostileToPlayer && hostileToPlayer(npc))
            consider(player, true);
        for (NpcShip* other : ships)
            if (other != npc && other->IsAlive() && NpcHostileToNpc(npc, other))
                consider(other, false);

        if (target == nullptr)
            continue;

        target->TakeDamage(targetIsPlayer ? PIRATE_WEAPON_DAMAGE : NPC_WEAPON_DAMAGE);
        npc->ResetFireTimer();
        if (fires != nullptr)
            fires->push_back({ npos, target->GetPosition(), npc->GetFaction(), targetIsPlayer });
    }
}

void Simulation::StepSystemAgents(SystemState& st, float dt)
{
    StepNpcAi(st, nullptr, {});  // AI without the player

    for (auto& e : st.entities)  // movement
        e->Update(dt);

    StepNpcCombat(st, nullptr, false, {}, nullptr);  // NPC-vs-NPC combat, no events

    // Cleanup of the fallen.
    st.entities.erase(std::remove_if(st.entities.begin(), st.entities.end(),
                                     [](const std::unique_ptr<Entity>& e)
                                     {
                                         NpcShip* n = dynamic_cast<NpcShip*>(e.get());
                                         return n != nullptr && !n->IsAlive();
                                     }),
                      st.entities.end());
}

void Simulation::StepActiveSystemAgents(SystemState& st, Combatant* player, bool playerHidden,
                                        const std::function<bool(const NpcShip*)>& hostileToPlayer,
                                        std::vector<FireEvent>* fires, float dt)
{
    StepNpcAi(st, player, hostileToPlayer);  // AI sees the player (pursuit/flight)

    for (auto& e : st.entities)  // movement
        e->Update(dt);

    StepNpcCombat(st, player, playerHidden, hostileToPlayer,
                  fires);  // combat with the player, beams

    // Cleanup of the fallen.
    st.entities.erase(std::remove_if(st.entities.begin(), st.entities.end(),
                                     [](const std::unique_ptr<Entity>& e)
                                     {
                                         NpcShip* n = dynamic_cast<NpcShip*>(e.get());
                                         return n != nullptr && !n->IsAlive();
                                     }),
                      st.entities.end());
}

void Simulation::ServerRespawnPlayer()
{
    if (!player_)
        return;
    for (auto& e : Active().entities)
        if (Station* st = dynamic_cast<Station*>(e.get()))
        {
            player_->Teleport(st->GetPosition());
            break;
        }
    player_->Repair();
    player_->ClearCargo();
    player_->DisengageAutopilot();
}

// Constructor/destructor — out of line: the unique_ptr<Ship> member needs Ship's full
// type at the point where the destructor is generated (here Ship.h is already included).
Simulation::Simulation() = default;
Simulation::~Simulation() = default;

void Simulation::LoadUniverse(const std::string& path)
{
    universe_ = WorldLoader::LoadUniverse(path);
}

// --- Player as a server agent (M4d-2b) ---

void Simulation::CreatePlayer(Vector2 pos, const ShipStats& stats)
{
    player_ = std::make_unique<Ship>(pos, stats);
}

void Simulation::StepPlayerShip(const Proto::Command& cmd, float pilotBonus, float dt)
{
    // The step itself is shared with the client's prediction (sim/PlayerStep.h) — one
    // implementation, so predicted and authoritative movement cannot drift apart.
    if (player_)
        Sim::StepPlayerShip(*player_, cmd, pilotBonus, dt);
}

bool Simulation::StepPlayerFire(SystemState& st, bool weaponOn, int targetId, float dt,
                                PlayerCombatEvents* ev)
{
    if (playerFireTimer_ > 0.0f)
        playerFireTimer_ -= dt;
    if (!weaponOn || !player_ || targetId == 0)
        return false;

    // Target — an NPC of the active system by id.
    NpcShip* target = nullptr;
    for (auto& e : st.entities)
        if (e->GetId() == targetId)
        {
            target = dynamic_cast<NpcShip*>(e.get());
            break;
        }
    if (target == nullptr || !target->IsAlive())
        return false;

    Vector2 shipPos = player_->GetPosition();
    float   dx = target->GetPosition().x - shipPos.x;
    float   dy = target->GetPosition().y - shipPos.y;
    if (std::sqrt(dx * dx + dy * dy) > PLAYER_WEAPON_RANGE || playerFireTimer_ > 0.0f)
        return false;

    target->TakeDamage(PLAYER_WEAPON_DAMAGE);
    playerFireTimer_ = PLAYER_FIRE_INTERVAL;

    if (ev != nullptr)
    {
        ev->shotFrom = shipPos;
        ev->shotTo = target->GetPosition();
    }

    // Account effects: attacking a lawful target is a crime; a kill is mission credit
    // (pirate) or a serious crime (lawful). Applied directly to the server account_ (in
    // the network — authoritative; in single-player account_ is ignored, the client applies
    // its own player_ from ev). We still fill ev (single-player/mission client).
    FactionId tf = target->GetFaction();
    if (Factions::IsLawful(tf))
    {
        if (ev != nullptr)
        {
            ev->hitLawful = true;
            ev->hitFaction = tf;
        }
        account_.AddReputation(tf, -0.4f);
        account_.AddBounty(tf, 5.0);
    }
    if (!target->IsAlive())
    {
        if (target->IsPirate())
        {
            if (ev != nullptr)
                ev->killedPirate = true;
            // Mission credit for the pirate into the server missions (in the network —
            // authoritative; in single-player missions_ is empty — progress goes into the
            // client MissionSystem via ev).
            missions_.OnPirateKilled();
        }
        else if (Factions::IsLawful(tf))
        {
            if (ev != nullptr)
            {
                ev->killedLawful = true;
                ev->killedFaction = tf;
            }
            account_.AddReputation(tf, -3.0f);
            account_.AddBounty(tf, 50.0);
        }
    }
    return true;
}

Simulation::PlayerMiningResult Simulation::StepPlayerMining(SystemState& st, float miningBonus,
                                                            float dt)
{
    PlayerMiningResult r;
    if (!player_ || !player_->IsMiningOn() ||
        player_->GetCargoUsed() >= player_->GetCargoCapacity())
        return r;

    Vector2 sp = player_->GetPosition();
    for (auto& e : st.entities)
    {
        AsteroidField* field = dynamic_cast<AsteroidField*>(e.get());
        if (field == nullptr || !field->HasOre())
            continue;

        float dx = field->GetPosition().x - sp.x;
        float dy = field->GetPosition().y - sp.y;
        if (std::sqrt(dx * dx + dy * dy) > field->GetSize() + MINING_RANGE)
            continue;

        r.fieldId = field->GetId();

        // The mining skill (passed by the client as a multiplier) speeds up extraction.
        float rate = player_->GetStats().miningRate * miningBonus;
        playerMiningProgress_ += rate * dt;
        while (playerMiningProgress_ >= 1.0f)
        {
            playerMiningProgress_ -= 1.0f;
            int got = field->Extract(1);
            if (got <= 0 || !player_->AddCargo(field->GetResource(), got))
                break;
            r.minedUnits += got;
        }
        break;
    }
    // Mining xp into the server account (in the network — authoritative; in single-player
    // ignored — the client credits its own player_ from r.minedUnits).
    if (r.minedUnits > 0)
        account_.GetSkills().AddXp(SkillType::Mining, 3.0f * (float)r.minedUnits);
    return r;
}

std::string Simulation::JumpGateDestIfNear(SystemState& st, int gateId) const
{
    if (!player_ || gateId == 0)
        return std::string();
    for (const auto& e : st.entities)
        if (e->GetId() == gateId)
        {
            const JumpGate* g = dynamic_cast<const JumpGate*>(e.get());
            if (g == nullptr)
                return std::string();
            Vector2 sp = player_->GetPosition();
            float   dx = g->GetPosition().x - sp.x;
            float   dy = g->GetPosition().y - sp.y;
            if (std::sqrt(dx * dx + dy * dy) > g->GetSize() + 200.0f)
                return std::string();
            return g->GetDestination();
        }
    return std::string();
}

double Simulation::StepPlayerLoot(SystemState& st, int derelictId)
{
    if (!player_ || derelictId == 0)
        return 0.0;
    for (auto& e : st.entities)
        if (e->GetId() == derelictId)
        {
            Derelict* dr = dynamic_cast<Derelict*>(e.get());
            if (dr == nullptr || dr->IsLooted())
                return 0.0;
            Vector2 sp = player_->GetPosition();
            float   dx = dr->GetPosition().x - sp.x;
            float   dy = dr->GetPosition().y - sp.y;
            if (std::sqrt(dx * dx + dy * dy) > dr->GetSize() + 120.0f)
                return 0.0;
            dr->SetLooted();
            double reward = dr->GetReward();
            account_.AddMoney(
                reward);  // in the network — authoritative; in single-player account_ is ignored
            return reward;
        }
    return 0.0;
}

Simulation::PlayerSellResult Simulation::StepPlayerSell(SystemState& st, int resourceType,
                                                        int amount)
{
    PlayerSellResult r;
    if (!player_)
        return r;
    ResourceType type = (ResourceType)resourceType;
    int          have = player_->GetCargoAmount(type);
    int          sold = amount < have ? amount : have;
    if (sold <= 0)
        return r;
    r.gross = st.market.Sell(type, sold);  // revenue at the current price + price sag
    player_->RemoveCargo(type, sold);
    r.sold = sold;

    // Net revenue into the server account: trading-skill multiplier + reputation of the
    // docked station's faction. In the network — authoritative; in single-player account_
    // is ignored (the client computes its own from r.gross). Station faction — by
    // playerDockedStationId_ (server docking); in single-player it is 0, but the account_ branch is
    // unused there.
    FactionId sf = FactionId::Independent;
    for (auto& e : st.entities)
        if (e->GetId() == playerDockedStationId_)
        {
            if (Station* station = dynamic_cast<Station*>(e.get()))
                sf = station->GetFaction();
            break;
        }
    float sellMul = 1.0f;
    switch (Factions::TierOf(account_.GetReputation(sf)))
    {
        case RepTier::Hostile: sellMul = 0.85f; break;
        case RepTier::Liked: sellMul = 1.10f; break;
        case RepTier::Allied: sellMul = 1.20f; break;
        default: break;
    }
    double revenue = r.gross * account_.GetSkills().GetBonus(SkillType::Trading) * sellMul;
    account_.AddMoney(revenue);
    account_.GetSkills().AddXp(SkillType::Trading, (float)(revenue * 0.05));
    account_.AddReputation(sf, (float)(revenue * 0.002));
    r.revenue = revenue;
    return r;
}

void Simulation::RefitPlayer(const ShipStats& stats)
{
    if (player_)
        player_->Refit(stats);
}

void Simulation::StepPlayerAccountTick(float dt)
{
    // Piloting xp accrues in flight (not docked); the wanted level decays slowly.
    if (!IsPlayerDocked())
        account_.GetSkills().AddXp(SkillType::Piloting, 4.0f * dt);
    account_.DecayBounty(1.0 * dt);
}

void Simulation::PayBounty(FactionId faction)
{
    double b = account_.GetBounty(faction);
    if (b > 0.0 && account_.CanAfford(b))
    {
        account_.AddMoney(-b);
        account_.SetBounty(faction, 0.0);
    }
}

bool Simulation::BuyShip(int catalogIndex)
{
    const std::vector<ShipType>& catalog = GetShipCatalog();
    if (!player_ || catalogIndex < 0 || catalogIndex >= (int)catalog.size())
        return false;

    // Price multiplier by the docked station's faction reputation (as on the client).
    FactionId sf = FactionId::Independent;
    for (auto& e : Active().entities)
        if (e->GetId() == playerDockedStationId_)
        {
            if (Station* s = dynamic_cast<Station*>(e.get()))
                sf = s->GetFaction();
            break;
        }
    float buyMul = 1.0f;
    switch (Factions::TierOf(account_.GetReputation(sf)))
    {
        case RepTier::Hostile: buyMul = 1.15f; break;
        case RepTier::Liked: buyMul = 0.92f; break;
        case RepTier::Allied: buyMul = 0.85f; break;
        default: break;
    }
    double price = catalog[catalogIndex].price * buyMul;
    if (!account_.CanAfford(price))
        return false;
    account_.AddMoney(-price);
    player_->Refit(catalog[catalogIndex].stats);
    return true;
}

bool Simulation::AccountHostileToFaction(FactionId f) const
{
    if (f == FactionId::Pirates)
        return true;
    if (account_.IsWanted(f))
        return true;
    RepTier t = Factions::TierOf(account_.GetReputation(f));
    return t == RepTier::Hostile || t == RepTier::Hated;
}

void Simulation::GenerateDockOffers()
{
    Station*              giver = nullptr;
    std::vector<Station*> all;
    for (auto& e : Active().entities)
        if (Station* s = dynamic_cast<Station*>(e.get()))
        {
            all.push_back(s);
            if (s->GetId() == playerDockedStationId_)
                giver = s;
        }
    if (giver == nullptr)
        return;

    // Reward multiplier by the station faction's reputation (as on the client in Dock).
    float repMul = 1.0f;
    switch (Factions::TierOf(account_.GetReputation(giver->GetFaction())))
    {
        case RepTier::Hostile: repMul = 0.8f; break;
        case RepTier::Liked: repMul = 1.15f; break;
        case RepTier::Allied: repMul = 1.3f; break;
        default: break;
    }
    missions_.GenerateOffers(giver, all, repMul);
}

bool Simulation::MissionCompletableNow(const Mission& m) const
{
    if (playerDockedStationId_ == 0 || !player_)
        return false;
    switch (m.type)
    {
        case MissionType::Bounty:
            return m.giverStationId == playerDockedStationId_ && m.progress >= m.targetCount;
        case MissionType::Mining:
            return m.giverStationId == playerDockedStationId_ &&
                   player_->GetCargoAmount(m.resource) >= m.targetCount;
        case MissionType::Delivery: return m.destStationId == playerDockedStationId_;
    }
    return false;
}

bool Simulation::CompleteMission(int activeIndex)
{
    std::vector<Mission>& active = missions_.Active();
    if (activeIndex < 0 || activeIndex >= (int)active.size())
        return false;
    const Mission& m = active[activeIndex];
    if (!MissionCompletableNow(m))
        return false;

    if (m.type == MissionType::Mining && player_)
        player_->RemoveCargo(m.resource, m.targetCount);
    account_.AddMoney(m.rewardMoney);
    account_.AddReputation(m.faction, m.rewardRep);
    active.erase(active.begin() + activeIndex);
    return true;
}

int Simulation::StepPlayerDock(SystemState& st)
{
    static constexpr float DOCK_RANGE = 90.0f;  // margin on the station radius (as on the client)
    if (!player_ || player_->IsWarping())
        return 0;

    Vector2 pp = player_->GetPosition();
    for (auto& e : st.entities)
    {
        Station* station = dynamic_cast<Station*>(e.get());
        if (station == nullptr)
            continue;
        float dx = station->GetPosition().x - pp.x;
        float dy = station->GetPosition().y - pp.y;
        if (std::sqrt(dx * dx + dy * dy) <= station->GetSize() + DOCK_RANGE)
        {
            // Reputation admittance (M4f-4): at the Hated tier the owner station refuses
            // (same threshold as in single-player Game::Dock; Hostile still admits). Reputation
            // is authoritative on the server (account_) — we notify the player via the snapshot.
            FactionId sf = station->GetFaction();
            if (Factions::TierOf(account_.GetReputation(sf)) == RepTier::Hated)
            {
                outMessages_.push_back("Docking denied: hostile reputation");
                return 0;
            }
            player_->DisengageAutopilot();
            player_->Stop();
            playerDockedStationId_ = station->GetId();
            GenerateDockOffers();  // fresh station mission board (M4f-2)
            return playerDockedStationId_;
        }
    }
    return 0;
}

void Simulation::ServerEnterSystem(const std::string& destId, const std::string& fromId)
{
    if (!HasSystem(destId))
        return;
    Activate(destId);
    if (!player_)
        return;

    // Arrival point — at the gate leading back to the origin system (as on the client).
    Vector2 arrival = { 0.0f, 3000.0f };
    if (!fromId.empty())
        for (auto& e : Active().entities)
            if (JumpGate* g = dynamic_cast<JumpGate*>(e.get()))
                if (g->GetDestination() == fromId)
                {
                    Vector2 gp = g->GetPosition();
                    float   d = std::sqrt(gp.x * gp.x + gp.y * gp.y);
                    float   k = d > 1.0f ? (d - (g->GetSize() + 200.0f)) / d : 0.0f;
                    arrival = { gp.x * k, gp.y * k };
                    break;
                }
    player_->Teleport(arrival);
    player_->DisengageAutopilot();
    player_->CancelWarp();
    playerDockedStationId_ = 0;  // the jump releases the docking
    missions_.ClearOffers();     // clear the board of the station we left; active missions
                                 // survive the jump (they address stations by id) — otherwise
                                 // Bounty/Delivery into another system would be uncompletable
}

int Simulation::RandRange(int lo, int hi)
{
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    if (hi <= lo)
        return lo;
    return lo + (int)(rng_ % (unsigned)(hi - lo + 1));
}

float Simulation::Rand01()
{
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return (rng_ & 0xFFFFFFu) / (float)0x1000000u;
}

void Simulation::InitGalaxy()
{
    for (const auto& info : universe_.systems)
    {
        SystemState& st = systems_[info.id];  // creates a cold skeleton (no entities)
        st.id = info.id;
        if (!st.agg.seeded)
            SeedAggregate(st, info);
    }
}

// Starting population of a system from its base security (heuristics close to the
// former role-based spawn, but in aggregate numbers).
void Simulation::SeedAggregate(SystemState& st, const WorldLoader::SystemInfo& info)
{
    float            sec = info.security;
    SystemAggregate& a = st.agg;
    a.baseSecurity = sec;
    a.security = sec;
    a.prosperity = sec;
    a.traders = 2.0f + sec * 4.0f;
    a.miners = 2.0f;
    a.police = roundf(sec * 4.0f);
    a.pirates = std::max(0.0f, roundf((0.7f - sec) * 8.0f));
    a.controller = info.owner.empty() ? FactionId::Independent : FactionFromString(info.owner);
    a.seeded = true;
}

std::vector<std::string> Simulation::Neighbors(const std::string& id) const
{
    std::vector<std::string> out;
    for (const auto& l : universe_.links)
    {
        if (l.a == id)
            out.push_back(l.b);
        else if (l.b == id)
            out.push_back(l.a);
    }
    return out;
}

std::string Simulation::SystemName(const std::string& id) const
{
    for (const auto& s : universe_.systems)
        if (s.id == id)
            return s.name;
    return id;
}

void Simulation::PushEvent(const std::string& msg)
{
    events_.push_back(msg);
    if (events_.size() > 8)
        events_.erase(events_.begin());  // keep the last 8
}

// M0 macro on REAL numbers (population in the aggregate is filled by Game::RecountAgg).
// Security/economy drift, economy diffusion, controller changes. The population is not
// changed — that is done by real battles and the Game spawn director.
void Simulation::StepWorldMacro()
{
    // Security and economy drift from the real populations.
    for (auto& kv : systems_)
    {
        SystemAggregate& a = kv.second.agg;
        a.security = ClampF(a.security + (a.baseSecurity - a.security) * 0.05f +
                                (a.police - a.pirates) * 0.02f,
                            0.0f, 1.0f);
        a.prosperity = ClampF(a.prosperity + (a.security - 0.5f) * 0.02f +
                                  (a.traders - 3.0f) * 0.008f - a.pirates * 0.01f,
                              0.0f, 1.0f);
    }

    // Economic diffusion along gate lines (trade links neighbors).
    for (const auto& l : universe_.links)
    {
        auto a = systems_.find(l.a);
        auto b = systems_.find(l.b);
        if (a == systems_.end() || b == systems_.end())
            continue;
        float dp = b->second.agg.prosperity - a->second.agg.prosperity;
        a->second.agg.prosperity = ClampF(a->second.agg.prosperity + dp * 0.02f, 0.0f, 1.0f);
        b->second.agg.prosperity = ClampF(b->second.agg.prosperity - dp * 0.02f, 0.0f, 1.0f);
    }

    // Controller changes (on real numbers; reinforcements are provided by the director).
    for (auto& kv : systems_)
    {
        SystemAggregate& a = kv.second.agg;
        if (a.controller != FactionId::Pirates && a.pirates > a.police * 2.0f + 2.0f &&
            a.security < 0.25f)
        {
            a.controller = FactionId::Pirates;
            a.baseSecurity = std::min(a.baseSecurity, 0.15f);
            PushEvent("Pirates seized " + SystemName(kv.first));
        }
        else if (a.controller == FactionId::Pirates && a.police > a.pirates && a.security > 0.4f)
        {
            // Law restored (the director brought police from a strong neighbor).
            for (const std::string& nid : Neighbors(kv.first))
            {
                auto n = systems_.find(nid);
                if (n != systems_.end() && Factions::IsLawful(n->second.agg.controller) &&
                    n->second.agg.security > 0.6f)
                {
                    a.controller = n->second.agg.controller;
                    a.baseSecurity = std::max(a.baseSecurity, 0.5f);
                    PushEvent(SystemName(kv.first) + " liberated by " +
                              FactionName(n->second.agg.controller));
                    break;
                }
            }
        }
    }
}

void Simulation::RecountAgg(SystemState& st)
{
    int tr = 0, mi = 0, po = 0, pi = 0;
    for (auto& e : st.entities)
        if (NpcShip* n = dynamic_cast<NpcShip*>(e.get()))
            switch (n->GetRole())
            {
                case NpcRole::Trader: tr++; break;
                case NpcRole::Miner: mi++; break;
                case NpcRole::Police: po++; break;
                case NpcRole::Pirate: pi++; break;
                case NpcRole::Warship: po++; break;  // combat factions — count as "police"
            }
    st.agg.traders = (float)tr;
    st.agg.miners = (float)mi;
    st.agg.police = (float)po;
    st.agg.pirates = (float)pi;
}

// System nodes for spawning. Gates are marked "dangerous" if they lead into an unsafe/
// pirate system — only there do pirate ambushes belong.
Simulation::SpawnNodes Simulation::GatherNodes(const SystemState& st) const
{
    SpawnNodes nd;
    for (const auto& e : st.entities)
    {
        if (dynamic_cast<Station*>(e.get()) != nullptr)
            nd.stations.push_back(e->GetPosition());
        else if (JumpGate* g = dynamic_cast<JumpGate*>(e.get()))
        {
            Vector2 p = g->GetPosition();
            nd.gates.push_back(p);
            auto dest = systems_.find(g->GetDestination());
            if (dest != systems_.end())
            {
                const SystemAggregate& da = dest->second.agg;
                if (da.controller == FactionId::Pirates || da.security < DANGER_SEC)
                    nd.dangerGates.push_back(p);  // border with low-sec — ambush spot
            }
        }
        else if (dynamic_cast<AsteroidField*>(e.get()) != nullptr)
        {
            Vector2 p = e->GetPosition();
            nd.fields.push_back(p);
            if (sqrtf(p.x * p.x + p.y * p.y) > World::MID_RADIUS)
                nd.outerSpots.push_back(p);  // far periphery
        }
    }
    return nd;
}

std::vector<Vector2> Simulation::PirateSpots(const SpawnNodes& nd)
{
    std::vector<Vector2> hot = nd.outerSpots;
    hot.insert(hot.end(), nd.dangerGates.begin(), nd.dangerGates.end());
    if (hot.empty())
        hot = nd.fields;  // no periphery/danger gates — at the fields, but not at peaceful gates
    return hot;
}

Vector2 Simulation::PirateSpawnPos(const std::vector<Vector2>& pool, const Vector2* avoid)
{
    Vector2 base = pool[RandRange(0, (int)pool.size() - 1)];
    Vector2 pos = base;
    for (int attempt = 0; attempt < 4; attempt++)
    {
        pos = { base.x + RandRange(-450, 450), base.y + RandRange(-450, 450) };
        if (avoid == nullptr)
            break;  // no one to avoid (background/hydrate)
        float dx = pos.x - avoid->x, dy = pos.y - avoid->y;
        if (dx * dx + dy * dy >= SPAWN_MIN_PLAYER_DIST * SPAWN_MIN_PLAYER_DIST)
            break;  // far enough from the player
    }
    return pos;
}

void Simulation::SpawnNpcInto(SystemState& st, Vector2 pos, FactionId faction, NpcRole role,
                              std::vector<Vector2> waypoints)
{
    auto npc = std::make_unique<NpcShip>(pos, faction, role, std::move(waypoints));
    npc->SetId(NextAgentId());
    st.entities.push_back(std::move(npc));
}

// Spawn director: tops up a system's population to targets by security/controller,
// accounting for the "pressure" from losses. A pirate system loses trade/police; a strong
// lawful neighbor sends reinforcements (police), which leads to reconquest.
void Simulation::TopUpSystem(SystemState& st, const Vector2* avoid)
{
    SpawnNodes           nd = GatherNodes(st);
    std::vector<Vector2> lanes = nd.stations;
    lanes.insert(lanes.end(), nd.gates.begin(), nd.gates.end());
    std::vector<Vector2> patrolRoute = lanes;
    patrolRoute.insert(patrolRoute.end(), nd.fields.begin(), nd.fields.end());
    std::vector<Vector2> hot = PirateSpots(nd);

    const SystemAggregate& agg = st.agg;
    float                  sec = agg.security;
    FactionId              ctrl = agg.controller;
    bool                   lawful = Factions::IsLawful(ctrl);

    // Police faction and its target. A pirate system has no police UNLESS there is a
    // strong lawful neighbor — then it sends reinforcements.
    FactionId policeFac = lawful ? ctrl : FactionId::TradersGuild;
    int       policeTarget = lawful ? (int)roundf(sec * 4.0f) : 0;
    if (ctrl == FactionId::Pirates)
        for (const std::string& nid : Neighbors(st.id))
        {
            auto n = systems_.find(nid);
            if (n != systems_.end() && Factions::IsLawful(n->second.agg.controller) &&
                n->second.agg.security > 0.6f && n->second.agg.police >= 3.0f)
            {
                policeFac = n->second.agg.controller;
                policeTarget = 4;  // neighbor's reinforcements
                break;
            }
        }

    int piratesTarget =
        (ctrl == FactionId::Pirates) ? 6 : std::max(0, (int)roundf((0.7f - sec) * 8.0f));
    int       tradersTarget = (ctrl == FactionId::Pirates) ? 1 : (int)roundf(2.0f + sec * 4.0f);
    int       minersTarget = 2;
    FactionId tradeFac = lawful ? ctrl : FactionId::Independent;

    // "Pressure": recent losses temporarily cut the target (recovers in MaintainWorld).
    tradersTarget = (int)roundf(tradersTarget * (1.0f - agg.supTraders));
    minersTarget = (int)roundf(minersTarget * (1.0f - agg.supMiners));
    policeTarget = (int)roundf(policeTarget * (1.0f - agg.supPolice));
    piratesTarget = (int)roundf(piratesTarget * (1.0f - agg.supPirates));

    // Current population by role.
    int tr = 0, mi = 0, po = 0, pi = 0;
    for (auto& e : st.entities)
        if (NpcShip* n = dynamic_cast<NpcShip*>(e.get()))
            switch (n->GetRole())
            {
                case NpcRole::Trader: tr++; break;
                case NpcRole::Miner: mi++; break;
                case NpcRole::Police: po++; break;
                case NpcRole::Pirate: pi++; break;
                case NpcRole::Warship: po++; break;
            }

    auto pick = [&](const std::vector<Vector2>& v) -> Vector2
    { return v[RandRange(0, (int)v.size() - 1)]; };

    // Top up by no more than a couple of units per step (smoothly, no bursts).
    int  budget = 2;
    auto canSpawn = [&]() { return budget > 0; };

    if (lanes.size() >= 2)
        while (tr < tradersTarget && canSpawn())
        {
            SpawnNpcInto(st, pick(lanes), tradeFac, NpcRole::Trader, lanes);
            tr++;
            budget--;
        }
    if (!nd.fields.empty())
        while (mi < minersTarget && canSpawn())
        {
            Vector2              spot = pick(nd.fields);
            std::vector<Vector2> near = { spot };
            SpawnNpcInto(st, spot, FactionId::Independent, NpcRole::Miner, near);
            mi++;
            budget--;
        }
    if (patrolRoute.size() >= 2)
        while (po < policeTarget && canSpawn())
        {
            SpawnNpcInto(st, pick(patrolRoute), policeFac, NpcRole::Police, patrolRoute);
            po++;
            budget--;
        }
    if (!hot.empty())
        while (pi < piratesTarget && canSpawn())
        {
            Vector2              spot = PirateSpawnPos(hot, avoid);
            std::vector<Vector2> patrol = { spot };
            SpawnNpcInto(st, spot, FactionId::Pirates, NpcRole::Pirate, patrol);
            pi++;
            budget--;
        }
}

void Simulation::MaintainWorld(float dt, const std::string& activeId, const Vector2* activeAvoid)
{
    // The simulation clock lives here because MaintainWorld is called once per tick by
    // every driver of the world -- the host loop and the batch mode -- which makes it the
    // one place elapsed time can accumulate without being counted twice or not at all.
    time_ += dt;
    maintAccum_ += dt;
    while (maintAccum_ >= MAINT_STEP)
    {
        // Recount the real populations; accumulate losses since the last step as "pressure".
        for (auto& kv : systems_)
        {
            SystemAggregate& a = kv.second.agg;
            float prevTr = a.traders, prevMi = a.miners, prevPo = a.police, prevPi = a.pirates;
            RecountAgg(kv.second);
            AccumSuppress(a.supTraders, prevTr - a.traders);
            AccumSuppress(a.supMiners, prevMi - a.miners);
            AccumSuppress(a.supPolice, prevPo - a.police);
            AccumSuppress(a.supPirates, prevPi - a.pirates);
        }
        StepWorldMacro();
        for (auto& kv : systems_)
        {
            // Recovery: the pressure slowly subsides.
            SystemAggregate& a = kv.second.agg;
            a.supTraders *= SUPPRESS_DECAY;
            a.supMiners *= SUPPRESS_DECAY;
            a.supPolice *= SUPPRESS_DECAY;
            a.supPirates *= SUPPRESS_DECAY;
            // player-avoidance — only in the active system (where the player is).
            const Vector2* avoid = (kv.first == activeId) ? activeAvoid : nullptr;
            TopUpSystem(kv.second, avoid);
        }
        maintAccum_ -= MAINT_STEP;
    }
}

// Materialize (hydrate) the NPCs of a system from its cold aggregate: as many ships of
// each role as the aggregate "accumulated" — that many we spawn in suitable places.
void Simulation::HydrateSystem(SystemState& st)
{
    // Stable ids for the static objects (stations/planets/gates/fields) from JSON —
    // for snapshots/selection over the network. NPCs get an id on spawn (SpawnNpcInto).
    for (auto& e : st.entities)
        if (e->GetId() == 0)
            e->SetId(NextAgentId());

    SpawnNodes             nd = GatherNodes(st);
    const SystemAggregate& agg = st.agg;

    // The controlling faction holds the law; if control is with pirates/no one — fall back
    // to the station's faction, otherwise the Guild.
    FactionId owner = agg.controller;
    if (!Factions::IsLawful(owner))
    {
        owner = FactionId::TradersGuild;
        for (auto& e : st.entities)
            if (Station* s = dynamic_cast<Station*>(e.get()))
                if (Factions::IsLawful(s->GetFaction()))
                {
                    owner = s->GetFaction();
                    break;
                }
    }

    auto pick = [&](const std::vector<Vector2>& v) -> Vector2
    { return v[RandRange(0, (int)v.size() - 1)]; };

    // Traders cruise the lanes (stations + gates).
    std::vector<Vector2> lanes = nd.stations;
    lanes.insert(lanes.end(), nd.gates.begin(), nd.gates.end());
    if (lanes.size() >= 2)
    {
        FactionId tradeFactions[] = { owner, FactionId::Independent, FactionId::TradersGuild };
        int       traders = (int)roundf(agg.traders);
        for (int i = 0; i < traders; i++)
            SpawnNpcInto(st, pick(lanes), tradeFactions[i % 3], NpcRole::Trader, lanes);
    }

    // Miners — at the fields.
    if (!nd.fields.empty())
    {
        int miners = (int)roundf(agg.miners);
        for (int i = 0; i < miners; i++)
        {
            Vector2              spot = pick(nd.fields);
            std::vector<Vector2> near = { spot };
            Vector2 start = { spot.x + RandRange(-200, 200), spot.y + RandRange(-200, 200) };
            SpawnNpcInto(st, start, FactionId::Independent, NpcRole::Miner, near);
        }
    }

    // The owner's police patrol the whole system (lanes + fields).
    std::vector<Vector2> patrolRoute = lanes;
    patrolRoute.insert(patrolRoute.end(), nd.fields.begin(), nd.fields.end());
    if (patrolRoute.size() >= 2)
    {
        int police = (int)roundf(agg.police);
        for (int i = 0; i < police; i++)
            SpawnNpcInto(st, pick(patrolRoute), owner, NpcRole::Police, patrolRoute);
    }

    // Pirates — on the dark periphery and at gates into dangerous systems (not at peaceful gates).
    std::vector<Vector2> hot = PirateSpots(nd);
    if (!hot.empty())
    {
        int pirates = (int)roundf(agg.pirates);
        for (int i = 0; i < pirates; i++)
        {
            Vector2              spot = PirateSpawnPos(hot, nullptr);
            std::vector<Vector2> patrol = { spot };
            SpawnNpcInto(st, spot, FactionId::Pirates, NpcRole::Pirate, patrol);
        }
    }
}

void Simulation::MaterializeAllSystems(const std::string& systemsDir)
{
    for (const auto& info : universe_.systems)
    {
        SystemState& st = systems_[info.id];  // created by InitGalaxy (aggregate already exists)
        st.id = info.id;
        if (st.entities.empty())
            st.entities = WorldLoader::LoadSystem(systemsDir + info.file);
        if (!st.populated)
        {
            HydrateSystem(st);
            st.populated = true;
        }
    }
}

// World snapshot of a system for the client: each entity -> id/kind/position/size and
// (for NPCs) faction/role/heading/hull fraction. The player and shots are added by the client.
Proto::Snapshot Simulation::BuildSnapshot(const std::string& systemId) const
{
    Proto::Snapshot snap;
    snap.systemId = systemId;
    auto it = systems_.find(systemId);
    if (it == systems_.end())
        return snap;

    for (const auto& e : it->second.entities)
    {
        Proto::EntitySnapshot es;
        es.id = e->GetId();
        es.pos = e->GetPosition();
        es.size = e->GetSize();
        es.name = e->GetName();

        if (NpcShip* n = dynamic_cast<NpcShip*>(e.get()))
        {
            es.kind = Proto::EntityKind::Npc;
            es.faction = n->GetFaction();
            es.role = (int)n->GetRole();
            es.heading = n->GetHeading();
            es.hullFrac = n->GetMaxHull() > 0.0f ? n->GetHull() / n->GetMaxHull() : 0.0f;
        }
        else if (Station* s = dynamic_cast<Station*>(e.get()))
        {
            es.kind = Proto::EntityKind::Station;
            es.faction = s->GetFaction();
        }
        else if (dynamic_cast<Star*>(e.get()) != nullptr)
            es.kind = Proto::EntityKind::Star;
        else if (dynamic_cast<Planet*>(e.get()) != nullptr)
            es.kind = Proto::EntityKind::Planet;
        else if (dynamic_cast<JumpGate*>(e.get()) != nullptr)
            es.kind = Proto::EntityKind::Gate;
        else if (AsteroidField* af = dynamic_cast<AsteroidField*>(e.get()))
        {
            es.kind = Proto::EntityKind::Field;
            es.ore = (int)af->GetResource();
        }
        else if (dynamic_cast<Nebula*>(e.get()) != nullptr)
            es.kind = Proto::EntityKind::Nebula;
        else if (dynamic_cast<Derelict*>(e.get()) != nullptr)
            es.kind = Proto::EntityKind::Derelict;

        snap.entities.push_back(es);
    }

    // System market prices (for the client's station screen) — AllResourceTypes order.
    for (ResourceType rt : AllResourceTypes())
        snap.marketPrices.push_back((float)it->second.market.GetPrice(rt));

    // Physical view of the player ship (for the networked client — authoritative on the
    // server; in single-player the client augments the view with its own weaponOn/docked/nearby
    // fields).
    if (player_)
    {
        Proto::PlayerView& p = snap.player;
        p.pos = player_->GetPosition();
        p.vel = player_->GetVelocity();
        p.heading = player_->GetHeading();
        p.hull = player_->GetHull();
        p.maxHull = player_->GetMaxHull();
        p.shields = player_->GetShields();
        p.maxShields = player_->GetMaxShields();
        p.cargoUsed = player_->GetCargoUsed();
        p.cargoCap = player_->GetCargoCapacity();
        p.warpPhase = (int)player_->GetWarpPhase();
        p.warpAlign = player_->GetWarpAlignTimer();
        p.warpTarget = player_->GetWarpTarget();
        p.warpDrop = player_->GetWarpDrop();
        p.autopilot = player_->IsAutopilotOn();
        p.apTarget = player_->GetAutopilotTarget();
        p.apStop = player_->GetAutopilotStopDistance();
        p.docked = (playerDockedStationId_ != 0);
        p.dockedStationId = playerDockedStationId_;
        p.stabilizer = player_->IsStabilizerOn();
        p.mining = player_->IsMiningOn();
        for (ResourceType rt : AllResourceTypes())
            p.cargoByType.push_back(player_->GetCargoAmount(rt));

        // Player account (server-authoritative, M4f): the client shows it as a mirror.
        p.money = account_.GetMoney();
        for (int i = 0; i < 4; i++)
        {
            p.reputation.push_back(account_.GetReputation((FactionId)i));
            p.bounty.push_back(account_.GetBounty((FactionId)i));
        }
        p.skillXp = { (float)account_.GetSkills().GetXp(SkillType::Piloting),
                      (float)account_.GetSkills().GetXp(SkillType::Mining),
                      (float)account_.GetSkills().GetXp(SkillType::Trading) };
    }

    // Missions (M4f-2): the board — only when docked, active ones — always. completable
    // is computed by the server (account/cargo/docking are authoritative).
    auto toView = [this](const Mission& m)
    {
        Proto::MissionView v;
        v.type = (int)m.type;
        v.faction = (int)m.faction;
        v.title = m.title;
        v.description = m.description;
        v.giverStationId = m.giverStationId;
        v.destStationId = m.destStationId;
        v.resource = (int)m.resource;
        v.targetCount = m.targetCount;
        v.progress = m.progress;
        v.rewardMoney = m.rewardMoney;
        v.rewardRep = m.rewardRep;
        v.completable = MissionCompletableNow(m);
        return v;
    };
    if (IsPlayerDocked())
        for (const Mission& m : missions_.Offers())
            snap.missionOffers.push_back(toView(m));
    for (const Mission& m : missions_.Active())
        snap.missionActive.push_back(toView(m));

    return snap;
}

Proto::SystemLayout Simulation::BuildLayout(const std::string& systemId) const
{
    Proto::SystemLayout lay;
    lay.systemId = systemId;
    auto it = systems_.find(systemId);
    if (it == systems_.end())
        return lay;

    for (const auto& e : it->second.entities)
    {
        // NPCs — dynamics (created by the client from the snapshot), not in the static layout.
        if (dynamic_cast<NpcShip*>(e.get()) != nullptr)
            continue;

        Proto::EntityLayout el;
        el.id = e->GetId();
        el.pos = e->GetPosition();
        el.size = e->GetSize();
        el.color = e->GetColor();
        el.name = e->GetName();

        if (Star* s = dynamic_cast<Star*>(e.get()))
        {
            el.kind = Proto::EntityKind::Star;
            el.subType = (int)s->GetStarType();
        }
        else if (Planet* p = dynamic_cast<Planet*>(e.get()))
        {
            el.kind = Proto::EntityKind::Planet;
            el.subType = (int)p->GetPlanetType();
            el.orbitRadius = p->GetOrbitRadius();
            el.resource = (int)p->GetDeposit();
        }
        else if (Station* st = dynamic_cast<Station*>(e.get()))
        {
            el.kind = Proto::EntityKind::Station;
            el.faction = st->GetFaction();
            el.subType = (int)st->GetRole();
        }
        else if (AsteroidField* af = dynamic_cast<AsteroidField*>(e.get()))
        {
            el.kind = Proto::EntityKind::Field;
            el.resource = (int)af->GetResource();
        }
        else if (JumpGate* g = dynamic_cast<JumpGate*>(e.get()))
        {
            el.kind = Proto::EntityKind::Gate;
            el.dest = g->GetDestination();
        }
        else if (dynamic_cast<Nebula*>(e.get()) != nullptr)
        {
            el.kind = Proto::EntityKind::Nebula;
        }
        else if (Derelict* dr = dynamic_cast<Derelict*>(e.get()))
        {
            el.kind = Proto::EntityKind::Derelict;
            el.reward = dr->GetReward();
        }
        else
        {
            continue;  // unknown type — skip
        }

        lay.entities.push_back(el);
    }
    return lay;
}

Proto::GalaxyState Simulation::BuildGalaxyState()
{
    Proto::GalaxyState gs;
    for (auto& kv : systems_)
    {
        RecountAgg(kv.second);  // refresh the population from live entities
        const SystemAggregate&  a = kv.second.agg;
        Proto::GalaxySystemStat g;
        g.id = kv.first;
        g.security = a.security;
        g.pirates = (int)(a.pirates + 0.5f);
        g.prosperity = a.prosperity;
        g.controller = a.controller;
        gs.systems.push_back(std::move(g));
    }
    gs.events = events_;
    return gs;
}

// Server world persistence: the galaxy (system aggregates) + time. The sup* "pressure"
// field is transient — not written (on load the world has "rested").
void Simulation::SaveWorld(const std::string& path) const
{
    using nlohmann::json;
    json j;
    j["simTime"] = time_;
    json galaxy = json::object();
    for (const auto& kv : systems_)
    {
        const SystemAggregate& a = kv.second.agg;
        galaxy[kv.first] = { { "traders", a.traders },       { "miners", a.miners },
                             { "police", a.police },         { "pirates", a.pirates },
                             { "security", a.security },     { "baseSecurity", a.baseSecurity },
                             { "prosperity", a.prosperity }, { "controller", (int)a.controller } };
    }
    j["galaxy"] = galaxy;

    std::ofstream out(path);
    if (out.is_open())
        out << j.dump(2) << "\n";
}

bool Simulation::LoadWorld(const std::string& path)
{
    using nlohmann::json;
    std::ifstream in(path);
    if (!in.is_open())
        return false;
    json j = json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.contains("galaxy") || !j["galaxy"].is_object())
        return false;

    Reset();
    InitGalaxy();  // skeletons with default aggregates for all systems
    time_ = j.value("simTime", 0.0);
    for (auto it = j["galaxy"].begin(); it != j["galaxy"].end(); ++it)
    {
        const std::string& sid = it.key();
        if (!HasSystem(sid))
            continue;
        SystemAggregate& a = systems_[sid].agg;
        const json&      gj = it.value();
        a.traders = gj.value("traders", a.traders);
        a.miners = gj.value("miners", a.miners);
        a.police = gj.value("police", a.police);
        a.pirates = gj.value("pirates", a.pirates);
        a.security = gj.value("security", a.security);
        a.baseSecurity = gj.value("baseSecurity", a.baseSecurity);
        a.prosperity = gj.value("prosperity", a.prosperity);
        a.controller = (FactionId)gj.value("controller", (int)a.controller);
        a.seeded = true;
    }
    return true;
}

void Simulation::SaveAccount(const std::string& path) const
{
    using nlohmann::json;
    json j;
    j["money"] = account_.GetMoney();

    json rep = json::array();
    for (int i = 0; i < 4; i++)
        rep.push_back(account_.GetReputation((FactionId)i));
    j["reputation"] = rep;

    json bounty = json::array();
    for (int i = 0; i < 4; i++)
        bounty.push_back(account_.GetBounty((FactionId)i));
    j["bounty"] = bounty;

    const Skills& sk = account_.GetSkills();
    j["skills"] = { { "piloting", sk.GetXp(SkillType::Piloting) },
                    { "mining", sk.GetXp(SkillType::Mining) },
                    { "trading", sk.GetXp(SkillType::Trading) } };

    std::ofstream out(path);
    if (out.is_open())
        out << j.dump(2) << "\n";
}

bool Simulation::LoadAccount(const std::string& path)
{
    using nlohmann::json;
    std::ifstream in(path);
    if (!in.is_open())
        return false;
    json j = json::parse(in, nullptr, false);
    if (j.is_discarded())
        return false;

    account_.SetMoney(j.value("money", 500.0));
    if (j.contains("reputation"))
        for (int i = 0; i < 4 && i < (int)j["reputation"].size(); i++)
            account_.SetReputation((FactionId)i, (float)j["reputation"][i]);
    if (j.contains("bounty"))
        for (int i = 0; i < 4 && i < (int)j["bounty"].size(); i++)
            account_.SetBounty((FactionId)i, (double)j["bounty"][i]);
    if (j.contains("skills"))
    {
        Skills& sk = account_.GetSkills();
        sk.SetXp(SkillType::Piloting, (float)j["skills"].value("piloting", 0));
        sk.SetXp(SkillType::Mining, (float)j["skills"].value("mining", 0));
        sk.SetXp(SkillType::Trading, (float)j["skills"].value("trading", 0));
    }
    return true;
}

void Simulation::Activate(const std::string& id)
{
    auto it = systems_.find(id);
    if (it == systems_.end())
        return;
    active_ = &it->second;  // pointer in std::map is stable
    activeId_ = id;
}

void Simulation::Reset()
{
    systems_.clear();
    active_ = nullptr;
    activeId_.clear();
    agentIdCounter_ = 0;
}

const WorldLoader::SystemInfo* Simulation::ActiveInfo() const
{
    for (const auto& s : universe_.systems)
        if (s.id == activeId_)
            return &s;
    return nullptr;
}
