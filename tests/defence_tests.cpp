#include <doctest/doctest.h>

#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "economy/Resource.h"
#include "entities/Defence.h"
#include "entities/NpcShip.h"
#include "entities/Ship.h"
#include "entities/ShipType.h"
#include "entities/Structure.h"
#include "sim/ClientSession.h"
#include "sim/FactionMind.h"
#include "sim/Protocol.h"
#include "sim/Simulation.h"
#include "temp_dir.h"

#include "factions_named.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

// Rules of engagement (#340): the owner decides whom a player's armed structure fires at,
// and a first strike where the law protects the target is the owner's crime.

namespace
{
std::string DataFile(const char* name)
{
    return std::string(TEST_DATA_DIR) + name;
}

void LoadRegistries()
{
    REQUIRE(Factions::Load(DataFile("factions.json")));
    REQUIRE(Archetypes::Load(DataFile("archetypes.json")));
    REQUIRE(Blueprints::Load(DataFile("blueprints.json")));
}

void StartServer(Simulation& sim, const std::string& worldPath)
{
    sim.LoadUniverse(std::string(TEST_WORLD_DIR) + "universe.json");
    sim.Seed(1234u);
    if (worldPath.empty() || sim.LoadWorld(worldPath) != Save::Result::Ok)
        sim.InitGalaxy();
    sim.MaterializeAllSystems(std::string(TEST_WORLD_DIR) + "systems/");
}

ClientSession& Builder(Simulation& sim, const std::string& account)
{
    ClientSession& s =
        sim.CreateSession(sim.Universe().startId, Vector2{ 0.0f, 0.0f }, GetShipCatalog()[0].stats);
    s.accountName = account;
    return s;
}

// Somewhere in open space a beacon may go, with the ship moved there.
Vector2 FreeSpot(Simulation& sim, ClientSession& s, float from = 0.0f)
{
    const Blueprint& bp = *Blueprints::Find("beacon");
    for (int i = 0; i < 60; i++)
        for (int j = 0; j < 60; j++)
        {
            const Vector2 p = { 120000.0f + from + 9100.0f * (float)i,
                                -270000.0f + 9300.0f * (float)j };
            s.ship->Teleport(p);
            if (sim.PlacementProblem(s, bp, p).empty())
                return p;
        }
    FAIL("no free spot in the start system");
    return {};
}

bool Shoot(Simulation& sim, ClientSession& s, int id)
{
    s.weaponOn = true;
    s.fireTimer = 0.0f;
    Simulation::PlayerCombatEvents ev;
    return sim.StepPlayerFire(s, *sim.SystemOf(s), id, 0.5f, &ev);
}

void Law(Simulation& sim, const std::string& sys, bool claimed, FactionId holder)
{
    sim.SystemById(sys)->agg.claimed = claimed;
    sim.SystemById(sys)->agg.controller = holder;
}

const Structure* Named(Simulation& sim, const std::string& sys, const std::string& name)
{
    for (const auto& e : sim.SystemById(sys)->entities)
        if (e->GetKind() == EntityKind::Structure && e->GetName() == name)
            return static_cast<const Structure*>(e.get());
    return nullptr;
}

// An armed outpost of `owner`'s, finished, at a free spot of the start system, with no ship
// of the world's anywhere in it.
struct Battery
{
    Simulation                              sim;
    std::string                             sys;
    Vector2                                 at{};
    int                                     id = 0;
    std::vector<Simulation::PlayerPresence> players;
    std::vector<FireEvent>                  fires;

