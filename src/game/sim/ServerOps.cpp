#include "sim/ServerOps.h"

#include "sim/ServerConfig.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <system_error>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <signal.h>
#endif

namespace fs = std::filesystem;

namespace Host
{

// --- Backups ---------------------------------------------------------------------------------

const char* const WORLD_ENTRIES[] = { "server.json",   "world.json",   "regions",
                                      "accounts",      "designs.json", "corps.json",
                                      "allowlist.txt", "bans.txt",     nullptr };

std::string BackupName(const std::string& isoStamp)
{
    // "2026-10-17T09:41:07.123Z" -> "2026-10-17T09-41-07Z": seconds are enough to tell two
    // backups apart, and a second one in the same second is numbered.
    std::string s = isoStamp.substr(0, std::min<size_t>(isoStamp.size(), 19));
    std::replace(s.begin(), s.end(), ':', '-');
    return s + "Z";
}

namespace
{

bool IsUnfinished(const fs::path& p)
{
    return p.extension() == ".tmp";
}

bool CopyEntry(const fs::path& from, const fs::path& to, std::string& error)
{
    std::error_code ec;
    if (fs::is_directory(from, ec))
    {
        fs::create_directories(to, ec);
        for (fs::recursive_directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec))
        {
            const fs::path rel = fs::relative(it->path(), from, ec);
            if (it->is_directory(ec))
                fs::create_directories(to / rel, ec);
            else if (!IsUnfinished(it->path()))
                fs::copy_file(it->path(), to / rel, fs::copy_options::overwrite_existing, ec);
            if (ec)
                break;
        }
    }
    else
        fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    if (ec)
    {
        error = from.generic_string() + ": " + ec.message();
        return false;
    }
    return true;
}

}  // namespace

bool BackupWorld(const std::string& worldDir, const std::string& name, int keep, std::string& made,
                 std::string& error)
{
    const fs::path  root = worldDir.empty() ? fs::path(".") : fs::path(worldDir);
    const fs::path  backups = root / "backups";
    std::error_code ec;
    fs::create_directories(backups, ec);
    if (ec)
    {
        error = backups.generic_string() + ": " + ec.message();
        return false;
    }
    fs::path target = backups / name;
    for (int n = 2; fs::exists(target, ec); n++)
        target = backups / (name + "-" + std::to_string(n));
    // Copied under another name and renamed when whole, so a backup cut short by a full disk
    // or a power cut never looks like a complete one.
    const fs::path partial = fs::path(target.generic_string() + ".partial");
    fs::remove_all(partial, ec);
    fs::create_directories(partial, ec);
    int copied = 0;
    for (int i = 0; WORLD_ENTRIES[i] != nullptr; i++)
    {
        const fs::path from = root / WORLD_ENTRIES[i];
        if (!fs::exists(from, ec))
            continue;
        if (!CopyEntry(from, partial / WORLD_ENTRIES[i], error))
        {
            fs::remove_all(partial, ec);
            return false;
        }
        copied++;
    }
    if (copied == 0)
    {
        fs::remove_all(partial, ec);
        error = "there is no world in " + root.generic_string();
        return false;
    }
    fs::rename(partial, target, ec);
    if (ec)
    {
        error = target.generic_string() + ": " + ec.message();
        fs::remove_all(partial, ec);
        return false;
    }
    made = target.generic_string();

    // The oldest go once there are more than `keep`. Only folders named like a backup: the
    // owner may keep something else in backups/ and it is not this server's to remove.
    if (keep > 0)
    {
        std::vector<fs::path> all;
        for (fs::directory_iterator it(backups, ec), end; !ec && it != end; it.increment(ec))
        {
            const std::string n = it->path().filename().string();
            if (it->is_directory(ec) && n.size() >= 20 && n[4] == '-' && n[10] == 'T' &&
                n.find(".partial") == std::string::npos)
                all.push_back(it->path());
        }
        std::sort(all.begin(), all.end());
        for (size_t i = 0; i + (size_t)keep < all.size(); i++)
            fs::remove_all(all[i], ec);
    }
    return true;
}

// --- The admin channel -----------------------------------------------------------------------

namespace
{

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
    const fs::path  dir = fs::path(WithSlash(worldDir.empty() ? "." : worldDir)) / "admin";
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
    const fs::path  dir = fs::path(WithSlash(worldDir.empty() ? "." : worldDir)) / "admin";
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

// --- Stopping ------------------------------------------------------------------------------

namespace
{

volatile std::sig_atomic_t g_stop = 0;
std::atomic<bool>          g_stopped{ false };

#if defined(_WIN32)
BOOL WINAPI OnConsoleEvent(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT)
    {
        // The second one is the default: the process ends now.
        if (g_stop != 0)
            return FALSE;
        g_stop = 1;
        return TRUE;
    }
    // The window is closing (or the user logging off): Windows ends the process when this
    // returns, and after about five seconds whether it has or not. Wait for the save.
    g_stop = 1;
    for (int i = 0; i < 90 && !g_stopped.load(); i++)
        Sleep(50);
    return TRUE;
}
#else
extern "C" void OnStopSignal(int)
{
    g_stop = 1;
}
#endif

}  // namespace

void InstallStopHandlers()
{
#if defined(_WIN32)
    SetConsoleCtrlHandler(OnConsoleEvent, TRUE);
    std::signal(SIGTERM, [](int) { g_stop = 1; });
#else
    struct sigaction sa = {};
    sa.sa_handler = OnStopSignal;
    sigemptyset(&sa.sa_mask);
    // Once: a second Ctrl+C while the server saves is someone who wants it gone now.
    sa.sa_flags = SA_RESETHAND;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGHUP, &sa, nullptr);
#endif
}

bool StopRequested()
{
    return g_stop != 0;
}

void RequestStop()
{
    g_stop = 1;
}

void ClearStop()
{
    g_stop = 0;
    g_stopped = false;
}

void MarkStopped()
{
    g_stopped = true;
}

}  // namespace Host
