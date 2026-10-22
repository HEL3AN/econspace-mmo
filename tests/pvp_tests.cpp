#include <doctest/doctest.h>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "economy/Resource.h"
#include "entities/Derelict.h"
#include "entities/NpcShip.h"
#include "entities/Ship.h"
#include "entities/ShipType.h"
#include "entities/Station.h"
#include "sim/ClientSession.h"
#include "sim/FactionMind.h"
#include "sim/Protocol.h"
#include "sim/Simulation.h"
#include "sim/WorldRules.h"
#include "temp_dir.h"
#include "factions_named.h"
#include "goods_named.h"

// PvP under the law (#431 slice 10): a pilot may shoot another, and where law holds it is a
// crime -- the police come, the shooter is flagged a criminal and their security status falls.
// A flagged pilot is anyone's to shoot, lawful docks refuse them, and the flag waits for them
// while they are away. At a low status the police in high security engage on sight.

namespace
{
const std::string DATA = TEST_DATA_DIR;
constexpr float   IN_SIGHT = 3500.0f;  // a patrol's NPC_AGGRO_RANGE

// Helios Core, the Traders Guild's, at `security`, with `police` of the Guild's patrolling it
// and nothing next door; two pilots side by side out in space, `shooter` armed.
struct Scene
{
    std::unique_ptr<Simulation> sim;
    std::string                 sys;
    FactionId                   law = Fac::TradersGuild();
    ClientSession*              shooter = nullptr;
    ClientSession*              victim = nullptr;
    SystemState&                St() { return *sim->SystemById(sys); }

    // One shot at `who`, the cooldown spent.
    bool Fire(ClientSession& by, ClientSession& who, Simulation::PlayerCombatEvents& ev)
    {
        by.fireTimer = 0.0f;
        return sim->StepPlayerFire(by, St(), who.ship->GetId(), 1.0f / 60.0f, &ev);
    }
};

Scene PvpScene(float security, int police = 3)
{
    REQUIRE(Factions::Load(DATA + "factions.json"));
    REQUIRE(Archetypes::Load(DATA + "archetypes.json"));
    REQUIRE(Blueprints::Load(DATA + "blueprints.json"));
    const bool rules = WorldRules::Load(DATA + "world_rules.json");
    INFO(WorldRules::Error());
    REQUIRE(rules);
    Scene s;
    s.sim = std::make_unique<Simulation>();
    Simulation& sim = *s.sim;
    sim.LoadUniverse(std::string(TEST_WORLD_DIR) + "universe.json");
    sim.Seed(1234u);
    sim.InitGalaxy();
    sim.MaterializeAllSystems(std::string(TEST_WORLD_DIR) + "systems/");
    s.sys = sim.Universe().startId;
    for (auto& kv : sim.Systems())
    {
        SystemAggregate& a = kv.second.agg;
        for (FactionId f : Factions::All())
            a.presence[f] = 0.0f;
        a.police = a.pirates = 0.0f;
        a.policeFaction = s.law;
    }
    SystemAggregate& a = s.St().agg;
    a.claimed = true;
    a.controller = s.law;
    a.presence[s.law] = (float)police;
    a.police = (float)police;
    a.security = a.baseSecurity = security;
    sim.MindOf(s.law).stock = 0.0f;

    const Vector2 at = { 300000.0f, -300000.0f };
    s.shooter = &sim.CreateSession(s.sys, at, GetShipCatalog()[0].stats);
    s.shooter->accountName = "hunter";
    s.shooter->weaponOn = true;
    s.victim = &sim.CreateSession(s.sys, { at.x + 100.0f, at.y }, GetShipCatalog()[0].stats);
    s.victim->accountName = "trader";
    sim.Warm(s.St());
    return s;
}

int PoliceBeside(Scene& s, const ClientSession& p)
{
    int n = 0;
    for (const auto& e : s.St().entities)
        if (e->GetKind() == EntityKind::Npc &&
            static_cast<const NpcShip&>(*e).GetRole() == NpcRole::Police &&
            static_cast<const NpcShip&>(*e).IsAlive())
        {
            const Vector2 a = e->GetPosition(), b = p.ship->GetPosition();
            if (std::hypot(a.x - b.x, a.y - b.y) <= IN_SIGHT)
                n++;
        }
    return n;
}

const Station* LawfulStation(Scene& s)
{
    for (const auto& e : s.St().entities)
        if (e->GetKind() == EntityKind::Station &&
            Factions::IsLawful(static_cast<const Station&>(*e).GetFaction()))
            return static_cast<const Station*>(e.get());
    return nullptr;
}
}  // namespace

