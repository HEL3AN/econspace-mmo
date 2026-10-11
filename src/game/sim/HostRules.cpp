#include "sim/HostRules.h"

#include "sim/Protocol.h"
#include "sim/SaveFile.h"
#include "gen/Galaxy.h"
#include "gen/Region.h"
#include "sim/Simulation.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
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

std::string AccountSystem(const std::string& path)
{
    std::ifstream in(path);
    if (!in.is_open())
        return std::string();
    const nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.is_object())
        return std::string();
    const auto version = j.find("version");
    if (version != j.end() &&
        (!version->is_number_integer() || version->get<int>() > Save::ACCOUNT_VERSION))
        return std::string();  // a fresh account plays it: one that has nowhere yet
    const auto place = j.find("place");
    if (place == j.end() || !place->is_object())
        return std::string();
    const auto sys = place->find("system");
    return sys != place->end() && sys->is_string() ? sys->get<std::string>() : std::string();
}

bool NeedsStart(const Simulation& sim, const std::string& accountPath)
{
    const std::string sys = AccountSystem(accountPath);
    return sys.empty() || !sim.HasSystem(sys);
}

std::string PickStart(const Simulation& sim, const std::string& wanted, bool randomOnly,
                      uint64_t roll)
{
    const std::vector<Simulation::StartPoint>& starts = sim.Starts();
    if (starts.empty())
        return std::string();
    if (!randomOnly && !wanted.empty() && sim.IsStart(wanted))
        return wanted;
    return starts[(size_t)(roll % starts.size())].systemId;
}

Proto::Starts StartsMessage(const Simulation& sim, bool randomOnly)
{
    Proto::Starts m;
    m.randomOnly = randomOnly;
    for (const Simulation::StartPoint& sp : sim.Starts())
        m.options.push_back({ sp.systemId, sp.systemName, sp.station, sp.owner });
    return m;
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

std::string RegionsDir(const std::string& worldPath)
{
    const size_t slash = worldPath.find_last_of("/\\");
    return (slash == std::string::npos ? std::string() : worldPath.substr(0, slash + 1)) +
           "regions";
}

std::string WorldRulesRefusal(const Simulation::WorldHeader& header, const std::string& sealDir)
{
    // Before S3 nothing said which skeleton rules made a world, and there was one set.
    if (header.skeleton != 0 && header.skeleton != Gen::SKELETON_VERSION)
        return "its galaxy was laid out by skeleton rules v" + std::to_string(header.skeleton) +
               "; this build has v" + std::to_string(Gen::SKELETON_VERSION) +
               ". The same seed would put the regions elsewhere. Move it aside to start a new "
               "world; it has NOT been modified. Accounts are kept: a player whose system is "
               "gone chooses a start again.";
    if (header.generator == Gen::GENERATOR_VERSION)
        return std::string();
    // Other region rules: every region it has must be kept as it was made.
    std::string missing;
    int         count = 0;
    for (const Simulation::RegionOpening& o : header.log)
    {
        Gen::Region r;
        if (Simulation::ReadSealed(Simulation::SealedPath(sealDir, o.slot), header.seed, r))
            continue;
        missing += (count++ == 0 ? "" : ", ") + std::to_string(o.slot);
    }
    if (!header.log.empty() && count == 0)
        return std::string();
    return "it was generated by region rules v" + std::to_string(header.generator) +
           "; this build has v" + std::to_string(Gen::GENERATOR_VERSION) + ", and " +
           (header.log.empty() ? std::string("it is from before regions were sealed (#342 S3)")
                               : "region(s) " + missing + " are not sealed in " + sealDir) +
           ". The same seed would make a different galaxy" +
           (header.generator < 7 ? " (v7 replaced the hand-written systems with generated "
                                   "civilisation centres, #342)"
                                 : "") +
           ". Move it aside to start a new one; it has NOT been modified. Accounts are kept: a "
           "player whose system is gone chooses a start again.";
}

namespace
{
constexpr double HOUR = 3600.0;

std::string PairKey(const std::string& address, const std::string& account)
{
    return address + "|" + account;
}
}  // namespace

std::string Gate::RefuseConnection(int total, int fromAddress) const
{
    if (total >= limits_.maxClients)
        return "The server is full; try again later";
    if (fromAddress >= limits_.maxPerAddress)
        return "Too many connections from your address";
    return std::string();
}

bool Gate::MayRegister(const std::string& address, double now)
{
    auto it = registrations_.find(address);
    if (it == registrations_.end())
        return true;
    std::vector<double>& when = it->second;
    when.erase(std::remove_if(when.begin(), when.end(), [&](double t) { return now - t >= HOUR; }),
               when.end());
    return (int)when.size() < limits_.registrationsPerHour;
}

void Gate::Registered(const std::string& address, double now)
{
    registrations_[address].push_back(now);
}

double Gate::LoginWait(const std::string& address, const std::string& account, double now) const
{
    auto it = failures_.find(PairKey(address, account));
    if (it == failures_.end() || it->second.until <= now)
        return 0.0;
    return it->second.until - now;
}

void Gate::LoginFailed(const std::string& address, const std::string& account, double now)
{
    Failures& f = failures_[PairKey(address, account)];
    f.count++;
    double wait = 1.0;
    for (int i = 1; i < f.count && wait < limits_.maxBackoff; i++)
        wait *= 2.0;
    f.until = now + std::min(wait, limits_.maxBackoff);
    f.last = now;
}

void Gate::LoginSucceeded(const std::string& address, const std::string& account)
{
    failures_.erase(PairKey(address, account));
}

void Gate::Expire(double now)
{
    // A failure an hour old is forgotten; the doubling starts again from one second.
    for (auto it = failures_.begin(); it != failures_.end();)
        it = now - it->second.last >= HOUR ? failures_.erase(it) : std::next(it);
    for (auto it = registrations_.begin(); it != registrations_.end();)
    {
        std::vector<double>& when = it->second;
        when.erase(
            std::remove_if(when.begin(), when.end(), [&](double t) { return now - t >= HOUR; }),
            when.end());
        it = when.empty() ? registrations_.erase(it) : std::next(it);
    }
}

std::string VersionRefusal(int clientVersion)
{
    return "This server speaks protocol v" + std::to_string(Proto::PROTO_VERSION) +
           " and your client speaks v" + std::to_string(clientVersion) +
           ": update the one that is older";
}

}  // namespace Host
