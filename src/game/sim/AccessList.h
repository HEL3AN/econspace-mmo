#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

// Who may play on this server (#400): an allowlist and bans, as two text files in the
// world's folder beside world.json, so the owner can read and edit them with anything.
//
//   allowlist.txt   one account per line: the only names that may log in, or create an
//                   account. `#` starts a comment; anything after the name is ignored.
//   bans.txt        one account per line, then the reason, which the player is told.
//
// Whether the list is checked is decided once, when the server starts: yes if the server is
// reachable from other machines (--public) or allowlist.txt is there, unless --open says
// registration is open. A public server with no list therefore lets nobody in and says so,
// rather than letting anybody in -- the open registration path is the one that sends a
// password-equivalent in clear (#351). Bans are checked either way.
//
// The files are read again when they change on disk, and the console's allow, deny, ban
// and unban edit them line by line, so a comment the owner wrote stays where it was.
namespace Host
{

class AccessList
{
public:
    // Whether a server started this way checks its allowlist; see above.
    static bool ListRequired(bool isPublic, bool open, bool allowlistExists);

    // `dir` is the world's folder, ending in a separator (or empty: the working directory).
    // Reads both files; a missing file is an empty list.
    void Open(const std::string& dir, bool listRequired);

    const std::string& AllowlistPath() const { return allowPath_; }
    const std::string& BansPath() const { return banPath_; }
    bool               ListChecked() const { return listRequired_; }

    // Why the account (already canonical) may not come in -- what its Bye says -- or empty.
    std::string Refusal(const std::string& account) const;

    // Reads the files again if either changed on disk since they were last read. True if
    // they were read. `Reload` reads them regardless.
    bool ReloadIfChanged();
    void Reload();

    // The console's verbs. Each returns what to tell the operator; `ok` says whether
    // anything changed. A name that is not a name changes nothing.
    std::string Allow(const std::string& name, bool& ok);
    std::string Deny(const std::string& name, bool& ok);
    std::string Ban(const std::string& name, const std::string& reason, bool& ok);
    std::string Unban(const std::string& name, bool& ok);

    // Changes every time who may come in may have changed: read again, or edited. The host
    // compares it to the last it saw and removes whoever is now refused.
    int Generation() const { return generation_; }

    const std::set<std::string>&              Allowed() const { return allowed_; }
    const std::map<std::string, std::string>& Bans() const { return bans_; }
    // Lines of the files that were not a name, as "file:line: text", from the last read.
    const std::vector<std::string>& Problems() const { return problems_; }

private:
    struct Stamp
    {
        bool     exists = false;
        int64_t  time = 0;
        uint64_t size = 0;
        bool     operator!=(const Stamp& o) const
        {
            return exists != o.exists || time != o.time || size != o.size;
        }
    };
    static Stamp StampOf(const std::string& path);
    // Rewrites `path` without the lines naming `canonical`; false if there were none.
    bool RemoveLines(const std::string& path, const std::string& canonical, std::string& error);
    bool AppendLine(const std::string& path, const std::string& line, std::string& error);

    std::string                        allowPath_;
    std::string                        banPath_;
    bool                               listRequired_ = false;
    std::set<std::string>              allowed_;
    std::map<std::string, std::string> bans_;  // account -> reason
    std::vector<std::string>           problems_;
    Stamp                              allowStamp_;
    Stamp                              banStamp_;
    int                                generation_ = 0;
};

}  // namespace Host