TEST_CASE("shooting a pilot where law holds is a crime: flagged, status down, police called")
{
    Scene                          s = PvpScene(0.9f);
    const WorldRules::Law&         r = WorldRules::LawRules();
    Simulation::PlayerCombatEvents ev;
    const float                    hull = s.victim->ship->GetHull() + s.victim->ship->GetShields();
    REQUIRE(PoliceBeside(s, *s.shooter) == 0);

    REQUIRE(s.Fire(*s.shooter, *s.victim, ev));
    CHECK(s.victim->ship->GetHull() + s.victim->ship->GetShields() <
          hull);  // the hit is on the victim's own ship
    CHECK(ev.victimSession == s.victim->id);
    CHECK(ev.hitLawful);
    CHECK(ev.hitFaction == s.law);
    CHECK(s.shooter->account.IsCriminal());
    CHECK(s.shooter->account.CriminalLeft() == doctest::Approx(r.flagSeconds));
    CHECK(s.shooter->account.GetSecurityStatus() == doctest::Approx(-(float)r.statusCrime));
    CHECK(s.shooter->account.IsWanted(s.law));  // and the standing loss with the owner
    CHECK(s.shooter->account.GetReputation(s.law) < 0.0f);
    CHECK(PoliceBeside(s, *s.shooter) == 3);  // every free patrol, from real forces
    // The victim did nothing.
    CHECK_FALSE(s.victim->account.IsCriminal());
    CHECK(s.victim->account.GetSecurityStatus() == 0.0f);

    // A lawful dock refuses the criminal, and takes the victim.
    const Station* station = LawfulStation(s);
    REQUIRE(station != nullptr);
    CHECK(s.sim->DockRefusal(*s.shooter, *station).find("criminal") != std::string::npos);
    CHECK(s.sim->DockRefusal(*s.victim, *station).empty());

    // ...and the police hunt the flagged pilot, as they do anyone wanted.
    for (const auto& e : s.St().entities)
        if (e->GetKind() == EntityKind::Npc &&
            static_cast<const NpcShip&>(*e).GetRole() == NpcRole::Police)
            CHECK(s.sim->NpcHunts(s.St(), *s.shooter, static_cast<const NpcShip&>(*e)));

    // A second shot renews the flag rather than stacking it.
    s.sim->StepPilotLaw(*s.shooter, 100.0f);
    REQUIRE(s.Fire(*s.shooter, *s.victim, ev));
    CHECK(s.shooter->account.CriminalLeft() == doctest::Approx(r.flagSeconds));
}

