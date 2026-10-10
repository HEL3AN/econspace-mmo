#include "core/ShipDesign.h"

#include "core/JsonKeys.h"
#include <fstream>

using json = nlohmann::json;

namespace
{
const char* const POSITIONS[] = { "bow", "mid", "stern" };

// The socket kinds a section may expose: the kit's own (`edge`, `top`, `end`) and the ones
// the hull modules already declare (`front`, `side`, `spine`, `bottom`). A socket a design
// cannot name is a misspelling, not a new kind.
const char* const SOCKETS[] = { "top", "edge", "end", "front", "side", "spine", "bottom" };

// What goes here is mirrored, so it comes in pairs: one wing is a fish, not a ship.
bool Paired(const std::string& socket)
{
    return socket == "side" || socket == "edge";
}

template <size_t N> bool OneOf(const std::string& s, const char* const (&list)[N])
{
    for (const char* x : list)
        if (s == x)
            return true;
    return false;
}

// A non-negative number, or an error naming the field.
bool ReadNumber(const json& j, const char* key, float& out, std::string& err)
{
    if (!j.contains(key))
        return true;
    if (!j[key].is_number() || j[key].get<double>() < 0.0)
    {
        err = std::string("'") + key + "' is not a non-negative number";
        return false;
    }
    out = j[key].get<float>();
    return true;
}

bool ReadString(const json& j, const char* key, std::string& out, std::string& err)
{
    if (!j.contains(key) || !j[key].is_string() || j[key].get<std::string>().empty())
    {
        err = std::string("'") + key + "' is missing or not a name";
        return false;
    }
    out = j[key].get<std::string>();
    return true;
}

// Mass is required and more than zero: a part that weighs nothing makes a ship whose
// acceleration is whatever its drives say over nothing.
bool ReadMass(const json& j, float& out, std::string& err)
{
    if (!j.contains("mass"))
    {
        err = "'mass' is missing: every part weighs something";
        return false;
    }
    if (!ReadNumber(j, "mass", out, err))
        return false;
    if (out <= 0.0f)
    {
        err = "'mass' must be more than zero";
        return false;
    }
    return true;
}

bool ReadProvides(const json& j, Ships::Provides& p, std::string& err)
{
    if (!j.contains("provides"))
        return true;
    const json& pj = j["provides"];
    if (!pj.is_object())
    {
        err = "'provides' is not an object";
        return false;
    }
    // Only what a stat is derived from. Shields, hull points and sensors come with the stat
    // that reads them; accepted before that, they would be numbers nothing uses.
    if (!OnlyKnownKeys(pj, { "thrust", "rcs", "cargo", "mining" }, err) ||
        !ReadNumber(pj, "thrust", p.thrust, err) || !ReadNumber(pj, "rcs", p.rcs, err) ||
        !ReadNumber(pj, "cargo", p.cargo, err) || !ReadNumber(pj, "mining", p.mining, err))
    {
        err = "provides: " + err;
        return false;
    }
    return true;
}

// The same shape as a blueprint's cost (#39): resource name -> positive whole amount, and at
// least one of them -- every part is made of something.
bool ReadCost(const json& j, Ships::Cost& cost, std::string& err)
{
    if (!j.contains("cost") || !j["cost"].is_object() || j["cost"].empty())
    {
        err = "'cost' is missing: a part is an object of resource amounts";
        return false;
    }
    for (auto it = j["cost"].begin(); it != j["cost"].end(); ++it)
    {
        const ResourceType r = ResourceFromName(it.key());
        if (ResourceName(r) != it.key())
        {
            err = "cost: '" + it.key() + "' is not a resource";
            return false;
        }
        if (!it.value().is_number_integer() || it.value().get<int>() <= 0)
        {
            err = "cost: '" + it.key() + "' is not a positive whole amount";
            return false;
        }
        cost.emplace_back(r, it.value().get<int>());
    }
    return true;
}

bool ParseModule(const json& j, Ships::ModulePart& m, std::string& err)
{
    if (!j.is_object() || !OnlyKnownKeys(j, { "id", "mass", "provides", "cost" }, err))
    {
        if (err.empty())
            err = "not an object";
        return false;
    }
    return ReadString(j, "id", m.id, err) && ReadMass(j, m.mass, err) &&
           ReadProvides(j, m.provides, err) && ReadCost(j, m.cost, err);
}

bool ParseSection(const json& j, Ships::Section& s, std::string& err)
{
    if (!j.is_object() ||
        !OnlyKnownKeys(j, { "id", "position", "mass", "provides", "cost", "sockets" }, err))
    {
        if (err.empty())
            err = "not an object";
        return false;
    }
    if (!ReadString(j, "id", s.id, err) || !ReadString(j, "position", s.position, err))
        return false;
    if (!OneOf(s.position, POSITIONS))
    {
        err = "position '" + s.position + "' is not bow, mid or stern";
        return false;
    }
    if (!ReadMass(j, s.mass, err) || !ReadProvides(j, s.provides, err) || !ReadCost(j, s.cost, err))
        return false;
    if (j.contains("sockets"))
    {
        if (!j["sockets"].is_object())
        {
            err = "'sockets' is not an object of socket kind -> capacity";
            return false;
        }
        for (auto it = j["sockets"].begin(); it != j["sockets"].end(); ++it)
        {
            if (!OneOf(it.key(), SOCKETS))
            {
                err = "sockets: '" + it.key() + "' is not a socket kind";
                return false;
            }
            if (!it.value().is_number_integer() || it.value().get<int>() <= 0)
            {
                err = "sockets: '" + it.key() + "' is not a positive whole capacity";
                return false;
            }
            s.sockets.emplace_back(it.key(), it.value().get<int>());
        }
    }
    return true;
}

bool ParseFrame(const json& j, Ships::Frame& f, std::string& err)
{
    if (!j.is_object() ||
        !OnlyKnownKeys(j, { "id", "class", "mass", "provides", "cost", "mids" }, err))
    {
        if (err.empty())
            err = "not an object";
        return false;
    }
    if (!ReadString(j, "id", f.id, err) || !ReadString(j, "class", f.hullClass, err) ||
        !ReadMass(j, f.mass, err) || !ReadProvides(j, f.provides, err) || !ReadCost(j, f.cost, err))
        return false;
    const json& m = j.contains("mids") ? j["mids"] : json();
    if (!m.is_array() || m.size() != 2 || !m[0].is_number_integer() || !m[1].is_number_integer() ||
        m[0].get<int>() < 1 || m[1].get<int>() < m[0].get<int>())
    {
        err = "'mids' is not [least, most] with 1 <= least <= most";
        return false;
    }
    f.minMids = m[0].get<int>();
    f.maxMids = m[1].get<int>();
    return true;
}

bool ParseDesign(const json& j, Ships::Design& d, std::string& err)
{
    if (!j.is_object() ||
        !OnlyKnownKeys(j, { "id", "name", "frame", "bow", "mid", "stern", "fit" }, err))
    {
        if (err.empty())
            err = "not an object";
        return false;
    }
    if (!ReadString(j, "id", d.id, err) || !ReadString(j, "frame", d.frame, err) ||
        !ReadString(j, "bow", d.bow, err) || !ReadString(j, "stern", d.stern, err))
        return false;
    d.name = j.contains("name") && j["name"].is_string() ? j["name"].get<std::string>() : d.id;
    if (!j.contains("mid") || !j["mid"].is_array())
    {
        err = "'mid' is not a list of sections";
        return false;
    }
    for (const json& m : j["mid"])
    {
        if (!m.is_string())
        {
            err = "'mid' holds something that is not a section name";
            return false;
        }
        d.mids.push_back(m.get<std::string>());
    }
    if (!j.contains("fit") || !j["fit"].is_array())
    {
        err = "'fit' is not a list";
        return false;
    }
    for (const json& l : j["fit"])
    {
        Ships::FitLine line;
        if (!l.is_object() || !OnlyKnownKeys(l, { "module", "in", "on", "count" }, err))
        {
            err = "fit: " + (err.empty() ? std::string("a line is not an object") : err);
            return false;
        }
        if (!ReadString(l, "module", line.module, err) || !ReadString(l, "in", line.in, err) ||
            !ReadString(l, "on", line.on, err))
        {
            err = "fit: " + err;
            return false;
        }
        // Pinned: a count that carries function is a whole number in the design, never a
        // range for the seed to pick from.
        if (!l.contains("count") || !l["count"].is_number_integer() || l["count"].get<int>() < 1)
        {
            err = "fit: '" + line.module + "': 'count' is not a whole number of at least 1";
            return false;
        }
        line.count = l["count"].get<int>();
        d.fit.push_back(line);
    }
    return true;
}

void Add(Ships::Provides& sum, const Ships::Provides& p, float times = 1.0f)
{
    sum.thrust += p.thrust * times;
    sum.rcs += p.rcs * times;
    sum.cargo += p.cargo * times;
    sum.mining += p.mining * times;
}

void Add(int (&sum)[3], const Ships::Cost& c, int times = 1)
{
    for (const auto& [r, n] : c)
        sum[(int)r] += n * times;
}

template <class T> const T* FindIn(const std::vector<T>& v, const std::string& id)
{
    for (const T& x : v)
        if (x.id == id)
            return &x;
    return nullptr;
}

template <class T> bool Unique(const std::vector<T>& v, const char* what, std::string& err)
{
    for (size_t i = 0; i < v.size(); ++i)
        for (size_t k = i + 1; k < v.size(); ++k)
            if (v[i].id == v[k].id)
            {
                err = std::string(what) + " '" + v[i].id + "' is defined twice";
                return false;
            }
    return true;
}
}  // namespace

