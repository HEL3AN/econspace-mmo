#include "core/ArchetypeEdit.h"

#include <cstddef>
#include <vector>

namespace ArchetypeEdit
{
namespace
{
const size_t NPOS = std::string::npos;

size_t SkipWs(const std::string& t, size_t i)
{
    while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\n' || t[i] == '\r'))
        i++;
    return i;
}

// One index past the closing quote of the string starting at t[i] == '"'.
size_t SkipString(const std::string& t, size_t i)
{
    if (i >= t.size() || t[i] != '"')
        return NPOS;
    for (i++; i < t.size(); i++)
    {
        if (t[i] == '\\')
        {
            i++;  // whatever follows a backslash is not a delimiter
            continue;
        }
        if (t[i] == '"')
            return i + 1;
    }
    return NPOS;
}

// One index past the value starting at i: a string, an array, an object, or a bare token
// (number, true, false, null) which ends at the first delimiter.
size_t SkipValue(const std::string& t, size_t i)
{
    i = SkipWs(t, i);
    if (i >= t.size())
        return NPOS;
    if (t[i] == '"')
        return SkipString(t, i);
    if (t[i] == '{' || t[i] == '[')
    {
        int depth = 0;
        for (; i < t.size(); i++)
        {
            if (t[i] == '"')
            {
                size_t e = SkipString(t, i);
                if (e == NPOS)
                    return NPOS;
                i = e - 1;
                continue;
            }
            if (t[i] == '{' || t[i] == '[')
                depth++;
            else if (t[i] == '}' || t[i] == ']')
            {
                depth--;
                if (depth == 0)
                    return i + 1;
            }
        }
        return NPOS;
    }
    while (i < t.size() && t[i] != ',' && t[i] != '}' && t[i] != ']' && t[i] != '\n')
        i++;
    return i;
}

// The object that carries `"id": "<id>"`, as [begin, end) over the whole file. Found with
// one forward pass keeping the positions of the open braces, because scanning backwards
// for the enclosing brace cannot tell a brace in a string from a real one.
bool FindArchetype(const std::string& t, const std::string& id, size_t& begin, size_t& end)
{
    std::vector<size_t> open;
    for (size_t i = 0; i < t.size();)
    {
        const char c = t[i];
        if (c == '"')
        {
            const size_t tokEnd = SkipString(t, i);
            if (tokEnd == NPOS)
                return false;
            const size_t colon = SkipWs(t, tokEnd);
            const bool   isKey = colon < t.size() && t[colon] == ':';
            if (isKey && !open.empty() && t.compare(i, tokEnd - i, "\"id\"") == 0)
            {
                const size_t v = SkipWs(t, colon + 1);
                const size_t vEnd = (v < t.size() && t[v] == '"') ? SkipString(t, v) : NPOS;
                if (vEnd != NPOS && t.compare(v + 1, vEnd - v - 2, id) == 0)
                {
                    begin = open.back();
                    const size_t objEnd = SkipValue(t, begin);
                    if (objEnd == NPOS)
                        return false;
                    end = objEnd;
                    return true;
                }
            }
            i = tokEnd;
            continue;
        }
        if (c == '{' || c == '[')
            open.push_back(i);
        else if (c == '}' || c == ']')
        {
            if (open.empty())
                return false;
            open.pop_back();
        }
        i++;
    }
    return false;
}

// The value of `key` among the object's own members, as [begin, end). Members of nested
// objects are skipped whole, which is what keeps an archetype's `size` distinct from one
// inside `world`.
bool FindMember(const std::string& t, size_t objBegin, size_t objEnd, const std::string& key,
                size_t& valBegin, size_t& valEnd, size_t& lastMemberEnd)
{
    const std::string quoted = "\"" + key + "\"";
    lastMemberEnd = NPOS;
    size_t i = SkipWs(t, objBegin + 1);
    while (i < objEnd && t[i] != '}')
    {
        if (t[i] != '"')
            return false;  // not a member list: refuse rather than guess
        const size_t nameEnd = SkipString(t, i);
        if (nameEnd == NPOS)
            return false;
        const size_t colon = SkipWs(t, nameEnd);
        if (colon >= objEnd || t[colon] != ':')
            return false;
        const size_t vBegin = SkipWs(t, colon + 1);
        const size_t vEnd = SkipValue(t, vBegin);
        if (vEnd == NPOS)
            return false;
        lastMemberEnd = vEnd;
        if (t.compare(i, nameEnd - i, quoted) == 0)
        {
            valBegin = vBegin;
            valEnd = vEnd;
            return true;
        }
        i = SkipWs(t, vEnd);
        if (i < objEnd && t[i] == ',')
            i = SkipWs(t, i + 1);
    }
    return false;
}

// The indentation the object's members are written with, so an added key lines up with
// the ones that were already there.
std::string MemberIndent(const std::string& t, size_t objBegin)
{
    const size_t first = SkipWs(t, objBegin + 1);
    size_t       lineStart = t.rfind('\n', first);
    lineStart = (lineStart == NPOS) ? 0 : lineStart + 1;
    return t.substr(lineStart, first - lineStart);
}
}  // namespace

bool SetField(std::string& text, const std::string& id, const std::string& key,
              const std::string& valueJson)
{
    size_t objBegin = 0, objEnd = 0;
    if (!FindArchetype(text, id, objBegin, objEnd))
        return false;

    size_t valBegin = 0, valEnd = 0, lastMemberEnd = 0;
    if (FindMember(text, objBegin, objEnd, key, valBegin, valEnd, lastMemberEnd))
    {
        text.replace(valBegin, valEnd - valBegin, valueJson);
        return true;
    }
    if (lastMemberEnd == NPOS)
        return false;  // an empty object: nothing to line the new member up with

    const std::string indent = MemberIndent(text, objBegin);
    text.insert(lastMemberEnd, ",\n" + indent + "\"" + key + "\": " + valueJson);
    return true;
}

}  // namespace ArchetypeEdit
