#include "sim/HostRules.h"

#include "sim/Protocol.h"
#include "sim/SaveFile.h"
#include "sim/Simulation.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

namespace Host
{

std::string CanonicalAccount(const std::string& typed)
{
    // Rejecting is better than mangling: two names differing only in punctuation would
    // otherwise share one account file, which is one player spending another's money.
    if (typed.empty() || typed.size() > 32)
        return std::string();
    std::string out;
    out.reserve(typed.size());
    for (char c : typed)
    {
        const unsigned char u = (unsigned char)c;
        if (!std::isalnum(u) && c != '_' && c != '-')
            return std::string();
        out.push_back((char)std::tolower(u));
    }
    return out;
}

std::string AccountFile(const std::string& dir, const std::string& canonical)
{
    namespace fs = std::filesystem;
    const std::string want = "account_" + canonical + ".json";
    const std::string path = dir + want;
    std::error_code   ec;
    if (fs::exists(path, ec))
        return path;

    // Only on a file system that tells cases apart can the canonical name be missing while
    // a capitalised one is there.
    std::vector<fs::path> spellings;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
    {
        const std::string name = it->path().filename().string();
        if (name.size() != want.size())
            continue;
        std::string lower = name;
        for (char& c : lower)
            c = (char)std::tolower((unsigned char)c);
        if (lower == want)
            spellings.push_back(it->path());
    }
    if (spellings.size() == 1)
    {
        fs::rename(spellings[0], path, ec);
        if (ec)
            return spellings[0].generic_string();
    }
    return path;
}

Save::Result ReadAccountAuth(const std::string& path, std::string& salt, std::string& stored)
{
    salt.clear();
    stored.clear();
    std::ifstream in(path);
    if (!in.is_open())
        return Save::Result::Missing;
    const nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.is_object())
        return Save::Result::Corrupt;
    const auto version = j.find("version");
    if (version != j.end() && !version->is_number_integer())
        return Save::Result::Corrupt;
    const auto auth = j.find("auth");
    if (auth != j.end())
    {
        if (!auth->is_object())
            return Save::Result::Corrupt;
        const auto s = auth->find("salt");
        const auto t = auth->find("stored");
        if (s != auth->end() && s->is_string())
            salt = s->get<std::string>();
        if (t != auth->end() && t->is_string())
            stored = t->get<std::string>();
    }
    // Read for the secret either way: a file from a later build still names its owner, and
    // the owner still has to prove they are it (it is the account that is read-only, #20).
    if (version != j.end() && version->get<int>() > Save::ACCOUNT_VERSION)
        return Save::Result::TooNew;
    return Save::Result::Ok;
}

WorldStart OpenWorld(Simulation& sim, const std::string& path, std::string& why)
{
    why.clear();
    switch (path.empty() ? Save::Result::Missing : sim.LoadWorld(path))
    {
        case Save::Result::Ok: return WorldStart::Restored;
        case Save::Result::Missing: sim.InitGalaxy(); return WorldStart::Fresh;
        case Save::Result::TooNew:
            // Refusing to start beats starting a different galaxy: the checkpoint timer would
            // write this build's idea of the world over the one that is there (#20).
            why = path + " was written by a newer build (this one speaks world schema v" +
                  std::to_string(Save::WORLD_VERSION) +
                  ").\nMove it aside or upgrade the server; it has NOT been modified.";
            return WorldStart::Refused;
        case Save::Result::Corrupt: break;
    }
    why = path + " is damaged and cannot be read.\nThe last good saves are kept beside it (" +
          Save::BackupPath(path, 1) +
          " is the newest): copy one over it to go on with that "
          "world, or move it aside to start a new galaxy. It has NOT been modified.";
    return WorldStart::Refused;
}

std::string VersionRefusal(int clientVersion)
{
    return "This server speaks protocol v" + std::to_string(Proto::PROTO_VERSION) +
           " and your client speaks v" + std::to_string(clientVersion) +
           ": update the one that is older";
}

}  // namespace Host
