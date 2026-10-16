#include <doctest/doctest.h>

#include "goods_named.h"
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
        REQUIRE(probe.ship->AddCargo(Good::Iron(), 20));
        REQUIRE(probe.ship->AddCargo(Good::Crystal(), 5));
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
    REQUIRE(owner.ship->AddCargo(Good::Iron(), 20));
    REQUIRE(owner.ship->AddCargo(Good::Crystal(), 5));
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

TEST_CASE("a defence battery: a player's to build, armed below an outpost, and set like one (#340)")
{
    LoadRegistries();
    const Blueprint* bp = Blueprints::Find("battery");
    REQUIRE(bp != nullptr);
    CHECK(bp->byPlayers);
    CHECK_FALSE(bp->byFactions);
    CHECK(std::any_of(Blueprints::All().begin(), Blueprints::All().end(),
                      [](const Blueprint& x) { return x.id == "battery"; }));  // offered
    CHECK_FALSE(bp->wreck.empty());
    CHECK(bp->lifetime == 0.0f);                      // it stands until taken down
    CHECK_FALSE(Outposts::IsOutpost(bp->archetype));  // no faction claims a system with it

    // Armed, and modestly: less reach, damage and hull than a faction's outpost.
    const Blueprint& post = *Blueprints::Find("outpost");
    const Archetype* gun = Archetypes::Find(bp->archetype);
    const Archetype* outpost = Archetypes::Find(post.archetype);
    REQUIRE(gun != nullptr);
    CHECK(gun->Has(Component::Defensive));
    CHECK(gun->Has(Component::Buildable));
    CHECK(gun->weaponDamage > 0.0f);
    CHECK(gun->weaponDamage < outpost->weaponDamage);
    CHECK(gun->weaponRange < outpost->weaponRange);
    CHECK(bp->hull < post.hull);

    // Built like anything else, its rules set while it is a site, and taken apart again.
    Simulation sim;
    StartServer(sim, "");
    ClientSession& owner = Builder(sim, "hunter");
    owner.ship->ClearCargo();
    for (const auto& c : bp->cost)
        REQUIRE(owner.ship->AddCargo(c.first, c.second));
    Vector2 spot{};
    bool    found = false;
    for (int i = 0; i < 60 && !found; i++)
        for (int j = 0; j < 60 && !found; j++)
        {
            spot = { 120000.0f + 9100.0f * (float)i, -270000.0f + 9300.0f * (float)j };
            owner.ship->Teleport(spot);
            found = sim.PlacementProblem(owner, *bp, spot).empty();
        }
    REQUIRE(found);
    const int id = sim.Deploy(owner, "battery", spot, "Picket");
    REQUIRE(id != 0);
    Defence d = Mode(Engage::Strangers);
    d.players = { "ann" };
    CHECK(sim.DefenceSetProblem(owner, id, d).empty());
    REQUIRE(sim.SetDefence(owner, id, d));
    CHECK(sim.DismantleProblem(owner, id).empty());
    REQUIRE(sim.Dismantle(owner, id));
    for (const auto& c : bp->cost)
        CHECK(owner.ship->GetCargoAmount(c.first) ==
              c.second);  // a site just laid comes back whole
}

