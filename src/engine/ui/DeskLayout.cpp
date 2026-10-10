#include "ui/DeskLayout.h"

#include "ui/UiTheme.h"  // TITLE_HEIGHT

#include <algorithm>

namespace Ui
{

namespace
{
// How much of a window has to stay on the screen for it to be dragged back: its title bar
// vertically, this much of it horizontally. The rule a drag already followed.
constexpr float GRAB_MARGIN = 60.0f;

bool IsLeft(Anchor a)
{
    return a == Anchor::TopLeft || a == Anchor::BottomLeft;
}
bool IsRight(Anchor a)
{
    return a == Anchor::TopRight || a == Anchor::BottomRight;
}
bool IsBottom(Anchor a)
{
    return a == Anchor::BottomLeft || a == Anchor::Bottom || a == Anchor::BottomRight;
}

const Anchor ALL_ANCHORS[] = { Anchor::TopLeft,    Anchor::Top,    Anchor::TopRight,
                               Anchor::BottomLeft, Anchor::Bottom, Anchor::BottomRight };
}  // namespace

const char* DeskLayout::AnchorName(Anchor a)
{
    switch (a)
    {
        case Anchor::TopLeft: return "top_left";
        case Anchor::Top: return "top";
        case Anchor::TopRight: return "top_right";
        case Anchor::BottomLeft: return "bottom_left";
        case Anchor::Bottom: return "bottom";
        case Anchor::BottomRight: return "bottom_right";
    }
    return "top_left";
}

int DeskLayout::Add(const WindowSpec& spec, bool open)
{
    if (Find(spec.id) != NONE)
        return NONE;
    Entry e;
    e.spec = spec;
    e.open = open;
    e.z = ++nextZ_;
    e.anchor = spec.anchor;
    e.offset = { spec.place.x, spec.place.y };
    e.size = { spec.place.width, spec.place.height };
    if (Placed(e))
        Place(e);
    entries_.push_back(e);
    return (int)entries_.size() - 1;
}

int DeskLayout::Find(const std::string& id) const
{
    for (size_t i = 0; i < entries_.size(); i++)
        if (entries_[i].spec.id == id)
            return (int)i;
    return NONE;
}

void DeskLayout::SetOpen(int h, bool open)
{
    Entry& e = entries_[h];
    if (e.open == open)
        return;
    e.open = open;
    if (e.spec.persist)
        changed_ = true;
}

void DeskLayout::Raise(int h)
{
    entries_[h].z = ++nextZ_;
}

std::vector<int> DeskLayout::Order() const
{
    std::vector<int> order(entries_.size());
    for (size_t i = 0; i < order.size(); i++)
        order[i] = (int)i;
    std::sort(order.begin(), order.end(),
              [this](int a, int b)
              {
                  const Entry &ea = entries_[a], &eb = entries_[b];
                  if (ea.spec.layer != eb.spec.layer)
                      return ea.spec.layer < eb.spec.layer;
                  return ea.z < eb.z;
              });
    return order;
}

void DeskLayout::SetScreen(float width, float height)
{
    if (width == screenW_ && height == screenH_)
        return;
    screenW_ = width;
    screenH_ = height;
    for (Entry& e : entries_)
        if (Placed(e))
            Place(e);
}

Rectangle DeskLayout::Reachable(Rectangle r) const
{
    r.x = std::clamp(r.x, -r.width + GRAB_MARGIN, std::max(screenW_ - GRAB_MARGIN, 0.0f));
    r.y = std::clamp(r.y, 0.0f, std::max(screenH_ - (float)TITLE_HEIGHT, 0.0f));
    return r;
}

void DeskLayout::Place(Entry& e) const
{
    Rectangle r{ 0.0f, 0.0f, e.size.x, e.size.y };
    if (IsLeft(e.anchor))
        r.x = e.offset.x;
    else if (IsRight(e.anchor))
        r.x = screenW_ - e.offset.x - r.width;
    else
        r.x = screenW_ * 0.5f + e.offset.x - r.width * 0.5f;
    r.y = IsBottom(e.anchor) ? screenH_ - e.offset.y - r.height : e.offset.y;
    e.rect = Reachable(r);
}

void DeskLayout::Unplace(Entry& e) const
{
    const Rectangle r = e.rect;
    if (IsLeft(e.anchor))
        e.offset.x = r.x;
    else if (IsRight(e.anchor))
        e.offset.x = screenW_ - r.x - r.width;
    else
        e.offset.x = r.x + r.width * 0.5f - screenW_ * 0.5f;
    e.offset.y = IsBottom(e.anchor) ? screenH_ - r.y - r.height : r.y;
    e.size = { r.width, r.height };
}

void DeskLayout::SetRect(int h, Rectangle r)
{
    Entry& e = entries_[h];
    e.rect = r;
    if (Placed(e))
    {
        e.rect = Reachable(r);  // a drag cannot lose a window off the edge
        Unplace(e);
    }
}

void DeskLayout::Settle(int h)
{
    Entry& e = entries_[h];
    if (!Placed(e))
        return;
    // Thirds across, so a window left in the middle keeps to the middle; halves down.
    const float cx = e.rect.x + e.rect.width * 0.5f;
    const float cy = e.rect.y + e.rect.height * 0.5f;
    const int   col = cx < screenW_ / 3.0f ? 0 : (cx > screenW_ * 2.0f / 3.0f ? 2 : 1);
    const bool  bottom = cy > screenH_ * 0.5f;
    e.anchor = ALL_ANCHORS[(bottom ? 3 : 0) + col];
    Unplace(e);
    if (e.spec.persist)
        changed_ = true;
}

void DeskLayout::Reset()
{
    for (Entry& e : entries_)
    {
        if (!Placed(e))
            continue;
        e.anchor = e.spec.anchor;
        e.offset = { e.spec.place.x, e.spec.place.y };
        e.size = { e.spec.place.width, e.spec.place.height };
        Place(e);
        if (e.spec.persist)
            changed_ = true;
    }
}

int DeskLayout::HitTest(Vector2 p) const
{
    const std::vector<int> order = Order();
    for (auto it = order.rbegin(); it != order.rend(); ++it)
    {
        const Entry& e = entries_[*it];
        if (e.open && e.spec.layer != Layer::World && p.x >= e.rect.x &&
            p.x < e.rect.x + e.rect.width && p.y >= e.rect.y && p.y < e.rect.y + e.rect.height)
            return *it;
    }
    return NONE;
}

int DeskLayout::UpdateOwner(Vector2 mouse, bool anyButtonDown)
{
    // Held: the owner keeps it -- the world included -- unless it has gone away meanwhile.
    const bool keep = captured_ && anyButtonDown && (owner_ == NONE || entries_[owner_].open);
    if (!keep)
        owner_ = HitTest(mouse);
    captured_ = anyButtonDown;
    return owner_;
}

int DeskLayout::Escape()
{
    const std::vector<int> order = Order();
    for (auto it = order.rbegin(); it != order.rend(); ++it)
    {
        Entry& e = entries_[*it];
        if (!e.open || e.spec.layer == Layer::World)
            continue;
        if (e.spec.esc == EscRule::Block)
            return NONE;
        if (e.spec.esc == EscRule::Close)
        {
            SetOpen(*it, false);
            return *it;
        }
    }
    return NONE;
}

bool DeskLayout::TakeChanged()
{
    const bool c = changed_;
    changed_ = false;
    return c;
}

nlohmann::json DeskLayout::Save() const
{
    nlohmann::json out = nlohmann::json::object();
    for (const Entry& e : entries_)
    {
        if (!e.spec.persist)
            continue;
        out[e.spec.id] = { { "open", e.open },
                           { "anchor", AnchorName(e.anchor) },
                           { "offset", { e.offset.x, e.offset.y } },
                           { "size", { e.size.x, e.size.y } } };
    }
    return out;
}

void DeskLayout::Load(const nlohmann::json& windows)
{
    if (!windows.is_object())
        return;
    auto pair = [](const nlohmann::json& j, Vector2& v)
    {
        if (!j.is_array() || j.size() != 2 || !j[0].is_number() || !j[1].is_number())
            return false;
        v = { j[0].get<float>(), j[1].get<float>() };
        return true;
    };
    for (Entry& e : entries_)
    {
        if (!e.spec.persist)
            continue;
        const auto it = windows.find(e.spec.id);
        if (it == windows.end() || !it->is_object())
            continue;
        const nlohmann::json& w = *it;
        if (w.contains("open") && w["open"].is_boolean())
            e.open = w["open"].get<bool>();
        if (Placed(e))
        {
            if (w.contains("anchor") && w["anchor"].is_string())
                for (Anchor a : ALL_ANCHORS)
                    if (w["anchor"].get<std::string>() == AnchorName(a))
                        e.anchor = a;
            if (w.contains("offset"))
                pair(w["offset"], e.offset);
            Vector2 size;
            if (e.spec.resizable && w.contains("size") && pair(w["size"], size) && size.x > 0.0f &&
                size.y > 0.0f)
                e.size = size;
            Place(e);
        }
    }
}

bool DeskLayout::ReadFile(const nlohmann::json& file, const std::string& account, DeskLayout& into,
                          std::string& error)
{
    if (!file.is_object())
    {
        error = "not a JSON object";
        return false;
    }
    const int version = file.contains("version") && file["version"].is_number_integer()
                            ? file["version"].get<int>()
                            : 0;
    if (version > VERSION)
    {
        error = "written by a newer build (version " + std::to_string(version) + ")";
        return false;
    }
    const auto accounts = file.find("accounts");
    if (accounts != file.end() && accounts->is_object())
    {
        const auto mine = accounts->find(account);
        if (mine != accounts->end())
            into.Load(*mine);
    }
    return true;
}

void DeskLayout::WriteFile(nlohmann::json& file, const std::string& account, const DeskLayout& from)
{
    if (!file.is_object())
        file = nlohmann::json::object();
    file["version"] = VERSION;
    if (!file.contains("accounts") || !file["accounts"].is_object())
        file["accounts"] = nlohmann::json::object();
    file["accounts"][account] = from.Save();
}

}  // namespace Ui
