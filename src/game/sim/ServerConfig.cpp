#include "sim/ServerConfig.h"

#include "sim/SaveFile.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>

namespace Host
{

const char* const SERVER_CONFIG_FILE = "server.json";

std::string WithSlash(const std::string& dir)
{
    if (dir.empty() || dir.back() == '/' || dir.back() == '\\')
        return dir;
    return dir + "/";
}

std::string WorldDirOf(const std::string& worldPath)
{
    const size_t slash = worldPath.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : worldPath.substr(0, slash + 1);
}

namespace
{

const char* RegistrationName(Registration r)
{
    return r == Registration::Open ? "open" : "allowlist";
}

// A whole number in [lo, hi], or false. A float, a string or a bool is not one.
bool ReadInt(const nlohmann::json& v, long long lo, long long hi, long long& out)
{
    if (!v.is_number_integer())
        return false;
    const long long n = v.get<long long>();
    if (n < lo || n > hi)
        return false;
    out = n;
    return true;
}

// A number given on the command line, all of it a number.
bool ParseNumber(const std::string& s, long long lo, long long hi, long long& out)
{
    if (s.empty())
        return false;
    char*           end = nullptr;
    const long long n = std::strtoll(s.c_str(), &end, 10);
    if (end == nullptr || *end != '\0' || n < lo || n > hi)
        return false;
    out = n;
    return true;
}

}  // namespace

bool ParseServerConfig(const std::string& text, ServerConfig& out, std::string& error)
{
    nlohmann::json j;
    try
    {
        j = nlohmann::json::parse(text, nullptr, true, /*ignore_comments=*/true);
    }
    catch (const nlohmann::json::parse_error& e)
    {
        error = std::string("not JSON: ") + e.what();
        return false;
    }
    if (!j.is_object())
    {
        error = "not a JSON object";
        return false;
    }
    ServerConfig c = out;
    for (auto it = j.begin(); it != j.end(); ++it)
    {
        const std::string&    key = it.key();
        const nlohmann::json& v = it.value();
        long long             n = 0;
        bool                  ok = true;
        if (key == "port")
        {
            ok = ReadInt(v, 0, 65535, n);
            c.port = (int)n;
        }
        else if (key == "public" || key == "randomStarts" || key == "dev")
        {
            ok = v.is_boolean();
            const bool b = ok && v.get<bool>();
            (key == "public" ? c.isPublic : key == "dev" ? c.dev : c.randomStarts) = b;
        }
        else if (key == "registration")
        {
            const std::string s = v.is_string() ? v.get<std::string>() : std::string();
            ok = s == "open" || s == "allowlist";
            c.registration = s == "open" ? Registration::Open : Registration::Allowlist;
        }
        else if (key == "centres")
        {
            ok = ReadInt(v, 1, Gen::MAX_CENTRES, n);
            c.centres = (int)n;
        }
        else if (key == "maxRegions" || key == "maxSystems")
        {
            ok = ReadInt(v, 0, 1000000, n);
            (key == "maxRegions" ? c.maxRegions : c.maxSystems) = (int)n;
        }
        else if (key == "seed")
        {
            ok = v.is_number_unsigned() || (v.is_number_integer() && v.get<long long>() >= 0);
            c.seed = ok ? v.get<uint64_t>() : 0;
        }
        else if (key == "backupsKept")
        {
            ok = ReadInt(v, 0, 100000, n);
            c.backupsKept = (int)n;
        }
        else
        {
            error = "unknown setting \"" + key + "\"";
            return false;
        }
        if (!ok)
        {
            error = "\"" + key + "\" has a value it cannot take: " + v.dump();
            return false;
        }
    }
    out = c;
    return true;
}

bool LoadServerConfig(const std::string& path, ServerConfig& out, bool& exists, std::string& error)
{
    std::error_code ec;
    exists = std::filesystem::exists(path, ec);
    if (!exists)
        return true;
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        error = path + ": cannot be read";
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!ParseServerConfig(text, out, error))
    {
        error = path + ": " + error;
        return false;
    }
    return true;
}

