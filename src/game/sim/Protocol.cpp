#include "sim/Protocol.h"

#include "core/World.h"
#include "economy/Resource.h"
#include "entities/Derelict.h"

#include <nlohmann/json.hpp>

#include <algorithm>
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

// A faction crosses the wire by its string id, never by this process's index (#355): the
// two ends load their registries separately. An id the reader does not have reads as the
// unaffiliated faction -- permissive, like every other field.
json FacJ(FactionId f)
{
    return Factions::Id(f);
}

FactionId ToFac(const json& j)
{
    FactionId f = Factions::Unaffiliated();
    if (j.is_string())
        Factions::Find(j.get<std::string>(), f);
    return f;
}

// A value per faction, as { id: value }; out is indexed by this process's registry.
template <class T> json PerFactionJson(const std::vector<T>& v)
{
    json o = json::object();
    for (FactionId f : Factions::All())
        if ((size_t)f.Index() < v.size())
            o[Factions::Id(f)] = v[(size_t)f.Index()];
    return o;
}

template <class T> std::vector<T> ToPerFaction(const json& j)
{
    std::vector<T> out((size_t)Factions::Count(), T{});
    if (!j.is_object())
        return out;
    for (auto it = j.begin(); it != j.end(); ++it)
    {
        FactionId f;
        if (it->is_number() && Factions::Find(it.key(), f))
            out[(size_t)f.Index()] = it->get<T>();
    }
    return out;
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

int VersionOf(const json& j);

// Parses a message and checks the envelope. False means broken JSON, not an object, or
// written by a different protocol version -- in all three cases the caller must not
// trust the contents, because permissive per-field decoding would happily invent them.
bool OpenEnvelope(const std::string& s, json& j)
{
    j = json::parse(s, nullptr, false);
    if (j.is_discarded() || !j.is_object())
        return false;
    return VersionOf(j) == Proto::PROTO_VERSION;
}

// The type and version of a parsed message, without trusting either field to be what it
// should: `value()` throws on a field of the wrong type, and an exception out of a decoder is
// a peer ending the server for everyone with one line of JSON (#341).
int VersionOf(const json& j)
{
    const auto it = j.find("v");
    if (it == j.end() || !it->is_number_integer())
        return 0;
    // A positive number parses as unsigned; read it as that, and anything absurd as no version.
    if (it->is_number_unsigned())
    {
        const unsigned long long u = it->get<unsigned long long>();
        return u < 1000000ULL ? (int)u : 0;
    }
    const long long v = it->get<long long>();
    return v > 0 && v < 1000000 ? (int)v : 0;
}

std::string TypeOf(const json& j)
{
    const auto it = j.find("t");
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::string TextOf(const json& j, const char* key)
{
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

// Parses a message of one type, whatever version it carries.
bool OpenTyped(const std::string& s, const char* type, json& j)
{
    j = json::parse(s, nullptr, false);
    return !j.is_discarded() && j.is_object() && TypeOf(j) == type;
}

// One object of a system's static layer. Shared by the whole layout and by a delta, which
// describe an object the same way -- a delta's `added` is exactly what a layout sent later
// would have said.
json EntityLayoutsJson(const std::vector<Proto::EntityLayout>& v)
{
    json ents = json::array();
    for (const Proto::EntityLayout& e : v)
    {
        json ej = { { "id", e.id },
                    { "kind", (int)e.kind },
                    { "pos", V2(e.pos) },
                    { "size", e.size },
                    { "col", Col(e.color) },
                    { "name", e.name },
                    { "fac", FacJ(e.faction) },
                    { "sub", e.subType },
                    { "orbit", e.orbitRadius },
                    { "res", e.resource },
                    { "reward", e.reward },
                    { "dest", e.dest },
                    { "arch", e.archetype } };
        // Only when there is something to say: almost everything belongs to the world and
        // almost no wreck has been searched.
        if (!e.owner.empty())
            ej["own"] = e.owner;
        if (e.looted)
            ej["looted"] = true;
        if (e.completesAt > 0.0)
        {
            ej["start"] = e.startedAt;
            ej["done"] = e.completesAt;
        }
        if (e.expiresAt > 0.0)
            ej["exp"] = e.expiresAt;
        if (!e.blueprint.empty())
            ej["bp"] = e.blueprint;
        if (e.damage > 0.0f)
            ej["dmg"] = e.damage;
        ents.push_back(std::move(ej));
    }
    return ents;
}

std::vector<Proto::EntityLayout> ToEntityLayouts(const json& j, const char* key)
{
    std::vector<Proto::EntityLayout> out;
    if (!j.contains(key) || !j[key].is_array())
        return out;
    for (const json& ej : j[key])
    {
        if (!ej.is_object())
            continue;
        Proto::EntityLayout e;
        e.id = ej.value("id", 0);
        e.kind = (Proto::EntityKind)ej.value("kind", 0);
        e.pos = ToV2(ej.value("pos", json::array()));
        e.size = ej.value("size", 0.0f);
        e.color = ToCol(ej.value("col", json::array()));
        e.name = ej.value("name", std::string());
        e.faction = ToFac(ej.value("fac", json()));
        e.subType = ej.value("sub", 0);
        e.orbitRadius = ej.value("orbit", 0.0f);
        e.resource = ej.value("res", -1);
        e.reward = ej.value("reward", 0.0);
        e.dest = ej.value("dest", std::string());
        e.archetype = ej.value("arch", std::string());
        e.owner = ej.value("own", std::string());
        e.looted = ej.value("looted", false);
        e.startedAt = ej.value("start", 0.0);
        e.completesAt = ej.value("done", 0.0);
        e.expiresAt = ej.value("exp", 0.0);
        e.blueprint = ej.value("bp", std::string());
        e.damage = ej.value("dmg", 0.0f);
        out.push_back(std::move(e));
    }
    return out;
}

json MissionJson(const Proto::MissionView& m)
{
    return { { "type", m.type },        { "fac", FacJ(m.faction) },    { "title", m.title },
             { "desc", m.description }, { "giver", m.giverStationId }, { "dest", m.destStationId },
             { "res", m.resource },     { "tc", m.targetCount },       { "prog", m.progress },
             { "rm", m.rewardMoney },   { "rr", m.rewardRep },         { "done", m.completable } };
}

Proto::MissionView ToMission(const json& mj)
{
    Proto::MissionView m;
    m.type = mj.value("type", 0);
    m.faction = ToFac(mj.value("fac", json()));
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
    json j;
    if (!OpenTyped(s, "hello", j) || VersionOf(j) != PROTO_VERSION)
        return false;
    out.account = TextOf(j, "acct");
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
    json j;
    if (!OpenTyped(s, "chal", j) || VersionOf(j) != PROTO_VERSION)
        return false;
    out.nonce = TextOf(j, "nonce");
    out.salt = TextOf(j, "salt");
    const auto isNew = j.find("new");
    out.isNew = isNew != j.end() && isNew->is_boolean() && isNew->get<bool>();
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
    json j;
    if (!OpenTyped(s, "auth", j) || VersionOf(j) != PROTO_VERSION)
        return false;
    out.proof = TextOf(j, "proof");
    out.stored = TextOf(j, "stored");
    return true;
}

std::string EncodeBye(const Bye& b)
{
    json j;
    Stamp(j, "bye");
    j["why"] = b.reason;
    return j.dump();
}

// Any version (#341): a Bye is how a server tells a client of another version that it is one,
// and a reason nobody can read is a silent disconnect again. The message has had one field
// since it was introduced, so reading it across versions guesses at nothing.
bool DecodeBye(const std::string& s, Bye& out)
{
    json j;
    if (!OpenTyped(s, "bye", j))
        return false;
    out.reason = TextOf(j, "why");
    return true;
}

std::string EncodeCommand(const Command& c)
{
    // Only what differs from a command that says nothing (#341). A client sends sixty of
    // these a second and nearly every field of nearly every one is its default; the decoder
    // reads an absent field as exactly that default, so leaving it out says the same thing
    // in a quarter of the bytes. The sequence number is always there: it is what the
    // server acknowledges.
    json j;
    Stamp(j, "cmd");
    j["seq"] = c.seq;
    auto flag = [&](const char* key, bool v)
    {
        if (v)
            j[key] = true;
    };
    auto number = [&](const char* key, int v, int def)
    {
        if (v != def)
            j[key] = v;
    };
    auto point = [&](const char* key, Vector2 v)
    {
        if (v.x != 0.0f || v.y != 0.0f)
            j[key] = V2(v);
    };
    flag("thrust", c.thrust);
    if (c.turn != 0.0f)
        j["turn"] = c.turn;
    flag("brake", c.brake);
    flag("tStab", c.toggleStabilizer);
    flag("tMine", c.toggleMining);
    flag("tWeap", c.toggleWeapon);
    flag("dock", c.dock);
    flag("undock", c.undock);
    number("target", c.targetId, 0);
    number("navMode", c.navMode, 0);
    point("nav", c.navTarget);
    if (c.navStopDist != 0.0f)
        j["navStop"] = c.navStopDist;
    if (c.navViaSet)  // only on the one command that orders a bent warp
        j["navVia"] = V2(c.navVia);
    if (c.navHoldId != 0)
    {
        j["navHold"] = c.navHoldId;
        j["navRange"] = Round2(c.navRange);
    }
    number("gate", c.jumpGateId, 0);
    number("loot", c.lootId, 0);
    number("sellType", c.sellType, -1);
    number("sellAmt", c.sellAmount, 0);
    number("refit", c.refitShip, -1);
    number("buy", c.buyShip, -1);
    if (!c.payBountyFaction.empty())
        j["payB"] = c.payBountyFaction;
    if (!c.nameSystem.empty())
        j["nameSys"] = c.nameSystem;
    flag("dbgMoney", c.debugMoney);
    number("acceptM", c.acceptOffer, -1);
    number("completeM", c.completeMission, -1);
    if (c.orderKind != 0)  // an order's fields mean something only with an order
    {
        j["ordKind"] = c.orderKind;
        number("ordTarget", c.orderTarget, 0);
        point("ordPoint", c.orderPoint);
        if (c.orderStopDist != 120.0f)
            j["ordStop"] = c.orderStopDist;
        flag("ordWarp", c.orderWarp);
        flag("ordFull", c.orderUntilFull);
        if (!c.orderDestSystem.empty())
            j["ordDest"] = c.orderDestSystem;
        if (!c.orderBlueprint.empty())
        {
            j["ordBp"] = c.orderBlueprint;
            if (!c.orderName.empty())
                j["ordName"] = c.orderName;
        }
        flag("ordSafe", c.orderAvoidDanger);
    }
    flag("ordAbort", c.abortOrder);
    if (!c.deploy.empty())  // one command in a great many
    {
        j["deploy"] = c.deploy;
        j["deployAt"] = V2(c.deployPos);
        if (!c.deployName.empty())
            j["deployName"] = c.deployName;
    }
    number("dismantle", c.dismantleId, 0);
    return j.dump();
}

}  // namespace Proto

// A command is the one message every client sends sixty times a second, and every byte of
// it is the peer's choice (#341). Nothing is taken on trust: a field of the wrong type makes
// the whole command invalid, a float that is not finite does too (1e39 is a fine double and
// an infinity as a float, and an infinite turn rate became NaN positions broadcast to every
// player in the system), and everything else is clamped to what a client could mean.
namespace
{
// Further than any order could sensibly name: a system is SYSTEM_RADIUS across each way.
constexpr double POINT_LIMIT = 4.0 * (double)World::SYSTEM_RADIUS;
// The longest text a command carries -- a name, a blueprint id. More is not a name.
constexpr size_t TEXT_LIMIT = 256;
// Ids and sequence numbers: any int that is not negative.
constexpr long long ID_MAX = 2147483647LL;

bool ReadFloat(const json& j, const char* key, float def, double lo, double hi, float& out)
{
    out = def;
    const auto it = j.find(key);
    if (it == j.end())
        return true;
    if (!it->is_number())
        return false;
    const double v = it->get<double>();
    if (!std::isfinite(v))
        return false;
    out = (float)std::min(hi, std::max(lo, v));
    return true;
}

bool ReadPoint(const json& j, const char* key, Vector2& out)
{
    out = { 0.0f, 0.0f };
    const auto it = j.find(key);
    if (it == j.end())
        return true;
    if (!it->is_array() || it->size() != 2)
        return false;
    const json xy = { { "x", (*it)[0] }, { "y", (*it)[1] } };
    return ReadFloat(xy, "x", 0.0f, -POINT_LIMIT, POINT_LIMIT, out.x) &&
           ReadFloat(xy, "y", 0.0f, -POINT_LIMIT, POINT_LIMIT, out.y);
}

// An integer, or false if the field is something else. Outside [lo, hi] it reads as `def`:
// an index past the end of a list is no index, and an id cannot be negative.
bool ReadInt(const json& j, const char* key, int def, long long lo, long long hi, int& out)
{
    out = def;
    const auto it = j.find(key);
    if (it == j.end())
        return true;
    if (!it->is_number_integer())
        return false;
    if (it->is_number_unsigned())
    {
        const unsigned long long u = it->get<unsigned long long>();
        if (u <= (unsigned long long)hi && (long long)u >= lo)
            out = (int)u;
        return true;
    }
    const long long v = it->get<long long>();
    if (v >= lo && v <= hi)
        out = (int)v;
    return true;
}

bool ReadBool(const json& j, const char* key, bool& out)
{
    out = false;
    const auto it = j.find(key);
    if (it == j.end())
        return true;
    if (!it->is_boolean())
        return false;
    out = it->get<bool>();
    return true;
}

bool ReadText(const json& j, const char* key, std::string& out)
{
    out.clear();
    const auto it = j.find(key);
    if (it == j.end())
        return true;
    if (!it->is_string())
        return false;
    out = it->get<std::string>();
    return out.size() <= TEXT_LIMIT;
}

bool ReadCommand(const json& j, Proto::Command& out)
{
    const double far = POINT_LIMIT;
    const int    resources = (int)AllResourceTypes().size();
    bool         ok = true;
    ok = ok && ReadInt(j, "seq", 0, 0, ID_MAX, out.seq);
    ok = ok && ReadBool(j, "thrust", out.thrust);
    // A turn is an axis, -1 to 1. Fifty turned the ship round in one tick.
    ok = ok && ReadFloat(j, "turn", 0.0f, -1.0, 1.0, out.turn);
    ok = ok && ReadBool(j, "brake", out.brake);
    ok = ok && ReadBool(j, "tStab", out.toggleStabilizer);
    ok = ok && ReadBool(j, "tMine", out.toggleMining);
    ok = ok && ReadBool(j, "tWeap", out.toggleWeapon);
    ok = ok && ReadBool(j, "dock", out.dock);
    ok = ok && ReadBool(j, "undock", out.undock);
    ok = ok && ReadInt(j, "target", 0, 0, ID_MAX, out.targetId);
    ok = ok && ReadInt(j, "navMode", 0, 0, 5, out.navMode);
    ok = ok && ReadPoint(j, "nav", out.navTarget);
    ok = ok && ReadFloat(j, "navStop", 0.0f, 0.0, far, out.navStopDist);
    out.navViaSet = j.contains("navVia");
    ok = ok && ReadPoint(j, "navVia", out.navVia);
    ok = ok && ReadInt(j, "navHold", 0, 0, ID_MAX, out.navHoldId);
    ok = ok && ReadFloat(j, "navRange", 0.0f, 0.0, far, out.navRange);
    ok = ok && ReadInt(j, "gate", 0, 0, ID_MAX, out.jumpGateId);
    ok = ok && ReadInt(j, "loot", 0, 0, ID_MAX, out.lootId);
    // Indices into fixed lists: one past the end used to be read and written as if it were
    // there -- a bounty paid to faction 1000 is memory that is not a bounty.
    ok = ok && ReadInt(j, "sellType", -1, 0, resources - 1, out.sellType);
    ok = ok && ReadInt(j, "sellAmt", 0, 0, ID_MAX, out.sellAmount);
    ok = ok && ReadInt(j, "refit", -1, 0, ID_MAX, out.refitShip);
    ok = ok && ReadInt(j, "buy", -1, 0, ID_MAX, out.buyShip);
    ok = ok && ReadText(j, "payB", out.payBountyFaction);
    ok = ok && ReadText(j, "nameSys", out.nameSystem);
    ok = ok && ReadBool(j, "dbgMoney", out.debugMoney);
    ok = ok && ReadInt(j, "acceptM", -1, 0, ID_MAX, out.acceptOffer);
    ok = ok && ReadInt(j, "completeM", -1, 0, ID_MAX, out.completeMission);
    ok = ok && ReadInt(j, "ordKind", 0, 0, (int)Orders::Kind::Build, out.orderKind);
    ok = ok && ReadInt(j, "ordTarget", 0, 0, ID_MAX, out.orderTarget);
    ok = ok && ReadPoint(j, "ordPoint", out.orderPoint);
    ok = ok && ReadFloat(j, "ordStop", 120.0f, 0.0, far, out.orderStopDist);
    ok = ok && ReadBool(j, "ordWarp", out.orderWarp);
    ok = ok && ReadBool(j, "ordFull", out.orderUntilFull);
    ok = ok && ReadText(j, "ordDest", out.orderDestSystem);
    ok = ok && ReadBool(j, "ordSafe", out.orderAvoidDanger);
    ok = ok && ReadText(j, "ordBp", out.orderBlueprint);
    ok = ok && ReadText(j, "ordName", out.orderName);
    ok = ok && ReadBool(j, "ordAbort", out.abortOrder);
    ok = ok && ReadText(j, "deploy", out.deploy);
    ok = ok && ReadPoint(j, "deployAt", out.deployPos);
    ok = ok && ReadText(j, "deployName", out.deployName);
    ok = ok && ReadInt(j, "dismantle", 0, 0, ID_MAX, out.dismantleId);
    return ok;
}
}  // namespace

namespace Proto
{

bool DecodeCommand(const std::string& s, Command& out)
{
    json j;
    if (!OpenEnvelope(s, j))
        return false;
    Command c;
    if (!ReadCommand(j, c))
        return false;
    out = std::move(c);
    return true;
}

std::string EncodeSnapshot(const Snapshot& s)
{
    json j;
    Stamp(j, "snap");
    j["sys"] = s.systemId;
    j["time"] = s.time;

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
                    { "hold", p.holdMode },
                    { "holdId", p.holdTargetId },
                    { "holdR", Round2(p.holdRange) },
                    { "stab", p.stabilizer },
                    { "mine", p.mining },
                    { "weap", p.weaponOn },
                    { "docked", p.docked },
                    { "dockId", p.dockedStationId },
                    { "nearStation", p.nearbyStationId },
                    { "lastInput", p.lastInput },
                    { "cargoT", p.cargoByType },
                    { "money", p.money },
                    { "rep", PerFactionJson(p.reputation) },
                    { "bounty", PerFactionJson(p.bounty) },
                    { "skillXp", p.skillXp },
                    { "ordKind", p.orderKind },
                    { "ordStatus", p.orderStatus },
                    { "ordId", p.orderId },
                    { "ordDetail", p.orderDetail },
                    { "ships", p.ownedShips },
                    { "shipDesigns", p.shipDesigns },
                    { "ship", p.shipIndex } };
    if (p.warpViaSet)  // only during the first leg of a bent warp
        j["player"]["warpVia"] = V2(p.warpVia);

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
        if (e.faction != Factions::Unaffiliated())
            ej["fac"] = FacJ(e.faction);
        if (e.role != -1)
            ej["role"] = e.role;
        if (e.hullFrac != 1.0f)
            ej["hullFrac"] = Round3(e.hullFrac);
        if (e.ore != -1)
            ej["ore"] = e.ore;
        if (!e.name.empty())
            ej["name"] = e.name;
        if (!e.design.empty())
            ej["design"] = e.design;
        ents.push_back(std::move(ej));
    }
    j["ents"] = ents;

    json fires = json::array();
    for (const FireEvent& f : s.fires)
        fires.push_back({ { "from", V2(f.from) },
                          { "to", V2(f.to) },
                          { "fac", FacJ(f.shooterFaction) },
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

    // Absent means unchanged since the last snapshot that had them (#341).
    if (s.missionsIncluded)
    {
        json offers = json::array();
        for (const MissionView& m : s.missionOffers)
            offers.push_back(MissionJson(m));
        j["mOffers"] = offers;
        json activeM = json::array();
        for (const MissionView& m : s.missionActive)
            activeM.push_back(MissionJson(m));
        j["mActive"] = activeM;
    }
    return j.dump();
}

std::string MissionsKey(const Snapshot& s)
{
    json k = json::array();
    for (const MissionView& m : s.missionOffers)
        k.push_back(MissionJson(m));
    k.push_back(nullptr);  // the boundary: an offer taken moves from one list to the other
    for (const MissionView& m : s.missionActive)
        k.push_back(MissionJson(m));
    return k.dump();
}

void CarryMissions(Snapshot& incoming, const Snapshot& previous)
{
    if (incoming.missionsIncluded)
        return;
    incoming.missionOffers = previous.missionOffers;
    incoming.missionActive = previous.missionActive;
    incoming.missionsIncluded = true;
}

bool DecodeSnapshot(const std::string& s, Snapshot& out)
{
    json j;
    if (!OpenEnvelope(s, j))
        return false;

    out.systemId = j.value("sys", std::string());
    out.time = j.value("time", 0.0);

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
        p.warpViaSet = pj.contains("warpVia");
        p.warpVia = p.warpViaSet ? ToV2(pj["warpVia"]) : Vector2{ 0.0f, 0.0f };
        p.autopilot = pj.value("ap", false);
        p.apTarget = pj.contains("apTgt") ? ToV2(pj["apTgt"]) : Vector2{ 0.0f, 0.0f };
        p.apStop = pj.value("apStop", 0.0f);
        p.holdMode = pj.value("hold", 0);
        p.holdTargetId = pj.value("holdId", 0);
        p.holdRange = pj.value("holdR", 0.0f);
        p.stabilizer = pj.value("stab", true);
        p.mining = pj.value("mine", false);
        p.weaponOn = pj.value("weap", false);
        p.docked = pj.value("docked", false);
        p.dockedStationId = pj.value("dockId", 0);
        p.nearbyStationId = pj.value("nearStation", 0);
        p.lastInput = pj.value("lastInput", 0);
        p.cargoByType = pj.value("cargoT", std::vector<int>{});
        p.money = pj.value("money", 0.0);
        p.reputation = ToPerFaction<float>(pj.value("rep", json()));
        p.bounty = ToPerFaction<double>(pj.value("bounty", json()));
        p.skillXp = pj.value("skillXp", std::vector<float>{});
        p.orderKind = pj.value("ordKind", 0);
        p.orderStatus = pj.value("ordStatus", 0);
        p.orderId = pj.value("ordId", 0);
        p.orderDetail = pj.value("ordDetail", std::string());
        p.ownedShips = pj.value("ships", std::vector<int>{});
        p.shipDesigns = pj.value("shipDesigns", std::vector<std::string>{});
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
            e.faction = ToFac(ej.value("fac", json()));
            e.role = ej.value("role", -1);
            e.hullFrac = ej.value("hullFrac", 1.0f);
            e.ore = ej.value("ore", -1);
            e.name = ej.value("name", std::string());
            e.design = ej.value("design", std::string());
            out.entities.push_back(e);
        }

    out.fires.clear();
    if (j.contains("fires") && j["fires"].is_array())
        for (const json& fj : j["fires"])
        {
            FireEvent f;
            f.from = ToV2(fj.value("from", json::array()));
            f.to = ToV2(fj.value("to", json::array()));
            f.shooterFaction = ToFac(fj.value("fac", json()));
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

    out.missionsIncluded = j.contains("mOffers") || j.contains("mActive");
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
    j["rev"] = s.rev;
    j["ents"] = EntityLayoutsJson(s.entities);
    return j.dump();
}

bool DecodeLayout(const std::string& s, SystemLayout& out)
{
    json j;
    if (!OpenEnvelope(s, j))
        return false;

    out.systemId = j.value("sys", std::string());
    out.rev = j.value("rev", 0);
    out.entities = ToEntityLayouts(j, "ents");
    return true;
}

std::string EncodeLayoutDelta(const LayoutDelta& d)
{
    json j;
    Stamp(j, "ldelta");
    j["sys"] = d.systemId;
    j["rev"] = d.rev;
    j["add"] = EntityLayoutsJson(d.added);
    j["chg"] = EntityLayoutsJson(d.changed);
    j["rem"] = d.removed;
    return j.dump();
}

bool DecodeLayoutDelta(const std::string& s, LayoutDelta& out)
{
    json j;
    if (!OpenEnvelope(s, j) || j.value("t", std::string()) != "ldelta")
        return false;

    out = LayoutDelta{};
    out.systemId = j.value("sys", std::string());
    out.rev = j.value("rev", 0);
    out.added = ToEntityLayouts(j, "add");
    out.changed = ToEntityLayouts(j, "chg");
    if (j.contains("rem") && j["rem"].is_array())
        for (const json& r : j["rem"])
            if (r.is_number_integer())
                out.removed.push_back(r.get<int>());
    return true;
}

std::string EncodeDesignDef(const DesignDef& d)
{
    json j;
    Stamp(j, "design");
    j["design"] = Ships::ToJson(d.design);
    return j.dump();
}

bool DecodeDesignDef(const std::string& s, DesignDef& out)
{
    json j;
    if (!OpenEnvelope(s, j) || j.value("t", std::string()) != "design" || !j.contains("design"))
        return false;
    Ships::Design d;
    std::string   why;
    if (!Ships::FromJson(j["design"], d, why))
        return false;
    // A player design's id is what it is; one that says otherwise is not that design.
    if (!Ships::IsPlayerDesign(d.id) || Ships::ContentId(d) != d.id)
        return false;
    out.design = std::move(d);
    return true;
}

void LayoutMirror::Reset(const SystemLayout& lay)
{
    systemId = lay.systemId;
    rev = lay.rev;
    byId.clear();
    for (const EntityLayout& el : lay.entities)
        byId[el.id] = el;
}

bool LayoutMirror::Apply(const LayoutDelta& d)
{
    if (d.systemId != systemId || d.rev <= rev)
        return false;
    // Replaces before removals: an object added and removed again since the last delta is
    // only in `removed`, but a client that already had it from a layout must lose it.
    for (const EntityLayout& el : d.added)
        byId[el.id] = el;
    for (const EntityLayout& el : d.changed)
        byId[el.id] = el;
    for (int id : d.removed)
        byId.erase(id);
    rev = d.rev;
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
                        { "ctrl", FacJ(g.controller) } });
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
            g.controller = ToFac(gj.value("ctrl", json()));
            out.systems.push_back(g);
        }
    out.events = j.value("news", std::vector<std::string>{});
    return true;
}

