#pragma once

#include <nlohmann/json.hpp>
#include <vector>
#include <string>

// Content files are read with `value(key, default)`, which is permissive by design: a
// field that is absent takes its default. The cost is that a field that is misspelled is
// absent too, and "rnage" quietly becomes the default range with nothing anywhere to say
// so (#191). For data an author writes by hand -- archetypes, shapes -- a key the reader
// does not know is therefore a load error, the same as an unknown form (#122).
//
// Returns false with `error` naming the first key that is not in `known`. A non-object
// passes: whether the value has the right type is the caller's question.
inline bool OnlyKnownKeys(const nlohmann::json& j, const std::vector<const char*>& known,
                          std::string& error)
{
    if (!j.is_object())
        return true;
    for (auto it = j.begin(); it != j.end(); ++it)
    {
        bool found = false;
        for (const char* k : known)
            if (it.key() == k)
            {
                found = true;
                break;
            }
        if (!found)
        {
            error = "unknown field '" + it.key() + "'";
            return false;
        }
    }
    return true;
}