std::string ServerConfigText(const ServerConfig& c)
{
    const auto  b = [](bool v) { return v ? "true" : "false"; };
    std::string t;
    t += "// EconSpace server settings. `econserver host --world <this folder>` reads them; a "
         "flag\n";
    t += "// given to host (--port, --public, --private, --open, --allowlist, --centres, --seed,\n";
    t += "// --max-regions, --max-systems, --random-starts, --dev) overrides one for that run.\n";
    t += "// An unknown key is an error, so a misspelt one stops the server instead of being "
         "ignored.\n";
    t += "{\n";
    t += "  // The TCP port players connect to. Forward this port on the router.\n";
    t += "  \"port\": " + std::to_string(c.port) + ",\n";
    t += "  // true: other machines may connect. false: this machine only.\n";
    t += std::string("  \"public\": ") + b(c.isPublic) + ",\n";
    t += "  // \"allowlist\": only the names in allowlist.txt may log in or create an account.\n";
    t += "  // \"open\": anyone who finds the port may create one. The connection is not "
         "encrypted\n";
    t += "  // yet, so keep a server on the internet on the allowlist.\n";
    t += std::string("  \"registration\": \"") + RegistrationName(c.registration) + "\",\n";
    t += "  // A new world's civilisation centres, 1 to 6. A saved world keeps its own.\n";
    t += "  \"centres\": " + std::to_string(c.centres) + ",\n";
    t += "  // true: a new pilot is dealt a start instead of choosing one.\n";
    t += std::string("  \"randomStarts\": ") + b(c.randomStarts) + ",\n";
    t += "  // The cap: no region opens past this many regions or systems. 0: the defaults.\n";
    t += "  \"maxRegions\": " + std::to_string(c.maxRegions) + ",\n";
    t += "  \"maxSystems\": " + std::to_string(c.maxSystems) + ",\n";
    t += "  // A new world's seed; 0 picks one at random. A saved world keeps its own.\n";
    t += "  \"seed\": " + std::to_string(c.seed) + ",\n";
    t += "  // Debug commands (F1's money) for every client. Never on a server friends play on.\n";
    t += std::string("  \"dev\": ") + b(c.dev) + ",\n";
    t += "  // How many copies the console's `backup` keeps in backups/; 0 keeps every one.\n";
    t += "  \"backupsKept\": " + std::to_string(c.backupsKept) + "\n";
    t += "}\n";
    return t;
}

bool ApplyHostFlags(const std::vector<std::string>& args, ServerConfig& cfg, std::string& world,
                    std::string& packs, std::string& error)
{
    ServerConfig c = cfg;
    for (size_t i = 0; i < args.size(); i++)
    {
        const std::string& a = args[i];
        const bool         hasValue = i + 1 < args.size();
        long long          n = 0;
        // A value that is missing or not a number is named, not read as 0.
        const auto number = [&](long long lo, long long hi) -> bool
        {
            if (!hasValue || !ParseNumber(args[i + 1], lo, hi, n))
            {
                error =
                    a + " takes a number from " + std::to_string(lo) + " to " + std::to_string(hi);
                return false;
            }
            i++;
            return true;
        };
        if (i == 0 && ParseNumber(a, 0, 65535, n))
            c.port = (int)n;  // `host 50800`, as it always was
        else if (a == "--port")
        {
            if (!number(0, 65535))
                return false;
            c.port = (int)n;
        }
        else if (a == "--public")
            c.isPublic = true;
        else if (a == "--private")
            c.isPublic = false;
        else if (a == "--open")
            c.registration = Registration::Open;
        else if (a == "--allowlist")
            c.registration = Registration::Allowlist;
        else if (a == "--dev")
            c.dev = true;
        else if (a == "--random-starts")
            c.randomStarts = true;
        else if (a == "--centres")
        {
            if (!number(1, Gen::MAX_CENTRES))
                return false;
            c.centres = (int)n;
        }
        else if (a == "--seed")
        {
            if (!hasValue || args[i + 1].empty() || args[i + 1][0] == '-')
            {
                error = "--seed takes a number";
                return false;
            }
            char* end = nullptr;
            c.seed = std::strtoull(args[++i].c_str(), &end, 10);
            if (end == nullptr || *end != '\0')
            {
                error = "--seed takes a number";
                return false;
            }
        }
        else if (a == "--max-regions" || a == "--max-systems")
        {
            if (!number(1, 1000000))
                return false;
            (a == "--max-regions" ? c.maxRegions : c.maxSystems) = (int)n;
        }
        else if ((a == "--world" || a == "--packs") && hasValue)
            (a == "--world" ? world : packs) = args[++i];
        else
        {
            error = "unknown option " + a;
            return false;
        }
    }
    cfg = c;
    return true;
}

bool InitWorldDir(const std::string& dir, const ServerConfig& c, std::string& error)
{
    namespace fs = std::filesystem;
    if (dir.empty())
    {
        error = "no folder given";
        return false;
    }
    const std::string base = WithSlash(dir);
    const std::string configPath = base + SERVER_CONFIG_FILE;
    std::error_code   ec;
    if (fs::exists(configPath, ec))
    {
        error = configPath + " is already there; it has not been changed";
        return false;
    }
    fs::create_directories(base, ec);
    if (ec)
    {
        error = base + ": " + ec.message();
        return false;
    }
    ServerConfig written = c;
    if (written.registration == Registration::Auto)
        written.registration = Registration::Allowlist;
    if (!Save::WriteFileAtomic(configPath, ServerConfigText(written), 0))
    {
        error = configPath + " could not be written";
        return false;
    }
    const std::string allow = base + "allowlist.txt";
    if (!fs::exists(allow, ec))
    {
        std::ofstream out(allow, std::ios::binary);
        out << "# Who may play here: one account name per line. Anything after the name, and\n"
               "# a line starting with #, is ignored. The server reads this file again when it\n"
               "# changes; the console's `allow <name>` and `deny <name>` edit it too.\n";
        if (!out)
        {
            error = allow + " could not be written";
            return false;
        }
    }
    return true;
}

}  // namespace Host
