#include <doctest/doctest.h>

#include "goods_named.h"
#include "core/Archetype.h"
#include "core/Blueprint.h"
#include "core/Faction.h"
#include "entities/Defence.h"
#include "entities/NpcShip.h"
#include "entities/Ship.h"
#include "entities/ShipType.h"
#include "entities/Structure.h"
#include "sim/Chat.h"
#include "sim/ClientSession.h"
#include "sim/Corporation.h"
#include "sim/Protocol.h"
#include "sim/Simulation.h"
#include "temp_dir.h"

#include "factions_named.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>

// Player corporations (#347): who is in which, what each role may do, the corp channel, and
// the trust a member's structures extend to every member.

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

constexpr double NOW = 1.8e9;  // seconds, as Unix time

const std::set<std::string> ONLINE{ "anna", "boris", "cara", "dima" };

// Anna's corporation, with Boris in it and Cara a director.
Corps::Registry Founded()
{
    Corps::Registry r;
    REQUIRE(r.Create("anna", "Deep Haul", "dh", NOW).Ok());
    REQUIRE(r.InviteTo("anna", "Boris", ONLINE, NOW).Ok());
    REQUIRE(r.Join("boris", "DH", NOW + 1).Ok());
    REQUIRE(r.InviteTo("anna", "cara", ONLINE, NOW).Ok());
    REQUIRE(r.Join("cara", "dh", NOW + 2).Ok());
    REQUIRE(r.SetRole("anna", "cara", Corps::Role::Director).Ok());
    return r;
}
}  // namespace

TEST_CASE("founding a corporation: its name, its ticker, and one per pilot (#347)")
{
    LoadRegistries();
    Corps::Registry      r;
    const Corps::Outcome o = r.Create("anna", "Deep Haul", "dh", NOW);
    REQUIRE(o.Ok());
    const Corps::Corp* c = r.Of("anna");
    REQUIRE(c != nullptr);
    CHECK(c->id == o.corp);
    CHECK(c->id.rfind("c", 0) == 0);  // a string id, never an index (#355's rule)
    CHECK(c->ticker == "DH");         // kept upper case
    CHECK(c->Ceo()->account == "anna");
    CHECK(r.TickerOf("anna") == "DH");
    CHECK(r.FindTicker("dh") == c);

    CHECK_FALSE(r.Create("anna", "Second Try", "ST", NOW).Ok());   // in one already
    CHECK_FALSE(r.Create("boris", "deep haul", "XX", NOW).Ok());   // the name, in any case
    CHECK_FALSE(r.Create("boris", "Other Name", "Dh", NOW).Ok());  // the ticker
    CHECK_FALSE(r.Create("boris", "Ok Name", "X", NOW).Ok());      // too short a ticker
    CHECK_FALSE(r.Create("boris", "Ok Name", "AB-C", NOW).Ok());   // letters and digits only
    CHECK_FALSE(r.Create("boris", "x", "OK", NOW).Ok());           // a name is 3 to 24
    // Nobody passes as a faction.
    CHECK_FALSE(r.Create("boris", FactionName(Fac::TradersGuild()), "TG", NOW).Ok());
    CHECK(r.Create("boris", "Other Name", "ON", NOW).Ok());
    CHECK(r.All().size() == 2);
}

TEST_CASE("invites, joining and declining (#347)")
{
    LoadRegistries();
    Corps::Registry r;
    REQUIRE(r.Create("anna", "Deep Haul", "DH", NOW).Ok());

    CHECK_FALSE(r.Join("boris", "DH", NOW).Ok());              // not invited
    CHECK_FALSE(r.InviteTo("anna", "zed", ONLINE, NOW).Ok());  // not online
    const Corps::Outcome inv = r.InviteTo("anna", "Boris", ONLINE, NOW);
    REQUIRE(inv.Ok());
    CHECK(inv.subject == "boris");
    CHECK(inv.subjectText.find("Deep Haul") != std::string::npos);
    REQUIRE(r.InvitesFor("boris", NOW).size() == 1);

    SUBCASE("joined")
    {
        const Corps::Outcome j = r.Join("boris", "dh", NOW + 10);
        REQUIRE(j.Ok());
        CHECK(j.announce == "boris joined the corporation");
        CHECK(r.SameCorp("anna", "boris"));
        CHECK(r.InvitesFor("boris", NOW).empty());
        // A member invites nobody.
        CHECK_FALSE(r.InviteTo("boris", "cara", ONLINE, NOW).Ok());
    }
    SUBCASE("declined")
    {
        REQUIRE(r.Decline("boris", "DH").Ok());
        CHECK(r.InvitesFor("boris", NOW).empty());
        CHECK_FALSE(r.Join("boris", "DH", NOW).Ok());
    }
    SUBCASE("a week later the invite has run out")
    {
        CHECK_FALSE(r.Join("boris", "DH", NOW + Corps::INVITE_SECONDS + 1).Ok());
        r.Expire(NOW + Corps::INVITE_SECONDS + 1);
        CHECK(r.InvitesFor("boris", NOW).empty());
    }
    SUBCASE("founding one's own makes the invite moot")
    {
        REQUIRE(r.Create("boris", "Own Way", "OW", NOW).Ok());
        CHECK(r.InvitesFor("boris", NOW).empty());
    }
}

