#pragma once

#include "render/Silhouette.h"
#include <string>
#include <vector>

// The module library (#240): recognisable things -- a hatch, a turret, a dish, a tank --
// each written once in the composition grammar, in several variants, and placed on objects
// by name. A part that says `"module": "hatch"` becomes the parts of one of the hatch's
// variants, carried to where the part is, turned and scaled with it; which variant is the
// object's seed's choice unless the part names one. So two trade hubs differ in which
// hatches they wear (#139), and a hatch is drawn well once rather than badly everywhere.
//
// data/modules.json, loaded before the archetypes that use it:
//   { "modules": [ { "id": "hatch", "tags": ["opening"], "sockets": ["top", "edge"],
//                    "variants": [ { "id": "square", "shape": [ ...parts... ] }, ... ] } ] }
//
// More modules come in packs, one file per domain -- data/modules/weapons.json,
// data/modules/planet.json -- in the same format, all loaded after modules.json in name
// order. A pack is a file so that writing one never touches another; an id is still global,
// and the same id in two files is an error naming both.
//
// A module's parts are written in its own unit, radius 1 about its own centre. They may
// not repeat, mirror, orbit or sit on a sphere -- those place things about the *object's*
// centre -- but they may use a row, which is a line in the module's own frame.
namespace Render
{
struct ModuleVariant
{
    std::string id;
    Shape       shape;
};

struct Module
{
    std::string                id;
    std::string                pack;  // the file it came from: "modules", "weapons", ...
    std::vector<std::string>   tags;
    std::vector<std::string>   sockets;  // where it fits on a section (phase 3 of #240)
    std::vector<ModuleVariant> variants;
};

namespace Modules
{
// Replaces the library with the file's and then every pack in the `modules` directory beside
// it. A missing file is an empty library, not an error: nothing has to use modules. A file
// that is there and wrong is an error, said by name.
bool Load(const std::string& path, std::string& error);
void Clear();

const Module*              Find(const std::string& id);
const std::vector<Module>& All();
}  // namespace Modules
}  // namespace Render
