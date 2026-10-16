#include "sim/Chat.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>

namespace Chat
{

namespace
{

// The next code point of `s` from `i`, which moves past it; -1 for a byte that does not
// begin a valid sequence, which moves past that byte alone. Strict: an overlong form, a
// surrogate or anything past U+10FFFF is not a character, because a lenient decoder is how a
// filtered character gets through spelt another way.
long NextCodePoint(std::string_view s, size_t& i)
{
    const unsigned char b0 = (unsigned char)s[i];
    if (b0 < 0x80)
    {
        i++;
        return b0;
    }
    int  len = 0;
    long cp = 0;
    long min = 0;
    if ((b0 & 0xE0) == 0xC0)
        len = 2, cp = b0 & 0x1F, min = 0x80;
    else if ((b0 & 0xF0) == 0xE0)
        len = 3, cp = b0 & 0x0F, min = 0x800;
    else if ((b0 & 0xF8) == 0xF0)
        len = 4, cp = b0 & 0x07, min = 0x10000;
    else
    {
        i++;
        return -1;
    }
    if (i + (size_t)len > s.size())
    {
        i++;
        return -1;
    }
    for (int k = 1; k < len; k++)
    {
        const unsigned char b = (unsigned char)s[i + (size_t)k];
        if ((b & 0xC0) != 0x80)
        {
            i++;
            return -1;
        }
        cp = (cp << 6) | (b & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
    {
        i++;
        return -1;
    }
    i += (size_t)len;
    return cp;
}

void AppendUtf8(std::string& out, long cp)
{
    if (cp < 0x80)
        out += (char)cp;
    else if (cp < 0x800)
    {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    }
    else if (cp < 0x10000)
    {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
    else
    {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

// Characters that are whitespace to a reader: a tab, a line break, the separators.
bool Spacing(long cp)
{
    return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == 0x0B || cp == 0x0C ||
           cp == 0x85 || cp == 0xA0 || cp == 0x2028 || cp == 0x2029 || cp == 0x3000;
}

// Characters nobody should be able to put on someone else's screen: the C0 and C1 controls,
// DEL, the marks and overrides that reverse the direction of what follows, and the
// invisible ones that make two names look the same.
bool Hidden(long cp)
{
    return cp < 0x20 || (cp >= 0x7F && cp <= 0x9F) || cp == 0xAD ||
           (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x202A && cp <= 0x202E) ||
           (cp >= 0x2060 && cp <= 0x206F) || cp == 0xFEFF || (cp >= 0xFFF9 && cp <= 0xFFFB);
}

std::string Lower(std::string_view s)
{
    std::string out(s);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
    return out;
}

}  // namespace

std::string Sanitize(std::string_view raw, size_t maxChars)
{
    std::string out;
    size_t      chars = 0;
    bool        pendingSpace = false;
    for (size_t i = 0; i < raw.size() && chars < maxChars;)
    {
        const long cp = NextCodePoint(raw, i);
        if (cp < 0)
            continue;
        if (Spacing(cp))
        {
            pendingSpace = !out.empty();  // none at the start: the line is trimmed
            continue;
        }
        if (Hidden(cp))
            continue;
        if (pendingSpace)
        {
            if (chars + 1 >= maxChars)
                break;
            out += ' ';
            chars++;
            pendingSpace = false;
        }
        AppendUtf8(out, cp);
        chars++;
    }
    return out;
}

size_t CountChars(std::string_view utf8)
{
    size_t n = 0;
    for (size_t i = 0; i < utf8.size();)
        if (NextCodePoint(utf8, i) >= 0)
            n++;
    return n;
}

Delivery Hub::Notice(const Pilot& to, std::string text, double now)
{
    Delivery d;
    d.to = to.id;
    d.line.channel = NOTICE;
    d.line.text = std::move(text);
    d.line.at = (long long)now;
    return d;
}

bool Hub::Spend(const std::string& name, double now, double& wait)
{
    auto it = buckets_.find(name);
    if (it == buckets_.end())
        it = buckets_.emplace(name, Bucket{ (double)limits_.burst, now }).first;
    Bucket& b = it->second;
    if (now > b.last)
        b.tokens = std::min((double)limits_.burst, b.tokens + (now - b.last) / limits_.refill);
    b.last = std::max(b.last, now);
    if (b.tokens >= 1.0)
    {
        b.tokens -= 1.0;
        return true;
    }
    wait = (1.0 - b.tokens) * limits_.refill;
    return false;
}

std::vector<Delivery> Hub::Say(const Pilot& from, const Proto::ChatSay& say,
                               const std::vector<Pilot>& online, double now)
{
    std::vector<Delivery> out;
    const std::string     text = Sanitize(say.text);
    if (text.empty())
        return out;
    if (Muted(from.name, now))
    {
        const auto it = muted_.find(from.name);
        out.push_back(Notice(from,
                             it->second > 0.0
                                 ? "You are muted for " +
                                       std::to_string((int)std::ceil((it->second - now) / 60.0)) +
                                       " min more; nobody heard that"
                                 : "You are muted by the server's operator; nobody heard that",
                             now));
        return out;
    }

    Proto::ChatLine line;
    line.from = from.name;
    line.text = text;
    line.at = (long long)now;
    const std::string to = Lower(say.to);
    const Pilot*      recipient = nullptr;
    if (!to.empty())
    {
        for (const Pilot& p : online)
            if (p.name == to)
                recipient = &p;
        // Checked before the line is spent: a name mistyped is not a line said.
        if (recipient == nullptr)
        {
            out.push_back(
                Notice(from, "Nobody called " + Sanitize(say.to, 32) + " is online", now));
            return out;
        }
    }
    double wait = 0.0;
    if (!Spend(from.name, now, wait))
    {
        out.push_back(Notice(
            from, "You are talking too fast; wait " + std::to_string((int)std::ceil(wait)) + " s",
            now));
        return out;
    }

    if (recipient != nullptr)
    {
        line.channel = PRIVATE;
        line.to = recipient->name;
        out.push_back({ recipient->id, line });
        if (recipient->id != from.id)
            out.push_back({ from.id, line });
        return out;
    }
    line.channel = LOCAL;
    line.system = from.system;
    for (const Pilot& p : online)
        if (p.system == from.system)
            out.push_back({ p.id, line });
    std::vector<Proto::ChatLine>& kept = history_[from.system];
    kept.push_back(line);
    if ((int)kept.size() > limits_.history)
        kept.erase(kept.begin(), kept.begin() + (long)(kept.size() - (size_t)limits_.history));
    return out;
}

std::vector<Delivery> Hub::Broadcast(std::string_view text, const std::vector<Pilot>& online,
                                     double now)
{
    std::vector<Delivery> out;
    Proto::ChatLine       line;
    line.channel = SERVER;
    line.text = Sanitize(text);
    line.at = (long long)now;
    if (line.text.empty())
        return out;
    for (const Pilot& p : online)
        out.push_back({ p.id, line });
    return out;
}

std::vector<Proto::ChatLine> Hub::History(const std::string& system, double now) const
{
    std::vector<Proto::ChatLine> out;
    const auto                   it = history_.find(system);
    if (it == history_.end())
        return out;
    for (const Proto::ChatLine& l : it->second)
        if ((double)l.at >= now - limits_.historyAge)
        {
            out.push_back(l);
            out.back().history = true;
        }
    return out;
}

std::vector<Presence> Hub::Track(const std::vector<Pilot>& online, double now)
{
    std::map<std::string, std::vector<std::string>> members;
    std::map<int, std::string>                      where;
    for (const Pilot& p : online)
    {
        members[p.system].push_back(p.name);
        where[p.id] = p.system;
    }
    for (auto& kv : members)
        std::sort(kv.second.begin(), kv.second.end());

    std::set<std::string> changed;  // systems whose members are not what they were
    for (const auto& kv : members)
    {
        const auto it = members_.find(kv.first);
        if (it == members_.end() || it->second != kv.second)
            changed.insert(kv.first);
    }

    std::vector<Presence> out;
    for (const Pilot& p : online)
    {
        const auto was = where_.find(p.id);
        const bool arrived = was == where_.end() || was->second != p.system;
        if (!arrived && changed.count(p.system) == 0)
            continue;
        Presence pr;
        pr.to = p.id;
        pr.members.system = p.system;
        pr.members.pilots = members[p.system];
        if (arrived)
            pr.history = History(p.system, now);
        out.push_back(std::move(pr));
    }
    members_ = std::move(members);
    where_ = std::move(where);
    return out;
}

void Hub::Mute(const std::string& name, double until)
{
    muted_[Lower(name)] = until;
}

bool Hub::Unmute(const std::string& name)
{
    return muted_.erase(Lower(name)) > 0;
}

bool Hub::Muted(const std::string& name, double now) const
{
    const auto it = muted_.find(Lower(name));
    return it != muted_.end() && (it->second <= 0.0 || now < it->second);
}

void Hub::Expire(double now)
{
    // A bucket full again is the same as no bucket.
    for (auto it = buckets_.begin(); it != buckets_.end();)
        if (now - it->second.last >= limits_.refill * limits_.burst)
            it = buckets_.erase(it);
        else
            ++it;
    for (auto it = muted_.begin(); it != muted_.end();)
        if (it->second > 0.0 && now >= it->second)
            it = muted_.erase(it);
        else
            ++it;
    for (auto it = history_.begin(); it != history_.end();)
    {
        std::vector<Proto::ChatLine>& v = it->second;
        v.erase(std::remove_if(v.begin(), v.end(), [&](const Proto::ChatLine& l)
                               { return (double)l.at < now - limits_.historyAge; }),
                v.end());
        if (v.empty())
            it = history_.erase(it);
        else
            ++it;
    }
}

}  // namespace Chat