TEST_CASE("roles: who removes whom, handing over, and leaving (#347)")
{
    LoadRegistries();
    Corps::Registry r = Founded();
    REQUIRE(r.InviteTo("cara", "dima", ONLINE, NOW).Ok());  // a director invites
    REQUIRE(r.Join("dima", "DH", NOW).Ok());

    CHECK_FALSE(r.Kick("boris", "dima").Ok());  // a member removes nobody
    CHECK_FALSE(r.Kick("cara", "anna").Ok());   // a director, not the CEO...
    CHECK_FALSE(r.SetRole("cara", "boris", Corps::Role::Director).Ok());  // ...nor gives roles
    const Corps::Outcome k = r.Kick("cara", "dima");                      // ...but members
    REQUIRE(k.Ok());
    CHECK(k.subject == "dima");
    CHECK(r.Of("dima") == nullptr);
    CHECK(r.Kick("anna", "cara").Ok());  // the CEO removes anyone
    CHECK(r.Of("cara") == nullptr);

    // The CEO cannot leave others behind without a CEO; handing over makes them a director.
    CHECK_FALSE(r.Leave("anna").Ok());
    REQUIRE(r.SetRole("anna", "boris", Corps::Role::Ceo).Ok());
    const Corps::Corp* c = r.Of("anna");
    REQUIRE(c != nullptr);
    CHECK(c->Ceo()->account == "boris");
    CHECK(c->Find("anna")->role == Corps::Role::Director);
    CHECK(c->members.front().account == "boris");  // the CEO first
    REQUIRE(r.Leave("anna").Ok());
    // The last one out disbands it, and its name and ticker are free again.
    const Corps::Outcome last = r.Leave("boris");
    REQUIRE(last.Ok());
    CHECK(last.told.find("disbanded") != std::string::npos);
    CHECK(r.All().empty());
    CHECK(r.Create("cara", "Deep Haul", "DH", NOW).Ok());
}

TEST_CASE("who manages whose structures (#347)")
{
    LoadRegistries();
    const Corps::Registry r = Founded();
    CHECK(r.Manages("boris", "boris"));       // one's own
    CHECK(r.Manages("anna", "boris"));        // the CEO
    CHECK(r.Manages("cara", "boris"));        // a director
    CHECK_FALSE(r.Manages("boris", "anna"));  // a member, nobody else's
    CHECK_FALSE(r.Manages("dima", "anna"));   // nor a stranger
    CHECK_FALSE(r.Manages("", ""));
    CHECK_FALSE(r.SameCorp("", ""));
    CHECK_FALSE(r.SameCorp("anna", "dima"));
}

