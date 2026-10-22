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
                                      "allowlist.txt", "bans.txt",     "names_blocked.txt",
                                      nullptr };

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
