#include "core/ShipDesign.h"

#include "core/Faction.h"
#include "core/JsonKeys.h"
#include "render/Modules.h"
#include <cmath>
#include <cstdio>
#include <fstream>

using json = nlohmann::json;

namespace
{
const char* const POSITIONS[] = { "bow", "mid", "stern" };

// The socket kinds a section may expose, in the kit's own words (world_format.md, "A ship's
// socket kinds"). A ship's end is always one or the other: a `bow` at the bow, a `stern` at
// the stern -- so there is exactly one stern, and it is where the drives are. A socket a
// design cannot name is a misspelling, not a new kind.
const char* const SOCKETS[] = { "top", "edge", "bow", "stern", "front", "side", "spine", "bottom" };

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
    // Only what a stat is derived from. Hull and damage arrived with the NPCs that read them
    // (#279 step 4); shields and sensors come with the stat that reads them -- accepted
    // before that, they would be numbers nothing uses.
    if (!OnlyKnownKeys(pj, { "thrust", "rcs", "cargo", "mining", "hull", "damage" }, err) ||
        !ReadNumber(pj, "thrust", p.thrust, err) || !ReadNumber(pj, "rcs", p.rcs, err) ||
        !ReadNumber(pj, "cargo", p.cargo, err) || !ReadNumber(pj, "mining", p.mining, err) ||
        !ReadNumber(pj, "hull", p.hull, err) || !ReadNumber(pj, "damage", p.damage, err))
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
    if (!j.is_object() || !OnlyKnownKeys(j, { "id", "mass", "provides", "cost", "look" }, err))
    {
        if (err.empty())
            err = "not an object";
        return false;
    }
    if (!ReadString(j, "id", m.id, err) || !ReadMass(j, m.mass, err) ||
        !ReadProvides(j, m.provides, err) || !ReadCost(j, m.cost, err))
        return false;
    if (j.contains("look"))
    {
        if (!j["look"].is_object())
        {
            err = "'look' is not an object of socket kind -> { mount, turn, scale, z }";
            return false;
        }
        for (auto it = j["look"].begin(); it != j["look"].end(); ++it)
        {
            if (!OneOf(it.key(), SOCKETS))
            {
                err = "look: '" + it.key() + "' is not a socket kind";
                return false;
            }
            if (!it.value().is_object() ||
                !OnlyKnownKeys(it.value(), { "mount", "turn", "scale", "z" }, err))
            {
                err = "look: " + it.key() + ": " + (err.empty() ? "not an object" : err);
                return false;
            }
            m.look.emplace_back(it.key(), it.value());
        }
    }
    return true;
}

// A ship section's drawing (#279 step 3). Its hull is read here, without the module library:
// it may not name a module, so nothing about the class rule waits for the renderer.
bool ParseSectionShape(const json& j, Ships::Section& s, std::string& err)
{
    if (!j.is_object() || !OnlyKnownKeys(j, { "sections", "kit", "parts" }, err) ||
        !j.contains("sections") || !j["sections"].is_array() || j["sections"].empty())
    {
        err = "shape: " +
              (err.empty() ? std::string("{ \"sections\": [...], \"kit\", \"parts\" }") : err);
        return false;
    }
    if (j.contains("kit") && !j["kit"].is_array())
    {
        err = "shape: 'kit' is a list of kit lines; the frame says how a ship is mirrored";
        return false;
    }
    if (j.contains("parts") && !j["parts"].is_array())
    {
        err = "shape: 'parts' is a list of parts";
        return false;
    }
    Render::Shape hull;
    std::string   why;
    if (!Render::ParseShape(json{ { "sections", j["sections"] }, { "parts", json::array() } }, hull,
                            why))
    {
        err = "shape: " + why;
        return false;
    }
    // A ship is mirrored about the way it flies, and a ship's hull is the same for every ship
    // of its design: what the seed varies goes on it, as trim.
    auto symmetric = [&](const json& pj, const char* what) -> bool
    {
        for (const json& e : pj)
        {
            if (!e.is_object())
                continue;  // ParseShape has said so where it reads them
            if (e.contains("repeat"))
            {
                err = std::string("shape: a ") + what +
                      " repeats; a ship is mirrored, never repeated -- `repeat: 2` puts the " +
                      "second wing in front of the nose";
                return false;
            }
            if (e.value("mirror", false))
                continue;
            const json at = e.value("at", json::array({ 0.0, 0.0 }));
            const bool onAxis =
                at.is_array() && at.size() == 2 && at[1].is_number() && at[1].get<double>() == 0.0;
            // Along the axis, or for a form that is its own mirror both ways (a bar, a
            // capsule, a lattice, anything round), across it too.
            const std::string form = e.value("form", std::string("disc"));
            const bool        both = form == "bar" || form == "capsule" || form == "lattice" ||
                                     form == "disc" || form == "ring";
            const json        angle = e.value("angle", json(0.0));
            const bool square = angle.is_number() && std::fmod(std::fabs(angle.get<double>()),
                                                               both ? 90.0 : 180.0) == 0.0;
            if (!onAxis || !square)
            {
                err = std::string("shape: a ") + what +
                      " is off the axis or turned across it without `mirror`; a ship is " +
                      "bilateral";
                return false;
            }
        }
        return true;
    };
    if (!symmetric(j["sections"], "hull part") ||
        (j.contains("parts") && !symmetric(j["parts"], "part")))
        return false;
    for (const Render::Part& p : hull.parts)
        if (!p.module.empty() || !p.vary.empty() || !p.palette.empty() || p.chance < 1.0f)
        {
            err = "shape: a hull part is fixed -- no module, range, palette or chance; what "
                  "the seed varies is trim, on the hull";
            return false;
        }
    s.shape = j;
    s.hull = hull.parts;
    const Rectangle box = Render::MeasuredBounds(hull);
    s.aft = box.x;
    s.fore = box.x + box.width;
    return true;
}

