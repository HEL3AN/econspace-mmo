#include "core/DataDir.h"

#include "raylib.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
std::string g_path = "data/";

std::string WithSlash(std::string dir)
{
    if (!dir.empty() && dir.back() != '/' && dir.back() != '\\')
        dir += '/';
    return dir;
}

// A folder is data/ when it has the file every binary reads first.
bool IsDataDir(const std::string& dir)
{
    return !dir.empty() && FileExists((dir + "factions.json").c_str());
}

// For comparing paths: one kind of slash, and on Windows one case.
std::string Normal(std::string p)
{
    for (char& c : p)
    {
        if (c == '\\')
            c = '/';
#ifdef _WIN32
        c = (char)std::tolower((unsigned char)c);
#endif
    }
    return WithSlash(p);
}

// Whether the executable is still where the build put it. Only then does the source tree's
// data/ win over the copy beside it -- a copy that is a build behind the editor's edits.
bool InBuildTree(const std::string& exeDir)
{
#if defined(ECON_DEV_DATA_DIR) && defined(ECON_BUILD_DIR)
    const std::string build = Normal(ECON_BUILD_DIR);
    return Normal(exeDir).compare(0, build.size(), build) == 0;
#else
    (void)exeDir;
    return false;
#endif
}

std::string DevDir()
{
#ifdef ECON_DEV_DATA_DIR
    return ECON_DEV_DATA_DIR;
#else
    return std::string();
#endif
}
}  // namespace

std::string DataDir::Resolve(int& argc, char** argv, const char* who)
{
    std::string chosen, why;
    // --data <dir> is taken out of argv, so every mode's positional parsing stays as it was.
    for (int i = 1; i < argc; i++)
    {
        if (std::strcmp(argv[i], "--data") != 0)
            continue;
        if (i + 1 >= argc)
        {
            std::fprintf(stderr, "%s: --data needs a folder\n", who);
            argc = i;
            break;
        }
        chosen = WithSlash(argv[i + 1]);
        why = "--data";
        for (int j = i; j + 2 <= argc; j++)
            argv[j] = argv[j + 2];
        argc -= 2;
        break;
    }
    if (!chosen.empty())
    {
        if (!IsDataDir(chosen))
            std::fprintf(stderr, "%s: %s has no factions.json -- is it a data folder?\n", who,
                         chosen.c_str());
    }
    else if (const char* env = std::getenv("ECONSPACE_DATA"); env && *env)
    {
        chosen = WithSlash(env);
        why = "ECONSPACE_DATA";
    }
    else
    {
        const std::string exe = WithSlash(GetApplicationDirectory());
        const std::string dev = DevDir();
        const struct
        {
            bool        usable;
            std::string dir;
            const char* why;
        } candidates[] = {
            { InBuildTree(exe), dev, "the source tree, dev build" },
            { true, exe + "data/", "beside the executable" },
            { true, exe + "../data/", "beside the executable's folder" },
            { true, dev, "the source tree, dev fallback" },
        };
        for (const auto& c : candidates)
            if (c.usable && IsDataDir(c.dir))
            {
                chosen = c.dir;
                why = c.why;
                break;
            }
    }
    if (chosen.empty())
    {
        std::fprintf(stderr,
                     "%s: no data/ folder found beside the executable or one level up; "
                     "pass --data <dir>\n",
                     who);
        return chosen;
    }
    std::fprintf(stderr, "%s: data from %s (%s)\n", who, chosen.c_str(), why.c_str());
    g_path = chosen;
    return chosen;
}

const std::string& DataDir::Path()
{
    return g_path;
}
