#include "render/TreatmentConfig.h"

#include <nlohmann/json.hpp>
#include <cmath>
#include <fstream>

using nlohmann::json;
// Reading takes a plain json; writing takes an ordered one, so the file the game produces
// has its members in the order a person would write them rather than alphabetically --
// `pass` first, because it is the only thing that identifies an entry (#130).
using nlohmann::ordered_json;

namespace Render
{
namespace
{
struct Named
{
    PassKind    kind;
    const char* name;
    const char* scaleMeaning;
};

const Named NAMES[] = {
    { PassKind::Bloom, "bloom", "how far the glow reaches" },
    { PassKind::Pixelate, "pixelate", "how large one pixel is" },
    { PassKind::Scanlines, "scanlines", "how far apart the lines sit" },
    { PassKind::Noise, "noise", "how coarse the grain is" },
    { PassKind::Fringe, "fringe", "how much the image bows" },
    { PassKind::Vignette, "vignette", "how far in the corners reach" },
};
}  // namespace

const char* PassName(PassKind k)
{
    for (const Named& n : NAMES)
        if (n.kind == k)
            return n.name;
    return "?";
}

const char* ScaleMeaning(PassKind k)
{
    for (const Named& n : NAMES)
        if (n.kind == k)
            return n.scaleMeaning;
    return "";
}

bool PassFromName(const std::string& s, PassKind& out)
{
    for (const Named& n : NAMES)
        if (s == n.name)
        {
            out = n.kind;
            return true;
        }
    return false;
}

const std::vector<PassKind>& AllPasses()
{
    static const std::vector<PassKind> all = []
    {
        std::vector<PassKind> v;
        for (const Named& n : NAMES)
            v.push_back(n.kind);
        return v;
    }();
    return all;
}

TreatmentConfig TreatmentConfig::Default()
{
    // Bloom first, then pixels: the glow is resolved on the coarse grid with everything
    // else, which is what makes a bright edge bleed into whole pixels rather than into
    // fractions of one. Reversing these two is the single biggest change available here,
    // which is why the order is data.
    TreatmentConfig c;
    c.chain = {
        { PassKind::Bloom, true, 0.85f, 2.2f },     { PassKind::Pixelate, true, 1.0f, 2.0f },
        { PassKind::Scanlines, true, 0.30f, 2.0f }, { PassKind::Noise, true, 0.10f, 1.0f },
        { PassKind::Fringe, true, 0.30f, 1.0f },    { PassKind::Vignette, true, 0.50f, 1.0f },
    };
    return c;
}

void TreatmentConfig::MoveUp(size_t index)
{
    if (index > 0 && index < chain.size())
        std::swap(chain[index], chain[index - 1]);
}

void TreatmentConfig::MoveDown(size_t index)
{
    if (index + 1 < chain.size())
        std::swap(chain[index], chain[index + 1]);
}

bool LoadTreatment(const std::string& path, TreatmentConfig& out, std::string& error)
{
    error.clear();
    std::ifstream in(path);
    if (!in.is_open())
    {
        error = "cannot open " + path;
        return false;
    }

    json j = json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.is_object())
    {
        error = path + " is not valid JSON";
        return false;
    }

    TreatmentConfig cfg;
    cfg.enabled = j.value("enabled", true);
    cfg.treatHud = j.value("treatHud", false);

    if (j.contains("chain") && j["chain"].is_array())
    {
        for (const json& e : j["chain"])
        {
            if (!e.is_object())
                continue;
            PassKind kind;
            if (!PassFromName(e.value("pass", std::string()), kind))
                continue;  // an unknown pass is skipped, not fatal -- see the header
            Pass p;
            p.kind = kind;
            p.enabled = e.value("enabled", true);
            p.amount = e.value("amount", 1.0f);
            p.scale = e.value("scale", 1.0f);
            cfg.chain.push_back(p);
        }
    }

    // A file with no usable chain is a file that would turn the look off without saying
    // so. Take the rest of it and keep the chain that ships.
    if (cfg.chain.empty())
        cfg.chain = TreatmentConfig::Default().chain;

    out = cfg;
    return true;
}

// A knob a human set by dragging, at the precision a human can mean.
//
// These are floats, and nlohmann widens a float to a double before printing it -- so a
// value the slider produced arrives in the file as 0.8521126508712769. Three decimals is
// already finer than the slider can express, and the other fourteen digits would be noise
// in every diff of this file from here on. The same widening cost this project real
// bandwidth once before (#97).
static double Rounded(float v)
{
    return std::round((double)v * 1000.0) / 1000.0;
}

bool SaveTreatment(const std::string& path, const TreatmentConfig& cfg, std::string& error)
{
    error.clear();
    ordered_json j;
    j["enabled"] = cfg.enabled;
    j["treatHud"] = cfg.treatHud;
    j["chain"] = ordered_json::array();
    for (const Pass& p : cfg.chain)
        j["chain"].push_back(ordered_json{ { "pass", PassName(p.kind) },
                                           { "enabled", p.enabled },
                                           { "amount", Rounded(p.amount) },
                                           { "scale", Rounded(p.scale) } });

    std::ofstream file(path);
    if (!file.is_open())
    {
        error = "cannot write " + path;
        return false;
    }
    // Unlike archetypes.json this file is written by the game far more often than it is
    // read by a human, so a plain dump is right: nobody hand-formats it (#118 wrote its
    // own editor precisely because the archetype file *is* hand-formatted).
    file << j.dump(4) << "\n";
    return true;
}

}  // namespace Render