TEST_CASE("a flagged criminal is free to shoot, and lawless space charges nothing")
{
    SUBCASE("the criminal")
    {
        Scene s = PvpScene(0.9f);
        s.victim->account.SetCriminalLeft(600.0);
        Simulation::PlayerCombatEvents ev;
        REQUIRE(s.Fire(*s.shooter, *s.victim, ev));
        CHECK_FALSE(ev.hitLawful);
        CHECK_FALSE(s.shooter->account.IsCriminal());
        CHECK(s.shooter->account.GetSecurityStatus() == 0.0f);
        CHECK_FALSE(s.shooter->account.IsWanted(s.law));
        CHECK(PoliceBeside(s, *s.shooter) == 0);
    }
    SUBCASE("security zero: no law holds")
    {
        Scene                          s = PvpScene(0.0f);
        Simulation::PlayerCombatEvents ev;
        REQUIRE(s.Fire(*s.shooter, *s.victim, ev));
        CHECK_FALSE(ev.hitLawful);
        CHECK_FALSE(s.shooter->account.IsCriminal());
    }
    SUBCASE("a pirate system")
    {
        Scene s = PvpScene(0.9f);
        s.St().agg.controller = Factions::Pirates();
        Simulation::PlayerCombatEvents ev;
        REQUIRE(s.Fire(*s.shooter, *s.victim, ev));
        CHECK_FALSE(ev.hitLawful);
        CHECK_FALSE(s.shooter->account.IsCriminal());
    }
    SUBCASE("a pilot who is docked, or out of range, is not a target")
    {
        Scene                          s = PvpScene(0.9f);
        Simulation::PlayerCombatEvents ev;
        s.victim->ship->Teleport(
            { s.shooter->ship->GetPosition().x + Simulation::PLAYER_WEAPON_RANGE * 3.0f,
              s.shooter->ship->GetPosition().y });
        CHECK_FALSE(s.Fire(*s.shooter, *s.victim, ev));
        s.victim->ship->Teleport(s.shooter->ship->GetPosition());
        s.victim->dockedStationId = LawfulStation(s)->GetId();
        CHECK_FALSE(s.Fire(*s.shooter, *s.victim, ev));
        CHECK_FALSE(s.shooter->account.IsCriminal());
        // ...and nobody shoots themselves.
        CHECK_FALSE(s.Fire(*s.shooter, *s.shooter, ev));
    }
}

TEST_CASE("the flag runs down only while the pilot is here, and a criminal's ship lingers")
{
    Scene                  s = PvpScene(0.9f);
    const WorldRules::Law& r = WorldRules::LawRules();
    ClientSession&         p = *s.shooter;
    s.sim->MarkCrime(p, false);
    REQUIRE(p.account.IsCriminal());
    s.sim->StepPlayerAccountTick(p, 100.0f);
    CHECK(p.account.CriminalLeft() == doctest::Approx(r.flagSeconds - 100.0));

    // Gone: the ship stays a minute rather than the usual half, and the flag waits.
    REQUIRE(s.sim->BeginLinger(p));
    CHECK(p.lingerLeft == doctest::Approx((float)r.flagLinger));
    CHECK(s.sim->StepLinger(Simulation::LINGER_SECONDS + 1.0f).empty());
    s.sim->StepPlayerAccountTick(p, 100.0f);
    CHECK(p.account.CriminalLeft() == doctest::Approx(r.flagSeconds - 100.0));
    // An innocent pilot's ship goes after the usual time.
    REQUIRE(s.sim->BeginLinger(*s.victim));
    CHECK(s.victim->lingerLeft == doctest::Approx(Simulation::LINGER_SECONDS));

    // Saved with the account, and back with what was left of it.
    const TempDir     tmp;
    const std::string path = tmp.Path("hunter.json");
    REQUIRE(s.sim->SaveAccount(p, path));
    ClientSession& back = s.sim->CreateSession(s.sys, { 0.0f, 0.0f }, GetShipCatalog()[0].stats);
    REQUIRE(s.sim->LoadAccount(back, path) == Save::Result::Ok);
    CHECK(back.account.CriminalLeft() == doctest::Approx(r.flagSeconds - 100.0));
    CHECK(back.account.GetSecurityStatus() == doctest::Approx(-(float)r.statusCrime));

    // Back in the world, it runs out, and says so.
    const int seq = back.LastEventSeq();
    s.sim->StepPlayerAccountTick(back, (float)r.flagSeconds);
    CHECK_FALSE(back.account.IsCriminal());
    bool told = false;
    for (const Ev::Event& e : back.EventsSince(seq))
        told = told || e.text.find("criminal flag has lapsed") != std::string::npos;
    CHECK(told);
    const Station* station = LawfulStation(s);
    REQUIRE(station != nullptr);
    CHECK(s.sim->DockRefusal(back, *station).empty());
}

