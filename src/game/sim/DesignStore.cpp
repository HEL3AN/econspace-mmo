#include "sim/DesignStore.h"

#include "core/Archetype.h"
#include "sim/SaveFile.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <fstream>

using nlohmann::json;

std::string DesignStore::File(Ships::Design d, std::string& error)
{
    // What it is called and what it was called before are not what it is.
    d.id = Ships::ContentId(d);
    d.name = d.name.empty() ? d.id : d.name;
    auto it = designs_.find(d.id);
    if (it != designs_.end())
        return d.id;
    if (!Ships::Validate(Archetypes::ShipCatalogue(), d, error) || !Archetypes::AddDesign(d, error))
        return std::string();
    const std::string id = d.id;
    designs_[id].design = std::move(d);
    return id;
}

const Ships::Design* DesignStore::Find(const std::string& id) const
{
    auto it = designs_.find(id);
    if (it != designs_.end())
        return &it->second.design;
    return Archetypes::ShipCatalogue().FindDesign(id);
}

void DesignStore::SetUsers(const std::string& account, const std::set<std::string>& ids)
{
    for (auto& [id, e] : designs_)
        if (ids.count(id) != 0)
            e.users.insert(account);
        else
            e.users.erase(account);
}

int DesignStore::Prune(const std::set<std::string>& live)
{
    int gone = 0;
    for (auto it = designs_.begin(); it != designs_.end();)
        if (it->second.users.empty() && live.count(it->first) == 0)
        {
            it = designs_.erase(it);
            gone++;
        }
        else
            ++it;
    return gone;
}

bool DesignStore::Save(const std::string& path) const
{
    json list = json::array();
    for (const auto& [id, e] : designs_)
    {
        json d = Ships::ToJson(e.design);
        list.push_back({ { "design", d }, { "users", e.users } });
    }
    const json j = { { "version", Save::DESIGNS_VERSION }, { "designs", list } };
    // Renamed into place, never half-written, like every save (#341): a truncated file
    // would be every refitted ship's design gone at the next boot.
    return Save::WriteFileAtomic(path, j.dump(2) + "\n", 1);
}

Save::Result DesignStore::Load(const std::string& path)
{
    std::ifstream in(path);
    if (!in.is_open())
        return Save::Result::Missing;
    const json j = json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.is_object())
        return Save::Result::Corrupt;
    if (j.value("version", Save::UNVERSIONED) > Save::DESIGNS_VERSION)
        return Save::Result::TooNew;
    designs_.clear();
    if (!j.contains("designs") || !j["designs"].is_array())
        return Save::Result::Ok;
    for (const json& e : j["designs"])
    {
        Ships::Design d;
        std::string   why;
        if (!e.is_object() || !e.contains("design") || !Ships::FromJson(e["design"], d, why))
            continue;
        const std::string was = d.id;
        // Filed again, so it is checked against today's catalogue and its id is today's hash
        // of it: a design whose parts changed meaning is not that design any more.
        const std::string id = File(d, why);
        if (id.empty() || id != was)
        {
            fprintf(stderr, "designs: %s is dropped: %s\n", was.c_str(),
                    id.empty() ? why.c_str() : "its parts no longer hash to its id");
            if (!id.empty())
                designs_.erase(id);
            continue;
        }
        if (e.contains("users") && e["users"].is_array())
            for (const json& u : e["users"])
                if (u.is_string())
                    designs_[id].users.insert(u.get<std::string>());
    }
    return Save::Result::Ok;
}
