#include "sim/Protocol.h"

#include "core/Route.h"
#include "core/World.h"
#include "economy/Resource.h"
#include "entities/Derelict.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>

namespace
{
using nlohmann::json;

json V2(Vector2 v)
{
    return json::array({ v.x, v.y });
}

// A player-view distance is sent to a hundredth, not to the last bit of a float (#16).
// The rounding is done and kept in double: rounding a float and letting JSON widen it writes
// the float's exact value back out -- 2615.79 becomes "2615.7900390625", which is longer than
// what it replaced. Entities are integers since #349 (WireOf, below).
double Round2(float v)
{
    return std::round((double)v * 100.0) / 100.0;
}

// A number read as the type asked for, clamped to its range: casting a double that does not
// fit is undefined, and a peer chooses the double.
template <class T> T Clamped(const json& v)
{
    static_assert(std::is_arithmetic_v<T> && !std::is_same_v<T, bool>);
    if constexpr (std::is_integral_v<T>)
    {
        if (v.is_number_integer())
        {
            // Through long long, which holds anything the wire carries for a 32-bit field;
            // an unsigned past it is as far out of range as any other.
            if (v.is_number_unsigned() &&
                v.get<unsigned long long>() >
                    (unsigned long long)std::numeric_limits<long long>::max())
                return std::numeric_limits<T>::max();
            const long long n = v.get<long long>();
            if (n < (long long)std::numeric_limits<T>::lowest())
                return std::numeric_limits<T>::lowest();
            if (n > (long long)std::numeric_limits<T>::max())
                return std::numeric_limits<T>::max();
            return (T)n;
        }
        const double d = v.get<double>();
        if (!(d > (double)std::numeric_limits<T>::lowest()))
            return std::numeric_limits<T>::lowest();
        if (!(d < (double)std::numeric_limits<T>::max()))
            return std::numeric_limits<T>::max();
        return (T)d;
    }
    else
    {
        const double top = (double)std::numeric_limits<T>::max();
        return (T)std::clamp(v.get<double>(), -top, top);
    }
}

// A field read as what it must be, or `def` when it is missing or something else. nlohmann's
// `value()` does the first and throws on the second, and an exception out of a decoder is a
// peer ending the process for everyone with one line of JSON (#341, found again by the
// fuzzers, #518).
template <class T> T Val(const json& j, const char* key, T def)
{
    const auto it = j.find(key);  // an end() for anything that is not an object
    if (it == j.end())
        return def;
    if constexpr (std::is_same_v<T, bool>)
        return it->is_boolean() ? it->template get<bool>() : def;
    else if constexpr (std::is_arithmetic_v<T>)
        return it->is_number() ? Clamped<T>(*it) : def;
    else
    {
        try
        {
            return it->template get<T>();
        }
        catch (const json::exception&)
        {
            return def;
        }
    }
}

Vector2 ToV2(const json& j, Vector2 def = { 0.0f, 0.0f })
{
    if (j.is_array() && j.size() >= 2 && j[0].is_number() && j[1].is_number())
        return { Clamped<float>(j[0]), Clamped<float>(j[1]) };
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
        if (!e.ownerName.empty() && e.ownerName != e.owner)
            ej["ownn"] = e.ownerName;
        if (e.looted)
            ej["looted"] = true;
        if (!e.holds.empty())
            ej["holds"] = e.holds;
        if (e.completesAt > 0.0)
        {
            ej["start"] = e.startedAt;
            ej["done"] = e.completesAt;
        }
        if (e.expiresAt > 0.0)
            ej["exp"] = e.expiresAt;
        if (!e.blueprint.empty())
            ej["bp"] = e.blueprint;
        if (!e.grewFrom.empty())
            ej["bpFrom"] = e.grewFrom;
        if (e.damage > 0.0f)
        {
            ej["dmg"] = e.damage;
            ej["dmgAt"] = e.damagedAt;
        }
        if (!e.mends)
            ej["nomend"] = true;
        if (!e.defence.IsDefault())
            ej["def"] = DefenceJson(e.defence);
        if (e.licence != 0)
            ej["lic"] = json::array({ e.licence, e.licenceUntil });
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
        e.id = Val(ej, "id", 0);
        e.kind = (Proto::EntityKind)Val(ej, "kind", 0);
        e.pos = ToV2(Val(ej, "pos", json::array()));
        e.size = Val(ej, "size", 0.0f);
        e.color = ToCol(Val(ej, "col", json::array()));
        e.name = Val(ej, "name", std::string());
        e.faction = ToFac(Val(ej, "fac", json()));
        e.subType = Val(ej, "sub", 0);
        e.orbitRadius = Val(ej, "orbit", 0.0f);
        e.resource = Val(ej, "res", -1);
        e.reward = Val(ej, "reward", 0.0);
        e.dest = Val(ej, "dest", std::string());
        e.archetype = Val(ej, "arch", std::string());
        e.owner = Val(ej, "own", std::string());
        e.ownerName = Val(ej, "ownn", e.owner);
        e.looted = Val(ej, "looted", false);
        const auto holds = ej.find("holds");
        if (holds != ej.end() && holds->is_array() && holds->size() <= (size_t)Goods::Count())
            for (const json& n : *holds)
                e.holds.push_back(n.is_number_integer() ? std::max(0, n.get<int>()) : 0);
        e.startedAt = Val(ej, "start", 0.0);
        e.completesAt = Val(ej, "done", 0.0);
        e.expiresAt = Val(ej, "exp", 0.0);
        e.blueprint = Val(ej, "bp", std::string());
        const auto from = ej.find("bpFrom");
        if (from != ej.end() && from->is_array())
            for (const json& id : *from)
                if (id.is_string())
                    e.grewFrom.push_back(id.get<std::string>());
        e.damage = Val(ej, "dmg", 0.0f);
        e.damagedAt = Val(ej, "dmgAt", 0.0);
        e.mends = !Val(ej, "nomend", false);
        if (ej.contains("def"))
            DefenceFrom(ej["def"], e.defence);  // what it cannot read leaves the default
        const auto lic = ej.find("lic");
        if (lic != ej.end() && lic->is_array() && lic->size() == 2 &&
            (*lic)[0].is_number_integer() && (*lic)[1].is_number())
        {
            e.licence = (*lic)[0].get<int>();
            e.licenceUntil = (*lic)[1].get<double>();
        }
        out.push_back(std::move(e));
    }
    return out;
}

// A licence (#431 slice 6), in the snapshot and in a corporation's view.
json LicenceJson(const Proto::LicenceView& l)
{
    json j = { { "holder", l.holder }, { "party", l.party },     { "ok", l.admitted },
               { "rent", l.rent },     { "days", l.periodDays }, { "state", l.state },
               { "until", l.until },   { "pay", l.canPay } };
    if (!l.system.empty())
        j["sys"] = l.system;
    if (!l.why.empty())
        j["why"] = l.why;
    if (!l.where.empty())
        j["where"] = l.where;
    if (l.atHolder)
        j["here"] = true;
    return j;
}

Proto::LicenceView LicenceFrom(const json& j)
{
    Proto::LicenceView l;
    l.system = Val(j, "sys", std::string());
    l.holder = Val(j, "holder", std::string());
    l.party = Val(j, "party", std::string());
    l.admitted = Val(j, "ok", false);
    l.why = Val(j, "why", std::string());
    l.rent = Val(j, "rent", 0.0);
    l.periodDays = Val(j, "days", 7.0);
    l.state = Val(j, "state", std::string("none"));
    l.until = Val(j, "until", 0.0);
    l.canPay = Val(j, "pay", false);
    l.where = Val(j, "where", std::string());
    l.atHolder = Val(j, "here", false);
    return l;
}

json MissionJson(const Proto::MissionView& m)
{
    json j = {
        { "type", m.type },        { "fac", FacJ(m.faction) },    { "title", m.title },
        { "desc", m.description }, { "giver", m.giverStationId }, { "dest", m.destStationId },
        { "res", m.resource },     { "tc", m.targetCount },       { "prog", m.progress },
        { "rm", m.rewardMoney },   { "rr", m.rewardRep },         { "done", m.completable }
    };
    if (m.offerId != 0)  // an offer's number (#425)
        j["oid"] = m.offerId;
    if (m.planId != 0 || !m.systemId.empty())  // a plan's job (#345), a contract (#343)
    {
        j["plan"] = m.planId;
        j["sys"] = m.systemId;
        j["where"] = m.systemName;
        j["dl"] = m.deadline;
        j["site"] = m.siteId;
        if (!m.siteName.empty())
            j["siteName"] = m.siteName;
        if (m.against != Factions::Unaffiliated())
            j["vs"] = FacJ(m.against);  // the other side of a siege (#407)
    }
    if (!m.convoySystemId.empty())  // an escort's convoy (#430)
        j["cv"] = { { "sys", m.convoySystemId },
                    { "where", m.convoySystemName },
                    { "next", m.convoyNextName },
                    { "go", m.convoyMoving },
                    { "h", m.haulerId },
                    { "esc", m.convoyEscortIds },
                    { "rode", m.rode } };
    return j;
}

