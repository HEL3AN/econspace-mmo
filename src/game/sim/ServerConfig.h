#pragma once

#include "gen/Galaxy.h"

#include <cstdint>
#include <string>
#include <vector>

// A server's settings and the folder its world lives in (#400).
//
// A world is one directory: world.json, regions/, accounts/, designs.json, corps.json,
// allowlist.txt, bans.txt, server.json, logs/ and backups/. `econserver init <dir>` makes one
// and writes a commented server.json; `econserver host --world <dir>` runs it. Flags given to
// `host` override the file for that run and are never written back to it.
//
// Without --world the world is the executable's own folder, as it always was, so a dev build
// and an old install keep running on the files they have. Nothing moves between the two on
// its own: a world beside the executable stays there until the owner copies it.
namespace Host
{

// What registration the server runs with. Auto is what the server did before server.json:
// the allowlist is checked when the server is public or allowlist.txt exists.
enum class Registration
{
    Auto,
    Allowlist,  // only the names in allowlist.txt may log in or create an account
    Open,       // anyone may create an account
};

struct ServerConfig
{
    int          port = 50800;
    bool         isPublic = false;  // reachable from other machines, not only this one
    Registration registration = Registration::Auto;
    int          centres =
        Gen::DEFAULT_CENTRES;  // a new world's civilisation centres; a saved one keeps its own
    bool     randomStarts = false;
    int      maxRegions = 0;  // the cap; 0: what the world ran with, or the default
    int      maxSystems = 0;
    uint64_t seed = 0;  // a new world's seed; 0: chosen at random
    bool     dev = false;
    int      backupsKept = 10;  // copies `backup` keeps in backups/; 0 keeps every one
};

// The file's name inside a world directory.
extern const char* const SERVER_CONFIG_FILE;

// `dir` with a separator at its end (`dir` itself if it has one, or is empty).
std::string WithSlash(const std::string& dir);
// The world directory a world.json is in, ending in a separator.
std::string WorldDirOf(const std::string& worldPath);

// server.json's text read into `out`, over what `out` already holds. Strict like the
// archetypes (#191): an unknown key or a value of the wrong kind is an error naming it, since
// a misspelt "pubilc": true would otherwise run a server nobody can reach. Comments (`//`)
// are allowed -- the file init writes is commented.
bool ParseServerConfig(const std::string& text, ServerConfig& out, std::string& error);
// The same from a file. A missing file is not an error: `exists` is false and `out` is
// left as it was.
bool LoadServerConfig(const std::string& path, ServerConfig& out, bool& exists, std::string& error);
// The commented server.json init writes for `c`.
std::string ServerConfigText(const ServerConfig& c);

// What `host` and `init` were told on the command line, from `args` (everything after the
// mode; --data is already gone). Values go into `cfg`, over what server.json said; `world`
// gets --world's, `packs` --packs'. `host <port>` -- a number first -- is the port, as it was.
// An unknown flag is an error: a typo here is a setting silently not made.
bool ApplyHostFlags(const std::vector<std::string>& args, ServerConfig& cfg, std::string& world,
                    std::string& packs, std::string& error);

// `econserver init <dir>`: the directory (made if missing), a server.json for `c` and an
// allowlist.txt that says how it is used. A server.json already there is never written over;
// that is an error, and nothing is changed.
bool InitWorldDir(const std::string& dir, const ServerConfig& c, std::string& error);

}  // namespace Host