namespace Ships
{
const ModulePart* Catalogue::FindModule(const std::string& id) const
{
    return FindIn(modules, id);
}
const Section* Catalogue::FindSection(const std::string& id) const
{
    return FindIn(sections, id);
}
const Frame* Catalogue::FindFrame(const std::string& id) const
{
    return FindIn(frames, id);
}
const Design* Catalogue::FindDesign(const std::string& id) const
{
    return FindIn(designs, id);
}

bool Validate(const Catalogue& c, const Design& d, std::string& error)
{
    const std::string where = "design '" + d.id + "': ";
    const Frame*      frame = c.FindFrame(d.frame);
    if (frame == nullptr)
    {
        error = where + "frame '" + d.frame + "' does not exist";
        return false;
    }
    if ((int)d.mids.size() < frame->minMids || (int)d.mids.size() > frame->maxMids)
    {
        error = where + "frame '" + frame->id + "' takes " + std::to_string(frame->minMids) +
                " to " + std::to_string(frame->maxMids) + " mid sections, not " +
                std::to_string(d.mids.size());
        return false;
    }

    // Each position's sections, checked to stand where they are put.
    std::vector<std::pair<std::string, const Section*>> placed;
    auto place = [&](const std::string& id, const char* position) -> bool
    {
        const Section* s = c.FindSection(id);
        if (s == nullptr)
        {
            error = where + "section '" + id + "' does not exist";
            return false;
        }
        if (s->position != position)
        {
            error = where + "section '" + id + "' is a " + s->position + " section, not a " +
                    position + " one";
            return false;
        }
        placed.emplace_back(position, s);
        return true;
    };
    if (!place(d.bow, "bow"))
        return false;
    for (const std::string& m : d.mids)
        if (!place(m, "mid"))
            return false;
    if (!place(d.stern, "stern"))
        return false;

    // Every module placed, within what the sockets at its position hold. Lines that share a
    // position and a socket share its capacity.
    struct Use
    {
        std::string in, on;
        int         used = 0;
    };
    std::vector<Use> uses;
    for (const FitLine& line : d.fit)
    {
        const ModulePart* m = c.FindModule(line.module);
        if (m == nullptr)
        {
            error = where + "module '" + line.module + "' has no part entry";
            return false;
        }
        if (!OneOf(line.in, POSITIONS))
        {
            error = where + "'" + line.module + "' is in '" + line.in +
                    "', which is not bow, mid or stern";
            return false;
        }
        if (m->provides.thrust > 0.0f && line.in != "stern")
        {
            error = where + "'" + line.module + "' is a drive and drives sit in the stern";
            return false;
        }
        if (Paired(line.on) && line.count % 2 != 0)
        {
            error = where + "'" + line.module + "' on '" + line.on + "' is mirrored and comes " +
                    "in pairs, not " + std::to_string(line.count);
            return false;
        }
        Use* use = nullptr;
        for (Use& u : uses)
            if (u.in == line.in && u.on == line.on)
                use = &u;
        if (use == nullptr)
        {
            uses.push_back({ line.in, line.on, 0 });
            use = &uses.back();
        }
        use->used += line.count;

        int capacity = 0;
        for (const auto& [position, s] : placed)
            if (position == line.in)
                for (const auto& [kind, n] : s->sockets)
                    if (kind == line.on)
                        capacity += n;
        if (use->used > capacity)
        {
            error = where + "the " + line.in + " has " + std::to_string(capacity) + " '" + line.on +
                    "' sockets and the fit puts " + std::to_string(use->used) + " modules there";
            return false;
        }
    }
    return true;
}

bool Derive(const Catalogue& c, const Design& d, Stats& out, std::string& error)
{
    if (!Validate(c, d, error))
        return false;

    float    mass = 0.0f;
    Provides sum;
    int      cost[3] = { 0, 0, 0 };
    int      parts = 0;

    const Frame* frame = c.FindFrame(d.frame);
    mass += frame->mass;
    Add(sum, frame->provides);
    Add(cost, frame->cost);
    ++parts;

    std::vector<std::string> sections = { d.bow };
    sections.insert(sections.end(), d.mids.begin(), d.mids.end());
    sections.push_back(d.stern);
    for (const std::string& id : sections)
    {
        const Section* s = c.FindSection(id);
        mass += s->mass;
        Add(sum, s->provides);
        Add(cost, s->cost);
        ++parts;
    }
    for (const FitLine& line : d.fit)
    {
        const ModulePart* m = c.FindModule(line.module);
        mass += m->mass * (float)line.count;
        Add(sum, m->provides, (float)line.count);
        Add(cost, m->cost, line.count);
        parts += line.count;
    }

    const Rules& r = c.rules;
    out = Stats();
    out.mass = mass;
    out.acceleration = sum.thrust / mass;
    out.maxSpeed = r.speedBase + r.speedPerAccel * out.acceleration;
    out.rcsAccel = sum.rcs / mass;
    out.turnSpeed = out.rcsAccel / r.rcsPerTurn;
    out.cargoCapacity = (int)(sum.cargo + 0.5f);
    out.miningRate = sum.mining;
    for (ResourceType t : AllResourceTypes())
        if (cost[(int)t] > 0)
            out.cost.emplace_back(t, cost[(int)t]);
    out.parts = parts;
    out.buildSeconds = r.buildSecondsPerMass * mass + r.buildSecondsPerPart * (float)parts;
    return true;
}

bool Parse(const json& j, Catalogue& out, std::string& error)
{
    error.clear();
    if (!j.is_object())
    {
        error = "not a ship catalogue (not an object)";
        return false;
    }
    if (!OnlyKnownKeys(j, { "rules", "modules", "sections", "frames", "designs" }, error))
        return false;

    Catalogue c;
    if (!j.contains("rules") || !j["rules"].is_object())
    {
        error = "'rules' is missing";
        return false;
    }
    const json& rj = j["rules"];
    if (!OnlyKnownKeys(rj,
                       { "speedBase", "speedPerAccel", "rcsPerTurn", "buildSecondsPerMass",
                         "buildSecondsPerPart" },
                       error))
    {
        error = "rules: " + error;
        return false;
    }
    for (const char* key : { "speedBase", "speedPerAccel", "rcsPerTurn", "buildSecondsPerMass",
                             "buildSecondsPerPart" })
        if (!rj.contains(key))
        {
            error = std::string("rules: '") + key + "' is missing";
            return false;
        }
    if (!ReadNumber(rj, "speedBase", c.rules.speedBase, error) ||
        !ReadNumber(rj, "speedPerAccel", c.rules.speedPerAccel, error) ||
        !ReadNumber(rj, "rcsPerTurn", c.rules.rcsPerTurn, error) ||
        !ReadNumber(rj, "buildSecondsPerMass", c.rules.buildSecondsPerMass, error) ||
        !ReadNumber(rj, "buildSecondsPerPart", c.rules.buildSecondsPerPart, error))
    {
        error = "rules: " + error;
        return false;
    }
    if (c.rules.rcsPerTurn <= 0.0f)
    {
        error = "rules: 'rcsPerTurn' must be more than zero";
        return false;
    }

    // Each list is read the same way: an array of entries, each named in its error.
    auto readList = [&](const char* key, auto& list, auto parse) -> bool
    {
        if (!j.contains(key) || !j[key].is_array())
        {
            error = std::string("'") + key + "' is not a list";
            return false;
        }
        for (const json& e : j[key])
        {
            typename std::decay_t<decltype(list)>::value_type item;
            std::string                                       err;
            if (!parse(e, item, err))
            {
                error = std::string(key) + " '" + item.id + "': " + err;
                return false;
            }
            list.push_back(std::move(item));
        }
        return Unique(list, key, error);
    };
    if (!readList("modules", c.modules, ParseModule) ||
        !readList("sections", c.sections, ParseSection) ||
        !readList("frames", c.frames, ParseFrame) || !readList("designs", c.designs, ParseDesign))
        return false;

    for (const Design& d : c.designs)
        if (!Validate(c, d, error))
            return false;

    out = std::move(c);
    return true;
}

bool Load(const std::string& path, Catalogue& out, std::string& error)
{
    std::ifstream in(path);
    if (!in.is_open())
    {
        error = "cannot open " + path;
        return false;
    }
    const json j = json::parse(in, nullptr, false);
    if (j.is_discarded())
    {
        error = path + ": not valid JSON";
        return false;
    }
    if (!Parse(j, out, error))
    {
        error = path + ": " + error;
        return false;
    }
    return true;
}
}  // namespace Ships
