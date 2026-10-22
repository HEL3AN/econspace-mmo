// The living galaxy: NPC behaviour, NPC combat, the spawn director and the macro step.
//
// One translation unit of Simulation (#17), not a separate class: these are the rules
// that run in every system whether or not anyone is watching, and they share the
// population aggregate the spawn director reads and writes.

#include "sim/Simulation.h"
#include "core/Archetype.h"
#include "core/Faction.h"
#include "core/Orbits.h"
#include "sim/ClientSession.h"

#include "core/World.h"
#include "entities/AsteroidField.h"
#include "entities/Combatant.h"
#include "entities/JumpGate.h"
#include "entities/Nebula.h"
#include "entities/NpcShip.h"
#include "entities/Ship.h"
#include "entities/Station.h"
#include "entities/Structure.h"

#include <algorithm>
#include <chrono>
#include <set>
#include <cmath>

namespace
{
// An NPC fights as a player does (owner, 2026-10-22, #186): its design's volley
// (Ships::Stats::volley) at the player's cooldown, from the player's range. What differs
// between a raider and a pilot is the fit, in data/ships.json.
constexpr float NPC_WEAPON_RANGE = Sim::PLAYER_WEAPON_RANGE;
constexpr float NPC_AGGRO_RANGE = 3500.0f;   // AI target detection radius
constexpr float NPC_THREAT_RANGE = 2000.0f;  // distance at which peaceful ones flee
constexpr float DEFENCE_PERIOD = 1.0f;       // seconds between a station's shots

// Spawn director. "Pressure": losses in a role raise the suppression of its spawn,
// which then slowly recovers — the player/battles really change the population.
constexpr float SUPPRESS_PER_KILL = 0.30f;        // suppression gain per 1 loss
constexpr float SUPPRESS_DECAY = 0.97f;           // suppression falloff per coarse step (~2 s)
constexpr float DANGER_SEC = 0.35f;               // below this the system is "unsafe" (ambushes)
constexpr float SPAWN_MIN_PLAYER_DIST = 2600.0f;  // do not spawn closer to the player
constexpr float MAINT_STEP = 2.0f;                // coarse world maintenance period
constexpr int   DIRECTOR_BUDGET = 2;              // ships the director adds to a system a pass

// A cold system's fights per pass (#295): how many seconds, per pirate and per ship of the
// other side, the one has the other under its guns. That is how often ships meet in a system
// a million units across -- geometry and behaviour, not guns -- and what a meeting costs is
// the guns' arithmetic: a side loses (seconds of fire) x (the other's dps) / (its own hull),
// with both from the designs the doctrines fly (Simulation::FighterOf), the numbers a hot
// system's ships fight with (one combat model, owner 2026-10-22, #186). A refit of the
// pirates' raider moves cold losses as it moves hot ones.
//
// Measured on the hot world -- seeds 3 and 7, two hours each, every system hot, 75 000
// system-passes, every death counted by role against the populations that met (27 traders,
// 4 miners, 2 pirates and one policeman in all that time) -- when every armed ship took off a
// ninth of its target's hull a second: the losses seen then, times nine.
constexpr float COLD_TRADER_FIRE = 5.4e-4f;   // pirate on trader
constexpr float COLD_MINER_FIRE = 5.4e-5f;    // pirate on miner
constexpr float COLD_PIRATE_FIRE = 1.35e-4f;  // police on pirate
constexpr float COLD_POLICE_FIRE = 6.3e-5f;   // pirate on police

// A system a gate into is an ambush spot: the pirates hold it, or its security is low.
bool Dangerous(const SystemAggregate& a)
{
    return a.controller == Factions::Pirates() || a.security < DANGER_SEC;
}

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

// Adds the wall time since `from` to `into` and restarts the stopwatch, when there is
// somewhere to add it (#295). Nothing is read from the clock when nobody asked.
using BenchClock = std::chrono::steady_clock;
void Lap(double* into, BenchClock::time_point& from)
{
    if (into == nullptr)
        return;
    const BenchClock::time_point now = BenchClock::now();
    *into += std::chrono::duration<double>(now - from).count();
    from = now;
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
        if (NpcShip* n = e->GetKind() == EntityKind::Npc ? static_cast<NpcShip*>(e.get()) : nullptr)
            if (n->IsAlive())
                ships.push_back(n);
    return ships;
}
}  // namespace

bool Simulation::NpcHostileToNpc(const SystemState& st, const NpcShip* a, const NpcShip* b) const
{
    // A siege's warships (#295, slice 5) and the garrison they face fight by the siege's
    // arithmetic, the same whether or not anyone is here to see it; shooting at each other on
    // top would make a watched siege go differently from an unwatched one. They are the
    // siege's face, and a player is the one thing in the system that can change its course.
    if (a->GetRole() == NpcRole::Warship || b->GetRole() == NpcRole::Warship)
        return false;
    // ...and so does a builder convoy (#342 S6): its losses are its plan's arithmetic.
    if (a->GetTask() != 0 || b->GetTask() != 0)
        return false;
    // A corporation's guard (#431 slice 12) has no faction to take sides by: its patrol's
    // rules decide, and whoever it would fight fights it back.
    if (a->GetRole() == NpcRole::Guard)
        return GuardEngages(st, *a, *b);
    if (b->GetRole() == NpcRole::Guard)
        return GuardEngages(st, *b, *a);
    Stance s = Factions::Relation(a->GetFaction(), b->GetFaction());
    return s == Stance::Hostile || s == Stance::War;
}