Proto::MissionView ToMission(const json& mj)
{
    Proto::MissionView m;
    m.type = Val(mj, "type", 0);
    m.faction = ToFac(Val(mj, "fac", json()));
    m.title = Val(mj, "title", std::string());
    m.description = Val(mj, "desc", std::string());
    m.giverStationId = Val(mj, "giver", 0);
    m.destStationId = Val(mj, "dest", 0);
    m.offerId = Val(mj, "oid", 0);
    m.resource = Val(mj, "res", 0);
    m.targetCount = Val(mj, "tc", 0);
    m.progress = Val(mj, "prog", 0);
    m.rewardMoney = Val(mj, "rm", 0.0);
    m.rewardRep = Val(mj, "rr", 0.0f);
    m.completable = Val(mj, "done", false);
    m.planId = Val(mj, "plan", 0);
    m.systemId = Val(mj, "sys", std::string());
    m.systemName = Val(mj, "where", std::string());
    m.deadline = Val(mj, "dl", 0.0);
    m.siteId = Val(mj, "site", 0);
    m.siteName = Val(mj, "siteName", std::string());
    if (mj.contains("vs"))
        m.against = ToFac(mj["vs"]);
    if (mj.contains("cv") && mj["cv"].is_object())
    {
        const json& cv = mj["cv"];
        m.convoySystemId = Val(cv, "sys", std::string());
        m.convoySystemName = Val(cv, "where", std::string());
        m.convoyNextName = Val(cv, "next", std::string());
        m.convoyMoving = Val(cv, "go", false);
        m.haulerId = Val(cv, "h", 0);
        if (cv.contains("esc") && cv["esc"].is_array())
            for (const json& id : cv["esc"])
                if (id.is_number_integer())
                    m.convoyEscortIds.push_back(id.get<int>());
        m.rode = Val(cv, "rode", false);
    }
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
    j["build"] = h.build;
    j["data"] = h.data;
    return j.dump();
}

