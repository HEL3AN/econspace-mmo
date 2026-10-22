#include "gen/Names.h"

#include "gen/Rng.h"

namespace Gen
{
namespace
{
constexpr uint64_t NAMING = 0x4E414D45ull;  // a purpose of its own: "NAME"
constexpr int      ATTEMPTS = 64;

bool IsVowel(char c)
{
    return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u' || c == 'y';
}

char Upper(char c)
{
    return c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c;
}

const std::string& Pick(Rng& rng, const std::vector<std::string>& from)
{
    return from[(size_t)rng.Range(0, (int)from.size() - 1)];
}

// One word: syllables of an onset and a vowel, the last closed by a coda or an ending.
std::string Word(const NameCulture& c, Rng& rng)
{
    const int   n = rng.Range(c.minSyllables, c.maxSyllables);
    std::string w;
    for (int i = 0; i < n; i++)
    {
        w += Pick(rng, c.onsets);
        w += Pick(rng, c.vowels);
    }
    if (!c.endings.empty() && rng.Chance(c.endingChance))
        w += Pick(rng, c.endings);
    else
        w += Pick(rng, c.codas);
    return w;
}

// Something a mouth can say: 3 to 12 letters, no letter three times running, no doubled
// vowel but e and o, no more than two vowels or three consonants together.
bool Pronounceable(const std::string& w)
{
    if (w.size() < 3 || w.size() > 12)
        return false;
    int vowels = 0, consonants = 0;
    for (size_t i = 0; i < w.size(); i++)
    {
        if (i >= 2 && w[i] == w[i - 1] && w[i] == w[i - 2])
            return false;
        // "ee" and "oo" are English; "aa", "ii", "uu", "yy" read as typing mistakes.
        if (i >= 1 && w[i] == w[i - 1] && IsVowel(w[i]) && w[i] != 'e' && w[i] != 'o')
            return false;
        if (w[i] == '\'')
        {
            vowels = consonants = 0;
            continue;
        }
        if (IsVowel(w[i]))
        {
            vowels++;
            consonants = 0;
        }
        else
        {
            consonants++;
            vowels = 0;
        }
        if (vowels > 2 || consonants > 3)
            return false;
    }
    return w[0] != '\'' && w.back() != '\'';
}

// The shape Names::ValidSystemName asks of a name (sim/Names.h, which gen/ does not reach).
bool Shaped(const std::string& n)
{
    if (n.size() < 3 || n.size() > 24 ||
        !((n[0] >= 'A' && n[0] <= 'Z') || (n[0] >= 'a' && n[0] <= 'z')))
        return false;
    for (size_t i = 0; i < n.size(); i++)
        if (n[i] == ' ' && (i + 1 == n.size() || n[i + 1] == ' '))
            return false;
    return true;
}

std::string Fill(const std::string& pattern, const std::string& word, Rng& rng)
{
    std::string out;
    for (size_t i = 0; i < pattern.size(); i++)
    {
        if (pattern[i] == '{' && i + 2 < pattern.size() && pattern[i + 2] == '}')
        {
            const char t = pattern[i + 1];
            if (t == 'w')
                out += std::string(1, Upper(word[0])) + word.substr(1);
            else if (t == 'W')
                for (char ch : word)
                    out += Upper(ch);
            else
                out += std::to_string(rng.Range(2, 99));
            i += 2;
        }
        else
            out += pattern[i];
    }
    return out;
}
}  // namespace

uint64_t TextKey(const std::string& text)
{
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : text)
    {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

std::string FoldName(const std::string& name)
{
    std::string out = name;
    for (char& c : out)
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
    return out;
}

namespace
{
// Short on purpose: it guards against what syllables can spell by accident, in English, and
// throwing a harmless generated name away costs only another draw.
const char* const BLOCKED[] = {
    "fuck", "shit", "cunt",  "nigg", "nigr",  "fag",    "rape", "cock",   "dick",
    "piss", "slut", "whore", "twat", "wank",  "nazi",   "kkk",  "anus",   "porn",
    "cum",  "tits", "bitch", "jizz", "spunk", "retard", "sex",  "hitler", "arse",
    "puss", "crap", "kike",  "spic", "chink", "dyke",   "homo", "pedo",   "semen",
};
// Of those, the ones no innocent word holds: a chosen name is refused with one anywhere in it.
// The rest refuse a chosen name only as a word of their own -- "Parsec", "Torpedo" and
// "Peacock" are names, not insults (the Scunthorpe problem).
const char* const BLOCKED_ANYWHERE[] = { "fuck", "cunt",   "nigg",   "nigr", "whore",
                                         "jizz", "retard", "hitler", "bitch" };

// The folded letters with everything else taken out: what an anywhere-entry is looked for in.
std::string Bare(const std::string& text)
{
    std::string bare;
    for (char c : FoldName(text))
        if (c >= 'a' && c <= 'z')
            bare += c;
    return bare;
}

// The words of a name: runs of letters, split on spaces, hyphens, apostrophes and digits.
std::vector<std::string> Words(const std::string& name)
{
    std::vector<std::string> out(1);
    for (char c : FoldName(name))
        if (c >= 'a' && c <= 'z')
            out.back() += c;
        else if (!out.back().empty())
            out.emplace_back();
    if (out.back().empty())
        out.pop_back();
    return out;
}

// A word is `w`, or `w` made plural.
bool IsWord(const std::string& token, const std::string& w)
{
    return !w.empty() && (token == w || token == w + "s" || token == w + "es");
}
}  // namespace

bool NameBlocked(const std::string& name)
{
    std::string bare;
    for (char c : FoldName(name))
        if (c != ' ' && c != '-' && c != '\'')
            bare += c;
    for (const char* b : BLOCKED)
        if (bare.find(b) != std::string::npos)
            return true;
    return false;
}

bool ChosenNameBlocked(const std::string& name, const std::vector<std::string>& extra)
{
    const std::vector<std::string> words = Words(name);
    const std::string              bare = Bare(name);
    auto                           asWord = [&](const std::string& w)
    {
        for (const std::string& t : words)
            if (IsWord(t, w))
                return true;
        return false;
    };
    for (const char* b : BLOCKED)
        if (asWord(b))
            return true;
    for (const char* b : BLOCKED_ANYWHERE)
        if (bare.find(b) != std::string::npos)
            return true;
    // The name's words between spaces, for an entry of more than one word ("rival corp").
    std::string spaced = " ";
    for (const std::string& t : words)
        spaced += t + " ";
    for (const std::string& entry : extra)
    {
        const bool anywhere = !entry.empty() && entry[0] == '*';
        if (anywhere)
        {
            const std::string w = Bare(entry.substr(1));
            if (!w.empty() && bare.find(w) != std::string::npos)
                return true;
            continue;
        }
        const std::vector<std::string> phrase = Words(entry);
        if (phrase.size() == 1 && asWord(phrase[0]))
            return true;
        if (phrase.size() > 1)
        {
            std::string p = " ";
            for (const std::string& t : phrase)
                p += t + " ";
            if (spaced.find(p) != std::string::npos)
                return true;
        }
    }
    return false;
}

std::string CultureName(const NameCulture& culture, uint64_t key,
                        const std::set<std::string>& taken)
{
    if (culture.onsets.empty() || culture.vowels.empty() || culture.codas.empty() ||
        culture.patterns.empty())
        return std::string();
    for (int attempt = 0; attempt < ATTEMPTS; attempt++)
    {
        Rng               rng(Key(key, NAMING, (uint64_t)attempt));
        const std::string word = Word(culture, rng);
        if (!Pronounceable(word))
            continue;
        const std::string name = Fill(Pick(rng, culture.patterns), word, rng);
        if (Shaped(name) && !NameBlocked(name) && taken.count(FoldName(name)) == 0)
            return name;
    }
    return std::string();
}

std::string ChartedName(FactionId faction, uint64_t worldSeed, const std::string& systemId,
                        const std::set<std::string>& taken)
{
    const FactionDef&  def = Factions::Def(faction);
    const NameCulture* culture = def.culture.empty() ? nullptr : Factions::Culture(def.culture);
    if (culture == nullptr)
        return std::string();
    return CultureName(*culture, Key(worldSeed, TextKey(systemId), TextKey(def.id)), taken);
}

}  // namespace Gen
