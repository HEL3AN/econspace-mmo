#include "sim/AccessList.h"

#include "sim/HostRules.h"
#include "sim/SaveFile.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>

namespace Host
{
namespace
{

std::string Trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a]))
        a++;
    while (b > a && std::isspace((unsigned char)s[b - 1]))
        b--;
    return s.substr(a, b - a);
}

// One line of either file: the name it is about, typed as it was, and what follows it. A
// line that is blank or only a comment has no name.
void SplitLine(const std::string& raw, std::string& name, std::string& rest)
{
    std::string  line = raw;
    const size_t hash = line.find('#');
    if (hash != std::string::npos)
        line.erase(hash);
    line = Trim(line);
    size_t end = 0;
    while (end < line.size() && !std::isspace((unsigned char)line[end]))
        end++;
    name = line.substr(0, end);
    rest = Trim(line.substr(end));
}

bool ReadText(const std::string& path, std::string& text)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

std::vector<std::string> Lines(const std::string& text)
{
    std::vector<std::string> out;
    std::istringstream       in(text);
    for (std::string line; std::getline(in, line);)
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        out.push_back(line);
    }
    return out;
}

}  // namespace

bool AccessList::ListRequired(bool isPublic, bool open, bool allowlistExists)
{
    if (open)
        return false;
    return isPublic || allowlistExists;
}

void AccessList::Open(const std::string& dir, bool listRequired)
{
    allowPath_ = dir + "allowlist.txt";
    banPath_ = dir + "bans.txt";
    listRequired_ = listRequired;
    Reload();
}

std::string AccessList::Refusal(const std::string& account) const
{
    const auto ban = bans_.find(account);
    if (ban != bans_.end())
        return ban->second.empty() ? "You are banned from this server"
                                   : "You are banned from this server: " + ban->second;
    if (listRequired_ && allowed_.count(account) == 0)
        return "This server is by invitation and " + account +
               " is not on its list; ask its operator to add you";
    return std::string();
}

AccessList::Stamp AccessList::StampOf(const std::string& path)
{
    namespace fs = std::filesystem;
    Stamp           s;
    std::error_code ec;
    if (!fs::exists(path, ec) || ec)
        return s;
    s.exists = true;
    const auto t = fs::last_write_time(path, ec);
    if (!ec)
        s.time = (int64_t)t.time_since_epoch().count();
    const auto size = fs::file_size(path, ec);
    if (!ec)
        s.size = (uint64_t)size;
    return s;
}

void AccessList::Reload()
{
    allowStamp_ = StampOf(allowPath_);
    banStamp_ = StampOf(banPath_);
    allowed_.clear();
    bans_.clear();
    problems_.clear();

    const auto readFile = [this](const std::string& path, bool isBans)
    {
        std::string text;
        if (!ReadText(path, text))
            return;
        const std::vector<std::string> lines = Lines(text);
        for (size_t i = 0; i < lines.size(); i++)
        {
            std::string typed, rest;
            SplitLine(lines[i], typed, rest);
            if (typed.empty())
                continue;
            const std::string name = CanonicalAccount(typed);
            if (name.empty())
            {
                // Said, not skipped in silence: a misspelt line is a friend who cannot get in.
                problems_.push_back(path + ":" + std::to_string(i + 1) + ": \"" + typed +
                                    "\" is not an account name");
                continue;
            }
            if (isBans)
                bans_[name] = rest;
            else
                allowed_.insert(name);
        }
    };
    readFile(allowPath_, false);
    readFile(banPath_, true);
    generation_++;
}

bool AccessList::ReloadIfChanged()
{
    if (StampOf(allowPath_) != allowStamp_ || StampOf(banPath_) != banStamp_)
    {
        Reload();
        return true;
    }
    return false;
}

bool AccessList::RemoveLines(const std::string& path, const std::string& canonical,
                             std::string& error)
{
    std::string text;
    if (!ReadText(path, text))
        return false;
    std::string out;
    bool        removed = false;
    for (const std::string& line : Lines(text))
    {
        std::string typed, rest;
        SplitLine(line, typed, rest);
        if (!typed.empty() && CanonicalAccount(typed) == canonical)
        {
            removed = true;
            continue;
        }
        out += line;
        out += '\n';
    }
    if (removed && !Save::WriteFileAtomic(path, out, 0))
    {
        error = path + " could not be written";
        return false;
    }
    return removed;
}

bool AccessList::AppendLine(const std::string& path, const std::string& line, std::string& error)
{
    std::string text;
    ReadText(path, text);  // none yet is an empty list
    if (!text.empty() && text.back() != '\n')
        text += '\n';
    text += line;
    text += '\n';
    if (!Save::WriteFileAtomic(path, text, 0))
    {
        error = path + " could not be written";
        return false;
    }
    return true;
}

std::string AccessList::Allow(const std::string& typed, bool& ok)
{
    ok = false;
    const std::string name = CanonicalAccount(typed);
    if (name.empty())
        return "\"" + typed + "\" is not an account name";
    if (allowed_.count(name) != 0)
        return name + " is already on the allowlist";
    std::string error;
    if (!AppendLine(allowPath_, name, error))
        return error;
    Reload();
    ok = true;
    std::string said = name + " added to " + allowPath_;
    if (bans_.count(name) != 0)
        said += " (but banned: unban to let them in)";
    else if (!listRequired_)
        said += " (registration is open on this server, so the list is not checked)";
    return said;
}

std::string AccessList::Deny(const std::string& typed, bool& ok)
{
    ok = false;
    const std::string name = CanonicalAccount(typed);
    if (name.empty())
        return "\"" + typed + "\" is not an account name";
    std::string error;
    if (!RemoveLines(allowPath_, name, error))
        return error.empty() ? name + " is not on the allowlist" : error;
    Reload();
    ok = true;
    return name + " removed from " + allowPath_ +
           (listRequired_ ? ""
                          : " (registration is open on this server, so this keeps nobody out)");
}

std::string AccessList::Ban(const std::string& typed, const std::string& reason, bool& ok)
{
    ok = false;
    const std::string name = CanonicalAccount(typed);
    if (name.empty())
        return "\"" + typed + "\" is not an account name";
    // A second ban replaces the first's reason rather than adding a line.
    std::string error;
    RemoveLines(banPath_, name, error);
    if (!error.empty())
        return error;
    // The reason is one line, and a '#' in it would start a comment.
    std::string why = Trim(reason);
    for (char& c : why)
        if (c == '\n' || c == '\r' || c == '#')
            c = ' ';
    if (!AppendLine(banPath_, why.empty() ? name : name + " " + why, error))
        return error;
    Reload();
    ok = true;
    return name + " banned" + (why.empty() ? std::string() : " (" + why + ")");
}

std::string AccessList::Unban(const std::string& typed, bool& ok)
{
    ok = false;
    const std::string name = CanonicalAccount(typed);
    if (name.empty())
        return "\"" + typed + "\" is not an account name";
    std::string error;
    if (!RemoveLines(banPath_, name, error))
        return error.empty() ? name + " is not banned" : error;
    Reload();
    ok = true;
    return name + " unbanned";
}

}  // namespace Host