TEST_CASE("security status falls with crimes and recovers with time and pirate kills")
{
    Scene                  s = PvpScene(0.9f);
    const WorldRules::Law& r = WorldRules::LawRules();
    ClientSession&         p = *s.shooter;

    // A kill costs the hit and the kill.
    s.victim->ship->TakeDamage(s.victim->ship->GetShields());
    s.victim->ship->SetHull(1.0f);
    Simulation::PlayerCombatEvents ev;
    REQUIRE(s.Fire(p, *s.victim, ev));
    CHECK(ev.killedPilot);
    CHECK(ev.killedLawful);
    CHECK(p.account.GetSecurityStatus() == doctest::Approx(-(float)(r.statusCrime + r.statusKill)));

    // Time, without a flag, brings it back towards zero and no further.
    p.account.SetCriminalLeft(0.0);
    p.account.SetSecurityStatus(-1.0f);
    s.sim->StepPilotLaw(p, 86400.0f);
    CHECK(p.account.GetSecurityStatus() == doctest::Approx(-1.0f + (float)r.statusPerDay));
    s.sim->StepPilotLaw(p, 10.0f * 86400.0f);
    CHECK(p.account.GetSecurityStatus() == 0.0f);
    // Not while flagged: the clock is the flag's.
    p.account.SetSecurityStatus(-1.0f);
    p.account.SetCriminalLeft(10.0 * 86400.0);
    s.sim->StepPilotLaw(p, 86400.0f);
    CHECK(p.account.GetSecurityStatus() == doctest::Approx(-1.0f));
    p.account.SetCriminalLeft(0.0);

    // A pirate shot down is a little of it back.
    auto pirate = std::make_unique<NpcShip>(p.ship->GetPosition(), Fac::Pirates(), NpcRole::Pirate,
                                            std::vector<Vector2>{});
    pirate->SetId(987654);
    pirate->SetHull(1.0f);
    s.St().entities.push_back(std::move(pirate));
    p.fireTimer = 0.0f;
    REQUIRE(s.sim->StepPlayerFire(p, s.St(), 987654, 1.0f / 60.0f, &ev));
    CHECK(ev.killedPirate);
    CHECK(p.account.GetSecurityStatus() == doctest::Approx(-1.0f + (float)r.statusPirateKill));
    // ...and the scale has ends.
    p.account.SetSecurityStatus(-50.0f);
    CHECK(p.account.GetSecurityStatus() == Player::STATUS_MIN);
}

TEST_CASE("at a low status the police in high security engage on sight")
{
    const WorldRules::Law& r = WorldRules::LawRules();
    auto                   shotAt = [&](float security, float status)
    {
        Scene          s = PvpScene(security);
        ClientSession& p = *s.victim;
        p.account.SetSecurityStatus(status);
        // Beside one of the system's patrols, wanted by nobody and flagged by nobody.
        const NpcShip* police = nullptr;
        for (const auto& e : s.St().entities)
            if (police == nullptr && e->GetKind() == EntityKind::Npc &&
                static_cast<const NpcShip&>(*e).GetRole() == NpcRole::Police)
                police = static_cast<const NpcShip*>(e.get());
        REQUIRE(police != nullptr);
        CHECK(s.sim->NpcHunts(s.St(), p, *police) == s.sim->PoliceOnSight(s.St(), p, *police));
        s.shooter->ship->Teleport({ -400000.0f, 400000.0f });  // out of the way
        p.ship->Teleport({ police->GetPosition().x + 100.0f, police->GetPosition().y });
        Simulation::PlayerPresence pr;
        pr.session = &p;
        pr.ship = p.ship.get();
        std::vector<FireEvent> fires;
        for (int i = 0; i < 300; i++)
            s.sim->StepNpcCombat(s.St(), { pr }, &fires);
        for (const FireEvent& f : fires)
            if (f.targetSessionId == p.id)
                return true;
        return false;
    };
    const float low = -(float)r.onSightBelow - 1.0f;
    CHECK(shotAt(0.9f, low));
    CHECK_FALSE(shotAt(0.9f, -(float)r.onSightBelow + 1.0f));
    CHECK_FALSE(shotAt((float)r.onSightFrom - 0.1f, low));  // below high security: not on sight
}

