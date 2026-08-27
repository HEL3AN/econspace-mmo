// Everything the server does on behalf of one player: the verbs (fire, mine, loot, sell,
// dock, jump, refit, buy), the account behind them, and the mission board.
//
// One translation unit of Simulation (#17). It is also the preparation for #3: what a
// per-connection session would own is collected here first, instead of being scattered
// through the same file as the world and the NPCs.

#include "sim/Simulation.h"
#include "sim/PlayerStep.h"
#include "sim/SimTuning.h"

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

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>

namespace
{
// Player combat (server logic). The ranges are shared and live elsewhere: the weapon's
// in Sim::PLAYER_WEAPON_RANGE, which the client also draws, and the mining laser's in
// Sim::MINING_RANGE, which the standing-order executor also reads.
constexpr float PLAYER_WEAPON_DAMAGE = 16.0f;  // player shot damage
constexpr float PLAYER_FIRE_INTERVAL = 0.5f;   // cooldown between player shots
}  // namespace

void Simulation::ServerRespawnPlayer()
{
    if (!player_)
        return;
    RecordEvent(Ev::Kind::ShipDestroyed, "Ship destroyed; respawned with cargo lost");
    for (auto& e : Active().entities)
        if (Station* st =
                e->GetKind() == EntityKind::Station ? static_cast<Station*>(e.get()) : nullptr)
        {
            player_->Teleport(st->GetPosition());
            break;
        }
    player_->Repair();
    player_->ClearCargo();
    player_->DisengageAutopilot();
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

bool Simulation::StepPlayerFire(SystemState& st, int targetId, float dt, PlayerCombatEvents* ev)
{
    if (playerFireTimer_ > 0.0f)
        playerFireTimer_ -= dt;
    if (!playerWeaponOn_ || !player_ || targetId == 0)
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
        AsteroidField* field =
            e->GetKind() == EntityKind::Field ? static_cast<AsteroidField*>(e.get()) : nullptr;
        if (field == nullptr || !field->HasOre())
            continue;

        float dx = field->GetPosition().x - sp.x;
        float dy = field->GetPosition().y - sp.y;
        if (std::sqrt(dx * dx + dy * dy) > field->GetSize() + Sim::MINING_RANGE)
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
            if (e->GetKind() != EntityKind::Gate)
                return std::string();
            const JumpGate* g = static_cast<const JumpGate*>(e.get());
            Vector2         sp = player_->GetPosition();
            float           dx = g->GetPosition().x - sp.x;
            float           dy = g->GetPosition().y - sp.y;
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
            Derelict* dr =
                e->GetKind() == EntityKind::Derelict ? static_cast<Derelict*>(e.get()) : nullptr;
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
            if (Station* station =
                    e->GetKind() == EntityKind::Station ? static_cast<Station*>(e.get()) : nullptr)
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
            if (Station* s =
                    e->GetKind() == EntityKind::Station ? static_cast<Station*>(e.get()) : nullptr)
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
        if (Station* s =
                e->GetKind() == EntityKind::Station ? static_cast<Station*>(e.get()) : nullptr)
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
    if (!player_ || player_->IsWarping())
        return 0;

    Vector2 pp = player_->GetPosition();
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
        // Hostile still admits. Reputation is authoritative here (account_) — the player
        // is told via the snapshot. Who owns a dock is still Station-specific state, so
        // this one narrowing stays until ownership becomes a component too (#41).
        if (e->GetKind() == EntityKind::Station)
        {
            FactionId sf = static_cast<Station*>(e.get())->GetFaction();
            if (Factions::TierOf(account_.GetReputation(sf)) == RepTier::Hated)
            {
                RecordEvent(Ev::Kind::Notice, "Docking denied: hostile reputation");
                return 0;
            }
        }
        player_->DisengageAutopilot();
        player_->Stop();
        playerDockedStationId_ = e->GetId();
        RecordEvent(Ev::Kind::Docked, "Docked at " + e->GetName());
        GenerateDockOffers();  // fresh station mission board (M4f-2)
        return playerDockedStationId_;
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
            if (JumpGate* g =
                    e->GetKind() == EntityKind::Gate ? static_cast<JumpGate*>(e.get()) : nullptr)
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
