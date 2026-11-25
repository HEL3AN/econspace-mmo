#include "sim/EventKinds.h"

#include "core/Archetype.h"
#include "core/JsonKeys.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace FieldEvents
{
namespace
{
std::vector<Kind> g_kinds;
std::string       g_error;
std::string       g_hash;

// FNV-1a over the file's bytes, as hex: a world's note of the events it was made with.
std::string HashOf(const std::string& text)
{
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : text)
        h = (h ^ c) * 1099511628211ull;
    char buf[24];
    snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)h);
    return buf;
}

bool Number(const nlohmann::json& j, const char* key, double& out, const std::string& where,
            std::string& why)
{
    if (!j.contains(key))
        return true;
    if (!j[key].is_number() || j[key].get<double>() < 0.0)
    {
        why = where + ": '" + key + "' is not a number of zero or more";
        return false;
    }
    out = j[key].get<double>();
    return true;
}

// [lo, hi], two numbers of zero or more with lo not above hi.
bool Range(const nlohmann::json& j, const char* key, double& lo, double& hi,
           const std::string& where, std::string& why)
{
    if (!j.contains(key))
        return true;
    const nlohmann::json& r = j[key];
    if (!r.is_array() || r.size() != 2 || !r[0].is_number() || !r[1].is_number() ||
        r[0].get<double>() < 0.0 || r[0].get<double>() > r[1].get<double>())
    {
        why = where + ": '" + key + "' is [low, high], two numbers of zero or more";
        return false;
    }
    lo = r[0].get<double>();
    hi = r[1].get<double>();
    return true;
}

bool ReadKind(const nlohmann::json& j, size_t index, Kind& k, std::string& why)
{
    const std::string where = "events.json: kind " + std::to_string(index);
    if (!j.is_object())
    {
        why = where + ": not an object";
        return false;
    }
    std::string bad;
    if (!OnlyKnownKeys(j,
                       { "id", "name", "weight", "needs", "ores", "share", "moves", "hours", "size",
                         "orbit", "field", "archetypes", "line" },
                       bad))
    {
        why = where + ": " + bad;
        return false;
    }
    k.id = j.value("id", std::string());
    if (k.id.empty())
    {
        why = where + ": no id";
        return false;
    }
    const std::string at = "events.json: '" + k.id + "'";
    k.name = j.value("name", k.id);
    k.needs = j.value("needs", std::string());
    if (k.needs != "planet" && k.needs != "belt")
    {
        why = at + ": needs is planet or belt";
        return false;
    }
    k.field = j.value("field", k.name);
    k.line = j.value("line", std::string());
    if (!Number(j, "weight", k.weight, at, why) || !Number(j, "moves", k.moves, at, why) ||
        !Range(j, "share", k.shareLo, k.shareHi, at, why) ||
        !Range(j, "hours", k.hoursLo, k.hoursHi, at, why) ||
        !Range(j, "size", k.sizeLo, k.sizeHi, at, why))
        return false;
    if (k.hoursLo <= 0.0 || k.shareHi > 1.0 || k.moves > 1.0)
    {
        why = at + ": hours must be above zero, share and moves are shares of 0 to 1";
        return false;
    }
    if (j.contains("ores"))
    {
        if (!j["ores"].is_object())
        {
            why = at + ": ores is an object of ore id to weight";
            return false;
        }
        for (auto it = j["ores"].begin(); it != j["ores"].end(); ++it)
        {
            if (!it.value().is_number() || it.value().get<double>() < 0.0)
            {
                why = at + ": ores '" + it.key() + "' is not a weight of zero or more";
                return false;
            }
            k.ores[it.key()] = it.value().get<double>();
        }
    }
    if (k.needs == "planet" && k.ores.empty())
    {
        why = at + ": a kind that needs a planet says what it is made of (ores)";
        return false;
    }
    if (k.needs == "belt" && k.moves <= 0.0)
    {
        why = at + ": a kind that needs a belt says how much of it it moves (moves)";
        return false;
    }
    if (j.contains("orbit"))
    {
        std::string           o;
        const nlohmann::json& orbit = j["orbit"];
        if (!orbit.is_object() || !OnlyKnownKeys(orbit, { "radius", "speed" }, o))
        {
            why = at + ": orbit is { radius, speed }" + (o.empty() ? "" : " (" + o + ")");
            return false;
        }
        if (!Number(orbit, "radius", k.orbitRadius, at, why) ||
            !Number(orbit, "speed", k.orbitSpeed, at, why))
            return false;
    }
    if (j.contains("archetypes"))
    {
        std::string           o;
        const nlohmann::json& a = j["archetypes"];
        if (!a.is_object() || !OnlyKnownKeys(a, { "public", "signature" }, o))
        {
            why = at + ": archetypes is { public, signature }" + (o.empty() ? "" : " (" + o + ")");
            return false;
        }
        k.publicArchetype = a.value("public", std::string());
        k.signatureArchetype = a.value("signature", k.publicArchetype);
    }
    for (const std::string* id : { &k.publicArchetype, &k.signatureArchetype })
    {
        const Archetype* a = Archetypes::Find(*id);
        if (a == nullptr || a->kind != EntityKind::Field)
        {
            why = at + ": archetype '" + *id + "' is not a field archetype";
            return false;
        }
    }
    return true;
}
}  // namespace

bool Load(const std::string& path)
{
    g_error.clear();
    std::ifstream in(path);
    if (!in.is_open())
    {
        g_error = "cannot open " + path;
        return false;
    }
    std::stringstream text;
    text << in.rdbuf();
    const nlohmann::json j = nlohmann::json::parse(text.str(), nullptr, false);
    std::string          bad;
    if (j.is_discarded() || !j.is_object() || !j.contains("kinds") || !j["kinds"].is_array())
    {
        g_error = path + ": not an events file (no 'kinds' list)";
        return false;
    }
    if (!OnlyKnownKeys(j, { "schema", "kinds" }, bad))
    {
        g_error = path + ": " + bad;
        return false;
    }
    std::vector<Kind> kinds;
    for (size_t i = 0; i < j["kinds"].size(); i++)
    {
        Kind k;
        if (!ReadKind(j["kinds"][i], i, k, g_error))
            return false;
        for (const Kind& other : kinds)
            if (other.id == k.id)
            {
                g_error = path + ": two kinds are called '" + k.id + "'";
                return false;
            }
        kinds.push_back(std::move(k));
    }
    g_kinds = std::move(kinds);
    g_hash = HashOf(text.str());
    return true;
}

const std::string& Error()
{
    return g_error;
}

const std::vector<Kind>& Kinds()
{
    return g_kinds;
}

const Kind* Find(const std::string& id)
{
    for (const Kind& k : g_kinds)
        if (k.id == id)
            return &k;
    return nullptr;
}

std::string Names()
{
    std::string out;
    for (const Kind& k : g_kinds)
        out += " " + k.id;
    return out;
}

const std::string& Hash()
{
    return g_hash;
}

void Set(const std::vector<Kind>& kinds)
{
    g_kinds = kinds;
    g_hash = "test";
}
}  // namespace FieldEvents
