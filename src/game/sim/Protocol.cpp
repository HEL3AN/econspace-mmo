#include "sim/Protocol.h"

#include <nlohmann/json.hpp>

#include <cmath>

namespace
{
using nlohmann::json;

json V2(Vector2 v)
{
    return json::array({ v.x, v.y });
}

// Positions and headings are sent to a precision a player could see, not to the last bit
// of a float (#16). A hundredth of a world unit is far below one pixel at any zoom, and
// the shorter number is the whole point: JSON pays for every digit.
//
// The rounding is done and kept in double. Rounding a float and letting JSON widen it
// writes the float's exact value back out -- 2615.79 becomes "2615.7900390625", which is
// longer than what it replaced. This was measured, not assumed.
double Round2(float v)
{
    return std::round((double)v * 100.0) / 100.0;
}

double Round3(float v)
{
    return std::round((double)v * 1000.0) / 1000.0;
}

json V2Rounded(Vector2 v)
{
    return json::array({ Round2(v.x), Round2(v.y) });
}

Vector2 ToV2(const json& j, Vector2 def = { 0.0f, 0.0f })
{
    if (j.is_array() && j.size() >= 2)
        return { (float)j[0], (float)j[1] };
    return def;
}

json Col(Color c)
{
    return json::array({ c.r, c.g, c.b, c.a });
}

Color ToCol(const json& j, Color def = { 255, 255, 255, 255 })
{
    if (j.is_array() && j.size() >= 4)
        return { (unsigned char)j[0], (unsigned char)j[1], (unsigned char)j[2],
                 (unsigned char)j[3] };
    return def;
}

// Every message carries the wire version alongside its type; see PROTO_VERSION.
void Stamp(json& j, const char* type)
{
    j["v"] = Proto::PROTO_VERSION;
    j["t"] = type;
}

// Parses a message and checks the envelope. False means broken JSON, not an object, or
// written by a different protocol version -- in all three cases the caller must not
// trust the contents, because permissive per-field decoding would happily invent them.
bool OpenEnvelope(const std::string& s, json& j)
{
    j = json::parse(s, nullptr, false);
    if (j.is_discarded() || !j.is_object())
        return false;
    return j.value("v", 0) == Proto::PROTO_VERSION;
}

json MissionJson(const Proto::MissionView& m)
{
    return { { "type", m.type },        { "fac", m.faction },          { "title", m.title },
             { "desc", m.description }, { "giver", m.giverStationId }, { "dest", m.destStationId },
             { "res", m.resource },     { "tc", m.targetCount },       { "prog", m.progress },
             { "rm", m.rewardMoney },   { "rr", m.rewardRep },         { "done", m.completable } };
}

Proto::MissionView ToMission(const json& mj)
{
    Proto::MissionView m;
    m.type = mj.value("type", 0);
    m.faction = mj.value("fac", 0);
    m.title = mj.value("title", std::string());
    m.description = mj.value("desc", std::string());
    m.giverStationId = mj.value("giver", 0);
    m.destStationId = mj.value("dest", 0);
    m.resource = mj.value("res", 0);
    m.targetCount = mj.value("tc", 0);
    m.progress = mj.value("prog", 0);
    m.rewardMoney = mj.value("rm", 0.0);
    m.rewardRep = mj.value("rr", 0.0f);
    m.completable = mj.value("done", false);
    return m;
}
}  // namespace

