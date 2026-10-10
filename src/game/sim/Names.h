#pragma once

#include <cctype>
#include <string>

// What a player may call a system they found (#145). Names are world state everybody sees,
// so the rule is narrow on purpose: 3 to 24 characters, starting with a letter, of
// letters, digits, spaces, apostrophes and hyphens, no doubled spaces. Uniqueness and who
// may name are the simulation's business; this is only the shape of the name, shared by
// the server and the agent so both refuse the same things for the same reason.
namespace Names
{
inline bool ValidSystemName(const std::string& n, std::string& why)
{
    if (n.size() < 3 || n.size() > 24)
    {
        why = "a name is 3 to 24 characters";
        return false;
    }
    if (!std::isalpha((unsigned char)n[0]))
    {
        why = "a name starts with a letter";
        return false;
    }
    for (size_t i = 0; i < n.size(); i++)
    {
        const unsigned char c = (unsigned char)n[i];
        if (!(std::isalnum(c) || c == ' ' || c == '\'' || c == '-'))
        {
            why = "letters, digits, spaces, ' and - only";
            return false;
        }
        if (c == ' ' && (i + 1 == n.size() || n[i + 1] == ' '))
        {
            why = "no trailing or doubled spaces";
            return false;
        }
    }
    return true;
}

// Two names that differ only in case are the same name.
inline bool SameName(const std::string& a, const std::string& b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); i++)
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]))
            return false;
    return true;
}
}  // namespace Names
