#pragma once

#include "temp_dir.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <string>

// The shipped blueprints with walls of a fixture's size: a wall is measured in the reference
// fleet's guns (#568), and the real ones take a few holdings' spare ships and an hour -- a
// scene of one pirate den and an outpost needs the walls scaled to it. Two reference ships,
// a tier that mends a quarter of one of them's fire, and every `holds` a sixtieth: an outpost of
// about four thousand points, mending about as much as a frigate fires, falling in a minute or
// so to a den's few ships -- the scene the tests were written on. The shipped numbers are
// tested in damage_tests.
inline std::string SmallWalls(const std::string& data, double divisor = 60.0)
{
    static const TempDir tmp;
    const std::string    path = tmp.Path("small_walls_" + std::to_string((int)divisor) + ".json");
    nlohmann::json       j;
    {
        std::ifstream in(data + "blueprints.json");
        j = nlohmann::json::parse(in);
    }
    j["siege"]["referenceShips"] = 2;
    j["siege"]["seconds"] = 900;
    for (auto& t : j["tiers"])
        if (t.contains("mendShips"))
            t["mendShips"] = t["mendShips"].get<double>() * 0.25;
    for (auto& b : j["blueprints"])
        if (b.contains("holds"))
            b["holds"] = b["holds"].get<double>() / divisor;
    std::ofstream out(path);
    out << j.dump();
    return path;
}
