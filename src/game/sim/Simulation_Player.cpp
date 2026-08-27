// Everything the server does on behalf of one player: the verbs (fire, mine, loot, sell,
// dock, jump, refit, buy), the account behind them, and the mission board.
//
// One translation unit of Simulation (#17). It is also the preparation for #3: what a
// per-connection session would own is collected here first, instead of being scattered
// through the same file as the world and the NPCs.

#include "sim/Simulation.h"
#include "sim/ClientSession.h"
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

void Simulation::ServerRespawnPlayer(ClientSession& s)
{
    if (!s.ship)
        return;
    s.RecordEvent(Ev::Kind::ShipDestroyed, "Ship destroyed; respawned with cargo lost");
    for (auto& e : SystemOf(s)->entities)
        if (Station* st =
                e->GetKind() == EntityKind::Station ? static_cast<Station*>(e.get()) : nullptr)
        {
            s.ship->Teleport(st->GetPosition());
            break;
        }
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
    // The step itself is shared with the client's prediction (sim/PlayerStep.h) — one
    // implementation, so predicted and authoritative movement cannot drift apart.
    if (s.ship)
        Sim::StepPlayerShip(*s.ship, cmd, pilotBonus, dt);
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
    // (pirate) or a serious crime (lawful). Applied directly to the server s.account (in
    // the network — authoritative; in single-player s.account is ignored, the client applies
    // its own s.ship from ev). We still fill ev (single-player/mission client).
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
            // Mission credit for the pirate into the server missions (in the network —
            // authoritative; in single-player s.missions is empty — progress goes into the
            // client MissionSystem via ev).
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
        float rate = s.ship->GetStats().miningRate * miningBonus;
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
    // Mining xp into the server account (in the network — authoritative; in single-player
    // ignored — the client credits its own s.ship from r.minedUnits).
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
            if (e->GetKind() != EntityKind::Gate)
                return std::string();
            const JumpGate* g = static_cast<const JumpGate*>(e.get());
            Vector2         sp = s.ship->GetPosition();
            float           dx = g->GetPosition().x - sp.x;
            float           dy = g->GetPosition().y - sp.y;
            if (std::sqrt(dx * dx + dy * dy) > g->GetSize() + 200.0f)
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
            Derelict* dr =
                e->GetKind() == EntityKind::Derelict ? static_cast<Derelict*>(e.get()) : nullptr;
            if (dr == nullptr || dr->IsLooted())
                return 0.0;
            Vector2 sp = s.ship->GetPosition();
            float   dx = dr->GetPosition().x - sp.x;
            float   dy = dr->GetPosition().y - sp.y;
            if (std::sqrt(dx * dx + dy * dy) > dr->GetSize() + 120.0f)
                return 0.0;
            dr->SetLooted();
            double reward = dr->GetReward();
            s.account.AddMoney(
                reward);  // in the network — authoritative; in single-player s.account is ignored
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
    ResourceType type = (ResourceType)resourceType;
    int          have = s.ship->GetCargoAmount(type);
    int          sold = amount < have ? amount : have;
    if (sold <= 0)
        return r;
    r.gross = st.market.Sell(type, sold);  // revenue at the current price + price sag
    s.ship->RemoveCargo(type, sold);
    r.sold = sold;

    // Net revenue into the server account: trading-skill multiplier + reputation of the
    // docked station's faction. In the network — authoritative; in single-player s.account
    // is ignored (the client computes its own from r.gross). Station faction — by
    // s.dockedStationId (server docking); in single-player it is 0, but the s.account branch is
    // unused there.
    FactionId sf = FactionId::Independent;
    for (auto& e : st.entities)
        if (e->GetId() == s.dockedStationId)
        {
            if (Station* station =
                    e->GetKind() == EntityKind::Station ? static_cast<Station*>(e.get()) : nullptr)
                sf = station->GetFaction();
            break;
        }
    float sellMul = 1.0f;
    switch (Factions::TierOf(s.account.GetReputation(sf)))
    {
        case RepTier::Hostile: sellMul = 0.85f; break;
        case RepTier::Liked: sellMul = 1.10f; break;
        case RepTier::Allied: sellMul = 1.20f; break;
        default: break;
    }
    double revenue = r.gross * s.account.GetSkills().GetBonus(SkillType::Trading) * sellMul;
    s.account.AddMoney(revenue);
    s.account.GetSkills().AddXp(SkillType::Trading, (float)(revenue * 0.05));
    s.account.AddReputation(sf, (float)(revenue * 0.002));
    r.revenue = revenue;
    return r;
}

void Simulation::RefitPlayer(ClientSession& s, const ShipStats& stats)
{
    if (s.ship)
        s.ship->Refit(stats);
}

void Simulation::StepPlayerAccountTick(ClientSession& s, float dt)
{
    // Piloting xp accrues in flight (not docked); the wanted level decays slowly.
    if (!s.IsDocked())
        s.account.GetSkills().AddXp(SkillType::Piloting, 4.0f * dt);
    s.account.DecayBounty(1.0 * dt);
}

void Simulation::PayBounty(ClientSession& s, FactionId faction)
{
    double b = s.account.GetBounty(faction);
    if (b > 0.0 && s.account.CanAfford(b))
    {
        s.account.AddMoney(-b);
        s.account.SetBounty(faction, 0.0);
    }
}