void Simulation::StepNpcAi(SystemState& st, const std::vector<PlayerPresence>& players)
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
            for (const PlayerPresence& p : players)
            {
                if (p.ship == nullptr || p.session == nullptr || !NpcHunts(st, *p.session, *n))
                    continue;
                float d = Dist(p.ship->GetPosition(), npos);
                if (d < bestD)
                {
                    bestD = d;
                    bestPos = p.ship->GetPosition();
                    found = true;
                }
            }
            for (NpcShip* o : ships)
            {
                if (o == n || !NpcHostileToNpc(st, n, o))
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
                if (o == n || !o->IsCombatant() || !NpcHostileToNpc(st, o, n))
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

void Simulation::StepNpcCombat(SystemState& st, const std::vector<PlayerPresence>& players,
                               std::vector<FireEvent>* fires)
{
    std::vector<NpcShip*> ships = AliveShips(st);
    for (NpcShip* npc : ships)
    {
        if (!npc->IsAlive() || !npc->IsCombatant() || !npc->ReadyToFire())
            continue;

        Vector2              npos = npc->GetPosition();
        float                bestD = NPC_WEAPON_RANGE;
        Combatant*           target = nullptr;
        const ClientSession* targetPlayer = nullptr;  // null means the target is an NPC
        NpcShip*             targetNpc = nullptr;

        auto consider = [&](Combatant* c, const ClientSession* player, NpcShip* other)
        {
            float d = Dist(c->GetPosition(), npos);
            if (d <= bestD)
            {
                bestD = d;
                target = c;
                targetPlayer = player;
                targetNpc = other;
            }
        };

        for (const PlayerPresence& p : players)
            if (p.ship != nullptr && p.session != nullptr && !p.hidden &&
                NpcHunts(st, *p.session, *npc))
                consider(p.ship, p.session, nullptr);
        for (NpcShip* other : ships)
            if (other != npc && other->IsAlive() && NpcHostileToNpc(st, npc, other))
                consider(other, nullptr, other);

        if (target == nullptr)
        {
            // A besieger with nothing to fight fires on the outpost it was sent against
            // (#295, slice 5). The beam is the siege's: what the outpost loses is the siege's
            // arithmetic, the same where nobody is, so the shot itself takes nothing off.
            if (npc->GetRole() != NpcRole::Warship || npc->GetTask() != 0 || fires == nullptr)
                continue;
            for (const auto& e : st.entities)
            {
                FactionId owner;
                if (e->GetKind() != EntityKind::Structure ||
                    !Outposts::FactionOf(e->GetOwner(), owner) ||
                    !Outposts::IsOutpost(static_cast<const Structure&>(*e).GetBuilds()))
                    continue;
                const Stance rel = Factions::Relation(npc->GetFaction(), owner);
                if ((rel != Stance::Hostile && rel != Stance::War) ||
                    Dist(e->GetPosition(), npos) > NPC_WEAPON_RANGE + e->GetSize())
                    continue;
                npc->ResetFireTimer();
                FireEvent fe;
                fe.from = npos;
                fe.to = e->GetPosition();
                fe.shooterFaction = npc->GetFaction();
                fe.targetEntityId = e->GetId();  // a hidden base besieged (#415)
                fires->push_back(fe);
                break;
            }
            continue;
        }

        // What a volley takes off is the design's guns (#279 step 4), the same against a ship
        // or a pilot, and the same a player's ship of that design would take off (#186).
        const int targetSession = targetPlayer != nullptr ? targetPlayer->id : 0;
        // A corporation's guard that fires first is its corporation's act, as a structure's
        // battery is its owner's (#340, #431 slice 12): a crime where the target is under law.
        const bool firstStrike =
            npc->GetRole() == NpcRole::Guard &&
            !GuardProvoked(st, *npc, targetSession, targetNpc != nullptr ? targetNpc->GetId() : 0);
        target->TakeDamage(npc->GetDamage());
        npc->ResetFireTimer();
        if (firstStrike)
            ChargeFirstStrike(st, *npc, targetPlayer, targetNpc, !target->IsAlive());
        if (targetPlayer != nullptr)  // the player's own guns nearby answer it (#340)
            ProvokeGuards(st, *targetPlayer, target->GetPosition(), 0, npc->GetId());
        else if (targetNpc != nullptr && targetNpc->GetRole() == NpcRole::Guard)
            ProvokePatrol(st, targetNpc->GetCorp(), target->GetPosition(), 0, npc->GetId());
        if (fires != nullptr)
        {
            FireEvent fe;
            fe.from = npos;
            fe.to = target->GetPosition();
            fe.shooterFaction = npc->GetFaction();
            fe.targetSessionId = targetSession;
            fires->push_back(fe);
        }
    }
}

void Simulation::StepStationDefence(SystemState& st, const std::vector<PlayerPresence>& players,
                                    std::vector<FireEvent>* fires, float dt)
{
    std::vector<NpcShip*> ships;  // gathered once, and only if something can shoot
    bool                  gathered = false;
    for (auto& e : st.entities)
    {
        // Who a battery shoots at depends on whose it is. A station is its faction's; a
        // structure (#39) is a faction's when a faction built it, and keeps its doctrine.
        // Otherwise it has no side to take: it fires as its owner's rules of engagement say
        // (#340) -- by default only back at whoever has been shooting at it. A site has no
        // guns yet.
        if (!e->Has(Component::Defensive))
            continue;
        FactionId      owner = Factions::Unaffiliated();
        bool           sided = false;
        const Defence* rules = nullptr;
        if (e->GetKind() == EntityKind::Station)
        {
            owner = static_cast<const Station*>(e.get())->GetFaction();
            sided = true;
        }
        else if (e->GetKind() == EntityKind::Structure)
        {
            const Structure& t = static_cast<const Structure&>(*e);
            if (t.IsBuilding())
                continue;
            sided = Outposts::FactionOf(e->GetOwner(), owner);
            if (!sided)
            {
                rules = &t.GetDefence();
                if (rules->engage == Engage::Hold)
                    continue;
            }
        }
        else
            continue;
        const Archetype* a = e->GetArchetype();
        float&           cooldown = st.defenceCooldown[e->GetId()];
        cooldown -= dt;
        if (cooldown > 0.0f || a->weaponDamage <= 0.0f)
            continue;

        if (!gathered)
        {
            ships = AliveShips(st);
            gathered = true;
        }
        // Who shot at it, or at its owner's ship within its reach (#340), lately.
        auto lately = [&](const std::map<int, std::map<int, double>>& book, int who)
        {
            const auto of = book.find(e->GetId());
            if (of == book.end())
                return false;
            const auto when = of->second.find(who);
            return when != of->second.end() && time_ < when->second;
        };
        auto                 shotAtUs = [&](int session) { return lately(st.provokedBy, session); };
        const Vector2        spos = e->GetPosition();
        float                bestD = e->GetSize() + a->weaponRange;
        Combatant*           target = nullptr;
        const ClientSession* targetPlayer = nullptr;
        NpcShip*             targetNpc = nullptr;
        bool                 firstStrike = false;  // not answering a shot: the owner's own choice

        auto consider = [&](Combatant* c, const ClientSession* player, NpcShip* npc, bool first)
        {
            float d = Dist(c->GetPosition(), spos);
            if (d <= bestD)
            {
                bestD = d;
                target = c;
                targetPlayer = player;
                targetNpc = npc;
                firstStrike = first;
            }
        };

        // A player's structure: whom its owner's rules name, never the owner. Who shot at it
        // lately is answered in every mode but hold fire; the rest is a first strike.
        const std::string& account = e->GetOwner();
        auto               enemyOfOwner = [&](FactionId f)
        {
            if (f == Factions::Pirates())
                return true;  // everyone's enemy, as they are to every player
            float standing = 0.0f;
            return OwnerStanding(account, f, standing) && standing < rules->threshold;
        };
        auto strikesFirst = [&](const ClientSession& p)
        {
            if (rules->engage == Engage::Strangers)
                return !rules->Allows(p.accountName);
            if (rules->engage == Engage::Enemies)
                for (FactionId f : Factions::All())
                    if (Factions::IsLawful(f) && p.account.IsWanted(f))
                        return true;  // wanted by the law: fair game
            return false;
        };
        auto strikesFirstAt = [&](FactionId f)
        {
            if (rules->engage == Engage::Strangers)
                return !rules->Allows(f);
            return rules->engage == Engage::Enemies && enemyOfOwner(f);
        };

        for (const PlayerPresence& p : players)
        {
            if (p.ship == nullptr || p.session == nullptr || p.hidden || !p.ship->IsAlive())
                continue;
            if (sided)
            {
                // A criminal only where security brings the guns in (#431 slice 9).
                if (GunsOn(st, *p.session, owner) || shotAtUs(p.session->id))
                    consider(p.ship, p.session, nullptr, false);
            }
            else if (account.empty() || p.session->accountName != account)
            {
                // A member of the owner's corporation is a friend in every mode (#347): never
                // fired on first, and on no list it has to be named on. A shot from one is
                // still answered -- the corporation is trust, not immunity.
                const bool friendOf = corps_.SameCorp(p.session->accountName, account);
                if (shotAtUs(p.session->id))
                    consider(p.ship, p.session, nullptr, false);
                else if (rules != nullptr && !friendOf && strikesFirst(*p.session))
                    consider(p.ship, p.session, nullptr, true);
            }
        }
        for (NpcShip* n : ships)
        {
            // Besiegers are the siege's arithmetic to count, not the battery's (#295).
            if (!n->IsAlive() || n->GetRole() == NpcRole::Warship || n->GetTask() != 0)
                continue;
            if (sided)
            {
                const Stance rel = Factions::Relation(owner, n->GetFaction());
                if (rel == Stance::Hostile || rel == Stance::War)
                    consider(n, nullptr, n, false);
            }
            else if (n->GetRole() == NpcRole::Guard && !n->GetCorp().empty() &&
                     n->GetCorp() == corps_.IdOf(account))
                continue;  // its owner's corporation's own patrol (#431 slice 12)
            else if (lately(st.provokedByNpc, n->GetId()))
                consider(n, nullptr, n, false);  // it shot the owner: answering, not first
            else if (rules != nullptr && strikesFirstAt(n->GetFaction()))
                consider(n, nullptr, n, true);
        }

        if (target == nullptr)
        {
            cooldown = 0.0f;  // ready the moment something comes into range
            continue;
        }
        target->TakeDamage(a->weaponDamage * DEFENCE_PERIOD);
        cooldown = DEFENCE_PERIOD;
        if (firstStrike)
            ChargeFirstStrike(st, *e, targetPlayer, targetNpc, !target->IsAlive());
        // A player's battery that hits another player's ship is its owner shooting: that
        // player's own guns nearby answer the owner, as they would the owner's ship (#340).
        if (!sided && targetPlayer != nullptr)
            for (const auto& kv : sessions_)
                if (!account.empty() && kv.second.accountName == account)
                    ProvokeGuards(st, *targetPlayer, target->GetPosition(), kv.second.id, 0);
        const int targetSession = targetPlayer != nullptr ? targetPlayer->id : 0;
        // A hidden base that fires on a pilot has given itself away to them (#415), before the
        // snapshot that shows the shot and the damage.
        if (targetSession != 0)
        {
            const auto victim = sessions_.find(targetSession);
            if (victim != sessions_.end())
                RevealByFire(victim->second, st, *e);
        }
        if (fires != nullptr)
        {
            FireEvent fe;
            fe.from = spos;
            fe.to = target->GetPosition();
            fe.shooterFaction = owner;
            fe.targetSessionId = targetSession;
            fe.shooterEntityId = e->GetId();
            fires->push_back(fe);
        }
    }
}

void Simulation::ChargeFirstStrike(SystemState& st, const Entity& structure,
                                   const ClientSession* player, const NpcShip* npc, bool killed)
{
    // The crime is against whoever's law protects the target, by #41's table: a lawful
    // faction's ship is that faction's wherever it is; a player is protected by the law of the
    // system they are in, unless that law has no quarrel with firing on them -- they are
    // wanted by it, or it holds them hostile.
    FactionId victim;
    if (npc != nullptr)
    {
        victim = npc->GetFaction();
        if (!Factions::IsLawful(victim))
            return;
    }
    else if (player != nullptr)
    {
        if (!LawIn(st, victim) || AccountHostileToFaction(*player, victim) ||
            player->account.IsCriminal())
            return;
    }
    else
        return;
    const std::string& account = structure.GetOwner();
    ChargeOwner(account, victim, false);
    if (killed)
        ChargeOwner(account, victim, true);

    // Said to the owner once a minute, not once a volley.
    double& told = st.crimeToldUntil[structure.GetId()];
    if (time_ < told)
        return;
    told = time_ + PROVOKED_SECONDS;
    for (auto& kv : sessions_)
        if (kv.second.accountName == account)
            kv.second.RecordEvent(
                Ev::Kind::Notice,
                structure.GetName() + " in " + SystemName(st.id) + " is firing first on " +
                    (npc != nullptr ? FactionName(victim) + " ships" : player->accountName) +
                    ": a crime against " + FactionName(victim) + ", on your account");
}

// --- Law (#431 slice 9) ---
//
// A crime is answered by ships that exist: the system's own police, patrols its stations
// launch out of what the faction has to spare, and police lent by systems nearby -- each of
// them missing from where it came from until it is back, as a siege's force is (#295). There
// is no invincible police: the higher the security, the more come and the sooner, and a fleet
// big enough could in principle fight all of it.

bool Simulation::IsCrime(const SystemState& st, const ClientSession& by, const Entity& victim,
                         FactionId& law) const
{
    if (victim.GetKind() == EntityKind::Npc)
    {
        // A corporation's guard (#431 slice 12) is as safe as the system's law makes it, as
        // its members' structures are (LawOver) -- except from its own corporation.
        const NpcShip& n = static_cast<const NpcShip&>(victim);
        if (n.GetRole() == NpcRole::Guard)
            return corps_.IdOf(by.accountName) != n.GetCorp() && LawIn(st, law);
        law = n.GetFaction();
        return Factions::IsLawful(law);
    }
    if (victim.GetKind() == EntityKind::Structure)
        return LawOver(st, static_cast<const Structure&>(victim), by.accountName, law);
    if (victim.GetKind() == EntityKind::PlayerShip)
    {
        // Another pilot is under the law of the system they are in (#431 L1, L2): where a
        // lawful faction holds it and security is above zero. A pilot flagged a criminal, or
        // one that law holds as an enemy -- wanted by it, or hated -- is fair game.
        const ClientSession* pilot = PilotOf(victim.GetId());
        if (pilot == nullptr || pilot == &by || !LawIn(st, law) || st.agg.security <= 0.0f)
            return false;
        return !pilot->account.IsCriminal() && !AccountHostileToFaction(*pilot, law);
    }
    return false;
}

const ClientSession* Simulation::PilotOf(int shipId) const
{
    for (const auto& kv : sessions_)
        if (kv.second.ship && kv.second.ship->GetId() == shipId)
            return &kv.second;
    return nullptr;
}

ClientSession* Simulation::PilotTarget(const ClientSession& by, const SystemState& st, int shipId)
{
    for (auto& kv : sessions_)
    {
        ClientSession& p = kv.second;
        if (!p.ship || p.ship->GetId() != shipId)
            continue;
        if (&p == &by || p.systemId != st.id || p.IsDocked() || !p.ship->IsAlive() || InCover(p))
            return nullptr;
        return &p;
    }
    return nullptr;
}

void Simulation::MarkCrime(ClientSession& s, bool killed)
{
    const WorldRules::Law& r = WorldRules::LawRules();
    const bool             was = s.account.IsCriminal();
    s.account.SetCriminalLeft(r.flagSeconds);  // renewed by every crime, never stacked
    // As standing and bounty are charged: the hit, and the kill on top (ChargeAttack).
    s.account.AddSecurityStatus(-(float)(r.statusCrime + (killed ? r.statusKill : 0.0)));
    if (!was)
        s.RecordEvent(Ev::Kind::UnderAttack,
                      "You are flagged a criminal for " +
                          std::to_string((int)std::lround(r.flagSeconds / 60.0)) +
                          " min: anyone may shoot you without a crime, and lawful docks refuse "
                          "you");
}

void Simulation::StepPilotLaw(ClientSession& s, float dt)
{
    if (dt <= 0.0f || s.Lingering())
        return;  // the flag runs down only while the pilot is here (Decided after the rounds, 6)
    if (s.account.IsCriminal())
    {
        s.account.SetCriminalLeft(s.account.CriminalLeft() - dt);
        if (!s.account.IsCriminal())
            s.RecordEvent(Ev::Kind::Notice, "Your criminal flag has lapsed");
        return;
    }
    const float status = s.account.GetSecurityStatus();
    if (status < 0.0f)
        s.account.SetSecurityStatus(
            std::min(0.0f, status + (float)(WorldRules::LawRules().statusPerDay * dt / 86400.0)));
}

bool Simulation::PoliceOnSight(const SystemState& st, const ClientSession& p,
                               const NpcShip& police) const
{
    const WorldRules::Law& r = WorldRules::LawRules();
    return police.GetRole() == NpcRole::Police && Factions::IsLawful(police.GetFaction()) &&
           st.agg.security >= r.onSightFrom &&
           p.account.GetSecurityStatus() <= -(float)r.onSightBelow;
}

bool Simulation::NpcHunts(const SystemState& st, const ClientSession& p, const NpcShip& n) const
{
    if (n.GetRole() == NpcRole::Guard)  // a corporation's, by its patrol's rules (slice 12)
        return GuardHunts(st, n, p);
    if (AccountHostileToFaction(p, n.GetFaction()) || PoliceOnSight(st, p, n))
        return true;
    // A flagged criminal is anyone's to shoot (#431 L4), and the police's to chase where the
    // law holds at all.
    return p.account.IsCriminal() && n.GetRole() == NpcRole::Police &&
           Factions::IsLawful(n.GetFaction()) && st.agg.security > 0.0f;
}

bool Simulation::GunsOn(const SystemState& st, const ClientSession& p, FactionId owner) const
{
    // An enemy of the faction is one everywhere, as it is to the faction's ships.
    const RepTier t = Factions::TierOf(p.account.GetReputation(owner));
    if (owner == Factions::Pirates() || t == RepTier::Hostile || t == RepTier::Hated)
        return true;
    // A criminal only where the law is strong enough to bring the guns in: one this faction
    // wants, or a pilot flagged for a crime anywhere if it is a faction of the law (#431 L4).
    const bool criminal =
        p.account.IsWanted(owner) || (p.account.IsCriminal() && Factions::IsLawful(owner));
    return criminal && st.agg.security >= WorldRules::LawRules().gunsFrom;
}

int Simulation::TakePolice(SystemState& home, FactionId law, int want)
{
    // Ships the faction has here, and no more than the presence behind them: what it takes
    // away is missing from both, so the director does not make it good while it is gone.
    float&    presence = home.agg.presence[law];
    const int backed = (int)std::floor(presence + 1e-4f);
    int       take = 0;
    if (IsHot(home))
    {
        std::vector<NpcShip*> free;
        for (NpcShip* n : AliveShips(home))
            if (n->GetRole() == NpcRole::Police && n->GetFaction() == law && n->GetTask() == 0 &&
                !n->IsLeaving())
                free.push_back(n);
        take = std::max(0, std::min({ want, (int)free.size(), backed }));
        // The last made go first: the same ships on every server.
        std::sort(free.begin(), free.end(),
                  [](const NpcShip* a, const NpcShip* b) { return a->GetId() > b->GetId(); });
        free.resize((size_t)take);
        home.entities.erase(
            std::remove_if(home.entities.begin(), home.entities.end(),
                           [&](const std::unique_ptr<Entity>& e)
                           { return std::find(free.begin(), free.end(), e.get()) != free.end(); }),
            home.entities.end());
    }
    else if (home.agg.policeFaction == law)
        take = std::max(0, std::min({ want, (int)std::floor(home.agg.police + 1e-4f), backed }));
    // Off the count as they set out -- a recount does not read their going as a loss.
    home.agg.police = std::max(0.0f, home.agg.police - (float)take);
    presence = std::max(0.0f, presence - (float)take);
    return take;
}

int Simulation::CallNextDoor(SystemState& st, const ClientSession& by, FactionId law,
                             const WorldRules::Law::Band& band)
{
    if (band.reach <= 0 || band.called <= 0)
        return 0;
    // Every system within reach, nearest first, ties by id.
    std::vector<std::pair<int, std::string>> near;
    std::set<std::string>                    seen{ st.id };
    std::vector<std::string>                 ring{ st.id };
    for (int hop = 1; hop <= band.reach && !ring.empty(); hop++)
    {
        std::vector<std::string> next;
        for (const std::string& id : ring)
            for (const std::string& n : Neighbors(id))
                if (seen.insert(n).second)
                {
                    near.push_back({ hop, n });
                    next.push_back(n);
                }
        ring = std::move(next);
    }
    std::sort(near.begin(), near.end());

    const WorldRules::Law&  r = WorldRules::LawRules();
    std::vector<Plan::Lent> lent;
    int                     need = band.called, hops = 1, sent = 0;
    for (const auto& n : near)
    {
        SystemState* home = need > 0 ? SystemById(n.second) : nullptr;
        const int    took = home != nullptr ? TakePolice(*home, law, need) : 0;
        if (took <= 0)
            continue;
        lent.push_back({ n.second, (float)took, n.first });
        need -= took;
        sent += took;
        hops = std::max(hops, n.first);
    }
    if (lent.empty())
        return 0;
    Plan p;
    p.id = nextPlanId_++;
    p.kind = Plan::Kind::Response;
    p.faction = law;
    p.from = lent.front().from;
    p.target = st.id;
    p.startedAt = time_;
    p.arrivesAt = time_ + band.delay + hops * r.hop;
    p.dueAt = p.arrivesAt + r.stay;
    p.force = (float)sent;
    p.lent = lent;
    p.pursued = by.accountName;
    plans_[p.id] = p;
    due_.insert({ p.dueAt, p.id });
    return sent;
}

int Simulation::AnswerHere(SystemState& st, ClientSession& by, FactionId law,
                           const WorldRules::Law::Band& band)
{
    if (!by.ship)
        return 0;
    const WorldRules::Law& r = WorldRules::LawRules();
    const Vector2          scene = by.ship->GetPosition();
    // Around the criminal, inside the range at which a patrol engages, each from its own
    // bearing; the AI does the rest, as for any wanted ship in sight.
    int  dropped = 0;
    auto drop = [&](NpcShip& n)
    {
        const float turn = (float)dropped++ * 2.39996f;  // the golden angle: never stacked
        n.SetPosition({ scene.x + std::cos(turn) * (float)r.dropDistance,
                        scene.y + std::sin(turn) * (float)r.dropDistance });
        n.Engage(scene);
    };

    // The patrols that would act on this shooter -- whoever the crime made wanted by them --
    // nearest first. One already in sight is answering and needs no warp, but it is one of
    // the band's.
    std::vector<NpcShip*> free;
    int                   inSight = 0;
    for (NpcShip* n : AliveShips(st))
    {
        if (n->GetRole() != NpcRole::Police || n->GetTask() != 0 || n->IsLeaving() ||
            !NpcHunts(st, by, *n))
            continue;
        if (Dist(n->GetPosition(), scene) <= NPC_AGGRO_RANGE)
            inSight++;
        else
            free.push_back(n);
    }
    std::sort(free.begin(), free.end(),
              [&](const NpcShip* a, const NpcShip* b)
              {
                  const float da = Dist(a->GetPosition(), scene);
                  const float db = Dist(b->GetPosition(), scene);
                  return da != db ? da < db : a->GetId() < b->GetId();
              });
    free.resize((size_t)std::min((int)free.size(), std::max(0, band.local - inSight)));
    for (NpcShip* n : free)
        drop(*n);
    int came = (int)free.size();

    // A station of the law launches patrols on top: out of the faction's strength here that
    // is not flying yet, then bought out of its stock. Only the system's own police -- the
    // director flies every lawful faction's strength here under one flag.
    if (band.launch <= 0 || st.agg.policeFaction != law || !Factions::IsLawful(law))
        return came;
    bool station = false;
    for (const auto& e : st.entities)
        station = station || (e->GetKind() == EntityKind::Station &&
                              static_cast<const Station&>(*e).GetFaction() == law);
    if (!station)
        return came;
    int flying = 0;
    for (NpcShip* n : AliveShips(st))
        if (n->GetRole() == NpcRole::Police && n->GetFaction() == law && n->GetTask() == 0 &&
            !n->IsLeaving())
            flying++;
    float&    presence = st.agg.presence[law];
    const int spare =
        std::max(0, (int)std::floor(presence - SiegeForceIn(st.id, law) - (float)flying + 1e-4f));
    int         launch = std::min(band.launch, spare);
    const float price = (float)r.launchCost * OutpostCost();
    float&      stock = minds_[law].stock;
    while (launch < band.launch && price > 0.0f && stock >= price)
    {
        stock -= price;
        presence += 1.0f;  // bought: strength here like any other, and it fades like it
        launch++;
    }
    if (launch <= 0)
        return came;
    const SpawnNodes     nd = GatherNodes(st);
    std::vector<Vector2> beat = nd.stations;
    beat.insert(beat.end(), nd.gates.begin(), nd.gates.end());
    beat.insert(beat.end(), nd.fields.begin(), nd.fields.end());
    if (beat.empty())
        beat.push_back(scene);
    for (int i = 0; i < launch; i++)
    {
        SpawnNpcInto(st, scene, law, NpcRole::Police, beat);
        drop(static_cast<NpcShip&>(*st.entities.back()));
    }
    return came + launch;
}

bool Simulation::CallPolice(SystemState& st, ClientSession& by, FactionId law)
{
    // Only where there are ships to send (#295): a cold system has none, and nobody can be
    // shooting in one anyway -- whoever is shooting has made it hot.
    if (!IsHot(st) || !by.ship || time_ < st.policeCalledUntil)
        return false;
    const WorldRules::Law& r = WorldRules::LawRules();
    WorldRules::Law::Band  band;
    if (!r.BandFor(st.agg.security, band))
        return false;  // lawless in all but name: nobody comes
    st.policeCalledUntil = time_ + r.callSeconds;

    const int   called = CallNextDoor(st, by, law, band);
    std::string fromNextDoor;
    if (called > 0)
        fromNextDoor = ", " + std::to_string(called) + " more on their way from next door";
    if (band.delay > 0.0)
    {
        // The slower the lower the security: the answer from here comes later.
        st.policeDueAt = time_ + band.delay;
        st.policeFor = by.id;
        st.policeLaw = law;
        by.RecordEvent(Ev::Kind::UnderAttack,
                       FactionName(law) + " police are called to the crime: in about " +
                           std::to_string((int)std::lround(band.delay)) + " s" + fromNextDoor);
        return true;
    }
    const int here = AnswerHere(st, by, law, band);
    if (here == 0 && called == 0)
        return false;
    by.RecordEvent(Ev::Kind::UnderAttack, FactionName(law) + " police answer the crime: " +
                                              std::to_string(here) + " here" + fromNextDoor);
    return true;
}

void Simulation::StepPoliceCall(SystemState& st)
{
    if (st.policeDueAt <= 0.0 || time_ < st.policeDueAt)
        return;
    st.policeDueAt = 0.0;
    const auto            it = sessions_.find(st.policeFor);
    WorldRules::Law::Band band;
    // Whoever it was for has left, or docked: there is nobody to warp to.
    if (it == sessions_.end() || it->second.systemId != st.id || !it->second.ship ||
        it->second.IsDocked() || !WorldRules::LawRules().BandFor(st.agg.security, band))
        return;
    const int here = AnswerHere(st, it->second, st.policeLaw, band);
    if (here > 0)
        it->second.RecordEvent(Ev::Kind::UnderAttack,
                               FactionName(st.policeLaw) +
                                   " police answer the crime: " + std::to_string(here) + " here");
}

void Simulation::FieldResponse(SystemState& st, Plan& p)
{
    if (!IsHot(st) || p.fielded > 0 || !p.arrived)
        return;
    // Beside whoever they answer, if they are still here and in space; otherwise the police
    // wait out their time as numbers, and go home.
    const ClientSession* who = nullptr;
    for (const auto& kv : sessions_)
        if (kv.second.accountName == p.pursued && kv.second.systemId == st.id && kv.second.ship &&
            !kv.second.IsDocked())
            who = &kv.second;
    if (who == nullptr)
        return;
    const Vector2 scene = who->ship->GetPosition();
    const float   drop = (float)WorldRules::LawRules().dropDistance;
    const int     n = (int)std::lround(p.force);
    for (int i = 0; i < n; i++)
    {
        const float turn = (float)i * 2.39996f;
        SpawnNpcInto(st, { scene.x + std::cos(turn) * drop, scene.y + std::sin(turn) * drop },
                     p.faction, NpcRole::Police, { scene }, i);
        NpcShip& ship = static_cast<NpcShip&>(*st.entities.back());
        ship.SetTask(p.id);  // the Response's, counted by it and not by the system
        ship.Engage(scene);
    }
    p.fielded = n;
}

void Simulation::StepResponses()
{
    std::vector<int> over;
    for (auto& kv : plans_)
    {
        Plan& p = kv.second;
        if (p.kind != Plan::Kind::Response)
            continue;
        SystemState* st = SystemById(p.target);
        if (st == nullptr)
        {
            over.push_back(p.id);
            continue;
        }
        if (!p.arrived)
        {
            if (time_ >= p.arrivesAt)
            {
                p.arrived = true;
                FieldResponse(*st, p);
            }
            continue;
        }
        if (!IsHot(*st))
        {
            p.fielded = 0;
            continue;
        }
        // A policeman a player shot down is one fewer to go home.
        int alive = 0;
        for (const auto& e : st->entities)
            if (e->GetKind() == EntityKind::Npc &&
                static_cast<const NpcShip&>(*e).GetTask() == p.id &&
                static_cast<const NpcShip&>(*e).IsAlive())
                alive++;
        if (alive < p.fielded)
            p.force = std::max(0.0f, p.force - (float)(p.fielded - alive));
        p.fielded = alive;
        if (p.force < 0.5f)
            over.push_back(p.id);
    }
    for (int id : over)
    {
        const auto it = plans_.find(id);
        if (it == plans_.end())
            continue;
        RecallResponse(it->second);
        plans_.erase(it);  // its due entry finds nothing
    }
}

void Simulation::RecallResponse(const Plan& p)
{
    SystemState* st = SystemById(p.target);
    if (st != nullptr && IsHot(*st))
        st->entities.erase(std::remove_if(st->entities.begin(), st->entities.end(),
                                          [&](const std::unique_ptr<Entity>& e)
                                          {
                                              return e->GetKind() == EntityKind::Npc &&
                                                     static_cast<const NpcShip&>(*e).GetTask() ==
                                                         p.id;
                                          }),
                           st->entities.end());
    SendHome(p, WorldRules::LawRules().hop);
}

void Simulation::StepSystemAgents(SystemState& st, const std::vector<PlayerPresence>& players,
                                  std::vector<FireEvent>* fires, float dt)
{
    if (!IsHot(st))
        return;          // nobody to step: the coarse pass keeps a cold system (#295)
    StepPoliceCall(st);  // a slower answer to a crime, when its time comes (#431 slice 9)
    StepNpcAi(st, players);

    // Planets and what orbits them are where the clock says (#210), before anything this
    // tick asks where a station or a belt is.
    Orbits::Place(st.entities, time_);

    for (auto& e : st.entities)  // movement
        e->Update(dt);

    StepNpcCombat(st, players, fires);
    StepStationDefence(st, players, fires, dt);

    // Cleanup of the fallen. A hauler leaves what it carried in its wreck (#343).
    std::vector<const NpcShip*> wrecked;
    for (const auto& e : st.entities)
        if (e->GetKind() == EntityKind::Npc && !static_cast<const NpcShip&>(*e).IsAlive() &&
            static_cast<const NpcShip&>(*e).GetCargoAmount() > 0)
            wrecked.push_back(static_cast<const NpcShip*>(e.get()));
    for (const NpcShip* n : wrecked)
        LeaveCargoWreck(st, *n);
    st.entities.erase(std::remove_if(st.entities.begin(), st.entities.end(),
                                     [](const std::unique_ptr<Entity>& e)
                                     {
                                         NpcShip* n = e->GetKind() == EntityKind::Npc
                                                          ? static_cast<NpcShip*>(e.get())
                                                          : nullptr;
                                         return n != nullptr && !n->IsAlive();
                                     }),
                      st.entities.end());
}

// Where a system's security settles (#428): its base, moved by the balance of police and
// pirates by at most SECURITY_SWING either way. The balance is a share, not a difference of
// counts -- police come from a faction's presence, so a garrison of ten would otherwise push
// every policed system to 1.0 and the number would say nothing. On top, what its holder pays
// for police (#431 L5, slice 11): security is bought, and a system nobody pays for keeps what
// its place and its ships give it.
float Simulation::SecurityTarget(const SystemAggregate& a)
{
    const float police = std::max(0.0f, a.police), pirates = std::max(0.0f, a.pirates);
    const float balance = (police - pirates) / (police + pirates + SECURITY_BALANCE_SOFTNESS);
    const float paid = (float)WorldRules::SecurityRules().Swing(a.paidPolice);
    return ClampF(a.baseSecurity + SECURITY_SWING * balance + paid, 0.0f, 1.0f);
}

// M0 macro on REAL numbers (population in the aggregate is filled by Game::RecountAgg).
// Security/economy drift, economy diffusion, controller changes. The population is not
// changed — that is done by real battles and the Game spawn director.
void Simulation::StepWorldMacro()
{
    // The first passes after a start are the world settling into its numbers, not events
    // anybody should read about (#143).
    const bool settling = macroSteps_ < SETTLE_STEPS;
    macroSteps_++;
    if (marketsStale_)
        PriceMarkets();  // a mouth opened or collapsed: supply routes changed (#342 S5)
    // What holders pay for police this pass (#431 slice 11), before security drifts on it.
    StepPolicePay(MAINT_STEP);
    // ...and for their patrols (#431 slice 12).
    StepPatrols(MAINT_STEP);

    // Security and economy drift from the real populations.
    for (auto& kv : systems_)
    {
        SystemAggregate& a = kv.second.agg;
        a.security = ClampF(a.security + (SecurityTarget(a) - a.security) * 0.05f, 0.0f, 1.0f);
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

    // Who reaches where, and who holds what: the same rule for every faction (#231).
    StepFactions();
    StepControl(settling);
}

void Simulation::RecountAgg(SystemState& st)
{
    if (!IsHot(st))
        return;
    int tr = 0, mi = 0, po = 0, pi = 0;
    for (auto& e : st.entities)
        if (NpcShip* n = e->GetKind() == EntityKind::Npc &&
                                 !static_cast<NpcShip&>(*e).IsLeaving() &&
                                 static_cast<NpcShip&>(*e).GetTask() == 0
                             ? static_cast<NpcShip*>(e.get())
                             : nullptr)
            switch (n->GetRole())
            {
                case NpcRole::Trader: tr++; break;
                case NpcRole::Miner: mi++; break;
                case NpcRole::Police: po++; break;
                case NpcRole::Pirate: pi++; break;
                case NpcRole::Warship: break;  // a siege's, kept by its plan (#295)
                case NpcRole::Guard: break;    // a corporation's patrol (#431 slice 12)
            }
    st.agg.traders = (float)tr;
    st.agg.miners = (float)mi;
    st.agg.police = (float)po;
    st.agg.pirates = (float)pi;
}

int Simulation::LivePirates(const SystemState& st)
{
    int n = 0;
    for (const auto& e : st.entities)
        if (e->GetKind() == EntityKind::Npc &&
            static_cast<const NpcShip*>(e.get())->GetRole() == NpcRole::Pirate &&
            !static_cast<const NpcShip*>(e.get())->IsLeaving())  // the system's, as RecountAgg
            n++;
    return n;
}

// System nodes for spawning. Gates are marked "dangerous" if they lead into an unsafe/
// pirate system — only there do pirate ambushes belong.
Simulation::SpawnNodes Simulation::GatherNodes(const SystemState& st) const
{
    SpawnNodes nd;
    for (const auto& e : st.entities)
    {
        if (e->GetKind() == EntityKind::Station)
            nd.stations.push_back(e->GetPosition());
        else if (JumpGate* g = e->GetKind() == EntityKind::Gate && SignatureOf(*e) <= 0.0f
                                   ? static_cast<JumpGate*>(e.get())
                                   : nullptr)  // NPCs do not wait at a hole nobody sees (S9)
        {
            Vector2 p = g->GetPosition();
            nd.gates.push_back(p);
            auto dest = systems_.find(g->GetDestination());
            if (dest != systems_.end())
            {
                if (Dangerous(dest->second.agg))
                    nd.dangerGates.push_back(p);  // border with low-sec — ambush spot
            }
        }
        else if (e->GetKind() == EntityKind::Field)
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

Vector2 Simulation::PirateSpawnPos(const std::vector<Vector2>& pool,
                                   const std::vector<Vector2>& avoid)
{
    Vector2 base = pool[RandRange(0, (int)pool.size() - 1)];
    Vector2 pos = base;
    for (int attempt = 0; attempt < 4; attempt++)
    {
        pos = { base.x + RandRange(-3000, 3000), base.y + RandRange(-3000, 3000) };
        if (avoid.empty())
            break;  // no one to avoid (background/hydrate)
        // Far enough from EVERY player in the system: clearing one player's space by
        // dropping the ambush into another's is not an improvement.
        bool clear = true;
        for (Vector2 a : avoid)
        {
            float dx = pos.x - a.x, dy = pos.y - a.y;
            if (dx * dx + dy * dy < SPAWN_MIN_PLAYER_DIST * SPAWN_MIN_PLAYER_DIST)
            {
                clear = false;
                break;
            }
        }
        if (clear)
            break;
    }
    return pos;
}

void Simulation::SpawnNpcInto(SystemState& st, Vector2 pos, FactionId faction, NpcRole role,
                              std::vector<Vector2> waypoints, int pick)
{
    // Which of its role's designs it flies is decided once, when it is made (#279 step 4), so
    // a role with several in its faction's doctrine flies all of them and a ship keeps its
    // own for life. Not by the id when the caller has a better key: ids depend on what was
    // made before, and a system that warms must find the same sky whatever warmed first.
    const int id = NextAgentId();
    auto      npc = std::make_unique<NpcShip>(
        pos, faction, role, std::move(waypoints),
        Archetypes::ShipCatalogue().Pick(Factions::Id(faction), NpcRoleId(role),
                                         (unsigned)(pick >= 0 ? pick : id)));
    npc->SetId(id);
    st.entities.push_back(std::move(npc));
}

Simulation::Room Simulation::RoomOf(const SpawnNodes& nd)
{
    Room r;
    r.lanes = nd.stations.size() + nd.gates.size();
    r.fields = nd.fields.size();
    // PirateSpots: the periphery and the gates into danger, else the belts.
    r.dark = !nd.fields.empty() || !nd.dangerGates.empty();
    return r;
}

Simulation::Room Simulation::RoomOf(const SystemState& st) const
{
    Room r;
    r.lanes = (size_t)(st.profile.stations + st.profile.gates);
    r.fields = (size_t)st.profile.belts;
    r.dark = r.fields > 0;
    // A gate leads to each neighbour; one into danger is where an ambush belongs.
    for (const std::string& n : Neighbors(st.id))
    {
        if (r.dark)
            break;
        auto it = systems_.find(n);
        r.dark = it != systems_.end() && Dangerous(it->second.agg);
    }
    return r;
}

Simulation::Population Simulation::PopulationOf(const SystemState& st, const Room& room) const
{
    const SystemAggregate& agg = st.agg;
    const float            sec = agg.security;
    const FactionId        ctrl = agg.controller;
    const bool             lawful = Factions::IsLawful(ctrl);
    Population             p;

    // Armed ships come from what each faction has committed here (#231), not from a formula
    // of security: the lawful as police under whichever of them is strongest here, the
    // pirates as pirates.
    p.policeFaction = lawful ? ctrl : Factions::DefaultPolice();
    // A faction's strength here that is besieging an outpost (#295, slice 5) is its siege's
    // warships, not police or pirates the director makes.
    PerFaction<float> here = agg.presence;
    for (FactionId f : Factions::All())
        here[f] = std::max(0.0f, here[f] - SiegeForceIn(st.id, f));
    float lawPresence = 0.0f, strongestLaw = 0.0f;
    for (FactionId f : Factions::All())
        if (Factions::IsLawful(f))
        {
            lawPresence += here[f];
            if (here[f] > strongestLaw)
            {
                strongestLaw = here[f];
                p.policeFaction = f;
            }
        }
    p.tradeFaction = lawful ? ctrl : Factions::Unaffiliated();
    const int traders = (ctrl == Factions::Pirates()) ? 1 : (int)roundf(2.0f + sec * 4.0f);

    // What it keeps once the losses are forgotten (#295): what it holds above this leaves.
    // The police a holder pays for fly on top of any faction's (#431 slice 11); of the
    // pirates' strength, the higher the security the more of it lies low -- and what it does
    // not keep here leaves for darker systems next door (StepMigration).
    const WorldRules::Security& sr = WorldRules::SecurityRules();
    const int                   paid = sr.Ships(agg.paidPolice);
    const float                 pirates = here[Factions::Pirates()] * (float)sr.PiratesFly(sec);
    p.keep = { traders, 2, (int)roundf(lawPresence) + paid, (int)roundf(pirates) };
    // "Pressure": recent losses temporarily cut the target (recovers in MaintainWorld).
    p.traders = (int)roundf(p.keep[0] * (1.0f - agg.supTraders));
    p.miners = (int)roundf(p.keep[1] * (1.0f - agg.supMiners));
    p.police = (int)roundf(p.keep[2] * (1.0f - agg.supPolice));
    p.pirates = (int)roundf(p.keep[3] * (1.0f - agg.supPirates));

    // Nowhere to put them, none of them: traders need two ends of a lane, miners a belt,
    // police a beat, pirates somewhere dark.
    if (room.lanes < 2)
        p.traders = p.keep[0] = 0;
    if (room.fields == 0)
        p.miners = p.keep[1] = 0;
    if (room.lanes + room.fields < 2)
        p.police = p.keep[2] = 0;
    if (!room.dark)
        p.pirates = p.keep[3] = 0;
    return p;
}

// Spawn director: tops up a system's population to targets by security/controller,
// accounting for the "pressure" from losses. A pirate system loses trade/police; a strong
// lawful neighbor sends reinforcements (police), which leads to reconquest.
void Simulation::TopUpSystem(SystemState& st, const std::vector<Vector2>& avoid)
{
    SpawnNodes           nd = GatherNodes(st);
    std::vector<Vector2> lanes = nd.stations;
    lanes.insert(lanes.end(), nd.gates.begin(), nd.gates.end());
    std::vector<Vector2> patrolRoute = lanes;
    patrolRoute.insert(patrolRoute.end(), nd.fields.begin(), nd.fields.end());
    std::vector<Vector2> hot = PirateSpots(nd);

    const Population target = PopulationOf(st, RoomOf(nd));
    st.agg.policeFaction = target.policeFaction;

    // Current population by role.
    int tr = 0, mi = 0, po = 0, pi = 0;
    for (auto& e : st.entities)
        if (NpcShip* n = e->GetKind() == EntityKind::Npc &&
                                 !static_cast<NpcShip&>(*e).IsLeaving() &&
                                 static_cast<NpcShip&>(*e).GetTask() == 0
                             ? static_cast<NpcShip*>(e.get())
                             : nullptr)
            switch (n->GetRole())
            {
                case NpcRole::Trader: tr++; break;
                case NpcRole::Miner: mi++; break;
                case NpcRole::Police: po++; break;
                case NpcRole::Pirate: pi++; break;
                case NpcRole::Warship: break;  // a siege's, not the director's (#295)
                case NpcRole::Guard: break;    // a corporation's patrol (#431 slice 12)
            }

    auto pick = [&](const std::vector<Vector2>& v) -> Vector2
    { return v[RandRange(0, (int)v.size() - 1)]; };

    // Top up by no more than a couple of units per step (smoothly, no bursts).
    int  budget = DIRECTOR_BUDGET;
    auto canSpawn = [&]() { return budget > 0; };

    // A role a neighbour has ships to spare of comes from next door, not from nowhere (#295).
    if (st.fromNextDoor[(int)NpcRole::Trader])
        tr = std::max(tr, target.traders);
    if (st.fromNextDoor[(int)NpcRole::Miner])
        mi = std::max(mi, target.miners);
    if (st.fromNextDoor[(int)NpcRole::Police])
        po = std::max(po, target.police);
    if (st.fromNextDoor[(int)NpcRole::Pirate])
        pi = std::max(pi, target.pirates);

    while (tr < target.traders && canSpawn())
    {
        SpawnNpcInto(st, pick(lanes), target.tradeFaction, NpcRole::Trader, lanes);
        tr++;
        budget--;
    }
    while (mi < target.miners && canSpawn())
    {
        Vector2              spot = pick(nd.fields);
        std::vector<Vector2> near = { spot };
        SpawnNpcInto(st, spot, Factions::Unaffiliated(), NpcRole::Miner, near);
        mi++;
        budget--;
    }
    while (po < target.police && canSpawn())
    {
        SpawnNpcInto(st, pick(patrolRoute), target.policeFaction, NpcRole::Police, patrolRoute);
        po++;
        budget--;
    }
    while (pi < target.pirates && canSpawn())
    {
        Vector2              spot = PirateSpawnPos(hot, avoid);
        std::vector<Vector2> patrol = { spot };
        SpawnNpcInto(st, spot, Factions::Pirates(), NpcRole::Pirate, patrol);
        pi++;
        budget--;
    }
}

void Simulation::ColdLosses(SystemState& st)
{
    // Expected losses over one pass, from the rates the hot world shows: pirates raid
    // traders and miners and trade shots with police, each pair at its own rate. They are
    // small -- a system is a million units across and ships seldom meet -- and they are the
    // same losses a hot system counts, so the faction step and the pressure see no seam.
    SystemAggregate& a = st.agg;
    const float      pirates = a.pirates, police = a.police;
    if (pirates <= 0.0f)
        return;
    // Who is here, as the director flies them (PopulationOf): the pirates' raiders, the
    // police under its flag, traders under a lawful controller's or nobody's, miners nobody's.
    const FactionId trade =
        Factions::IsLawful(a.controller) ? a.controller : Factions::Unaffiliated();
    const Ships::Fighter pirate = FighterOf(Factions::Pirates(), NpcRole::Pirate);
    const Ships::Fighter cop = FighterOf(a.policeFaction, NpcRole::Police);
    const Ships::Fighter trader = FighterOf(trade, NpcRole::Trader);
    const Ships::Fighter miner = FighterOf(Factions::Unaffiliated(), NpcRole::Miner);
    // The share of a ship's hull one second of the other's fire takes off.
    auto bite = [](const Ships::Fighter& by, const Ships::Fighter& on)
    { return on.hull > 0.0f ? by.dps / on.hull : 0.0f; };
    // Corporations' patrols (#431 slice 12) trade shots with the pirates as the police do, at
    // the same meeting rates, with their own design's guns and hull.
    float byPatrols = 0.0f;
    for (auto& kv : a.patrols)
    {
        const Corps::Patrol* set = PatrolOf(kv.first, st.id);
        if (set == nullptr || kv.second <= 0.0f)
            continue;
        const Ships::Fighter guard = PatrolFighter(*set);
        byPatrols += COLD_PIRATE_FIRE * bite(guard, pirate) * pirates * kv.second;
        kv.second -=
            std::min(kv.second, COLD_POLICE_FIRE * bite(pirate, guard) * pirates * kv.second);
    }
    const float lostPirates =
        std::min(pirates, COLD_PIRATE_FIRE * bite(cop, pirate) * pirates * police + byPatrols);
    const float lostPolice =
        std::min(police, COLD_POLICE_FIRE * bite(pirate, cop) * pirates * police);
    const float lostTraders =
        std::min(a.traders, COLD_TRADER_FIRE * bite(pirate, trader) * pirates * a.traders);
    const float lostMiners =
        std::min(a.miners, COLD_MINER_FIRE * bite(pirate, miner) * pirates * a.miners);
    a.pirates -= lostPirates;
    a.police -= lostPolice;
    a.traders -= lostTraders;
    a.miners -= lostMiners;
    AccumSuppress(a.supTraders, lostTraders);
    AccumSuppress(a.supMiners, lostMiners);
    AccumSuppress(a.supPolice, lostPolice);
    AccumSuppress(a.supPirates, lostPirates);
    a.lostPirates += lostPirates;
    a.lostPolice += lostPolice;
}

void Simulation::ColdTopUp(SystemState& st, const Population& target)
{
    // The spawn director, as numbers: up towards what it keeps here by the couple of ships a
    // pass it would send, and never down. In a hot system a ship stays until somebody
    // destroys it, whatever the director would send now; a cold one keeps the same count,
    // so that a system does not change its character by being looked at.
    SystemAggregate& a = st.agg;
    a.policeFaction = target.policeFaction;
    float budget = (float)DIRECTOR_BUDGET;
    // A role a neighbour has ships to spare of comes from next door, not from nowhere (#295).
    auto topUp = [&](float& count, int want, NpcRole role)
    {
        if (st.fromNextDoor[(int)role])
            return;
        const float step = std::min(std::max(0.0f, (float)want - count), budget);
        count += step;
        budget -= step;
    };
    topUp(a.traders, target.traders, NpcRole::Trader);
    topUp(a.miners, target.miners, NpcRole::Miner);
    topUp(a.police, target.police, NpcRole::Police);
    topUp(a.pirates, target.pirates, NpcRole::Pirate);

    // What every tick would have done to the rest of it: bodies move on. (The market is the
    // coarse pass's in every system, hot or cold: StepMarket.)
    Orbits::Place(st.entities, time_);
}

void Simulation::StepMarket(SystemState& st, float seconds)
{
    const double       hours = seconds / 3600.0;
    const MarketRules& k = Goods::Rules();
    // The belts (#348): the system's miners take a share of what is in each -- less where
    // pirates have thinned them out; two is a full crew -- and every belt grows back, whether
    // or not there is a market to sell to. A belt players work as well holds less, and gives
    // the miners less: the ore is shared, and ore gets dearer.
    const double crew = std::clamp((double)st.agg.miners / 2.0, 0.0, 1.0);
    const bool   trades = st.profile.markets > 0;
    if (trades)
        st.market.SetMarkets(st.profile.markets);
    if (st.profile.belts > 0)
        for (auto& e : st.entities)
        {
            if (e->GetKind() != EntityKind::Field)
                continue;
            AsteroidField& belt = static_cast<AsteroidField&>(*e);
            if (trades)
            {
                const double mined =
                    belt.Take(belt.GetOre() * std::min(1.0, k.beltMined * crew * hours));
                st.market.SetStock(belt.GetResource(), st.market.Stock(belt.GetResource()) + mined);
            }
            belt.Regrow(k.beltRegrow, hours);
        }
    // Nowhere to trade: nothing is kept. A system that gains a market starts at its targets.
    if (!trades)
        return;
    st.market.Step(st.profile.rates, hours);
}

void Simulation::MaintainWorld(float dt, MaintainCost* cost)
{
    BenchClock::time_point lap = cost != nullptr ? BenchClock::now() : BenchClock::time_point();
    // The simulation clock lives here because MaintainWorld is called once per tick by
    // every driver of the world -- the host loop and the batch mode -- which makes it the
    // one place elapsed time can accumulate without being counted twice or not at all.
    time_ += dt;
    maintAccum_ += dt;
    // Wherever a player is, the system is hot, and stays so for a while after (#295). Most
    // ways into a system warm it on arrival; this catches the rest.
    for (const auto& sv : sessions_)
        if (SystemState* st = SystemById(sv.second.systemId))
            Warm(*st);
    StepStructures();  // every tick: a site finishes when the clock says, not two seconds on
    StepOwners();      // ...and an owner who is back pays for what their structures did (#340)
    Lap(cost ? &cost->structures : nullptr, lap);
    while (maintAccum_ >= MAINT_STEP)
    {
        if (cost != nullptr)
            cost->passes++;
        // Recount the real populations; accumulate losses since the last step as "pressure".
        // A cold system has no ships to count, and its losses are arithmetic.
        for (auto& kv : systems_)
        {
            if (!IsHot(kv.second))
            {
                ColdLosses(kv.second);
                continue;
            }
            SystemAggregate& a = kv.second.agg;
            float prevTr = a.traders, prevMi = a.miners, prevPo = a.police, prevPi = a.pirates;
            RecountAgg(kv.second);
            AccumSuppress(a.supTraders, prevTr - a.traders);
            AccumSuppress(a.supMiners, prevMi - a.miners);
            AccumSuppress(a.supPolice, prevPo - a.police);
            AccumSuppress(a.supPirates, prevPi - a.pirates);
            FieldPatrols(kv.second);  // guards shot down, and as many as the patrol says (#431)
            // What was destroyed, for the faction step: strength a faction committed and
            // lost (#231). A fall in the count is a loss -- a ship that leaves (#295) was taken
            // off the count when it set out, so it is not one.
            a.lostPirates += std::max(0.0f, prevPi - a.pirates);
            a.lostPolice += std::max(0.0f, prevPo - a.police);
        }
        Lap(cost ? &cost->recount : nullptr, lap);
        StepWorldMacro();
        Lap(cost ? &cost->macro : nullptr, lap);
        StepMigration();  // before the director, which leaves to next door what it can spare
        size_t nth = 0;
        for (auto& kv : systems_)
        {
            const Population& target = passTargets_[nth++];
            // Recovery: the pressure slowly subsides.
            SystemAggregate& a = kv.second.agg;
            a.supTraders *= SUPPRESS_DECAY;
            a.supMiners *= SUPPRESS_DECAY;
            a.supPolice *= SUPPRESS_DECAY;
            a.supPirates *= SUPPRESS_DECAY;
            DeliverCargo(kv.second, false);  // haulers that have reached a market (#343)
            StepMarket(kv.second, MAINT_STEP);
            // Nobody here for long enough: the ships are counted and let go.
            if (IsHot(kv.second) && !allHot_ && time_ >= kv.second.warmUntil)
                Cool(kv.second);
            if (!IsHot(kv.second))
            {
                ColdTopUp(kv.second, target);
                continue;
            }
            // Player-avoidance, per system: everyone standing in this one.
            std::vector<Vector2> avoid;
            for (const auto& sv : sessions_)
                if (sv.second.systemId == kv.first && sv.second.ship &&
                    sv.second.dockedStationId == 0)
                    avoid.push_back(sv.second.ship->GetPosition());
            TopUpSystem(kv.second, avoid);
        }
        Lap(cost ? &cost->topUp : nullptr, lap);
        StepContracts();     // what the markets are short of, and the contracts for it (#343)
        StepLicences();      // rent due, lapses, and where each player's structure stands (#431)
        StepObservations();  // what stands where somebody is, onto their maps (#431 slice 14)
        // Last, when no pass over the systems is under way: what it opens adds systems (S3).
        StepTide(MAINT_STEP);
        Lap(cost ? &cost->macro : nullptr, lap);
        maintAccum_ -= MAINT_STEP;
    }
}