    explicit Battery(const std::string& owner)
    {
        LoadRegistries();
        StartServer(sim, "");
        ClientSession& probe = Builder(sim, "probe");
        sys = probe.systemId;
        probe.ship->ClearCargo();  // a spot is free for a hold that could pay for it
        REQUIRE(probe.ship->AddCargo(ResourceType::Iron, 20));
        REQUIRE(probe.ship->AddCargo(ResourceType::Crystal, 5));
        at = FreeSpot(sim, probe);
        sim.DestroySession(probe.id);
        auto& ents = sim.SystemById(sys)->entities;
        ents.erase(std::remove_if(ents.begin(), ents.end(), [](const std::unique_ptr<Entity>& e)
                                  { return e->GetKind() == EntityKind::Npc; }),
                   ents.end());
        const Blueprint& outpost = *Blueprints::Find("outpost");
        REQUIRE(Archetypes::Find(outpost.archetype)->Has(Component::Defensive));
        auto o = std::make_unique<Structure>(at, 0.0f, "Bastion", outpost.archetype);
        o->SetBlueprint(outpost.id);
        id = sim.AddStatic(sys, std::move(o), owner);
        REQUIRE(id != 0);
    }
    ClientSession& Pilot(const std::string& account, float dx)
    {
        ClientSession& s = Builder(sim, account);
        s.ship->Teleport({ at.x + dx, at.y });
        Simulation::PlayerPresence p;
        p.session = &s;
        p.ship = s.ship.get();
        players.push_back(p);
        return s;
    }
    NpcShip* Npc(FactionId f, NpcRole role, float dy)
    {
        auto n =
            std::make_unique<NpcShip>(Vector2{ at.x, at.y + dy }, f, role, std::vector<Vector2>{});
        n->SetId(900000 + (int)sim.SystemById(sys)->entities.size());
        NpcShip* raw = n.get();
        sim.SystemById(sys)->entities.push_back(std::move(n));
        return raw;
    }
    Structure& It()
    {
        for (auto& e : sim.SystemById(sys)->entities)
            if (e->GetId() == id)
                return static_cast<Structure&>(*e);
        FAIL("the battery is gone");
        throw 0;
    }
    void Rules(Defence d) { It().SetDefence(std::move(d)); }
    void Defend(int ticks = 120)
    {
        for (int i = 0; i < ticks; i++)
            sim.StepStationDefence(*sim.SystemById(sys), players, &fires, 1.0f / 60.0f);
    }
    bool FiredAt(int session) const
    {
        for (const FireEvent& f : fires)
            if (f.targetSessionId == session)
                return true;
        return false;
    }
};

Defence Mode(Engage e)
{
    Defence d;
    d.engage = e;
    return d;
}
}  // namespace

TEST_CASE("rules of engagement: each mode fires at whom it says, and never at the owner (#340)")
{
    Battery b("hunter");
    Law(b.sim, b.sys, false, Fac::Independent());
    ClientSession& owner = b.Pilot("hunter", 300.0f);
    ClientSession& ann = b.Pilot("ann", 400.0f);
    CHECK(b.It().GetDefence().engage == Engage::Return);  // the default is as before

    SUBCASE("return fire: only whoever shot at it")
    {
        b.Defend();
        CHECK(b.fires.empty());
        REQUIRE(Shoot(b.sim, ann, b.id));
        b.Defend();
        CHECK(b.FiredAt(ann.id));
        CHECK_FALSE(b.FiredAt(owner.id));
    }
    SUBCASE("hold fire: not even back")
    {
        b.Rules(Mode(Engage::Hold));
        REQUIRE(Shoot(b.sim, ann, b.id));
        b.Defend();
        CHECK(b.fires.empty());
    }
    SUBCASE("enemies on sight: wanted players, and factions below the owner's threshold")
    {
        b.Rules(Mode(Engage::Enemies));
        b.Defend();
        CHECK(b.fires.empty());  // an innocent stranger, and the owner

        ann.account.AddBounty(Fac::Syndicate(), 10.0);  // wanted by a lawful faction
        b.Defend();
        CHECK(b.FiredAt(ann.id));
        ann.account.SetBounty(Fac::Syndicate(), 0.0);

        b.fires.clear();
        ann.ship->Teleport({ b.at.x + 90000.0f, b.at.y });  // out of the way
        NpcShip*    trader = b.Npc(Fac::TradersGuild(), NpcRole::Trader, 500.0f);
        const float whole = trader->GetHull();
        b.Defend();
        CHECK(b.fires.empty());  // the owner has nothing against the guild
        owner.account.SetReputation(Fac::TradersGuild(), -20.0f);
        b.Defend();
        REQUIRE_FALSE(b.fires.empty());
        CHECK(b.fires.back().targetSessionId == 0);
        CHECK(trader->GetHull() < whole);
    }
    SUBCASE("everyone not on the list: strangers, but not the listed or the owner")
    {
        Defence d = Mode(Engage::Strangers);
        d.players = { "ann" };
        b.Rules(d);
        ClientSession& bob = b.Pilot("bob", 500.0f);
        b.Defend();
        CHECK(b.FiredAt(bob.id));
        CHECK_FALSE(b.FiredAt(ann.id));
        CHECK_FALSE(b.FiredAt(owner.id));

        // A faction on the list is spared; one that is not is fired on.
        b.fires.clear();
        bob.ship->Teleport({ b.at.x + 90000.0f, b.at.y });
        d.factions = { "TradersGuild" };
        b.Rules(d);
        b.Npc(Fac::TradersGuild(), NpcRole::Trader, 500.0f);
        b.Defend();
        CHECK(b.fires.empty());
        b.Npc(Fac::Pirates(), NpcRole::Pirate, 700.0f);
        b.Defend();
        CHECK_FALSE(b.fires.empty());
    }
    SUBCASE("a faction's outpost keeps its doctrine, whatever its rules say")
    {
        auto o = std::make_unique<Structure>(Vector2{ b.at.x, b.at.y + 30000.0f }, 0.0f, "Keep",
                                             Blueprints::Find("outpost")->archetype);
        o->SetDefence(Mode(Engage::Hold));
        REQUIRE(b.sim.AddStatic(b.sys, std::move(o), Outposts::OwnerOf(Fac::Syndicate())) != 0);
        ann.ship->Teleport({ b.at.x, b.at.y + 30400.0f });
        ann.account.SetReputation(Fac::Syndicate(), -60.0f);
        b.Defend();
        CHECK(b.FiredAt(ann.id));
    }
}