namespace Proto
{

std::string EncodeHello(const Hello& h)
{
    json j;
    Stamp(j, "hello");
    j["acct"] = h.account;
    return j.dump();
}

bool DecodeHello(const std::string& s, Hello& out)
{
    json j = json::parse(s, nullptr, false);
    if (j.is_discarded() || !j.is_object() || j.value("t", std::string()) != "hello" ||
        j.value("v", 0) != PROTO_VERSION)
        return false;
    out.account = j.value("acct", std::string());
    return true;
}

std::string EncodeChallenge(const Challenge& c)
{
    json j;
    Stamp(j, "chal");
    j["nonce"] = c.nonce;
    j["salt"] = c.salt;
    j["new"] = c.isNew;
    return j.dump();
}

bool DecodeChallenge(const std::string& s, Challenge& out)
{
    json j = json::parse(s, nullptr, false);
    if (j.is_discarded() || !j.is_object() || j.value("t", std::string()) != "chal" ||
        j.value("v", 0) != PROTO_VERSION)
        return false;
    out.nonce = j.value("nonce", std::string());
    out.salt = j.value("salt", std::string());
    out.isNew = j.value("new", false);
    return true;
}

std::string EncodeAuth(const Auth& a)
{
    json j;
    Stamp(j, "auth");
    j["proof"] = a.proof;
    j["stored"] = a.stored;
    return j.dump();
}

bool DecodeAuth(const std::string& s, Auth& out)
{
    json j = json::parse(s, nullptr, false);
    if (j.is_discarded() || !j.is_object() || j.value("t", std::string()) != "auth" ||
        j.value("v", 0) != PROTO_VERSION)
        return false;
    out.proof = j.value("proof", std::string());
    out.stored = j.value("stored", std::string());
    return true;
}

std::string EncodeBye(const Bye& b)
{
    json j;
    Stamp(j, "bye");
    j["why"] = b.reason;
    return j.dump();
}

bool DecodeBye(const std::string& s, Bye& out)
{
    json j = json::parse(s, nullptr, false);
    if (j.is_discarded() || !j.is_object() || j.value("t", std::string()) != "bye" ||
        j.value("v", 0) != PROTO_VERSION)
        return false;
    out.reason = j.value("why", std::string());
    return true;
}

std::string EncodeCommand(const Command& c)
{
    json j;
    Stamp(j, "cmd");
    j["seq"] = c.seq;
    j["thrust"] = c.thrust;
    j["turn"] = c.turn;
    j["brake"] = c.brake;
    j["tStab"] = c.toggleStabilizer;
    j["tMine"] = c.toggleMining;
    j["tWeap"] = c.toggleWeapon;
    j["dock"] = c.dock;
    j["undock"] = c.undock;
    j["target"] = c.targetId;
    j["navMode"] = c.navMode;
    j["nav"] = V2(c.navTarget);
    j["navStop"] = c.navStopDist;
    j["gate"] = c.jumpGateId;
    j["loot"] = c.lootId;
    j["sellType"] = c.sellType;
    j["sellAmt"] = c.sellAmount;
    j["refit"] = c.refitShip;
    j["buy"] = c.buyShip;
    j["payB"] = c.payBountyFaction;
    j["dbgMoney"] = c.debugMoney;
    j["acceptM"] = c.acceptOffer;
    j["completeM"] = c.completeMission;
    j["ordKind"] = c.orderKind;
    j["ordTarget"] = c.orderTarget;
    j["ordPoint"] = V2(c.orderPoint);
    j["ordStop"] = c.orderStopDist;
    j["ordWarp"] = c.orderWarp;
    j["ordFull"] = c.orderUntilFull;
    j["ordDest"] = c.orderDestSystem;
    j["ordAbort"] = c.abortOrder;
    return j.dump();
}

bool DecodeCommand(const std::string& s, Command& out)
{
    json j;
    if (!OpenEnvelope(s, j))
        return false;
    out.seq = j.value("seq", 0);
    out.thrust = j.value("thrust", false);
    out.turn = j.value("turn", 0.0f);
    out.brake = j.value("brake", false);
    out.toggleStabilizer = j.value("tStab", false);
    out.toggleMining = j.value("tMine", false);
    out.toggleWeapon = j.value("tWeap", false);
    out.dock = j.value("dock", false);
    out.undock = j.value("undock", false);
    out.targetId = j.value("target", 0);
    out.navMode = j.value("navMode", 0);
    out.navTarget = j.contains("nav") ? ToV2(j["nav"]) : Vector2{ 0.0f, 0.0f };
    out.navStopDist = j.value("navStop", 0.0f);
    out.jumpGateId = j.value("gate", 0);
    out.lootId = j.value("loot", 0);
    out.sellType = j.value("sellType", -1);
    out.sellAmount = j.value("sellAmt", 0);
    out.refitShip = j.value("refit", -1);
    out.buyShip = j.value("buy", -1);
    out.payBountyFaction = j.value("payB", -1);
    out.debugMoney = j.value("dbgMoney", false);
    out.acceptOffer = j.value("acceptM", -1);
    out.completeMission = j.value("completeM", -1);
    out.orderKind = j.value("ordKind", 0);
    out.orderTarget = j.value("ordTarget", 0);
    out.orderPoint = j.contains("ordPoint") ? ToV2(j["ordPoint"]) : Vector2{ 0.0f, 0.0f };
    out.orderStopDist = j.value("ordStop", 120.0f);
    out.orderWarp = j.value("ordWarp", false);
    out.orderUntilFull = j.value("ordFull", false);
    out.orderDestSystem = j.value("ordDest", std::string());
    out.abortOrder = j.value("ordAbort", false);
    return true;
}

std::string EncodeSnapshot(const Snapshot& s)
{
    json j;
    Stamp(j, "snap");
    j["sys"] = s.systemId;

    const PlayerView& p = s.player;
    j["player"] = { { "pos", V2(p.pos) },
                    { "vel", V2(p.vel) },
                    { "hdg", p.heading },
                    { "hull", p.hull },
                    { "maxHull", p.maxHull },
                    { "sh", p.shields },
                    { "maxSh", p.maxShields },
                    { "cargo", p.cargoUsed },
                    { "cargoCap", p.cargoCap },
                    { "warp", p.warpPhase },
                    { "warpAlign", p.warpAlign },
                    { "warpTgt", V2(p.warpTarget) },
                    { "warpDrop", p.warpDrop },
                    { "ap", p.autopilot },
                    { "apTgt", V2(p.apTarget) },
                    { "apStop", p.apStop },
                    { "stab", p.stabilizer },
                    { "mine", p.mining },
                    { "weap", p.weaponOn },
                    { "docked", p.docked },
                    { "dockId", p.dockedStationId },
                    { "nearStation", p.nearbyStationId },
                    { "lastInput", p.lastInput },
                    { "cargoT", p.cargoByType },
                    { "money", p.money },
                    { "rep", p.reputation },
                    { "bounty", p.bounty },
                    { "skillXp", p.skillXp },
                    { "ordKind", p.orderKind },
                    { "ordStatus", p.orderStatus },
                    { "ordId", p.orderId },
                    { "ordDetail", p.orderDetail },
                    { "ships", p.ownedShips },
                    { "ship", p.shipIndex } };

    // Only what this entity actually carries. Every field left at its decoding default is
    // a field the decoder will produce anyway, so writing it costs bytes and says nothing
    // (#16). What a station is called or how big it is comes from the layout instead.
    json ents = json::array();
    for (const EntitySnapshot& e : s.entities)
    {
        json ej = { { "id", e.id }, { "kind", (int)e.kind }, { "pos", V2Rounded(e.pos) } };
        if (e.heading != 0.0f)
            ej["hdg"] = Round3(e.heading);
        if (e.size != 0.0f)
            ej["size"] = Round2(e.size);
        if (e.faction != FactionId::Independent)
            ej["fac"] = (int)e.faction;
        if (e.role != -1)
            ej["role"] = e.role;
        if (e.hullFrac != 1.0f)
            ej["hullFrac"] = Round3(e.hullFrac);
        if (e.ore != -1)
            ej["ore"] = e.ore;
        if (!e.name.empty())
            ej["name"] = e.name;
        ents.push_back(std::move(ej));
    }
    j["ents"] = ents;

    json fires = json::array();
    for (const FireEvent& f : s.fires)
        fires.push_back({ { "from", V2(f.from) },
                          { "to", V2(f.to) },
                          { "fac", (int)f.shooterFaction },
                          { "pl", f.targetIsPlayer },
                          { "mine", f.fromPlayer } });
    j["fires"] = fires;

    json evs = json::array();
    for (const Ev::Event& e : s.events)
        evs.push_back({ { "seq", e.seq }, { "kind", (int)e.kind }, { "text", e.text } });
    j["events"] = evs;
    j["market"] = s.marketPrices;

    json acks = json::array();
    for (const TradeAck& a : s.tradeAcks)
        acks.push_back(
            { { "type", a.type }, { "sold", a.sold }, { "gross", a.gross }, { "rev", a.revenue } });
    j["trade"] = acks;

    json offers = json::array();
    for (const MissionView& m : s.missionOffers)
        offers.push_back(MissionJson(m));
    j["mOffers"] = offers;
    json activeM = json::array();
    for (const MissionView& m : s.missionActive)
        activeM.push_back(MissionJson(m));
    j["mActive"] = activeM;
    return j.dump();
}

bool DecodeSnapshot(const std::string& s, Snapshot& out)
{
    json j;
    if (!OpenEnvelope(s, j))
        return false;

    out.systemId = j.value("sys", std::string());

    if (j.contains("player") && j["player"].is_object())
    {
        const json& pj = j["player"];
        PlayerView& p = out.player;
        p.pos = ToV2(pj.value("pos", json::array()));
        p.vel = ToV2(pj.value("vel", json::array()));
        p.heading = pj.value("hdg", 0.0f);
        p.hull = pj.value("hull", 0.0f);
        p.maxHull = pj.value("maxHull", 0.0f);
        p.shields = pj.value("sh", 0.0f);
        p.maxShields = pj.value("maxSh", 0.0f);
        p.cargoUsed = pj.value("cargo", 0);
        p.cargoCap = pj.value("cargoCap", 0);
        p.warpPhase = pj.value("warp", 0);
        p.warpAlign = pj.value("warpAlign", 0.0f);
        p.warpTarget = pj.contains("warpTgt") ? ToV2(pj["warpTgt"]) : Vector2{ 0.0f, 0.0f };
        p.warpDrop = pj.value("warpDrop", 0.0f);
        p.autopilot = pj.value("ap", false);
        p.apTarget = pj.contains("apTgt") ? ToV2(pj["apTgt"]) : Vector2{ 0.0f, 0.0f };
        p.apStop = pj.value("apStop", 0.0f);
        p.stabilizer = pj.value("stab", true);
        p.mining = pj.value("mine", false);
        p.weaponOn = pj.value("weap", false);
        p.docked = pj.value("docked", false);
        p.dockedStationId = pj.value("dockId", 0);
        p.nearbyStationId = pj.value("nearStation", 0);
        p.lastInput = pj.value("lastInput", 0);
        p.cargoByType = pj.value("cargoT", std::vector<int>{});
        p.money = pj.value("money", 0.0);
        p.reputation = pj.value("rep", std::vector<float>{});
        p.bounty = pj.value("bounty", std::vector<double>{});
        p.skillXp = pj.value("skillXp", std::vector<float>{});
        p.orderKind = pj.value("ordKind", 0);
        p.orderStatus = pj.value("ordStatus", 0);
        p.orderId = pj.value("ordId", 0);
        p.orderDetail = pj.value("ordDetail", std::string());
        p.ownedShips = pj.value("ships", std::vector<int>{});
        p.shipIndex = pj.value("ship", 0);
    }

    out.entities.clear();
    if (j.contains("ents") && j["ents"].is_array())
        for (const json& ej : j["ents"])
        {
            EntitySnapshot e;
            e.id = ej.value("id", 0);
            e.kind = (EntityKind)ej.value("kind", 0);
            e.pos = ToV2(ej.value("pos", json::array()));
            e.heading = ej.value("hdg", 0.0f);
            e.size = ej.value("size", 0.0f);
            e.faction = (FactionId)ej.value("fac", 0);
            e.role = ej.value("role", -1);
            e.hullFrac = ej.value("hullFrac", 1.0f);
            e.ore = ej.value("ore", -1);
            e.name = ej.value("name", std::string());
            out.entities.push_back(e);
        }

    out.fires.clear();
    if (j.contains("fires") && j["fires"].is_array())
        for (const json& fj : j["fires"])
        {
            FireEvent f;
            f.from = ToV2(fj.value("from", json::array()));
            f.to = ToV2(fj.value("to", json::array()));
            f.shooterFaction = (FactionId)fj.value("fac", 0);
            f.targetIsPlayer = fj.value("pl", false);
            f.fromPlayer = fj.value("mine", false);
            out.fires.push_back(f);
        }

    out.events.clear();
    if (j.contains("events") && j["events"].is_array())
        for (const json& ej : j["events"])
        {
            Ev::Event e;
            e.seq = ej.value("seq", 0);
            e.kind = (Ev::Kind)ej.value("kind", 0);
            e.text = ej.value("text", std::string());
            out.events.push_back(std::move(e));
        }

    out.marketPrices = j.value("market", std::vector<float>{});

    out.tradeAcks.clear();
    if (j.contains("trade") && j["trade"].is_array())
        for (const json& aj : j["trade"])
        {
            TradeAck a;
            a.type = aj.value("type", 0);
            a.sold = aj.value("sold", 0);
            a.gross = aj.value("gross", 0.0);
            a.revenue = aj.value("rev", 0.0);
            out.tradeAcks.push_back(a);
        }

    out.missionOffers.clear();
    if (j.contains("mOffers") && j["mOffers"].is_array())
        for (const json& mj : j["mOffers"])
            out.missionOffers.push_back(ToMission(mj));
    out.missionActive.clear();
    if (j.contains("mActive") && j["mActive"].is_array())
        for (const json& mj : j["mActive"])
            out.missionActive.push_back(ToMission(mj));

    return true;
}

std::string EncodeLayout(const SystemLayout& s)
{
    json j;
    Stamp(j, "layout");
    j["sys"] = s.systemId;

    json ents = json::array();
    for (const EntityLayout& e : s.entities)
        ents.push_back({ { "id", e.id },
                         { "kind", (int)e.kind },
                         { "pos", V2(e.pos) },
                         { "size", e.size },
                         { "col", Col(e.color) },
                         { "name", e.name },
                         { "fac", (int)e.faction },
                         { "sub", e.subType },
                         { "orbit", e.orbitRadius },
                         { "res", e.resource },
                         { "reward", e.reward },
                         { "dest", e.dest } });
    j["ents"] = ents;
    return j.dump();
}

bool DecodeLayout(const std::string& s, SystemLayout& out)
{
    json j;
    if (!OpenEnvelope(s, j))
        return false;

    out.systemId = j.value("sys", std::string());
    out.entities.clear();
    if (j.contains("ents") && j["ents"].is_array())
        for (const json& ej : j["ents"])
        {
            EntityLayout e;
            e.id = ej.value("id", 0);
            e.kind = (EntityKind)ej.value("kind", 0);
            e.pos = ToV2(ej.value("pos", json::array()));
            e.size = ej.value("size", 0.0f);
            e.color = ToCol(ej.value("col", json::array()));
            e.name = ej.value("name", std::string());
            e.faction = (FactionId)ej.value("fac", 0);
            e.subType = ej.value("sub", 0);
            e.orbitRadius = ej.value("orbit", 0.0f);
            e.resource = ej.value("res", -1);
            e.reward = ej.value("reward", 0.0);
            e.dest = ej.value("dest", std::string());
            out.entities.push_back(e);
        }
    return true;
}

std::string EncodeGalaxy(const GalaxyState& s)
{
    json j;
    Stamp(j, "galaxy");
    json sys = json::array();
    for (const GalaxySystemStat& g : s.systems)
        sys.push_back({ { "id", g.id },
                        { "sec", g.security },
                        { "pir", g.pirates },
                        { "prosp", g.prosperity },
                        { "ctrl", (int)g.controller } });
    j["sys"] = sys;
    j["news"] = s.events;
    return j.dump();
}

bool DecodeGalaxy(const std::string& s, GalaxyState& out)
{
    json j;
    if (!OpenEnvelope(s, j))
        return false;

    out.systems.clear();
    if (j.contains("sys") && j["sys"].is_array())
        for (const json& gj : j["sys"])
        {
            GalaxySystemStat g;
            g.id = gj.value("id", std::string());
            g.security = gj.value("sec", 0.5f);
            g.pirates = gj.value("pir", 0);
            g.prosperity = gj.value("prosp", 0.5f);
            g.controller = (FactionId)gj.value("ctrl", 0);
            out.systems.push_back(g);
        }
    out.events = j.value("news", std::vector<std::string>{});
    return true;
}

std::string MessageType(const std::string& s)
{
    json j = json::parse(s, nullptr, false);
    if (j.is_discarded() || !j.is_object())
        return std::string();
    return j.value("t", std::string());
}

int MessageVersion(const std::string& s)
{
    json j = json::parse(s, nullptr, false);
    if (j.is_discarded() || !j.is_object())
        return 0;
    return j.value("v", 0);
}

void CompleteFromLayout(Snapshot& s, const std::map<int, EntityLayout>& layout)
{
    std::map<int, bool> present;
    for (EntitySnapshot& e : s.entities)
    {
        present[e.id] = true;
        auto it = layout.find(e.id);
        if (it == layout.end())
            continue;  // an NPC or another player: nothing static to fill in
        const EntityLayout& l = it->second;
        if (e.name.empty())
            e.name = l.name;
        if (e.size == 0.0f)
            e.size = l.size;
        if (e.faction == FactionId::Independent)
            e.faction = l.faction;
        if (e.ore == -1)
            e.ore = l.resource;
    }

    // Anything in the layout that the snapshot did not mention is standing exactly where
    // it was standing when the layout was sent -- a station, a gate, a belt (#97). It is
    // rebuilt here rather than sent, because sending it again every tick was describing a
    // world that had not changed.
    for (const auto& kv : layout)
    {
        if (present.count(kv.first))
            continue;
        const EntityLayout& l = kv.second;
        EntitySnapshot      e;
        e.id = l.id;
        e.kind = l.kind;
        e.pos = l.pos;
        e.size = l.size;
        e.name = l.name;
        e.faction = l.faction;
        e.ore = l.resource;
        s.entities.push_back(std::move(e));
    }
}

}  // namespace Proto