TEST_CASE("corps.json: written and read back, strict about what it holds (#347)")
{
    LoadRegistries();
    const Corps::Registry r = Founded();
    const TempDir         tmp;
    const std::string     path = tmp.Path("corps.json");
    REQUIRE(r.Save(path));

    Corps::Registry back;
    REQUIRE(back.Load(path) == Save::Result::Ok);
    REQUIRE(back.Of("cara") != nullptr);
    CHECK(back.Of("cara")->Find("cara")->role == Corps::Role::Director);
    CHECK(back.SameCorp("anna", "boris"));
    CHECK(back.TickerOf("boris") == "DH");
    // An id is never handed out twice.
    REQUIRE(back.Create("dima", "Fresh Start", "FS", NOW).Ok());
    CHECK(back.Of("dima")->id != back.Of("anna")->id);

    Corps::Registry none;
    CHECK(none.Load(tmp.Path("nothing.json")) == Save::Result::Missing);

    nlohmann::json j = r.ToJson();
    j["version"] = Save::CORPS_VERSION + 1;
    {
        std::ofstream out(tmp.Path("future.json"));
        out << j.dump();
    }
    CHECK(none.Load(tmp.Path("future.json")) == Save::Result::TooNew);

    // Two CEOs, or a pilot in two corporations, is damage -- not something to guess about.
    std::string    why;
    nlohmann::json two = r.ToJson();
    two["corps"][0]["members"][1]["role"] = "ceo";
    CHECK_FALSE(none.FromJson(two, why));
    nlohmann::json twice = r.ToJson();
    nlohmann::json other = twice["corps"][0];
    other["id"] = "c99";
    other["name"] = "Shadow";
    other["ticker"] = "SH";
    twice["corps"].push_back(other);
    CHECK_FALSE(none.FromJson(twice, why));
}

TEST_CASE("the corp channel reaches the corporation's members online, and nobody else (#347)")
{
    const Chat::Pilot              anna(1, "anna", "sol", "c1", "DH");
    const Chat::Pilot              boris(2, "boris", "vega", "c1", "DH");
    const Chat::Pilot              cara(3, "cara", "sol");
    const std::vector<Chat::Pilot> online{ anna, boris, cara };
    Chat::Hub                      hub;

    Proto::ChatSay say;
    say.channel = Chat::CORP;
    say.text = "form up";
    const std::vector<Chat::Delivery> out = hub.Say(anna, say, online, NOW);
    REQUIRE(out.size() == 2);
    for (const Chat::Delivery& d : out)
    {
        CHECK((d.to == 1 || d.to == 2));  // wherever they are, and not Cara beside Anna
        CHECK(d.line.channel == std::string(Chat::CORP));
        CHECK(d.line.corp == "DH");
        CHECK(d.line.from == "anna");
    }
    // Not kept for whoever arrives in the system: it was not said there.
    CHECK(hub.History("sol", NOW).empty());

    // In no corporation, a line to one is a notice.
    const std::vector<Chat::Delivery> none = hub.Say(cara, say, online, NOW);
    REQUIRE(none.size() == 1);
    CHECK(none[0].to == 3);
    CHECK(none[0].line.channel == std::string(Chat::NOTICE));

    // The server's word on the channel.
    const std::vector<Chat::Delivery> told = hub.ToCorp("c1", "DH", "boris joined", online, NOW);
    CHECK(told.size() == 2);
    CHECK(told[0].line.from.empty());
}

TEST_CASE("the local list carries each pilot's ticker, and says so when it changes (#347)")
{
    Chat::Hub                hub;
    std::vector<Chat::Pilot> online{ Chat::Pilot(1, "anna", "sol"),
                                     Chat::Pilot(2, "boris", "sol") };
    REQUIRE(hub.Track(online, NOW).size() == 2);
    CHECK(hub.Track(online, NOW).empty());  // nothing changed

    online[1].corp = "c1";
    online[1].ticker = "DH";
    const std::vector<Chat::Presence> p = hub.Track(online, NOW);
    REQUIRE(p.size() == 2);  // both are told: Boris is listed differently now
    CHECK(p[0].members.pilots == std::vector<std::string>{ "anna", "boris" });
    CHECK(p[0].members.corps == std::vector<std::string>{ "", "DH" });

    const std::string  wire = Proto::EncodeChatMembers(p[0].members);
    Proto::ChatMembers m;
    REQUIRE(Proto::DecodeChatMembers(wire, m));
    CHECK(m.corps == p[0].members.corps);
}