TEST_CASE("rules of engagement: a first strike under a lawful holder is the owner's crime (#340)")
{
    Battery b("hunter");
    Law(b.sim, b.sys, true, Fac::Independent());
    ClientSession& ann = b.Pilot("ann", 400.0f);
    b.Rules(Mode(Engage::Strangers));

    SUBCASE("charged at once while the owner is here")
    {
        ClientSession& owner = b.Pilot("hunter", 300.0f);
        b.Defend(70);  // two volleys, a second apart
        REQUIRE(b.FiredAt(ann.id));
        CHECK(owner.account.GetReputation(Fac::Independent()) ==
              doctest::Approx(-2.0f * Crime::HIT_STANDING));
        CHECK(owner.account.IsWanted(Fac::Independent()));
    }
    SUBCASE("returning fire, or firing on someone the law wants, is no crime")
    {
        ClientSession& owner = b.Pilot("hunter", 300.0f);
        b.Rules(Mode(Engage::Return));
        REQUIRE(Shoot(b.sim, ann, b.id));
        b.Defend();
        REQUIRE(b.FiredAt(ann.id));
        CHECK(owner.account.GetReputation(Fac::Independent()) == 0.0f);

        b.fires.clear();
        b.Rules(Mode(Engage::Strangers));
        b.sim.SetTime(b.sim.Time() + Simulation::PROVOKED_SECONDS + 1.0);
        ann.account.AddBounty(Fac::Independent(), 10.0);
        b.Defend();
        REQUIRE(b.FiredAt(ann.id));
        CHECK_FALSE(owner.account.IsWanted(Fac::Independent()));
    }
    SUBCASE("no law holds the system: nothing to charge")
    {
        ClientSession& owner = b.Pilot("hunter", 300.0f);
        Law(b.sim, b.sys, false, Fac::Independent());
        b.Defend();
        REQUIRE(b.FiredAt(ann.id));
        CHECK(owner.account.GetReputation(Fac::Independent()) == 0.0f);
    }
    SUBCASE("a lawful faction's ships are its own, wherever they are")
    {
        ClientSession& owner = b.Pilot("hunter", 300.0f);
        Law(b.sim, b.sys, false, Fac::Independent());
        ann.ship->Teleport({ b.at.x + 90000.0f, b.at.y });
        b.Npc(Fac::Syndicate(), NpcRole::Trader, 500.0f);
        b.Defend(70);
        CHECK(owner.account.GetReputation(Fac::Syndicate()) < 0.0f);
    }
    SUBCASE("charged on return while the owner is away, and it outlives a restart")
    {
        b.Defend(70);
        REQUIRE(b.FiredAt(ann.id));
        const float owed = b.sim.OwedStanding("hunter", Fac::Independent());
        CHECK(owed == doctest::Approx(2.0f * Crime::HIT_STANDING));

        const TempDir     tmp;
        const std::string path = tmp.Path("world_roe.json");
        REQUIRE(b.sim.SaveWorld(path));
        Simulation again;
        StartServer(again, path);
        CHECK(again.OwedStanding("hunter", Fac::Independent()) == doctest::Approx(owed));
        ClientSession& owner = Builder(again, "hunter");
        const int      seq = owner.LastEventSeq();
        again.StepOwners();
        CHECK(owner.account.GetReputation(Fac::Independent()) == doctest::Approx(-owed));
        CHECK(owner.account.IsWanted(Fac::Independent()));
        CHECK(again.OwedStanding("hunter", Fac::Independent()) == 0.0f);
        CHECK_FALSE(owner.EventsSince(seq).empty());  // and told so
    }
    SUBCASE("a cold system steps no battery")
    {
        SystemState& st = *b.sim.SystemById(b.sys);
        st.populated = false;  // as Cool leaves it (#295)
        REQUIRE_FALSE(Simulation::IsHot(st));
        b.sim.StepSystemAgents(st, b.players, &b.fires, 1.0f / 60.0f);
        CHECK(b.fires.empty());
    }
}