bool DecodeHello(const std::string& s, Hello& out)
{
    json j;
    if (!OpenTyped(s, "hello", j) || VersionOf(j) != PROTO_VERSION)
        return false;
    out.account = TextOf(j, "acct");
    out.build = TextOf(j, "build");
    out.data.clear();
    // Strings only: anything else in a file's place reads as missing, and so as a mismatch.
    if (const auto d = j.find("data"); d != j.end() && d->is_object())
        for (auto it = d->begin(); it != d->end(); ++it)
            out.data[it.key()] = it->is_string() ? it->get<std::string>() : std::string();
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

std::string EncodeStarts(const Starts& s)
{
    json j;
    Stamp(j, "starts");
    json options = json::array();
    for (const StartOption& o : s.options)
        options.push_back({ { "sys", o.system },
                            { "name", o.name },
                            { "station", o.station },
                            { "owner", o.owner } });
    j["options"] = std::move(options);
    j["random"] = s.randomOnly;
    return j.dump();
}

bool DecodeStarts(const std::string& s, Starts& out)
{
    json j;
    if (!OpenEnvelope(s, j) || TypeOf(j) != "starts")
        return false;
    out = Starts{};
    const auto options = j.find("options");
    if (options != j.end() && options->is_array())
        for (const json& o : *options)
        {
            if (!o.is_object())
                continue;
            StartOption opt;
            opt.system = TextOf(o, "sys");
            opt.name = TextOf(o, "name");
            opt.station = TextOf(o, "station");
            opt.owner = TextOf(o, "owner");
            if (!opt.system.empty())
                out.options.push_back(opt);
        }
    const auto random = j.find("random");
    out.randomOnly = random != j.end() && random->is_boolean() && random->get<bool>();
    return true;
}

std::string EncodeChooseStart(const ChooseStart& c)
{
    json j;
    Stamp(j, "start");
    j["sys"] = c.system;
    return j.dump();
}

bool DecodeChooseStart(const std::string& s, ChooseStart& out)
{
    json j;
    if (!OpenEnvelope(s, j) || TypeOf(j) != "start")
        return false;
    out.system = TextOf(j, "sys");
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
    if (!c.sellFrom.empty())
        j["sellFrom"] = c.sellFrom;
    if (!c.buyTo.empty())
        j["buyTo"] = c.buyTo;
    number("rentHangar", c.rentHangar, -1);
    number("buyType", c.buyType, -1);
    number("buyAmt", c.buyAmount, 0);
    if (c.moveType >= 0)  // (#472)
    {
        j["mvFrom"] = c.moveFrom;
        j["mvTo"] = c.moveTo;
        j["mvType"] = c.moveType;
        j["mvAmt"] = c.moveAmount;
    }
    number("storeType", c.storeType, -1);
    number("storeAmt", c.storeAmount, 0);
    number("takeType", c.takeType, -1);
    number("takeAmt", c.takeAmount, 0);
    number("refit", c.refitShip, -1);
    auto fitJson = [](const std::vector<Ships::FitLine>& lines)
    {
        json fit = json::array();
        for (const Ships::FitLine& l : lines)
            fit.push_back(
                { { "module", l.module }, { "in", l.in }, { "on", l.on }, { "count", l.count } });
        return fit;
    };
    if (c.refit)  // a refit's whole fit (#279), only on the command that asks for one
        j["fit"] = fitJson(c.refitLines);
    // The design library (#279 step 3): each only on the command that asks for it.
    if (!c.saveDesign.empty())
    {
        j["saveD"] = c.saveDesign;
        j["saveFit"] = fitJson(c.saveLines);
    }
    if (!c.applyDesign.empty())
        j["applyD"] = c.applyDesign;
    if (!c.renameDesign.empty())
    {
        j["renameD"] = c.renameDesign;
        j["renameTo"] = c.renameTo;
    }
    if (!c.deleteDesign.empty())
        j["deleteD"] = c.deleteDesign;
    number("buy", c.buyShip, -1);
    number("insure", c.insureShip, -1);
    if (!c.payBountyFaction.empty())
        j["payB"] = c.payBountyFaction;
    if (!c.nameSystem.empty())
        j["nameSys"] = c.nameSystem;
    if (!c.nameSystemId.empty())
        j["nameSysId"] = c.nameSystemId;
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
        if (!c.orderWaypoints.empty())
            j["ordVia"] = c.orderWaypoints;
        number("ordPref", c.orderPreference, 0);
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
    if (c.defenceId != 0)
    {
        j["defId"] = c.defenceId;
        j["def"] = DefenceJson(c.defence);
    }
    flag("scan", c.scan);
    flag("buyLic", c.buyLicence);
    if (!c.sellChart.empty())
        j["sellChart"] = c.sellChart;
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

// A list of short texts, at most `most` of them: a route's waypoints (#432).
bool ReadTextList(const json& j, const char* key, size_t most, std::vector<std::string>& out)
{
    out.clear();
    const auto it = j.find(key);
    if (it == j.end())
        return true;
    if (!it->is_array() || it->size() > most)
        return false;
    for (const json& t : *it)
    {
        if (!t.is_string() || t.get_ref<const std::string&>().size() > TEXT_LIMIT)
            return false;
        out.push_back(t.get<std::string>());
    }
    return true;
}

// A refit's fit (#279): a list of lines, each four typed fields and nothing else. Bounded,
// because a hull has a few dozen sockets and anything longer is not a fit.
bool ReadFit(const json& j, const char* key, bool& present, std::vector<Ships::FitLine>& out)
{
    constexpr size_t FIT_LINES_MAX = 64;
    present = false;
    out.clear();
    const auto it = j.find(key);
    if (it == j.end())
        return true;
    if (!it->is_array() || it->size() > FIT_LINES_MAX)
        return false;
    present = true;
    for (const json& l : *it)
    {
        if (!l.is_object() || l.size() != 4)
            return false;
        Ships::FitLine line;
        if (!ReadText(l, "module", line.module) || !ReadText(l, "in", line.in) ||
            !ReadText(l, "on", line.on) || !ReadInt(l, "count", 0, 0, 64, line.count))
            return false;
        out.push_back(std::move(line));
    }
    return true;
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
    ok = ok && ReadInt(j, "buyType", -1, 0, resources - 1, out.buyType);
    ok = ok && ReadInt(j, "buyAmt", 0, 0, ID_MAX, out.buyAmount);
    ok = ok && ReadText(j, "sellFrom", out.sellFrom);
    ok = ok && ReadText(j, "buyTo", out.buyTo);
    ok = ok && ReadInt(j, "rentHangar", -1, 0, ID_MAX, out.rentHangar);
    ok = ok && ReadText(j, "mvFrom", out.moveFrom);
    ok = ok && ReadText(j, "mvTo", out.moveTo);
    ok = ok && ReadInt(j, "mvType", -1, 0, resources - 1, out.moveType);
    ok = ok && ReadInt(j, "mvAmt", 0, 0, ID_MAX, out.moveAmount);
    ok = ok && ReadInt(j, "storeType", -1, 0, resources - 1, out.storeType);
    ok = ok && ReadInt(j, "storeAmt", 0, 0, ID_MAX, out.storeAmount);
    ok = ok && ReadInt(j, "takeType", -1, 0, resources - 1, out.takeType);
    ok = ok && ReadInt(j, "takeAmt", 0, 0, ID_MAX, out.takeAmount);
    ok = ok && ReadInt(j, "refit", -1, 0, ID_MAX, out.refitShip);
    ok = ok && ReadFit(j, "fit", out.refit, out.refitLines);
    bool saveFit = false;
    ok = ok && ReadText(j, "saveD", out.saveDesign);
    ok = ok && ReadFit(j, "saveFit", saveFit, out.saveLines);
    ok = ok && ReadText(j, "applyD", out.applyDesign);
    ok = ok && ReadText(j, "renameD", out.renameDesign);
    ok = ok && ReadText(j, "renameTo", out.renameTo);
    ok = ok && ReadText(j, "deleteD", out.deleteDesign);
    ok = ok && ReadInt(j, "buy", -1, 0, ID_MAX, out.buyShip);
    ok = ok && ReadInt(j, "insure", -1, 0, ID_MAX, out.insureShip);
    ok = ok && ReadText(j, "payB", out.payBountyFaction);
    ok = ok && ReadText(j, "nameSys", out.nameSystem);
    ok = ok && ReadText(j, "nameSysId", out.nameSystemId);
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
    ok = ok && ReadTextList(j, "ordVia", Proto::ROUTE_WAYPOINTS_MAX, out.orderWaypoints);
    ok = ok && ReadInt(j, "ordPref", 0, 0, Route::PREFERENCE_MAX, out.orderPreference);
    ok = ok && ReadText(j, "ordBp", out.orderBlueprint);
    ok = ok && ReadText(j, "ordName", out.orderName);
    ok = ok && ReadBool(j, "ordAbort", out.abortOrder);
    ok = ok && ReadText(j, "deploy", out.deploy);
    ok = ok && ReadPoint(j, "deployAt", out.deployPos);
    ok = ok && ReadText(j, "deployName", out.deployName);
    ok = ok && ReadInt(j, "dismantle", 0, 0, ID_MAX, out.dismantleId);
    ok = ok && ReadInt(j, "defId", 0, 0, ID_MAX, out.defenceId);
    ok = ok && ReadBool(j, "scan", out.scan);
    ok = ok && ReadBool(j, "buyLic", out.buyLicence);
    ok = ok && ReadText(j, "sellChart", out.sellChart);
    if (ok && j.contains("def"))
        ok = DefenceFrom(j["def"], out.defence);
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

}  // namespace Proto

// --- Snapshots on the wire (#349) ---------------------------------------------------------

namespace Proto
{
// One entity as the wire holds it: its fields as integers and strings, without its id, and a
// field at its default absent. Two of these are equal exactly when the client would rebuild
// the same entity from them, which is what makes "unchanged" a comparison rather than a guess.
struct EntityWire
{
    json j;
};

// Everything in a snapshot that persists from one to the next: the player, the market, the
// missions and the entities. Fires, journal entries and trade acknowledgements happen once
// and are carried by the message they happen in, never by a state.
struct SnapshotState
{
    std::string                                      sys;
    json                                             player = json::object();
    json                                             market = json::array();
    json                                             belts = json::array();
    json                                             offers = json::array();
    json                                             active = json::array();
    std::map<int, std::shared_ptr<const EntityWire>> ents;
};
}  // namespace Proto

namespace
{
using Proto::EntityWire;
using Proto::SnapshotState;

// The wire's units. A hundredth of a world unit is far below a pixel at any zoom (#16); at a
// million units out it is a nine-digit integer, comfortably inside an int.
constexpr double POS_UNIT = 100.0;
constexpr double ANGLE_UNIT = 1000.0;  // milliradians
constexpr double HULL_UNIT = 1000.0;   // thousandths of the hull
constexpr double SIZE_UNIT = 100.0;
constexpr double STATUS_UNIT = 100.0;  // hundredths of a security status point
// A far entity's heading, in milliradians: about three degrees, which is what a dot on the
// sensor screen can show.
constexpr long long FAR_ANGLE_STEP = 50;

long long Quantise(double v, double unit)
{
    if (!std::isfinite(v))
        return 0;
    return std::llround(v * unit);
}

// `grid` 0 is the near quality; otherwise the position is snapped to multiples of it.
EntityWire WireOf(const Proto::EntitySnapshot& e, float grid)
{
    EntityWire w;
    json&      j = w.j;
    j = json::object();
    j["k"] = (int)e.kind;
    Vector2 p = e.pos;
    if (grid > 0.0f)
        p = { std::round(p.x / grid) * grid, std::round(p.y / grid) * grid };
    j["p"] = json::array({ Quantise(p.x, POS_UNIT), Quantise(p.y, POS_UNIT) });
    long long h = Quantise(e.heading, ANGLE_UNIT);
    if (grid > 0.0f)
        h = (h / FAR_ANGLE_STEP) * FAR_ANGLE_STEP;
    if (h != 0)
        j["h"] = h;
    if (e.size != 0.0f)
        j["s"] = Quantise(e.size, SIZE_UNIT);
    if (e.faction != Factions::Unaffiliated())
        j["f"] = FacJ(e.faction);
    if (e.role != -1)
        j["r"] = e.role;
    const long long hp = Quantise(e.hullFrac, HULL_UNIT);
    if (hp != (long long)HULL_UNIT)
        j["hp"] = hp;
    if (e.ore != -1)
        j["o"] = e.ore;
    if (e.cargo >= 0 && e.cargoAmount > 0)
        j["cg"] = json::array({ e.cargo, e.cargoAmount });
    if (!e.name.empty())
        j["n"] = e.name;
    if (!e.design.empty())
        j["d"] = e.design;
    if (e.criminal)
        j["cr"] = 1;
    const long long status = Quantise(e.securityStatus, STATUS_UNIT);
    if (status != 0)
        j["ss"] = status;
    return w;
}

// An integer field of a wire object, or `def` when it is absent or not an integer.
long long IntOf(const json& j, const char* key, long long def)
{
    const auto it = j.find(key);
    return it != j.end() && it->is_number_integer() ? it->get<long long>() : def;
}

Proto::EntitySnapshot EntityOf(int id, const json& j)
{
    Proto::EntitySnapshot e;
    e.id = id;
    e.kind = (EntityKind)IntOf(j, "k", 0);
    const auto p = j.find("p");
    if (p != j.end() && p->is_array() && p->size() == 2 && (*p)[0].is_number_integer() &&
        (*p)[1].is_number_integer())
        e.pos = { (float)((double)(*p)[0].get<long long>() / POS_UNIT),
                  (float)((double)(*p)[1].get<long long>() / POS_UNIT) };
    e.heading = (float)((double)IntOf(j, "h", 0) / ANGLE_UNIT);
    e.size = (float)((double)IntOf(j, "s", 0) / SIZE_UNIT);
    e.faction = ToFac(Val(j, "f", json()));
    e.role = (int)IntOf(j, "r", -1);
    e.hullFrac = (float)((double)IntOf(j, "hp", (long long)HULL_UNIT) / HULL_UNIT);
    e.ore = (int)IntOf(j, "o", -1);
    const auto cg = j.find("cg");
    if (cg != j.end() && cg->is_array() && cg->size() == 2 && (*cg)[0].is_number_integer() &&
        (*cg)[1].is_number_integer())
    {
        e.cargo = (*cg)[0].get<int>();
        e.cargoAmount = (*cg)[1].get<int>();
    }
    e.name = TextOf(j, "n");
    e.design = TextOf(j, "d");
    e.criminal = IntOf(j, "cr", 0) != 0;
    e.securityStatus = (float)((double)IntOf(j, "ss", 0) / STATUS_UNIT);
    return e;
}

json PlayerJson(const Proto::PlayerView& p)
{
    json j = { { "pos", V2(p.pos) },
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
        j["warpVia"] = V2(p.warpVia);
    // Under the law (#431 slice 10): only once there is something to say.
    if (p.securityStatus != 0.0f)
        j["secSt"] = Round2(p.securityStatus);
    if (p.criminalLeft > 0.0)
        j["crimLeft"] = std::ceil(p.criminalLeft);  // whole seconds
    // Only when there is one; as a field of the player it goes again only when it changes.
    if (!p.library.empty())
    {
        json lib = json::array();
        for (const Proto::SavedDesign& d : p.library)
            lib.push_back(json::array({ d.name, d.design }));
        j["lib"] = lib;
    }
    // Only once a ship has been insured or lost (#344); then only when it changes.
    for (double cover : p.insured)
        if (cover > 0.0)
        {
            j["insured"] = p.insured;
            break;
        }
    if (p.loss.seq > 0)
        j["loss"] = { { "seq", p.loss.seq },     { "hull", p.loss.hull }, { "sys", p.loss.system },
                      { "value", p.loss.value }, { "paid", p.loss.paid }, { "cargo", p.loss.cargo },
                      { "woke", p.loss.wokeAt } };
    // Only where there is something to scan or a pass under way (#346).
    if (p.scan.until > 0.0 || !p.scan.sigs.empty())
    {
        json sigs = json::array();
        for (const Proto::SignatureView& g : p.scan.sigs)
        {
            json gj = { { "code", g.code }, { "p", Round2(g.progress) } };
            if (!g.kind.empty())
                gj["kind"] = g.kind;
            if (g.id != 0)
                gj["id"] = g.id;
            if (!g.dest.empty())
                gj["dest"] = g.dest;
            sigs.push_back(std::move(gj));
        }
        j["scan"] = { { "until", p.scan.until },
                      { "secs", p.scan.seconds },
                      { "pow", Round2(p.scan.power) },
                      { "sigs", sigs } };
    }
    // Only where building takes a licence (#431 slice 6).
    if (!p.licence.holder.empty())
        j["lic"] = LicenceJson(p.licence);
    // Only docked at a storage of the pilot's side (#438).
    if (p.storageCap > 0)
        j["store"] = { { "cap", p.storageCap }, { "t", p.storedByType } };
    if (p.hangar.capacity > 0)  // (#472)
        j["hgr"] = { { "cap", p.hangar.capacity },     { "free", p.hangar.free },
                     { "rent", p.hangar.rented },      { "most", p.hangar.most },
                     { "rate", p.hangar.rentPerUnit }, { "days", p.hangar.periodDays },
                     { "until", p.hangar.paidUntil },  { "owed", p.hangar.owed },
                     { "t", p.hangar.goods } };
    return j;
}

void ReadPlayer(const json& pj, Proto::PlayerView& p)
{
    p.pos = ToV2(Val(pj, "pos", json::array()));
    p.vel = ToV2(Val(pj, "vel", json::array()));
    p.heading = Val(pj, "hdg", 0.0f);
    p.hull = Val(pj, "hull", 0.0f);
    p.maxHull = Val(pj, "maxHull", 0.0f);
    p.shields = Val(pj, "sh", 0.0f);
    p.maxShields = Val(pj, "maxSh", 0.0f);
    p.cargoUsed = Val(pj, "cargo", 0);
    p.cargoCap = Val(pj, "cargoCap", 0);
    p.warpPhase = Val(pj, "warp", 0);
    p.warpAlign = Val(pj, "warpAlign", 0.0f);
    p.warpTarget = pj.contains("warpTgt") ? ToV2(pj["warpTgt"]) : Vector2{ 0.0f, 0.0f };
    p.warpDrop = Val(pj, "warpDrop", 0.0f);
    p.warpViaSet = pj.contains("warpVia");
    p.warpVia = p.warpViaSet ? ToV2(pj["warpVia"]) : Vector2{ 0.0f, 0.0f };
    p.autopilot = Val(pj, "ap", false);
    p.apTarget = pj.contains("apTgt") ? ToV2(pj["apTgt"]) : Vector2{ 0.0f, 0.0f };
    p.apStop = Val(pj, "apStop", 0.0f);
    p.holdMode = Val(pj, "hold", 0);
    p.holdTargetId = Val(pj, "holdId", 0);
    p.holdRange = Val(pj, "holdR", 0.0f);
    p.stabilizer = Val(pj, "stab", true);
    p.mining = Val(pj, "mine", false);
    p.weaponOn = Val(pj, "weap", false);
    p.docked = Val(pj, "docked", false);
    p.dockedStationId = Val(pj, "dockId", 0);
    p.nearbyStationId = Val(pj, "nearStation", 0);
    p.lastInput = Val(pj, "lastInput", 0);
    p.cargoByType = Val(pj, "cargoT", std::vector<int>{});
    p.money = Val(pj, "money", 0.0);
    p.reputation = ToPerFaction<float>(Val(pj, "rep", json()));
    p.bounty = ToPerFaction<double>(Val(pj, "bounty", json()));
    p.skillXp = Val(pj, "skillXp", std::vector<float>{});
    p.securityStatus = Val(pj, "secSt", 0.0f);
    p.criminalLeft = Val(pj, "crimLeft", 0.0);
    p.orderKind = Val(pj, "ordKind", 0);
    p.orderStatus = Val(pj, "ordStatus", 0);
    p.orderId = Val(pj, "ordId", 0);
    p.orderDetail = Val(pj, "ordDetail", std::string());
    p.ownedShips = Val(pj, "ships", std::vector<int>{});
    p.shipDesigns = Val(pj, "shipDesigns", std::vector<std::string>{});
    p.shipIndex = Val(pj, "ship", 0);
    p.insured = Val(pj, "insured", std::vector<double>{});
    p.loss = Proto::LossView{};
    const auto loss = pj.find("loss");
    if (loss != pj.end() && loss->is_object())
    {
        p.loss.seq = loss->value("seq", 0);
        p.loss.hull = loss->value("hull", std::string());
        p.loss.system = loss->value("sys", std::string());
        p.loss.value = loss->value("value", 0.0);
        p.loss.paid = loss->value("paid", 0.0);
        p.loss.cargo = loss->value("cargo", 0);
        p.loss.wokeAt = loss->value("woke", std::string());
    }
    p.scan = Proto::ScanView{};
    const auto scan = pj.find("scan");
    if (scan != pj.end() && scan->is_object())
    {
        p.scan.until = scan->value("until", 0.0);
        p.scan.seconds = scan->value("secs", 0.0);
        p.scan.power = scan->value("pow", 1.0);
        const auto sigs = scan->find("sigs");
        if (sigs != scan->end() && sigs->is_array())
            for (const json& gj : *sigs)
            {
                if (!gj.is_object())
                    continue;
                Proto::SignatureView g;
                g.code = Val(gj, "code", std::string());
                g.progress = Val(gj, "p", 0.0f);
                g.kind = Val(gj, "kind", std::string());
                g.id = Val(gj, "id", 0);
                g.dest = Val(gj, "dest", std::string());
                p.scan.sigs.push_back(std::move(g));
            }
    }
    p.storageCap = 0;
    p.storedByType.clear();
    const auto store = pj.find("store");
    if (store != pj.end() && store->is_object())
    {
        p.storageCap = store->value("cap", 0);
        p.storedByType = store->value("t", std::vector<int>{});
    }
    p.hangar = Proto::HangarView{};
    const auto hgr = pj.find("hgr");
    if (hgr != pj.end() && hgr->is_object())
    {
        p.hangar.capacity = hgr->value("cap", 0);
        p.hangar.free = hgr->value("free", 0);
        p.hangar.rented = hgr->value("rent", 0);
        p.hangar.most = hgr->value("most", 0);
        p.hangar.rentPerUnit = hgr->value("rate", 0.0);
        p.hangar.periodDays = hgr->value("days", 0.0);
        p.hangar.paidUntil = hgr->value("until", 0.0);
        p.hangar.owed = hgr->value("owed", 0.0);
        p.hangar.goods = hgr->value("t", std::vector<int>{});
    }
    p.licence = Proto::LicenceView{};
    const auto lic = pj.find("lic");
    if (lic != pj.end() && lic->is_object())
        p.licence = LicenceFrom(*lic);
    p.library.clear();
    const auto lib = pj.find("lib");
    if (lib != pj.end() && lib->is_array())
        for (const json& e : *lib)
            if (e.is_array() && e.size() == 2 && e[0].is_string() && e[1].is_string())
                p.library.push_back({ e[0].get<std::string>(), e[1].get<std::string>() });
}

json MissionsJson(const std::vector<Proto::MissionView>& v)
{
    json a = json::array();
    for (const Proto::MissionView& m : v)
        a.push_back(MissionJson(m));
    return a;
}

// The market as one list of [bid, ask, stock] per good (#343): it changes as a whole when
// anybody trades, so a delta sends it whole or not at all.
json MarketJson(const Proto::Snapshot& s)
{
    json m = json::array();
    for (size_t i = 0; i < s.marketPrices.size(); i++)
        m.push_back({ s.marketPrices[i], i < s.marketAsk.size() ? s.marketAsk[i] : 0.0f,
                      i < s.marketStock.size() ? s.marketStock[i] : 0 });
    return m;
}

// What is left of the belts (#348), as [id, ore, full] each: sent whole when any of it moves.
json BeltsJson(const Proto::Snapshot& s)
{
    json b = json::array();
    for (const Proto::Snapshot::BeltOre& o : s.belts)
        b.push_back({ o.id, o.ore, o.max });
    return b;
}

void ReadBelts(const json& b, Proto::Snapshot& out)
{
    for (const json& line : b)
        if (line.is_array() && line.size() == 3 && line[0].is_number_integer() &&
            line[1].is_number_integer() && line[2].is_number_integer())
            out.belts.push_back({ line[0].get<int>(), line[1].get<int>(), line[2].get<int>() });
}

void ReadMarket(const json& m, Proto::Snapshot& out)
{
    for (const json& line : m)
    {
        if (!line.is_array() || line.size() != 3 || !line[0].is_number() || !line[1].is_number() ||
            !line[2].is_number())
            continue;
        out.marketPrices.push_back(line[0].get<float>());
        out.marketAsk.push_back(line[1].get<float>());
        out.marketStock.push_back(line[2].get<int>());
    }
}

// The persistent half of `s`, every entity at the near quality: a whole snapshot's state.
std::shared_ptr<SnapshotState> StateOf(const Proto::Snapshot& s)
{
    auto st = std::make_shared<SnapshotState>();
    st->sys = s.systemId;
    st->player = PlayerJson(s.player);
    st->market = MarketJson(s);
    st->belts = BeltsJson(s);
    st->offers = MissionsJson(s.missionOffers);
    st->active = MissionsJson(s.missionActive);
    for (const Proto::EntitySnapshot& e : s.entities)
        st->ents[e.id] = std::make_shared<EntityWire>(WireOf(e, 0.0f));
    return st;
}

// What `cur` says that `base` does not, written into the message `out`. No base: everything.
// An object's fields are sent when they differ and as null when they went back to their
// default; an entity's position, when it had one before, as the integer step from it.
void WriteDelta(const SnapshotState* base, const SnapshotState& cur, json& out)
{
    static const SnapshotState none;
    const SnapshotState&       b = base != nullptr ? *base : none;

    json pl = json::object();
    for (auto it = cur.player.begin(); it != cur.player.end(); ++it)
    {
        const auto was = b.player.find(it.key());
        if (was == b.player.end() || *was != it.value())
            pl[it.key()] = it.value();
    }
    for (auto it = b.player.begin(); it != b.player.end(); ++it)
        if (!cur.player.contains(it.key()))
            pl[it.key()] = nullptr;
    if (!pl.empty())
        out["pl"] = std::move(pl);
    if (base == nullptr || cur.market != b.market)
        out["mk"] = cur.market;
    if (base == nullptr || cur.belts != b.belts)
        out["bt"] = cur.belts;
    if (base == nullptr || cur.offers != b.offers)
        out["mo"] = cur.offers;
    if (base == nullptr || cur.active != b.active)
        out["ma"] = cur.active;

    json ents = json::array();
    for (const auto& kv : cur.ents)
    {
        const auto was = b.ents.find(kv.first);
        if (was == b.ents.end())
        {
            json e = kv.second->j;
            e["i"] = kv.first;
            ents.push_back(std::move(e));
            continue;
        }
        if (was->second == kv.second)
            continue;  // the very state that was sent: a far entity between refreshes
        const json& o = was->second->j;
        const json& n = kv.second->j;
        if (o == n)
            continue;
        json e = json::object();
        e["i"] = kv.first;
        for (auto it = n.begin(); it != n.end(); ++it)
        {
            const auto old = o.find(it.key());
            if (old != o.end() && *old == it.value())
                continue;
            if (it.key() == "p" && old != o.end())
                e["dp"] =
                    json::array({ it.value()[0].get<long long>() - (*old)[0].get<long long>(),
                                  it.value()[1].get<long long>() - (*old)[1].get<long long>() });
            else
                e[it.key()] = it.value();
        }
        for (auto it = o.begin(); it != o.end(); ++it)
            if (!n.contains(it.key()))
                e[it.key()] = nullptr;
        ents.push_back(std::move(e));
    }
    if (!ents.empty())
        out["ents"] = std::move(ents);

    json rm = json::array();
    for (const auto& kv : b.ents)
        if (cur.ents.count(kv.first) == 0)
            rm.push_back(kv.first);
    if (!rm.empty())
        out["rm"] = std::move(rm);
}

// Merges `delta`'s fields into `obj`: a null removes one, anything else replaces it.
void MergeFields(json& obj, const json& delta)
{
    for (auto it = delta.begin(); it != delta.end(); ++it)
        if (it.value().is_null())
            obj.erase(it.key());
        else
            obj[it.key()] = it.value();
}

bool IsIntPair(const json& j)
{
    return j.is_array() && j.size() == 2 && j[0].is_number_integer() && j[1].is_number_integer();
}

// `base` (none: an empty state) with message `m` applied, into `out`. False for a message
// that does not fit its base -- a step for an entity that has no position to step from.
bool ApplyDelta(const SnapshotState* base, const json& m, SnapshotState& out)
{
    if (base != nullptr)
        out = *base;
    out.sys = TextOf(m, "sys");
    if (m.contains("pl"))
    {
        if (!m["pl"].is_object())
            return false;
        MergeFields(out.player, m["pl"]);
    }
    const std::pair<const char*, json*> lists[] = {
        { "mk", &out.market }, { "bt", &out.belts }, { "mo", &out.offers }, { "ma", &out.active }
    };
    for (const auto& l : lists)
        if (m.contains(l.first))
        {
            if (!m[l.first].is_array())
                return false;
            *l.second = m[l.first];
        }
    if (m.contains("rm"))
    {
        if (!m["rm"].is_array())
            return false;
        for (const json& id : m["rm"])
            if (id.is_number_integer())
                out.ents.erase(id.get<int>());
    }
    if (m.contains("ents"))
    {
        if (!m["ents"].is_array())
            return false;
        for (const json& e : m["ents"])
        {
            if (!e.is_object() || !e.contains("i") || !e["i"].is_number_integer())
                return false;
            const int  id = e["i"].get<int>();
            auto       w = std::make_shared<EntityWire>();
            const auto was = out.ents.find(id);
            if (was != out.ents.end())
                w->j = was->second->j;
            else
                w->j = json::object();
            json fields = e;
            fields.erase("i");
            if (fields.contains("dp"))
            {
                const json dp = fields["dp"];
                fields.erase("dp");
                if (!IsIntPair(dp) || !w->j.contains("p") || !IsIntPair(w->j["p"]))
                    return false;
                w->j["p"] = json::array({ w->j["p"][0].get<long long>() + dp[0].get<long long>(),
                                          w->j["p"][1].get<long long>() + dp[1].get<long long>() });
            }
            MergeFields(w->j, fields);
            out.ents[id] = std::move(w);
        }
    }
    return true;
}

// What happens once: the shots, the journal and the trade acknowledgements of this message.
void WriteOnce(const Proto::Snapshot& s, json& j)
{
    if (!s.fires.empty())
    {
        json fires = json::array();
        for (const FireEvent& f : s.fires)
            fires.push_back({ { "from", V2(f.from) },
                              { "to", V2(f.to) },
                              { "fac", FacJ(f.shooterFaction) },
                              { "pl", f.targetIsPlayer },
                              { "mine", f.fromPlayer } });
        j["fires"] = std::move(fires);
    }
    if (!s.events.empty())
    {
        json evs = json::array();
        for (const Ev::Event& e : s.events)
            evs.push_back({ { "seq", e.seq }, { "kind", (int)e.kind }, { "text", e.text } });
        j["events"] = std::move(evs);
    }
    if (!s.tradeAcks.empty())
    {
        json acks = json::array();
        for (const Proto::TradeAck& a : s.tradeAcks)
            acks.push_back({ { "type", a.type },
                             { "sold", a.sold },
                             { "gross", a.gross },
                             { "rev", a.revenue },
                             { "bought", a.bought },
                             { "cost", a.cost },
                             { "fee", a.fee } });
        j["trade"] = std::move(acks);
    }
}

// The snapshot a state and its message describe.
void ReadSnapshot(const SnapshotState& st, const json& j, Proto::Snapshot& out)
{
    out = Proto::Snapshot{};
    out.systemId = st.sys;
    out.time = Val(j, "time", 0.0);
    ReadPlayer(st.player, out.player);
    for (const auto& kv : st.ents)
        out.entities.push_back(EntityOf(kv.first, kv.second->j));
    ReadMarket(st.market, out);
    ReadBelts(st.belts, out);
    for (const json& mj : st.offers)
        out.missionOffers.push_back(ToMission(mj));
    for (const json& mj : st.active)
        out.missionActive.push_back(ToMission(mj));

    if (j.contains("fires") && j["fires"].is_array())
        for (const json& fj : j["fires"])
        {
            FireEvent f;
            f.from = ToV2(Val(fj, "from", json::array()));
            f.to = ToV2(Val(fj, "to", json::array()));
            f.shooterFaction = ToFac(Val(fj, "fac", json()));
            f.targetIsPlayer = Val(fj, "pl", false);
            f.fromPlayer = Val(fj, "mine", false);
            out.fires.push_back(f);
        }
    if (j.contains("events") && j["events"].is_array())
        for (const json& ej : j["events"])
        {
            Ev::Event e;
            e.seq = Val(ej, "seq", 0);
            e.kind = (Ev::Kind)Val(ej, "kind", 0);
            e.text = Val(ej, "text", std::string());
            out.events.push_back(std::move(e));
        }
    if (j.contains("trade") && j["trade"].is_array())
        for (const json& aj : j["trade"])
        {
            Proto::TradeAck a;
            a.type = Val(aj, "type", 0);
            a.sold = Val(aj, "sold", 0);
            a.gross = Val(aj, "gross", 0.0);
            a.revenue = Val(aj, "rev", 0.0);
            a.bought = Val(aj, "bought", 0);
            a.cost = Val(aj, "cost", 0.0);
            a.fee = Val(aj, "fee", 0.0);
            out.tradeAcks.push_back(a);
        }
}

// Where a new far entity's refresh falls in the interval, so that a system's worth of them
// arriving at once are not all refreshed in the same snapshot every second after.
double Stagger(int id)
{
    const double golden = 0.6180339887498949;
    const double x = (double)(unsigned)id * golden;
    return x - std::floor(x);
}
}  // namespace

namespace Proto
{

std::string EncodeSnapshot(const Snapshot& s)
{
    json j;
    Stamp(j, "snap");
    j["sys"] = s.systemId;
    j["time"] = s.time;
    WriteDelta(nullptr, *StateOf(s), j);
    WriteOnce(s, j);
    return j.dump();
}

bool DecodeSnapshot(const std::string& s, Snapshot& out)
{
    json j;
    if (!OpenEnvelope(s, j) || j.contains("b"))
        return false;
    try
    {
        SnapshotState st;
        if (!ApplyDelta(nullptr, j, st))
            return false;
        Snapshot snap;
        ReadSnapshot(st, j, snap);
        out = std::move(snap);
        return true;
    }
    catch (const json::exception&)
    {
        return false;  // a field of the wrong type: the message is broken, not the client
    }
}

std::string EncodeAck(const Ack& a)
{
    json j;
    Stamp(j, "ack");
    j["n"] = a.snapshot;
    return j.dump();
}

bool DecodeAck(const std::string& s, Ack& out)
{
    json j;
    if (!OpenEnvelope(s, j) || TypeOf(j) != "ack")
        return false;
    const long long n = IntOf(j, "n", -1);
    if (n < 0 || n > 2147483647LL)
        return false;
    out.snapshot = (int)n;
    return true;
}

// Whether two entities have the same wire form: every field WireOf reads.
static bool SameWire(const EntitySnapshot& a, const EntitySnapshot& b)
{
    return a.kind == b.kind && a.pos.x == b.pos.x && a.pos.y == b.pos.y && a.heading == b.heading &&
           a.size == b.size && a.faction == b.faction && a.role == b.role &&
           a.hullFrac == b.hullFrac && a.ore == b.ore && a.cargo == b.cargo &&
           a.cargoAmount == b.cargoAmount && a.name == b.name && a.design == b.design &&
           a.criminal == b.criminal && a.securityStatus == b.securityStatus;
}

std::shared_ptr<const EntityWire> WireCache::Of(const EntitySnapshot& e, float grid)
{
    Held& h = held_[{ e.id, grid }];
    if (!h.wire || !SameWire(h.from, e))
    {
        h.from = e;
        h.wire = std::make_shared<EntityWire>(WireOf(e, grid));
    }
    h.used = true;
    return h.wire;
}

bool WireCache::Sweep()
{
    for (auto it = held_.begin(); it != held_.end();)
        if (!it->second.used)
            it = held_.erase(it);
        else
        {
            it->second.used = false;
            ++it;
        }
    return !held_.empty();
}

std::string SnapshotSender::Encode(const Snapshot& s, const std::set<int>& relevant,
                                   WireCache* wires)
{
    const auto wireOf = [wires](const EntitySnapshot& e, float grid)
    {
        return wires != nullptr ? wires->Of(e, grid)
                                : std::shared_ptr<const EntityWire>(
                                      std::make_shared<EntityWire>(WireOf(e, grid)));
    };
    auto cur = std::make_shared<SnapshotState>();
    cur->sys = s.systemId;
    cur->player = PlayerJson(s.player);
    cur->market = MarketJson(s);
    cur->belts = BeltsJson(s);
    cur->offers = MissionsJson(s.missionOffers);
    cur->active = MissionsJson(s.missionActive);

    // The area of interest: what is relevant, then the nearest within the radius, up to the
    // cap. Everything else is on the far list.
    const Vector2                      me = s.player.pos;
    std::set<int>                      nearIds;
    std::vector<std::pair<float, int>> inRange;
    const float                        r2 = interest_.nearRadius * interest_.nearRadius;
    for (const EntitySnapshot& e : s.entities)
    {
        const float dx = e.pos.x - me.x, dy = e.pos.y - me.y;
        const float d2 = dx * dx + dy * dy;
        if (relevant.count(e.id) != 0)
            nearIds.insert(e.id);
        else if (d2 <= r2)
            inRange.push_back({ d2, e.id });
    }
    std::sort(inRange.begin(), inRange.end());
    for (const auto& c : inRange)
        if ((int)nearIds.size() < interest_.nearCap)
            nearIds.insert(c.second);

    std::map<int, Far> far;
    for (const EntitySnapshot& e : s.entities)
    {
        if (nearIds.count(e.id) != 0)
        {
            cur->ents[e.id] = wireOf(e, 0.0f);
            continue;
        }
        const auto held = far_.find(e.id);
        if (held != far_.end() && s.time >= held->second.at &&
            s.time - held->second.at < interest_.farRefresh)
        {
            cur->ents[e.id] = held->second.wire;
            far[e.id] = held->second;
            continue;
        }
        Far f;
        f.wire = wireOf(e, interest_.farGrid);
        f.at = s.time;
        if (held == far_.end())
            f.at -= Stagger(e.id) * interest_.farRefresh;
        cur->ents[e.id] = f.wire;
        far[e.id] = f;
    }
    far_ = std::move(far);

    std::shared_ptr<const SnapshotState> base;
    const auto                           b = sent_.find(acked_);
    if (b != sent_.end() && b->second->sys == cur->sys)
        base = b->second;  // a state from another system is no base: everything changed
    lastBase_ = base ? acked_ : 0;

    const int n = ++next_;
    json      j;
    Stamp(j, "snap");
    j["n"] = n;
    if (base)
        j["b"] = acked_;
    j["sys"] = s.systemId;
    j["time"] = s.time;
    WriteDelta(base.get(), *cur, j);
    WriteOnce(s, j);

    sent_[n] = std::move(cur);
    while (sent_.size() > HELD)
        sent_.erase(sent_.begin());
    return j.dump();
}

bool SnapshotSender::Due(double now)
{
    if (Unacknowledged() >= STALL_AFTER && now - lastDue_ < STALLED_INTERVAL)
        return false;
    lastDue_ = now;
    return true;
}

void SnapshotSender::Acknowledge(int n)
{
    if (n > acked_ && n <= next_)
        acked_ = n;
}

bool SnapshotReceiver::Decode(const std::string& msg, Snapshot& out)
{
    json j;
    if (!OpenEnvelope(msg, j) || TypeOf(j) != "snap")
        return false;
    try
    {
        std::shared_ptr<const SnapshotState> base;
        if (j.contains("b"))
        {
            const auto it = held_.find((int)IntOf(j, "b", 0));
            if (it == held_.end())
                return false;  // not held: wait for the whole one the server will send
            base = it->second;
        }
        auto st = std::make_shared<SnapshotState>();
        if (!ApplyDelta(base.get(), j, *st))
            return false;
        Snapshot snap;
        ReadSnapshot(*st, j, snap);
        const long long n = IntOf(j, "n", 0);
        if (n > 0 && n <= 2147483647LL)
        {
            held_[(int)n] = std::move(st);
            if ((int)n > latest_)
                latest_ = (int)n;
            // Twice what the server holds: it never names a base older than its own window.
            while (held_.size() > 2 * SnapshotSender::HELD)
                held_.erase(held_.begin());
        }
        out = std::move(snap);
        return true;
    }
    catch (const json::exception&)
    {
        return false;
    }
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

    out.systemId = Val(j, "sys", std::string());
    out.rev = Val(j, "rev", 0);
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
    if (!OpenEnvelope(s, j) || Val(j, "t", std::string()) != "ldelta")
        return false;

    out = LayoutDelta{};
    out.systemId = Val(j, "sys", std::string());
    out.rev = Val(j, "rev", 0);
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
    if (!OpenEnvelope(s, j) || Val(j, "t", std::string()) != "design" || !j.contains("design"))
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

bool ForViewer(std::vector<EntityLayout>& v, const ReadsOwner& reads)
{
    bool cut = false;
    for (EntityLayout& e : v)
    {
        if (!e.owner.empty() && reads && reads(e.owner))
            continue;
        if (e.defence.players.empty() && e.defence.factions.empty())
            continue;
        e.defence.players.clear();
        e.defence.factions.clear();
        cut = true;
    }
    return cut;
}

bool ForViewer(SystemLayout& lay, const ReadsOwner& reads)
{
    return ForViewer(lay.entities, reads);
}

bool ForViewer(LayoutDelta& d, const ReadsOwner& reads)
{
    const bool added = ForViewer(d.added, reads);
    const bool changed = ForViewer(d.changed, reads);
    return added || changed;
}

namespace
{
ReadsOwner Only(const std::string& viewer)
{
    return [viewer](const std::string& owner) { return !viewer.empty() && owner == viewer; };
}
}  // namespace

bool ForViewer(std::vector<EntityLayout>& v, const std::string& viewer)
{
    return ForViewer(v, Only(viewer));
}

bool ForViewer(SystemLayout& lay, const std::string& viewer)
{
    return ForViewer(lay.entities, Only(viewer));
}

bool ForViewer(LayoutDelta& d, const std::string& viewer)
{
    return ForViewer(d, Only(viewer));
}

bool OwnsPrivate(const LayoutDelta& d, const ReadsOwner& reads)
{
    if (!reads)
        return false;
    for (const std::vector<EntityLayout>* list : { &d.added, &d.changed })
        for (const EntityLayout& e : *list)
            if (!e.owner.empty() && (!e.defence.players.empty() || !e.defence.factions.empty()) &&
                reads(e.owner))
                return true;
    return false;
}

bool OwnsPrivate(const LayoutDelta& d, const std::string& account)
{
    return OwnsPrivate(d, Only(account));
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

FactionList RegistryMessage()
{
    FactionList out;
    for (FactionId f : Factions::All())
    {
        FactionDef d = Factions::Def(f);
        d.temperament = Temperament{};  // how it acts is the server's business
        out.factions.push_back(std::move(d));
    }
    for (FactionId a : Factions::All())
        for (FactionId b : Factions::All())
            if (a < b && Factions::Relation(a, b) != Stance::Neutral)
                out.relations.push_back(
                    { Factions::Id(a), Factions::Id(b), Factions::Relation(a, b) });
    return out;
}

void ApplyFactions(const FactionList& list)
{
    for (const FactionDef& d : list.factions)
        Factions::Add(d);
    for (const FactionRelation& r : list.relations)
    {
        FactionId a, b;
        if (Factions::Find(r.a, a) && Factions::Find(r.b, b))
            Factions::SetRelation(a, b, r.stance);
    }
}

std::string EncodeFactions(const FactionList& f)
{
    json j;
    Stamp(j, "factions");
    json list = json::array();
    for (const FactionDef& d : f.factions)
        list.push_back({ { "id", d.id },
                         { "name", d.name },
                         { "color", json::array({ d.color.r, d.color.g, d.color.b }) },
                         { "lawful", d.lawful },
                         { "kind", d.kind } });
    j["factions"] = std::move(list);
    json rel = json::array();
    for (const FactionRelation& r : f.relations)
        rel.push_back(json::array({ r.a, r.b, Factions::StanceName(r.stance) }));
    j["relations"] = std::move(rel);
    return j.dump();
}

bool DecodeFactions(const std::string& s, FactionList& out)
{
    json j;
    if (!OpenEnvelope(s, j) || TypeOf(j) != "factions")
        return false;
    out = FactionList{};
    const auto list = j.find("factions");
    if (list != j.end() && list->is_array())
        for (const json& o : *list)
        {
            if (!o.is_object())
                continue;
            FactionDef d;
            d.id = TextOf(o, "id");
            if (d.id.empty())
                continue;
            d.name = TextOf(o, "name");
            if (d.name.empty())
                d.name = d.id;
            d.kind = TextOf(o, "kind");
            const auto lawful = o.find("lawful");
            d.lawful = lawful == o.end() || !lawful->is_boolean() || lawful->get<bool>();
            const auto c = o.find("color");
            if (c != o.end() && c->is_array() && c->size() >= 3 && (*c)[0].is_number_integer() &&
                (*c)[1].is_number_integer() && (*c)[2].is_number_integer())
                d.color = Color{ (unsigned char)std::clamp((*c)[0].get<int>(), 0, 255),
                                 (unsigned char)std::clamp((*c)[1].get<int>(), 0, 255),
                                 (unsigned char)std::clamp((*c)[2].get<int>(), 0, 255), 255 };
            out.factions.push_back(std::move(d));
        }
    const auto rel = j.find("relations");
    if (rel != j.end() && rel->is_array())
        for (const json& r : *rel)
        {
            FactionRelation e;
            if (!r.is_array() || r.size() != 3 || !r[0].is_string() || !r[1].is_string() ||
                !r[2].is_string() || !Factions::ParseStance(r[2].get<std::string>(), e.stance))
                continue;
            e.a = r[0].get<std::string>();
            e.b = r[1].get<std::string>();
            out.relations.push_back(std::move(e));
        }
    return true;
}

std::string EncodeGalaxy(const GalaxyState& s)
{
    json j;
    Stamp(j, "galaxy");
    json sys = json::array();
    for (const GalaxySystemStat& g : s.systems)
    {
        json gj = { { "id", g.id },
                    { "sec", g.security },
                    { "pir", g.pirates },
                    { "prosp", g.prosperity },
                    { "ctrl", FacJ(g.controller) },
                    { "claim", g.claimed } };
        if (!g.holder.empty())
            gj["hold"] = { g.holder, g.holderTicker, g.holderName };
        if (g.fee >= 0.0f)
            gj["fee"] = g.fee;
        if (g.front)
            gj["front"] = true;
        if (g.chartPrice > 0)
            gj["chart"] = { g.chartPrice, g.chartWhat };
        if (!g.licenceBy.empty())
            gj["lic"] = { g.licenceBy, g.licenceRent };
        sys.push_back(std::move(gj));
    }
    j["sys"] = sys;
    j["news"] = s.events;
    if (s.chartBuyer)
        j["buyer"] = { { "fac", FacJ(s.chartFaction) }, { "treasury", s.chartTreasury } };
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
            g.id = Val(gj, "id", std::string());
            g.security = Val(gj, "sec", 0.5f);
            g.pirates = Val(gj, "pir", 0);
            g.prosperity = Val(gj, "prosp", 0.5f);
            g.controller = ToFac(Val(gj, "ctrl", json()));
            g.claimed = Val(gj, "claim", false);
            const auto hold = gj.find("hold");
            if (hold != gj.end() && hold->is_array() && hold->size() == 3 &&
                (*hold)[0].is_string() && (*hold)[1].is_string() && (*hold)[2].is_string())
            {
                g.holder = (*hold)[0].get<std::string>();
                g.holderTicker = (*hold)[1].get<std::string>();
                g.holderName = (*hold)[2].get<std::string>();
            }
            const auto fee = gj.find("fee");
            if (fee != gj.end() && fee->is_number() && fee->get<double>() >= 0.0 &&
                fee->get<double>() <= 1.0)
                g.fee = fee->get<float>();
            g.front = Val(gj, "front", false);
            const auto chart = gj.find("chart");
            if (chart != gj.end() && chart->is_array() && chart->size() == 2 &&
                (*chart)[0].is_number_integer() && (*chart)[0].get<long long>() > 0 &&
                (*chart)[0].get<long long>() < 1000000000LL && (*chart)[1].is_string())
            {
                g.chartPrice = (*chart)[0].get<int>();
                g.chartWhat = (*chart)[1].get<std::string>();
            }
            const auto lic = gj.find("lic");
            if (lic != gj.end() && lic->is_array() && lic->size() == 2 && (*lic)[0].is_string() &&
                (*lic)[1].is_number())
            {
                g.licenceBy = (*lic)[0].get<std::string>();
                g.licenceRent = (*lic)[1].get<double>();
            }
            out.systems.push_back(g);
        }
    out.events = Val(j, "news", std::vector<std::string>{});
    out.chartBuyer = false;
    const auto buyer = j.find("buyer");
    if (buyer != j.end() && buyer->is_object())
    {
        out.chartBuyer = true;
        out.chartFaction = ToFac(buyer->value("fac", json()));
        const auto t = buyer->find("treasury");
        out.chartTreasury =
            t != buyer->end() && t->is_number() ? std::max(0.0, t->get<double>()) : 0.0;
    }
    return true;
}

std::string EncodeUniverse(const WorldLoader::Universe& u)
{
    json j;
    Stamp(j, "universe");
    json sys = json::array();
    for (const WorldLoader::SystemInfo& si : u.systems)
    {
        sys.push_back({ { "id", si.id },
                        { "name", si.name },
                        { "pos", V2(si.mapPos) },
                        { "sec", si.security },
                        { "owner", si.owner },
                        { "charted", si.charted },
                        { "desig", si.designation },
                        { "found", si.discoverer },
                        { "known", si.knownBy },
                        { "chartedBy", si.chartedBy } });
        // The observed layer (#431 slice 14), only where there is one.
        if (si.observed.Observed())
            sys.back()["seen"] = WorldLoader::ToJson(si.observed);
        if (si.observedLive)
            sys.back()["live"] = true;
        if (si.drifted)  // cut off, or in the archive (#431 slice 15)
            sys.back()["drifted"] = true;
        if (si.nativeNamed)  // its name is its people's (#513)
            sys.back()["nat"] = true;
    }
    json links = json::array();
    for (const WorldLoader::SystemLink& l : u.links)
        if (l.kind.empty())
            links.push_back(json::array({ l.a, l.b }));
        else  // a roaming hole the pilot found (#342 S9): when it closes, and what it lets by
            links.push_back(json::array({ l.a, l.b, l.kind, l.closesAt, l.maxMass }));
    j["sys"] = sys;
    j["links"] = links;
    j["start"] = u.startId;
    return j.dump();
}

bool DecodeUniverse(const std::string& s, WorldLoader::Universe& out)
{
    json j;
    if (!OpenEnvelope(s, j) || Val(j, "t", std::string()) != "universe")
        return false;

    out = WorldLoader::Universe{};
    if (j.contains("sys") && j["sys"].is_array())
        for (const json& sj : j["sys"])
        {
            if (!sj.is_object())
                continue;
            WorldLoader::SystemInfo si;
            si.id = Val(sj, "id", std::string());
            si.name = Val(sj, "name", std::string());
            si.mapPos = ToV2(Val(sj, "pos", json::array()));
            si.security = Val(sj, "sec", 0.5f);
            si.owner = Val(sj, "owner", std::string());
            si.charted = Val(sj, "charted", true);
            si.designation = Val(sj, "desig", std::string());
            si.discoverer = Val(sj, "found", std::string());
            si.knownBy = Val(sj, "known", std::string());
            si.chartedBy = Val(sj, "chartedBy", std::string());
            si.observed = WorldLoader::SightingFrom(Val(sj, "seen", json()));
            si.observedLive = Val(sj, "live", false);
            si.drifted = Val(sj, "drifted", false);
            si.nativeNamed = Val(sj, "nat", false);
            out.systems.push_back(si);
        }
    if (j.contains("links") && j["links"].is_array())
        for (const json& lj : j["links"])
            if (lj.is_array() && lj.size() >= 2 && lj[0].is_string() && lj[1].is_string())
            {
                WorldLoader::SystemLink l{ lj[0].get<std::string>(), lj[1].get<std::string>() };
                if (lj.size() >= 5 && lj[2].is_string() && lj[3].is_number() && lj[4].is_number())
                {
                    l.kind = lj[2].get<std::string>();
                    l.closesAt = lj[3].get<double>();
                    l.maxMass = lj[4].get<float>();
                }
                out.links.push_back(std::move(l));
            }
    out.startId = Val(j, "start", std::string());
    return true;
}

// --- Chat (#347) ---------------------------------------------------------------------------
// A say is read by the server from anyone, so every field is read as what it must be or
// not at all -- a number where the text should be is an empty line, not an exception.

std::string EncodeChatSay(const ChatSay& c)
{
    json j;
    Stamp(j, "say");
    if (!c.to.empty())
        j["to"] = c.to;
    if (!c.channel.empty())
        j["ch"] = c.channel;
    j["text"] = c.text;
    return j.dump();
}

bool DecodeChatSay(const std::string& s, ChatSay& out)
{
    json j;
    if (!OpenEnvelope(s, j) || TypeOf(j) != "say")
        return false;
    out.to = TextOf(j, "to");
    out.text = TextOf(j, "text");
    out.channel = TextOf(j, "ch");
    return true;
}

std::string EncodeChatLines(const ChatLines& c)
{
    json j;
    Stamp(j, "chat");
    json lines = json::array();
    for (const ChatLine& l : c.lines)
    {
        json lj = { { "ch", l.channel }, { "text", l.text }, { "at", l.at } };
        if (!l.from.empty())
            lj["from"] = l.from;
        if (!l.to.empty())
            lj["to"] = l.to;
        if (!l.system.empty())
            lj["sys"] = l.system;
        if (!l.corp.empty())
            lj["corp"] = l.corp;
        if (l.history)
            lj["old"] = true;
        lines.push_back(std::move(lj));
    }
    j["lines"] = std::move(lines);
    return j.dump();
}

bool DecodeChatLines(const std::string& s, ChatLines& out)
{
    json j;
    if (!OpenEnvelope(s, j) || TypeOf(j) != "chat")
        return false;
    out = ChatLines{};
    const auto lines = j.find("lines");
    if (lines == j.end() || !lines->is_array())
        return true;
    for (const json& lj : *lines)
    {
        if (!lj.is_object())
            continue;
        ChatLine l;
        l.channel = TextOf(lj, "ch");
        l.from = TextOf(lj, "from");
        l.to = TextOf(lj, "to");
        l.system = TextOf(lj, "sys");
        l.corp = TextOf(lj, "corp");
        l.text = TextOf(lj, "text");
        const auto at = lj.find("at");
        l.at = at != lj.end() && at->is_number_integer() ? at->get<long long>() : 0;
        const auto old = lj.find("old");
        l.history = old != lj.end() && old->is_boolean() && old->get<bool>();
        if (!l.channel.empty())
            out.lines.push_back(std::move(l));
    }
    return true;
}

std::string EncodeChatMembers(const ChatMembers& m)
{
    json j;
    Stamp(j, "members");
    j["sys"] = m.system;
    j["pilots"] = m.pilots;
    // Only when anyone in local is in a corporation, and then one per pilot.
    for (const std::string& c : m.corps)
        if (!c.empty())
        {
            j["corps"] = m.corps;
            break;
        }
    return j.dump();
}

bool DecodeChatMembers(const std::string& s, ChatMembers& out)
{
    json j;
    if (!OpenEnvelope(s, j) || TypeOf(j) != "members")
        return false;
    out = ChatMembers{};
    out.system = TextOf(j, "sys");
    const auto pilots = j.find("pilots");
    if (pilots != j.end() && pilots->is_array())
        for (const json& p : *pilots)
            if (p.is_string())
                out.pilots.push_back(p.get<std::string>());
    const auto corps = j.find("corps");
    if (corps != j.end() && corps->is_array())
        for (const json& c : *corps)
            out.corps.push_back(c.is_string() ? c.get<std::string>() : std::string());
    out.corps.resize(out.pilots.size());  // one per pilot, whatever was sent
    return true;
}

// --- Corporations (#347) -------------------------------------------------------------------

std::string CorpView::RoleOf(const std::string& pilot) const
{
    for (const CorpMember& m : members)
        if (m.pilot == pilot)
            return m.role;
    return std::string();
}

std::string EncodeCorpAct(const CorpAct& a)
{
    json j;
    Stamp(j, "corp");
    j["verb"] = a.verb;
    if (!a.name.empty())
        j["name"] = a.name;
    if (!a.ticker.empty())
        j["ticker"] = a.ticker;
    if (!a.pilot.empty())
        j["pilot"] = a.pilot;
    if (!a.role.empty())
        j["role"] = a.role;
    if (!a.value.empty())
        j["value"] = a.value;
    if (!a.system.empty())
        j["system"] = a.system;
    if (!a.design.empty())
        j["design"] = a.design;
    if (!a.engage.empty())
        j["engage"] = a.engage;
    if (!a.threshold.empty())
        j["threshold"] = a.threshold;
    if (!a.allow.empty())
        j["allow"] = a.allow;
    return j.dump();
}

bool DecodeCorpAct(const std::string& s, CorpAct& out)
{
    json j;
    if (!OpenEnvelope(s, j) || TypeOf(j) != "corp")
        return false;
    out = CorpAct{};
    out.verb = TextOf(j, "verb");
    out.name = TextOf(j, "name");
    out.ticker = TextOf(j, "ticker");
    out.pilot = TextOf(j, "pilot");
    out.role = TextOf(j, "role");
    out.value = TextOf(j, "value");
    out.system = TextOf(j, "system");
    out.design = TextOf(j, "design");
    out.engage = TextOf(j, "engage");
    out.threshold = TextOf(j, "threshold");
    out.allow = TextOf(j, "allow");
    return !out.verb.empty();
}

std::string EncodeCorpView(const CorpView& v)
{
    json j;
    Stamp(j, "corpview");
    if (!v.id.empty())
    {
        j["id"] = v.id;
        j["name"] = v.name;
        j["ticker"] = v.ticker;
        j["founded"] = v.founded;
        json members = json::array();
        for (const CorpMember& m : v.members)
            members.push_back({ { "pilot", m.pilot }, { "role", m.role }, { "on", m.online } });
        j["members"] = std::move(members);
        j["treasury"] = v.treasury;
        if (!v.ledger.empty())
        {
            json ledger = json::array();
            for (const LedgerLine& l : v.ledger)
                ledger.push_back({ { "at", l.at },
                                   { "kind", l.kind },
                                   { "who", l.who },
                                   { "note", l.note },
                                   { "amount", l.amount },
                                   { "count", l.count } });
            j["ledger"] = std::move(ledger);
        }
        j["tax"] = v.tax;
        j["docking"] = v.docking;
        if (!v.holds.empty())
            j["holds"] = v.holds;
        if (!v.police.empty())
            j["police"] = v.police;
        if (!v.patrols.empty())
        {
            json patrols = json::array();
            for (const PatrolView& p : v.patrols)
                patrols.push_back({ { "system", p.system },
                                    { "systemName", p.systemName },
                                    { "ships", p.ships },
                                    { "present", p.present },
                                    { "design", p.design },
                                    { "engage", p.engage },
                                    { "threshold", p.threshold },
                                    { "allow", p.allow },
                                    { "upkeep", p.upkeep } });
            j["patrols"] = std::move(patrols);
        }
    }
    // A pilot in no corporation holds licences of their own (#509).
    if (!v.licences.empty())
    {
        json licences = json::array();
        for (const LicenceView& l : v.licences)
            licences.push_back(LicenceJson(l));
        j["licences"] = std::move(licences);
    }
    if (!v.invites.empty())
    {
        json invites = json::array();
        for (const CorpInvite& i : v.invites)
            invites.push_back(
                { { "name", i.name }, { "ticker", i.ticker }, { "by", i.by }, { "at", i.at } });
        j["invites"] = std::move(invites);
    }
    return j.dump();
}

bool DecodeCorpView(const std::string& s, CorpView& out)
{
    json j;
    if (!OpenEnvelope(s, j) || TypeOf(j) != "corpview")
        return false;
    out = CorpView{};
    out.id = TextOf(j, "id");
    out.name = TextOf(j, "name");
    out.ticker = TextOf(j, "ticker");
    const auto founded = j.find("founded");
    out.founded =
        founded != j.end() && founded->is_number_integer() ? founded->get<long long>() : 0;
    const auto members = j.find("members");
    if (members != j.end() && members->is_array())
        for (const json& mj : *members)
        {
            if (!mj.is_object())
                continue;
            CorpMember m;
            m.pilot = TextOf(mj, "pilot");
            m.role = TextOf(mj, "role");
            const auto on = mj.find("on");
            m.online = on != mj.end() && on->is_boolean() && on->get<bool>();
            if (!m.pilot.empty())
                out.members.push_back(std::move(m));
        }
    const auto treasury = j.find("treasury");
    out.treasury = treasury != j.end() && treasury->is_number() ? treasury->get<double>() : 0.0;
    const auto ledger = j.find("ledger");
    if (ledger != j.end() && ledger->is_array())
        for (const json& lj : *ledger)
            if (lj.is_object())
            {
                LedgerLine l;
                l.at = lj.value("at", 0LL);
                l.kind = TextOf(lj, "kind");
                l.who = TextOf(lj, "who");
                l.note = TextOf(lj, "note");
                l.amount = lj.value("amount", 0.0);
                l.count = lj.value("count", 1);
                if (!l.kind.empty())
                    out.ledger.push_back(std::move(l));
            }
    const auto tax = j.find("tax");
    out.tax = tax != j.end() && tax->is_number() ? tax->get<float>() : 0.0f;
    if (!TextOf(j, "docking").empty())
        out.docking = TextOf(j, "docking");
    const auto holds = j.find("holds");
    if (holds != j.end() && holds->is_array())
        for (const json& h : *holds)
            if (h.is_string())
                out.holds.push_back(h.get<std::string>());
    const auto licences = j.find("licences");
    if (licences != j.end() && licences->is_array())
        for (const json& l : *licences)
            if (l.is_object())
                out.licences.push_back(LicenceFrom(l));
    const auto police = j.find("police");
    if (police != j.end() && police->is_object())
        for (auto it = police->begin(); it != police->end(); ++it)
            if (it.value().is_number() && it.value().get<double>() > 0.0)
                out.police[it.key()] = it.value().get<double>();
    const auto patrols = j.find("patrols");
    if (patrols != j.end() && patrols->is_array())
        for (const json& pj : *patrols)
            if (pj.is_object())
            {
                PatrolView p;
                p.system = TextOf(pj, "system");
                p.systemName = TextOf(pj, "systemName");
                p.ships = Val(pj, "ships", 0);
                p.present = Val(pj, "present", 0.0f);
                p.design = TextOf(pj, "design");
                p.engage = TextOf(pj, "engage");
                p.threshold = Val(pj, "threshold", 0.0f);
                p.allow = TextOf(pj, "allow");
                p.upkeep = Val(pj, "upkeep", 0.0);
                if (!p.system.empty())
                    out.patrols.push_back(std::move(p));
            }
    const auto invites = j.find("invites");
    if (invites != j.end() && invites->is_array())
        for (const json& ij : *invites)
        {
            if (!ij.is_object())
                continue;
            CorpInvite i;
            i.name = TextOf(ij, "name");
            i.ticker = TextOf(ij, "ticker");
            i.by = TextOf(ij, "by");
            const auto at = ij.find("at");
            i.at = at != ij.end() && at->is_number_integer() ? at->get<long long>() : 0;
            if (!i.ticker.empty())
                out.invites.push_back(std::move(i));
        }
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

// What a wreck holds and what is left of a belt: the layout's and the snapshot's word on
// things the entity list does not carry.
static void CompleteHolds(EntitySnapshot& e, const EntityLayout& l, const Snapshot& s)
{
    if (l.kind == EntityKind::Derelict)
        e.holds = l.holds;
    if (l.kind == EntityKind::Field)
        for (const Snapshot::BeltOre& b : s.belts)
            if (b.id == l.id)
            {
                e.oreLeft = b.ore;
                e.oreMax = b.max;
            }
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
        CompleteHolds(e, l, s);
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
        CompleteHolds(e, l, s);
        s.entities.push_back(std::move(e));
    }
}

}  // namespace Proto