bool ParseSection(const json& j, Ships::Section& s, std::string& err)
{
    if (!j.is_object() ||
        !OnlyKnownKeys(j, { "id", "position", "mass", "provides", "cost", "sockets", "shape" },
                       err))
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
            // One bow and one stern: an end facing forward is only a bow's, and one facing
            // aft only a stern's. A mid section's ends are where it meets its neighbours.
            if ((it.key() == "bow" || it.key() == "stern") && it.key() != s.position)
            {
                err = "sockets: a " + s.position + " section has no '" + it.key() + "'";
                return false;
            }
            s.sockets.emplace_back(it.key(), it.value().get<int>());
        }
    }
    if (!j.contains("shape"))
    {
        err = "'shape' is missing: a section is drawn as something";
        return false;
    }
    return ParseSectionShape(j["shape"], s, err);
}

bool ParseFrame(const json& j, Ships::Frame& f, std::string& err)
{
    if (!j.is_object() ||
        !OnlyKnownKeys(
            j, { "id", "class", "mass", "provides", "cost", "mids", "minAspect", "bowHeavy" }, err))
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
    if (!j.contains("minAspect") || !j["minAspect"].is_number() ||
        j["minAspect"].get<double>() <= 0.0)
    {
        err = "'minAspect' is missing: a class says how long it is for its width";
        return false;
    }
    f.minAspect = j["minAspect"].get<float>();
    if (j.contains("bowHeavy") && !j["bowHeavy"].is_boolean())
    {
        err = "'bowHeavy' is true or false";
        return false;
    }
    f.bowHeavy = j.value("bowHeavy", false);
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
        if (!l.is_object() || !OnlyKnownKeys(l, { "module", "in", "on", "count", "scale" }, err))
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
        if (!ReadNumber(l, "scale", line.scale, err))
        {
            err = "fit: '" + line.module + "': " + err;
            return false;
        }
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
    sum.hull += p.hull * times;
    sum.damage += p.damage * times;
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

bool ArmedRole(const std::string& role)
{
    return role == "patrol" || role == "pirate" || role == "warship";
}

const std::vector<std::string>* Catalogue::DesignsFor(const std::string& faction,
                                                      const std::string& role) const
{
    for (const char* who : { faction.c_str(), "default" })
        for (const Doctrine& d : doctrines)
            if (d.faction == who)
                for (const auto& [r, list] : d.roles)
                    if (r == role)
                        return &list;
    return nullptr;
}

std::string Catalogue::Pick(const std::string& faction, const std::string& role, unsigned key) const
{
    const std::vector<std::string>* list = DesignsFor(faction, role);
    if (list == nullptr || list->empty())
        return std::string();
    return (*list)[key % list->size()];
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

    // The class rule, on the hull as it is laid out.
    const Silhouette look = Measure(c, d);
    if (look.length < frame->minAspect * look.width)
    {
        char buf[160];
        std::snprintf(buf, sizeof buf,
                      "a %s is at least %.2f times as long as it is wide, and "
                      "this hull is %.2f",
                      frame->hullClass.c_str(), (double)frame->minAspect,
                      (double)(look.length / look.width));
        error = where + buf;
        return false;
    }
    if (look.massAt > 0.0f && !frame->bowHeavy)
    {
        error = where + "its hull carries its mass forward of the middle, and only a class " +
                "that works in front of itself does (`bowHeavy`); a " + frame->hullClass +
                " is heaviest at its drives";
        return false;
    }
    return true;
}

namespace
{
// A design's sections in the order a ship is written, bow first, each with the position it
// stands at and how far along x its own frame is moved: laid end to end, stern to bow, and
// the whole centred on the middle of its length.
struct Laid
{
    const Section* section;
    std::string    position;
    float          dx;
};

std::vector<Laid> LayOut(const Catalogue& c, const Design& d)
{
    std::vector<Laid> out;
    out.push_back({ c.FindSection(d.bow), "bow", 0.0f });
    for (const std::string& m : d.mids)
        out.push_back({ c.FindSection(m), "mid", 0.0f });
    out.push_back({ c.FindSection(d.stern), "stern", 0.0f });
    float cursor = 0.0f;
    for (size_t k = out.size(); k-- > 0;)
    {
        out[k].dx = cursor - out[k].section->aft;
        cursor = out[k].dx + out[k].section->fore;
    }
    for (Laid& l : out)
        l.dx -= 0.5f * cursor;
    return out;
}

// A part's JSON moved `dx` along the ship.
json Moved(json part, float dx)
{
    json at = part.value("at", json::array({ 0.0, 0.0 }));
    if (at[0].is_array())  // a range moves as a whole
        for (json& x : at[0])
            x = x.get<double>() + (double)dx;
    else
        at[0] = at[0].get<double>() + (double)dx;
    part["at"] = at;
    return part;
}
}  // namespace

Silhouette Measure(const Catalogue& c, const Design& d)
{
    Silhouette    out;
    Render::Shape hull;
    for (const Laid& l : LayOut(c, d))
        for (Render::Part p : l.section->hull)
        {
            p.at.x += l.dx;
            hull.parts.push_back(p);
        }
    const Rectangle box = Render::MeasuredBounds(hull);
    out.length = box.width;
    out.width = box.height;

    // Where the area is: a fixed grid over the box, each point counted once whatever covers
    // it. Every mirrored part is its two copies.
    std::vector<Render::Part> copies;
    for (const Render::Part& p : hull.parts)
    {
        Render::Part a = p;
        a.mirror = false;
        copies.push_back(a);
        if (p.mirror)
        {
            a.at.y = -a.at.y;
            a.angle = -a.angle;
            copies.push_back(a);
        }
    }
    const int n = 160;
    double    sum = 0.0;
    int       count = 0;
    for (int i = 0; i < n; i++)
        for (int k = 0; k < n; k++)
        {
            const Vector2 at = { box.x + box.width * ((float)i + 0.5f) / (float)n,
                                 box.y + box.height * ((float)k + 0.5f) / (float)n };
            for (const Render::Part& p : copies)
                if (Render::Covers(p, at))
                {
                    sum += at.x;
                    count++;
                    break;
                }
        }
    const float middle = box.x + 0.5f * box.width;
    out.massAt = count > 0 && box.width > 0.0f
                     ? ((float)(sum / (double)count) - middle) / (0.5f * box.width)
                     : 0.0f;
    return out;
}

bool ShapeOf(const Catalogue& c, const Design& d, json& out, std::string& error)
{
    if (!Validate(c, d, error))
        return false;
    json sections = json::array(), parts = json::array(), function = json::array(),
         trim = json::array();
    std::vector<std::pair<std::string, int>> indexAt;  // position -> a hull part's index
    for (const Laid& l : LayOut(c, d))
    {
        const json& shape = l.section->shape;
        const int   first = (int)sections.size();
        json        own = json::array();
        for (const json& p : shape["sections"])
        {
            own.push_back((int)sections.size());
            indexAt.emplace_back(l.position, (int)sections.size());
            sections.push_back(Moved(p, l.dx));
        }
        if (shape.contains("parts"))
            for (const json& p : shape["parts"])
                parts.push_back(Moved(p, l.dx));
        if (shape.contains("kit"))
            for (json line : shape["kit"])
            {
                // A section's trim stays on the section: `in` counts its own hull parts.
                if (!line.is_object())
                {
                    error = "section '" + l.section->id + "': a kit line is not an object";
                    return false;
                }
                if (line.contains("fit"))
                {
                    error = "section '" + l.section->id + "': its kit is trim; what carries " +
                            "function is in a design's fit";
                    return false;
                }
                json in = json::array();
                if (!line.contains("in"))
                    in = own;
                else
                {
                    const json want =
                        line["in"].is_array() ? line["in"] : json::array({ line["in"] });
                    for (const json& k : want)
                    {
                        if (!k.is_number_integer() || k.get<int>() < 0 ||
                            k.get<int>() >= (int)own.size())
                        {
                            error = "section '" + l.section->id + "': a kit line's 'in' is " +
                                    "not one of its hull parts";
                            return false;
                        }
                        in.push_back(first + k.get<int>());
                    }
                }
                line["in"] = in;
                trim.push_back(line);
            }
    }
    for (const FitLine& f : d.fit)
    {
        json line = { { "of", f.module }, { "on", f.on }, { "count", f.count }, { "fit", true } };
        for (const auto& [kind, look] : c.FindModule(f.module)->look)
            if (kind == f.on)
                for (auto it = look.begin(); it != look.end(); ++it)
                    line[it.key()] = it.value();
        if (f.scale > 0.0f)
            line["scale"] = f.scale;
        json in = json::array();
        for (const auto& [position, index] : indexAt)
            if (position == f.in)
                in.push_back(index);
        line["in"] = in;
        function.push_back(line);
    }
    for (const json& t : trim)
        function.push_back(t);
    out = {
        { "sections", sections },
        { "kit",
          { { "symmetry", "bilateral" }, { "plain", c.rules.plain }, { "modules", function } } },
        { "parts", parts }
    };
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
    out.hull = sum.hull;
    out.damage = sum.damage;
    out.cruise = out.maxSpeed * r.cruise;
    for (ResourceType t : AllResourceTypes())
        if (cost[(int)t] > 0)
            out.cost.emplace_back(t, cost[(int)t]);
    out.parts = parts;
    out.buildSeconds = r.buildSecondsPerMass * mass + r.buildSecondsPerPart * (float)parts;
    return true;
}

// What each faction flies (#279 step 4): { faction id or "default": { role: [design, ...] } }.
// The default names a design for every role, so an NPC never goes without one; an armed role
// is flown only by designs that carry guns.
bool ReadDoctrines(const json& j, Catalogue& c, std::string& error)
{
    if (!j.contains("doctrines") || !j["doctrines"].is_object())
    {
        error = "'doctrines' is missing: an object of faction -> { role: [designs] }";
        return false;
    }
    for (auto f = j["doctrines"].begin(); f != j["doctrines"].end(); ++f)
    {
        const std::string where = "doctrine '" + f.key() + "': ";
        if (f.key() != "default" && Factions::Id(FactionFromString(f.key())) != f.key())
        {
            error = where + "no such faction";
            return false;
        }
        if (!f.value().is_object())
        {
            error = where + "not an object of role -> [designs]";
            return false;
        }
        Doctrine d;
        d.faction = f.key();
        for (auto r = f.value().begin(); r != f.value().end(); ++r)
        {
            if (!OneOf(r.key(), NPC_ROLES))
            {
                error = where + "'" + r.key() + "' is not an NPC role";
                return false;
            }
            if (!r.value().is_array() || r.value().empty())
            {
                error = where + r.key() + ": not a list of designs";
                return false;
            }
            std::vector<std::string> designs;
            for (const json& id : r.value())
            {
                const Design* design =
                    id.is_string() ? c.FindDesign(id.get<std::string>()) : nullptr;
                if (design == nullptr)
                {
                    error = where + r.key() + ": " +
                            (id.is_string() ? id.get<std::string>() : "?") + " is not a design";
                    return false;
                }
                Stats       st;
                std::string why;
                if (ArmedRole(r.key()) && (!Derive(c, *design, st, why) || st.damage <= 0.0f))
                {
                    error = where + r.key() + ": '" + design->id + "' carries no guns";
                    return false;
                }
                designs.push_back(design->id);
            }
            d.roles.emplace_back(r.key(), std::move(designs));
        }
        c.doctrines.push_back(std::move(d));
    }
    for (const char* role : NPC_ROLES)
    {
        const std::vector<std::string>* list = nullptr;
        for (const Doctrine& d : c.doctrines)
            if (d.faction == "default")
                for (const auto& [r, designs] : d.roles)
                    if (r == role)
                        list = &designs;
        if (list == nullptr)
        {
            error = std::string("doctrine 'default': names no design for '") + role + "'";
            return false;
        }
    }
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
    if (!OnlyKnownKeys(j, { "rules", "modules", "sections", "frames", "designs", "doctrines" },
                       error))
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
                         "buildSecondsPerPart", "plain", "cruise" },
                       error))
    {
        error = "rules: " + error;
        return false;
    }
    for (const char* key : { "speedBase", "speedPerAccel", "rcsPerTurn", "buildSecondsPerMass",
                             "buildSecondsPerPart", "plain", "cruise" })
        if (!rj.contains(key))
        {
            error = std::string("rules: '") + key + "' is missing";
            return false;
        }
    if (!ReadNumber(rj, "speedBase", c.rules.speedBase, error) ||
        !ReadNumber(rj, "speedPerAccel", c.rules.speedPerAccel, error) ||
        !ReadNumber(rj, "rcsPerTurn", c.rules.rcsPerTurn, error) ||
        !ReadNumber(rj, "buildSecondsPerMass", c.rules.buildSecondsPerMass, error) ||
        !ReadNumber(rj, "buildSecondsPerPart", c.rules.buildSecondsPerPart, error) ||
        !ReadNumber(rj, "plain", c.rules.plain, error) ||
        !ReadNumber(rj, "cruise", c.rules.cruise, error))
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
    if (!ReadDoctrines(j, c, error))
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