TEST_CASE("corporations on the wire (#347)")
{
    Proto::CorpAct a;
    a.verb = "role";
    a.pilot = "boris";
    a.role = "director";
    Proto::CorpAct a2;
    REQUIRE(Proto::DecodeCorpAct(Proto::EncodeCorpAct(a), a2));
    CHECK(a2.verb == "role");
    CHECK(a2.pilot == "boris");
    CHECK(a2.role == "director");
    CHECK(a2.name.empty());

    Proto::CorpView v;
    v.id = "c3";
    v.name = "Deep Haul";
    v.ticker = "DH";
    v.founded = 1800000000;
    v.members = { { "anna", "ceo", true }, { "boris", "member", false } };
    v.invites = { { "Other", "OT", "zed", 1800000100 } };
    Proto::CorpView v2;
    REQUIRE(Proto::DecodeCorpView(Proto::EncodeCorpView(v), v2));
    CHECK(v2.id == "c3");
    CHECK(v2.ticker == "DH");
    CHECK(v2.founded == 1800000000);
    REQUIRE(v2.members.size() == 2);
    CHECK(v2.members[0].online);
    CHECK_FALSE(v2.members[1].online);
    CHECK(v2.RoleOf("boris") == "member");
    CHECK(v2.RoleOf("zed").empty());
    REQUIRE(v2.invites.size() == 1);
    CHECK(v2.invites[0].by == "zed");

    Proto::CorpView empty;
    REQUIRE(Proto::DecodeCorpView(Proto::EncodeCorpView(Proto::CorpView{}), empty));
    CHECK(empty.id.empty());

    Proto::ChatSay say;
    say.channel = Chat::CORP;
    say.text = "hi";
    Proto::ChatSay say2;
    REQUIRE(Proto::DecodeChatSay(Proto::EncodeChatSay(say), say2));
    CHECK(say2.channel == std::string(Chat::CORP));

    // A director reads a member's allow-list; nobody else does.
    Proto::SystemLayout lay;
    Proto::EntityLayout el;
    el.id = 7;
    el.owner = "boris";
    el.defence.players = { "friend" };
    lay.entities.push_back(el);
    Proto::SystemLayout forDirector = lay, forStranger = lay;
    CHECK_FALSE(Proto::ForViewer(forDirector, [](const std::string& o) { return o == "boris"; }));
    CHECK(forDirector.entities[0].defence.players.size() == 1);
    CHECK(Proto::ForViewer(forStranger, [](const std::string&) { return false; }));
    CHECK(forStranger.entities[0].defence.players.empty());
}

// --- What a member's structures do for the corporation ----------------------------------------

namespace
{
ClientSession& Pilot(Simulation& sim, const std::string& account, Vector2 at)
{
    ClientSession& s =
        sim.CreateSession(sim.Universe().startId, Vector2{ 0.0f, 0.0f }, GetShipCatalog()[0].stats);
    s.accountName = account;
    s.ship->Teleport(at);
    return s;
}

// An armed battery of Anna's in the start system, far from everything, with no NPCs about.
struct Guarded
{
    Simulation                              sim;
    std::string                             sys;
    Vector2                                 at{ 400000.0f, -400000.0f };
    int                                     id = 0;
    std::vector<Simulation::PlayerPresence> players;
    std::vector<FireEvent>                  fires;