bool Simulation::BuyShip(ClientSession& s, int catalogIndex)
{
    const std::vector<ShipType>& catalog = GetShipCatalog();
    if (!s.ship || catalogIndex < 0 || catalogIndex >= (int)catalog.size())
        return false;

    // Price multiplier by the docked station's faction reputation (as on the client).
    FactionId sf = FactionId::Independent;
    for (auto& e : SystemOf(s)->entities)
        if (e->GetId() == s.dockedStationId)
        {
            if (Station* s =
                    e->GetKind() == EntityKind::Station ? static_cast<Station*>(e.get()) : nullptr)
                sf = s->GetFaction();
            break;
        }
    float buyMul = 1.0f;
    switch (Factions::TierOf(s.account.GetReputation(sf)))
    {
        case RepTier::Hostile: buyMul = 1.15f; break;
        case RepTier::Liked: buyMul = 0.92f; break;
        case RepTier::Allied: buyMul = 0.85f; break;
        default: break;
    }
    double price = catalog[catalogIndex].price * buyMul;
    if (!s.account.CanAfford(price))
        return false;
    s.account.AddMoney(-price);
    s.ship->Refit(catalog[catalogIndex].stats);
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

    // Reward multiplier by the station faction's reputation (as on the client in Dock).
    float repMul = 1.0f;
    switch (Factions::TierOf(s.account.GetReputation(giver->GetFaction())))
    {
        case RepTier::Hostile: repMul = 0.8f; break;
        case RepTier::Liked: repMul = 1.15f; break;
        case RepTier::Allied: repMul = 1.3f; break;
        default: break;
    }
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

bool Simulation::CompleteMission(ClientSession& s, int activeIndex)
{
    std::vector<Mission>& active = s.missions.Active();
    if (activeIndex < 0 || activeIndex >= (int)active.size())
        return false;
    const Mission& m = active[activeIndex];
    if (!MissionCompletableNow(s, m))
        return false;

    if (m.type == MissionType::Mining && s.ship)
        s.ship->RemoveCargo(m.resource, m.targetCount);
    s.account.AddMoney(m.rewardMoney);
    s.account.AddReputation(m.faction, m.rewardRep);
    active.erase(active.begin() + activeIndex);
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
        s.ship->Stop();
        s.dockedStationId = e->GetId();
        s.RecordEvent(Ev::Kind::Docked, "Docked at " + e->GetName());
        GenerateDockOffers(s);  // fresh station mission board (M4f-2)
        return s.dockedStationId;
    }
    return 0;
}

void Simulation::ServerEnterSystem(ClientSession& s, const std::string& destId,
                                   const std::string& fromId)
{
    if (!HasSystem(destId))
        return;
    s.systemId = destId;
    if (!s.ship)
        return;

    // Arrival point — at the gate leading back to the origin system (as on the client).
    Vector2 arrival = { 0.0f, 3000.0f };
    if (!fromId.empty())
        for (auto& e : SystemOf(s)->entities)
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
    s.ship->Teleport(arrival);
    s.ship->DisengageAutopilot();
    s.ship->CancelWarp();
    s.dockedStationId = 0;     // the jump releases the docking
    s.missions.ClearOffers();  // clear the board of the station we left; active missions
                               // survive the jump (they address stations by id) — otherwise
                               // Bounty/Delivery into another system would be uncompletable
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

        json cargo = json::array();
        for (ResourceType r : AllResourceTypes())
            if (int n = s.ship->GetCargoAmount(r))
                cargo.push_back({ { "resource", ResourceName(r) }, { "amount", n } });
        j["cargo"] = cargo;
    }

    // Missions address stations by stable id on purpose -- that is what lets one survive
    // a jump -- so ids are what gets written. A name would have to be resolved against a
    // galaxy that may have been edited since.
    json missions = json::array();
    for (const Mission& m : s.missions.Active())
        missions.push_back({ { "type", (int)m.type },
                             { "faction", (int)m.faction },
                             { "title", m.title },
                             { "description", m.description },
                             { "giver", m.giverStationId },
                             { "dest", m.destStationId },
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
    if (j.value("version", Save::UNVERSIONED) > Save::ACCOUNT_VERSION)
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
            if (pl.contains("pos") && pl["pos"].is_array() && pl["pos"].size() >= 2)
                s.ship->Teleport({ (float)pl["pos"][0], (float)pl["pos"][1] });
            s.ship->SetHeading((float)pl.value("heading", 0.0));
        }
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
            m.giverStationId = mj.value("giver", 0);
            m.destStationId = mj.value("dest", 0);
            m.resource = ResourceFromName(mj.value("resource", std::string()));
            m.targetCount = mj.value("target", 0);
            m.progress = mj.value("progress", 0);
            m.rewardMoney = mj.value("rewardMoney", 0.0);
            m.rewardRep = mj.value("rewardRep", 0.0f);
            active.push_back(std::move(m));
        }
        // The offer board is not restored: it belongs to the station the player was
        // docked at and is regenerated on the next dock.
        s.missions.SetMirror({}, std::move(active));
    }
    return Save::Result::Ok;
}