TEST_CASE("rules of engagement are the owner's to set, are checked, and are saved (#340)")
{
    Battery        b("hunter");
    ClientSession& owner = b.Pilot("hunter", 300.0f);
    ClientSession& ann = b.Pilot("ann", 400.0f);
    Defence        d = Mode(Engage::Enemies);
    d.threshold = 5.0f;

    CHECK_FALSE(b.sim.DefenceSetProblem(ann, b.id, d).empty());  // not hers
    Defence bad = d;
    bad.factions = { "Nobody" };
    CHECK_FALSE(b.sim.DefenceSetProblem(owner, b.id, bad).empty());
    bad = d;
    bad.threshold = 500.0f;
    CHECK_FALSE(b.sim.DefenceSetProblem(owner, b.id, bad).empty());
    owner.ship->ClearCargo();
    REQUIRE(owner.ship->AddCargo(ResourceType::Iron, 20));
    REQUIRE(owner.ship->AddCargo(ResourceType::Crystal, 5));
    const Vector2 spot = FreeSpot(b.sim, owner, 20000.0f);
    const int     beacon = b.sim.Deploy(owner, "beacon", spot, "Lamp");
    REQUIRE(beacon != 0);
    CHECK(b.sim.DefenceSetProblem(owner, beacon, d).find("no guns") != std::string::npos);
    b.sim.TakeLayoutDeltas();

    const int seq = owner.LastEventSeq();
    REQUIRE(b.sim.SetDefence(owner, b.id, d));
    CHECK(b.It().GetDefence() == d);
    bool told = false;
    for (const Ev::Event& e : owner.EventsSince(seq))
        told = told || e.text.rfind("Rules of engagement for Bastion: Enemies on sight", 0) == 0;
    CHECK(told);

    // Everyone in the system hears of it, as a delta, with the rules in the layout.
    bool inDelta = false;
    for (const Proto::LayoutDelta& delta : b.sim.TakeLayoutDeltas())
        for (const Proto::EntityLayout& el : delta.changed)
            inDelta = inDelta || (el.id == b.id && el.defence == d);
    CHECK(inDelta);

    // The save keeps it with the structure, and the owner's standings for while they are away.
    const TempDir     tmp;
    const std::string path = tmp.Path("world_roe_rules.json");
    REQUIRE(b.sim.SaveWorld(path));
    Simulation again;
    StartServer(again, path);
    const Structure* back = Named(again, b.sys, "Bastion");
    REQUIRE(back != nullptr);
    CHECK(back->GetDefence() == d);
    float standing = 1.0f;
    CHECK(again.OwnerStanding("hunter", Fac::Independent(), standing));
    CHECK(standing == 0.0f);
}

TEST_CASE("rules of engagement on the wire and as a player types them (#340)")
{
    LoadRegistries();
    Defence d = Mode(Engage::Strangers);
    ReadAllowList(" ann, traders guild ,Bob,,ann", d);
    CHECK(d.players == std::vector<std::string>{ "ann", "Bob" });
    CHECK(d.factions == std::vector<std::string>{ "TradersGuild" });
    CHECK(AllowListText(d) == "TradersGuild, ann, Bob");

    Proto::Command c;
    c.defenceId = 42;
    c.defence = d;
    Proto::Command back;
    REQUIRE(Proto::DecodeCommand(Proto::EncodeCommand(c), back));
    CHECK(back.defenceId == 42);
    CHECK(back.defence == d);
    // Nothing about the rules is sent when there are none to set.
    CHECK(Proto::EncodeCommand(Proto::Command()).find("def") == std::string::npos);

    nlohmann::json j = DefenceJson(d);
    j["engage"] = "berserk";
    Defence kept;
    CHECK_FALSE(DefenceFrom(j, kept));  // an unknown mode is no mode
    j = DefenceJson(d);
    j["colour"] = "red";
    CHECK_FALSE(DefenceFrom(j, kept));  // nor is an unknown field
    CHECK(kept == Defence());

    CHECK(DefenceCost(Engage::Return, true, Fac::Independent()).find("no crime") !=
          std::string::npos);
    CHECK(DefenceCost(Engage::Strangers, true, Fac::Independent()).find("Independent") !=
          std::string::npos);
    CHECK(DefenceCost(Engage::Enemies, false, Fac::Independent()).find("costs nothing") !=
          std::string::npos);
}