    Guarded()
    {
        LoadRegistries();
        sim.LoadUniverse(std::string(TEST_WORLD_DIR) + "universe.json");
        sim.Seed(1234u);
        sim.InitGalaxy();
        sim.MaterializeAllSystems(std::string(TEST_WORLD_DIR) + "systems/");
        sys = sim.Universe().startId;
        auto& ents = sim.SystemById(sys)->entities;
        ents.erase(std::remove_if(ents.begin(), ents.end(), [](const std::unique_ptr<Entity>& e)
                                  { return e->GetKind() == EntityKind::Npc; }),
                   ents.end());
        sim.SystemById(sys)->agg.claimed = false;  // nobody's law: only who is fired on matters
        auto o =
            std::make_unique<Structure>(at, 0.0f, "Picket", Blueprints::Find("battery")->archetype);
        o->SetBlueprint("battery");
        id = sim.AddStatic(sys, std::move(o), "anna");
        REQUIRE(id != 0);
        Corps::Registry& r = sim.Corporations();
        REQUIRE(r.Create("anna", "Deep Haul", "DH", NOW).Ok());
        REQUIRE(r.InviteTo("anna", "boris", { "boris" }, NOW).Ok());
        REQUIRE(r.Join("boris", "DH", NOW).Ok());
    }
    ClientSession& Add(const std::string& account, float dx)
    {
        ClientSession&             s = Pilot(sim, account, { at.x + dx, at.y });
        Simulation::PlayerPresence p;
        p.session = &s;
        p.ship = s.ship.get();
        players.push_back(p);
        return s;
    }
    Structure& It()
    {
        for (auto& e : sim.SystemById(sys)->entities)
            if (e->GetId() == id)
                return static_cast<Structure&>(*e);
        FAIL("the battery is gone");
        throw 0;
    }
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
}  // namespace

TEST_CASE("a member's battery never fires first on the corporation (#347)")
{
    Guarded        g;
    ClientSession& boris = g.Add("boris", 300.0f);
    ClientSession& cara = g.Add("cara", -300.0f);
    Defence        d;
    d.engage = Engage::Strangers;  // everyone not on the list -- and Boris is on no list
    g.It().SetDefence(d);
    g.Defend();
    CHECK(g.FiredAt(cara.id));
    CHECK_FALSE(g.FiredAt(boris.id));

    SUBCASE("...nor once he is wanted, in enemies on sight")
    {
        d.engage = Engage::Enemies;
        g.It().SetDefence(d);
        boris.account.AddBounty(Fac::Syndicate(), 10.0);
        g.Defend();
        CHECK_FALSE(g.FiredAt(boris.id));
    }
    // Cara out of reach from here on: the battery takes the nearer of two at once.
    g.fires.clear();
    cara.ship->Teleport({ g.at.x + 90000.0f, g.at.y });
    SUBCASE("...but a corp mate who shoots it is answered: trust, not immunity")
    {
        g.sim.SystemById(g.sys)->provokedBy[g.id][boris.id] =
            g.sim.Time() + Simulation::PROVOKED_SECONDS;
        g.Defend();
        CHECK(g.FiredAt(boris.id));
    }
    SUBCASE("...and out of the corporation, he is a stranger again")
    {
        REQUIRE(g.sim.Corporations().Leave("boris").Ok());
        g.Defend();
        CHECK(g.FiredAt(boris.id));
    }
}

TEST_CASE("a member's battery guards every member's ship (#347)")
{
    Guarded        g;
    ClientSession& boris = g.Add("boris", 300.0f);
    ClientSession& cara = g.Add("cara", -300.0f);
    g.Defend();
    REQUIRE(g.fires.empty());  // return fire, and nobody has shot anything

    // Cara shoots Boris's ship beside Anna's battery: it answers her.
    g.sim.ProvokeGuards(*g.sim.SystemById(g.sys), boris, boris.ship->GetPosition(), cara.id, 0);
    g.Defend();
    CHECK(g.FiredAt(cara.id));
    CHECK_FALSE(g.FiredAt(boris.id));

    // A stranger's ship is nobody's to guard.
    g.fires.clear();
    g.sim.SetTime(g.sim.Time() + Simulation::PROVOKED_SECONDS + 1.0);
    ClientSession& dima = g.Add("dima", 500.0f);
    g.sim.ProvokeGuards(*g.sim.SystemById(g.sys), dima, dima.ship->GetPosition(), cara.id, 0);
    g.Defend();
    CHECK(g.fires.empty());
}

TEST_CASE("the CEO or a director sets a member's rules; a member does not (#347)")
{
    Guarded          g;
    Corps::Registry& r = g.sim.Corporations();
    REQUIRE(r.InviteTo("anna", "cara", { "cara" }, NOW).Ok());
    REQUIRE(r.Join("cara", "DH", NOW).Ok());
    REQUIRE(r.SetRole("anna", "cara", Corps::Role::Director).Ok());
    ClientSession& anna = g.Add("anna", 900.0f);
    ClientSession& boris = g.Add("boris", 300.0f);
    ClientSession& cara = g.Add("cara", -300.0f);
    Defence        d;
    d.engage = Engage::Hold;

    CHECK(g.sim.DefenceSetProblem(boris, g.id, d).find("not yours") != std::string::npos);
    const int seq = anna.LastEventSeq();
    REQUIRE(g.sim.SetDefence(cara, g.id, d));
    CHECK(g.It().GetDefence().engage == Engage::Hold);
    bool told = false;  // the owner hears who set it
    for (const Ev::Event& e : anna.EventsSince(seq))
        told = told || e.text.rfind("cara set the rules of engagement for Picket", 0) == 0;
    CHECK(told);

    REQUIRE(r.SetRole("anna", "cara", Corps::Role::Member).Ok());
    CHECK_FALSE(g.sim.DefenceSetProblem(cara, g.id, d).empty());
}
