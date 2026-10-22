#include "core/AdminChannel.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <system_error>
#include <thread>

namespace fs = std::filesystem;

namespace Host
{

namespace
{

// The world's folder as a path: "." when none is named.
fs::path WorldFolder(const std::string& worldDir)
{
    return fs::path(worldDir.empty() ? std::string(".") : worldDir);
}

std::string ReadAll(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Written beside and renamed into place, so the other side never reads half of it.
bool WriteWhole(const fs::path& p, const std::string& text)
{
    const fs::path tmp = fs::path(p.generic_string() + ".tmp");
    {
        std::ofstream out(tmp, std::ios::binary);
        out << text;
        if (!out)
            return false;
    }
    std::error_code ec;
    fs::rename(tmp, p, ec);
    return !ec;
}

std::string NewRequestId()
{
    // Milliseconds first, zero-padded, so the names sort in the order the commands were given.
    const long long    ms = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
    std::random_device rd;
    char               buf[48];
    std::snprintf(buf, sizeof buf, "%015lld-%08x", ms, (unsigned)rd());
    return buf;
}

}  // namespace

bool AdminChannel::Open(const std::string& worldDir, std::string& error)
{
    const fs::path  dir = WorldFolder(worldDir) / "admin";
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec)
    {
        error = dir.generic_string() + ": " + ec.message();
        return false;
    }
#if !defined(_WIN32)
    fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace, ec);
#endif
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
    {
        std::error_code rm;
        fs::remove(it->path(), rm);
    }
    dir_ = dir.generic_string() + "/";
    return true;
}

std::vector<AdminChannel::Request> AdminChannel::Take()
{
    std::vector<Request> out;
    if (dir_.empty())
        return out;
    std::error_code ec;
    for (fs::directory_iterator it(dir_, ec), end; !ec && it != end; it.increment(ec))
        if (it->path().extension() == ".cmd")
        {
            Request r;
            r.id = it->path().stem().string();
            r.command = ReadAll(it->path());
            // Taken only if this side removed it: `admin` withdraws a command nobody took,
            // and the one that removes the file is the one it belongs to.
            std::error_code rm;
            if (!fs::remove(it->path(), rm) || rm)
                continue;
            while (!r.command.empty() && (r.command.back() == '\n' || r.command.back() == '\r'))
                r.command.pop_back();
            out.push_back(r);
        }
    std::sort(out.begin(), out.end(),
              [](const Request& a, const Request& b) { return a.id < b.id; });
    return out;
}

void AdminChannel::Answer(const std::string& id, const std::string& text)
{
    if (!dir_.empty())
        WriteWhole(fs::path(dir_) / (id + ".out"), text);
}

bool AdminRequest(const std::string& worldDir, const std::string& command, std::string& answer,
                  double pickupSeconds, double answerSeconds)
{
    using clock = std::chrono::steady_clock;
    const fs::path  dir = WorldFolder(worldDir) / "admin";
    std::error_code ec;
    if (!fs::is_directory(dir, ec))
    {
        answer = "No server has run in " + worldDir + " (there is no admin/ folder in it).";
        return false;
    }
    const std::string id = NewRequestId();
    const fs::path    cmd = dir / (id + ".cmd");
    const fs::path    out = dir / (id + ".out");
    if (!WriteWhole(cmd, command + "\n"))
    {
        answer = "Cannot write to " + dir.generic_string() +
                 " -- run this as the user the server runs as.";
        return false;
    }
    const auto start = clock::now();
    const auto since = [&]()
    { return std::chrono::duration<double>(clock::now() - start).count(); };
    bool taken = false;
    while (true)
    {
        if (fs::exists(out, ec))
        {
            // Renamed into place whole, so what is there is all of it.
            answer = ReadAll(out);
            fs::remove(out, ec);
            return true;
        }
        if (!taken && !fs::exists(cmd, ec))
            taken = true;
        if (!taken && since() > pickupSeconds)
        {
            // Withdrawn -- unless the server took it in this very moment, in which case the
            // answer is coming after all.
            if (fs::remove(cmd, ec) && !ec)
            {
                answer = "No server took the command within " + std::to_string((int)pickupSeconds) +
                         " s: is it running on " + worldDir + "?";
                return false;
            }
            taken = true;
        }
        if (taken && since() > pickupSeconds + answerSeconds)
        {
            answer = "The server took the command and has not answered; see its log.";
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

}  // namespace Host