TEST_CASE("return fire also answers whoever shoots the owner's ship in its reach (#340)")
{
    Battery        b("hunter");
    ClientSession& owner = b.Pilot("hunter", 300.0f);
    const float    reach = b.It().GetSize() + b.It().GetArchetype()->weaponRange;
    SystemState&   st = *b.sim.SystemById(b.sys);

    // A pirate beside the owner's ship: by default the battery does not fire first on it.
    auto raider =
        std::make_unique<NpcShip>(Vector2{ b.at.x + 300.0f, b.at.y + 120.0f }, Fac::Pirates(),
                                  NpcRole::Pirate, std::vector<Vector2>{});
    raider->SetId(990001);
    NpcShip* pirate = raider.get();
    st.entities.push_back(std::move(raider));
    b.Defend();
    REQUIRE(b.fires.empty());

    SUBCASE("it shoots the owner, and the battery answers -- not a first strike, so no crime")
    {
        Law(b.sim, b.sys, true, Fac::Independent());
        std::vector<FireEvent> npcFire;
        b.sim.StepNpcCombat(st, b.players, &npcFire);
        REQUIRE_FALSE(npcFire.empty());
        REQUIRE(npcFire.back().targetSessionId == owner.id);
        const float whole = pirate->GetHull();
        b.Defend();
        REQUIRE_FALSE(b.fires.empty());
        CHECK(b.fires.back().targetSessionId == 0);
        CHECK(pirate->GetHull() < whole);
        CHECK(b.sim.OwedStanding("hunter", Fac::Independent()) == 0.0f);

        // ...for a while, as for a shot at the battery itself.
        b.fires.clear();
        b.sim.SetTime(b.sim.Time() + Simulation::PROVOKED_SECONDS + 1.0);
        b.Defend();
        CHECK(b.fires.empty());
    }
    SUBCASE("hold fire is hold fire")
    {
        b.Rules(Mode(Engage::Hold));
        std::vector<FireEvent> npcFire;
        b.sim.StepNpcCombat(st, b.players, &npcFire);
        REQUIRE_FALSE(npcFire.empty());
        b.Defend();
        CHECK(b.fires.empty());
    }
    SUBCASE("out of its reach, the owner's ship is on its own")
    {
        const Vector2 far = { b.at.x + reach + 500.0f, b.at.y };
        owner.ship->Teleport(far);
        pirate->SetPosition({ far.x, far.y + 120.0f });
        std::vector<FireEvent> npcFire;
        b.sim.StepNpcCombat(st, b.players, &npcFire);
        REQUIRE_FALSE(npcFire.empty());
        pirate->SetPosition({ b.at.x + 300.0f, b.at.y + 120.0f });  // back in the battery's range
        b.Defend();
        CHECK(b.fires.empty());
    }
    SUBCASE("another player's battery firing on the owner provokes it against that player")
    {
        st.entities.erase(std::remove_if(st.entities.begin(), st.entities.end(),
                                         [](const std::unique_ptr<Entity>& e)
                                         { return e->GetId() == 990001; }),
                          st.entities.end());
        ClientSession& rival = b.Pilot("rival", -400.0f);
        auto o = std::make_unique<Structure>(Vector2{ b.at.x + 300.0f, b.at.y + 1000.0f }, 0.0f,
                                             "Spite", Blueprints::Find("battery")->archetype);
        o->SetBlueprint("battery");
        o->SetDefence(Mode(Engage::Strangers));
        REQUIRE(b.sim.AddStatic(b.sys, std::move(o), "rival") != 0);
        b.Defend(70);
        REQUIRE(b.FiredAt(owner.id));  // the rival's battery fired on the owner...
        CHECK(b.FiredAt(rival.id));    // ...so the owner's answers the rival
    }
}

TEST_CASE("an allow-list reaches its owner only (#340)")
{
    Proto::SystemLayout lay;
    Proto::EntityLayout mine, theirs, plain;
    mine.id = 1;
    mine.owner = "hunter";
    mine.defence = Mode(Engage::Strangers);
    mine.defence.players = { "ann" };
    mine.defence.factions = { "TradersGuild" };
    theirs = mine;
    theirs.id = 2;
    theirs.owner = "ann";
    plain.id = 3;
    lay.entities = { mine, theirs, plain };

    Proto::SystemLayout forHunter = lay;
    CHECK(Proto::ForViewer(forHunter, "hunter"));
    CHECK(forHunter.entities[0].defence == mine.defence);              // their own, whole
    CHECK(forHunter.entities[1].defence.engage == Engage::Strangers);  // another's mode...
    CHECK(forHunter.entities[1].defence.players.empty());              // ...not whom it trusts
    CHECK(forHunter.entities[1].defence.factions.empty());

    Proto::SystemLayout forNobody = lay;
    CHECK(Proto::ForViewer(forNobody, ""));
    CHECK(forNobody.entities[0].defence.players.empty());

    // A delta is cut the same way, and says when the owner's differs from everyone's.
    Proto::LayoutDelta d;
    d.changed = { mine };
    CHECK(Proto::OwnsPrivate(d, "hunter"));
    CHECK_FALSE(Proto::OwnsPrivate(d, "ann"));
    CHECK_FALSE(Proto::OwnsPrivate(d, ""));
    Proto::LayoutDelta forAnn = d;
    CHECK(Proto::ForViewer(forAnn, "ann"));
    Proto::LayoutDelta back;
    REQUIRE(Proto::DecodeLayoutDelta(Proto::EncodeLayoutDelta(forAnn), back));
    REQUIRE(back.changed.size() == 1);
    CHECK(back.changed[0].defence.engage == Engage::Strangers);
    CHECK(back.changed[0].defence.players.empty());
    Proto::LayoutDelta forHunterToo = d;
    CHECK_FALSE(Proto::ForViewer(forHunterToo, "hunter"));  // nothing of anyone else's to cut
}