TEST_CASE("a pilot shot down by a pilot is lost the way any ship is")
{
    Scene          s = PvpScene(0.0f);  // lawless: a fight, not a crime
    ClientSession& v = *s.victim;
    REQUIRE(v.ship->AddCargo(Good::Iron(), 20));
    v.ship->TakeDamage(v.ship->GetShields());
    v.ship->SetHull(1.0f);
    Simulation::PlayerCombatEvents ev;
    REQUIRE(s.Fire(*s.shooter, v, ev));
    CHECK(ev.killedPilot);
    CHECK_FALSE(v.ship->IsAlive());
    // Dead ships are not shot again.
    CHECK_FALSE(s.Fire(*s.shooter, v, ev));

    // The host's world tick takes it from there: the same loss as any (#344).
    const Vector2 fell = v.ship->GetPosition();
    s.sim->ServerRespawnPlayer(v);
    CHECK(v.lastLoss.seq == 1);
    CHECK(v.lastLoss.cargo == 20);
    CHECK(v.ship->IsAlive());
    bool wreck = false;
    for (const auto& e : s.St().entities)
        if (e->GetKind() == EntityKind::Derelict &&
            std::hypot(e->GetPosition().x - fell.x, e->GetPosition().y - fell.y) < 200.0f)
            wreck = true;
    CHECK(wreck);
    bool told = false;
    for (const Ev::Event& e : v.EventsSince(0))
        told = told || e.text.find("Shot down by hunter") != std::string::npos;
    CHECK(told);
}

TEST_CASE("an attack order hunts another pilot down")
{
    Scene          s = PvpScene(0.0f);
    ClientSession& v = *s.victim;
    v.ship->Teleport(
        { s.shooter->ship->GetPosition().x + 2000.0f, s.shooter->ship->GetPosition().y });
    v.ship->TakeDamage(v.ship->GetShields());
    v.ship->SetHull(30.0f);
    Orders::Order o;
    o.kind = Orders::Kind::Attack;
    o.targetId = v.ship->GetId();
    s.sim->GiveOrder(*s.shooter, o);
    for (int i = 0; i < 60 * 120 && s.shooter->HasRunningOrder(); i++)
        s.sim->StepPlayerOrder(*s.shooter, s.St(), 1.0f / 60.0f);
    CHECK(s.shooter->orderStatus == Orders::Status::Done);
    CHECK_FALSE(v.ship->IsAlive());
}

TEST_CASE("every pilot's snapshot shows another's flag and status, and the wire keeps them")
{
    Scene s = PvpScene(0.9f);
    s.sim->MarkCrime(*s.shooter, false);
    const Proto::Snapshot        snap = s.sim->BuildSnapshot(*s.victim, s.sys);
    const Proto::EntitySnapshot* seen = nullptr;
    for (const Proto::EntitySnapshot& e : snap.entities)
        if (e.id == s.shooter->ship->GetId())
            seen = &e;
    REQUIRE(seen != nullptr);
    CHECK(seen->criminal);
    CHECK(seen->securityStatus == doctest::Approx(s.shooter->account.GetSecurityStatus()));

    const Proto::Snapshot own = s.sim->BuildSnapshot(*s.shooter, s.sys);
    CHECK(own.player.criminalLeft > 0.0);
    CHECK(own.player.securityStatus < 0.0f);

    Proto::Snapshot back;
    REQUIRE(Proto::DecodeSnapshot(Proto::EncodeSnapshot(snap), back));
    bool found = false;
    for (const Proto::EntitySnapshot& e : back.entities)
        if (e.id == seen->id)
        {
            found = true;
            CHECK(e.criminal);
            CHECK(e.securityStatus == doctest::Approx(seen->securityStatus).epsilon(0.01));
        }
    CHECK(found);
    Proto::Snapshot ownBack;
    REQUIRE(Proto::DecodeSnapshot(Proto::EncodeSnapshot(own), ownBack));
    CHECK(ownBack.player.criminalLeft == doctest::Approx(std::ceil(own.player.criminalLeft)));
    CHECK(ownBack.player.securityStatus == doctest::Approx(own.player.securityStatus));
}