std::string EncodeUniverse(const WorldLoader::Universe& u)
{
    json j;
    Stamp(j, "universe");
    json sys = json::array();
    for (const WorldLoader::SystemInfo& si : u.systems)
        sys.push_back({ { "id", si.id },
                        { "name", si.name },
                        { "pos", V2(si.mapPos) },
                        { "sec", si.security },
                        { "owner", si.owner },
                        { "charted", si.charted },
                        { "desig", si.designation },
                        { "found", si.discoverer } });
    json links = json::array();
    for (const WorldLoader::SystemLink& l : u.links)
        links.push_back(json::array({ l.a, l.b }));
    j["sys"] = sys;
    j["links"] = links;
    j["start"] = u.startId;
    return j.dump();
}

bool DecodeUniverse(const std::string& s, WorldLoader::Universe& out)
{
    json j;
    if (!OpenEnvelope(s, j) || j.value("t", std::string()) != "universe")
        return false;

    out = WorldLoader::Universe{};
    if (j.contains("sys") && j["sys"].is_array())
        for (const json& sj : j["sys"])
        {
            if (!sj.is_object())
                continue;
            WorldLoader::SystemInfo si;
            si.id = sj.value("id", std::string());
            si.name = sj.value("name", std::string());
            si.mapPos = ToV2(sj.value("pos", json::array()));
            si.security = sj.value("sec", 0.5f);
            si.owner = sj.value("owner", std::string());
            si.charted = sj.value("charted", true);
            si.designation = sj.value("desig", std::string());
            si.discoverer = sj.value("found", std::string());
            out.systems.push_back(si);
        }
    if (j.contains("links") && j["links"].is_array())
        for (const json& lj : j["links"])
            if (lj.is_array() && lj.size() >= 2 && lj[0].is_string() && lj[1].is_string())
                out.links.push_back(
                    WorldLoader::SystemLink{ lj[0].get<std::string>(), lj[1].get<std::string>() });
    out.startId = j.value("start", std::string());
    return true;
}

std::string MessageType(const std::string& s)
{
    json j = json::parse(s, nullptr, false);
    if (j.is_discarded() || !j.is_object())
        return std::string();
    return TypeOf(j);
}

int MessageVersion(const std::string& s)
{
    json j = json::parse(s, nullptr, false);
    if (j.is_discarded() || !j.is_object())
        return 0;
    return VersionOf(j);
}

// The name an object goes by in the completed snapshot. The layout carries a wreck's name and
// whether it has been searched apart (#38), so a client building a Derelict from it does not
// say "(searched)" twice; everything that reads a snapshot gets the name a player would see.
static std::string ShownName(const EntityLayout& l)
{
    return l.kind == EntityKind::Derelict ? Derelict::DisplayName(l.name, l.looted) : l.name;
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
            e.name = ShownName(l);
        if (e.size == 0.0f)
            e.size = l.size;
        if (e.faction == Factions::Unaffiliated())
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
        e.name = ShownName(l);
        e.faction = l.faction;
        e.ore = l.resource;
        s.entities.push_back(std::move(e));
    }
}

}  // namespace Proto
