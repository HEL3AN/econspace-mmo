#include "ui/DeskLayout.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace Ui
{

namespace
{
// How much of a window has to stay on the screen for it to be dragged back: its title bar
// vertically, this much of it horizontally. The rule a drag already followed.
constexpr float GRAB_MARGIN = 60.0f;
// Two edges this close (pixels) are the same edge: what a snap leaves behind is exact, but
// a window placed from another anchor can be a rounding away.
constexpr float TOUCH = 1.0f;

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

bool Contains(const std::vector<int>& v, int x)
{
    return std::find(v.begin(), v.end(), x) != v.end();
}
}  // namespace

Vector2 SnapOffset(const std::vector<Rectangle>& moving, const std::vector<Rectangle>& others,
                   float screenW, float screenH, float reach)
{
    Vector2 best{ 0.0f, 0.0f };
    bool    haveX = false, haveY = false;
    auto    tryX = [&](float d)
    {
        if (std::fabs(d) <= reach && (!haveX || std::fabs(d) < std::fabs(best.x)))
        {
            best.x = d;
            haveX = true;
        }
    };
    auto tryY = [&](float d)
    {
        if (std::fabs(d) <= reach && (!haveY || std::fabs(d) < std::fabs(best.y)))
        {
            best.y = d;
            haveY = true;
        }
    };
    for (const Rectangle& m : moving)
    {
        const float mr = m.x + m.width, mb = m.y + m.height;
        tryX(-m.x);
        tryX(screenW - mr);
        tryY(-m.y);
        tryY(screenH - mb);
        for (const Rectangle& o : others)
        {
            const float orr = o.x + o.width, ob = o.y + o.height;
            // Only an edge the window could come to lie along: beside it, near enough.
            if (m.y < ob + reach && o.y < mb + reach)
            {
                tryX(orr - m.x);  // its left to their right: beside
                tryX(o.x - mr);   // its right to their left
                tryX(o.x - m.x);  // lefts in line
                tryX(orr - mr);   // rights in line
            }
            if (m.x < orr + reach && o.x < mr + reach)
            {
                tryY(ob - m.y);
                tryY(o.y - mb);
                tryY(o.y - m.y);
                tryY(ob - mb);
            }
        }
    }
    return best;
}

bool Touching(Rectangle a, Rectangle b)
{
    const float overlapY = std::min(a.y + a.height, b.y + b.height) - std::max(a.y, b.y);
    const float overlapX = std::min(a.x + a.width, b.x + b.width) - std::max(a.x, b.x);
    const bool  besides =
        std::fabs(a.x + a.width - b.x) <= TOUCH || std::fabs(b.x + b.width - a.x) <= TOUCH;
    const bool above =
        std::fabs(a.y + a.height - b.y) <= TOUCH || std::fabs(b.y + b.height - a.y) <= TOUCH;
    return (besides && overlapY > TOUCH) || (above && overlapX > TOUCH);
}

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
    const int h = (int)entries_.size() - 1;
    if (!Available(h))
    {
        entries_[h].held = open;
        entries_[h].open = false;
    }
    return h;
}

int DeskLayout::Find(const std::string& id) const
{
    for (size_t i = 0; i < entries_.size(); i++)
        if (entries_[i].spec.id == id)
            return (int)i;
    return NONE;
}

void DeskLayout::Touched(const Entry& e)
{
    if (e.spec.persist)
        changed_ = true;
}

void DeskLayout::SetOpen(int h, bool open)
{
    Entry& e = entries_[h];
    if (!Available(h))
    {
        if (e.held != open)
        {
            e.held = open;
            Touched(e);
        }
        return;
    }
    if (e.open == open)
        return;
    e.open = open;
    Touched(e);
}

void DeskLayout::SetContext(const std::string& name, bool active)
{
    if (name.empty() || ContextActive(name) == active)
        return;
    if (active)
        contexts_.push_back(name);
    else
        contexts_.erase(std::find(contexts_.begin(), contexts_.end(), name));
    for (int h = 0; h < (int)entries_.size(); h++)
    {
        Entry& e = entries_[h];
        if (e.spec.context != name)
            continue;
        if (!active)
        {
            e.held = e.open;
            e.open = false;
            if (move_.h != NONE && Contains(move_.windows, h))
                move_ = Move();  // the drag is let go of where it is
        }
        else if (e.held)
        {
            e.open = true;
            e.held = false;
            Raise(h);
        }
    }
}

bool DeskLayout::ContextActive(const std::string& name) const
{
    return std::find(contexts_.begin(), contexts_.end(), name) != contexts_.end();
}

bool DeskLayout::Available(int h) const
{
    const std::string& c = entries_[h].spec.context;
    return c.empty() || ContextActive(c);
}

void DeskLayout::Raise(int h)
{
    // A group comes forward together, each keeping its place among the others -- a stack's
    // hidden tabs stay behind its front one -- and h in front of them all.
    std::vector<int> group = Group(h);
    std::sort(group.begin(), group.end(),
              [this](int a, int b) { return entries_[a].z < entries_[b].z; });
    for (int g : group)
        if (g != h)
            entries_[g].z = ++nextZ_;
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
    r.y = std::clamp(r.y, 0.0f, std::max(screenH_ - TitlePx(), 0.0f));
    return r;
}

void DeskLayout::SetUnit(float pixels)
{
    if (pixels <= 0.0f || pixels == unit_)
        return;
    unit_ = pixels;
    for (Entry& e : entries_)
        if (Placed(e))
            Place(e);
}

void DeskLayout::Place(Entry& e) const
{
    const Vector2 off{ e.offset.x * unit_, e.offset.y * unit_ };
    Rectangle     r{ 0.0f, 0.0f, e.size.x * unit_, e.size.y * unit_ };
    if (IsLeft(e.anchor))
        r.x = off.x;
    else if (IsRight(e.anchor))
        r.x = screenW_ - off.x - r.width;
    else
        r.x = screenW_ * 0.5f + off.x - r.width * 0.5f;
    r.y = IsBottom(e.anchor) ? screenH_ - off.y - r.height : off.y;
    // Collapsed, the title bar stays where the top of the window was.
    if (e.collapsed)
        r.height = TitlePx();
    e.rect = Reachable(r);
}

void DeskLayout::Unplace(Entry& e) const
{
    const Rectangle r = e.rect;
    // A collapsed window keeps the height it opens out to.
    const float height = e.collapsed ? e.size.y * unit_ : r.height;
    if (IsLeft(e.anchor))
        e.offset.x = r.x;
    else if (IsRight(e.anchor))
        e.offset.x = screenW_ - r.x - r.width;
    else
        e.offset.x = r.x + r.width * 0.5f - screenW_ * 0.5f;
    e.offset.y = IsBottom(e.anchor) ? screenH_ - r.y - height : r.y;
    e.offset = { e.offset.x / unit_, e.offset.y / unit_ };
    e.size = { r.width / unit_, height / unit_ };
}

void DeskLayout::SetRect(int h, Rectangle r)
{
    for (int m : FrameOf(h))  // the tabs of a stack share one place
    {
        Entry& e = entries_[m];
        e.rect = r;
        if (Placed(e))
        {
            e.rect = Reachable(r);  // a drag cannot lose a window off the edge
            Unplace(e);
        }
    }
}

void DeskLayout::Resize(int h, float width, float height)
{
    Entry& e = entries_[h];
    if (!Placed(e) || !e.spec.resizable || e.pinned || e.collapsed)
        return;
    Rectangle     r = e.rect;
    const Vector2 min{ e.spec.minSize.x * unit_, e.spec.minSize.y * unit_ };
    r.width = std::clamp(width, min.x, std::max(screenW_, min.x));
    r.height = std::clamp(height, min.y, std::max(screenH_, min.y));
    SetRect(h, r);
}

Anchor DeskLayout::AnchorFor(Rectangle r) const
{
    // Thirds across, so a window left in the middle keeps to the middle; halves down.
    const float cx = r.x + r.width * 0.5f;
    const float cy = r.y + r.height * 0.5f;
    const int   col = cx < screenW_ / 3.0f ? 0 : (cx > screenW_ * 2.0f / 3.0f ? 2 : 1);
    const bool  bottom = cy > screenH_ * 0.5f;
    return ALL_ANCHORS[(bottom ? 3 : 0) + col];
}

void DeskLayout::SettleAll(const std::vector<int>& windows)
{
    // One anchor for the lot, from where the lot is: a group whose windows kept to different
    // corners would come apart the first time the screen changed size.
    bool      any = false;
    Rectangle box{};
    for (int w : windows)
    {
        const Entry& e = entries_[w];
        if (!Placed(e))
            continue;
        if (!any)
            box = e.rect;
        else
        {
            const float x1 = std::max(box.x + box.width, e.rect.x + e.rect.width);
            const float y1 = std::max(box.y + box.height, e.rect.y + e.rect.height);
            box.x = std::min(box.x, e.rect.x);
            box.y = std::min(box.y, e.rect.y);
            box.width = x1 - box.x;
            box.height = y1 - box.y;
        }
        any = true;
    }
    if (!any)
        return;
    const Anchor a = AnchorFor(box);
    for (int w : windows)
    {
        Entry& e = entries_[w];
        if (!Placed(e))
            continue;
        e.anchor = a;
        Unplace(e);
        Touched(e);
    }
}

void DeskLayout::Settle(int h)
{
    SettleAll(Group(h));
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
        e.stack = e.group = 0;
        e.pinned = e.collapsed = false;
        Place(e);
        Touched(e);
    }
}

bool DeskLayout::Covered(int h) const
{
    const Layer layer = entries_[h].spec.layer;
    for (const Entry& e : entries_)
        if (e.open && e.spec.covers && e.spec.layer > layer)
            return true;
    return false;
}

int DeskLayout::HitTest(Vector2 p) const
{
    const std::vector<int> order = Order();
    for (auto it = order.rbegin(); it != order.rend(); ++it)
    {
        const Entry& e = entries_[*it];
        if (e.spec.layer != Layer::World && Shown(*it) && p.x >= e.rect.x &&
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
        if (!e.open || e.spec.layer == Layer::World || e.pinned)
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

// --- Window behaviour ---------------------------------------------------------------------

void DeskLayout::SetPinned(int h, bool pinned)
{
    for (int m : FrameOf(h))
    {
        entries_[m].pinned = pinned && Placed(entries_[m]);
        Touched(entries_[m]);
    }
}

void DeskLayout::SetCollapsed(int h, bool collapsed)
{
    for (int m : FrameOf(h))
    {
        Entry& e = entries_[m];
        if (!Placed(e) || e.collapsed == collapsed)
            continue;
        e.collapsed = collapsed;
        Place(e);
        Touched(e);
    }
}

std::vector<int> DeskLayout::FrameOf(int h) const
{
    const int stack = entries_[h].stack;
    if (stack == 0)
        return { h };
    std::vector<int> frame;
    for (int i = 0; i < (int)entries_.size(); i++)
        if (entries_[i].stack == stack)
            frame.push_back(i);
    std::sort(frame.begin(), frame.end(),
              [this](int a, int b) { return entries_[a].tab < entries_[b].tab; });
    return frame;
}

std::vector<int> DeskLayout::Tabs(int h) const
{
    std::vector<int> tabs;
    for (int m : FrameOf(h))
        if (entries_[m].open)
            tabs.push_back(m);
    return tabs;
}

int DeskLayout::ActiveTab(int h) const
{
    int active = NONE;
    for (int m : FrameOf(h))
        if (entries_[m].open && (active == NONE || entries_[m].z > entries_[active].z))
            active = m;
    return active;
}

bool DeskLayout::Shown(int h) const
{
    return entries_[h].open && !Covered(h) && ActiveTab(h) == h;
}

std::vector<int> DeskLayout::Group(int h) const
{
    const int group = entries_[h].group;
    if (group == 0)
        return FrameOf(h);
    std::vector<int> all;
    for (int i = 0; i < (int)entries_.size(); i++)
        if (entries_[i].group == group)
            all.push_back(i);
    return all;
}

void DeskLayout::Dissolve()
{
    // A stack of one is a window on its own.
    std::map<int, int> stacks;
    for (const Entry& e : entries_)
        if (e.stack != 0)
            stacks[e.stack]++;
    for (Entry& e : entries_)
        if (e.stack != 0 && stacks[e.stack] < 2)
            e.stack = 0;
    // A group of one frame is no group. A frame is a stack, or a window on its own.
    std::map<int, std::vector<int>> frames;
    for (int i = 0; i < (int)entries_.size(); i++)
    {
        const Entry& e = entries_[i];
        if (e.group == 0)
            continue;
        const int key = e.stack != 0 ? e.stack : -(i + 1);
        if (!Contains(frames[e.group], key))
            frames[e.group].push_back(key);
    }
    for (Entry& e : entries_)
        if (e.group != 0 && frames[e.group].size() < 2)
            e.group = 0;
}

void DeskLayout::Ungroup(int h)
{
    for (int m : FrameOf(h))
    {
        entries_[m].group = 0;
        Touched(entries_[m]);
    }
    Dissolve();
}

bool DeskLayout::Stack(int h, int onto)
{
    if (h == onto || h == NONE || onto == NONE)
        return false;
    const Entry& target = entries_[onto];
    if (!Placed(entries_[h]) || !Placed(target) || entries_[h].spec.layer != target.spec.layer)
        return false;
    const std::vector<int> moving = FrameOf(h);
    if (Contains(moving, onto))
        return false;
    if (target.stack == 0)
    {
        entries_[onto].stack = ++nextStack_;
        entries_[onto].tab = ++nextTab_;
    }
    for (int m : moving)
    {
        Entry&       e = entries_[m];
        const Entry& t = entries_[onto];
        e.stack = t.stack;
        e.tab = ++nextTab_;
        e.group = t.group;  // it leaves its own group for the one it is now part of
        e.pinned = t.pinned;
        e.collapsed = t.collapsed;
        e.anchor = t.anchor;
        e.offset = t.offset;
        e.size = t.size;
        e.rect = t.rect;
        Touched(e);
    }
    Touched(entries_[onto]);
    Dissolve();
    Raise(h);  // what was dropped is the tab in front
    return true;
}

void DeskLayout::Unstack(int h)
{
    Entry& e = entries_[h];
    if (e.stack == 0)
        return;
    e.stack = 0;
    e.group = 0;
    Touched(e);
    Dissolve();
}

bool DeskLayout::BeginMove(int h, bool alone)
{
    move_ = Move();
    if (h == NONE || !Placed(entries_[h]) || entries_[h].pinned)
        return false;
    bool anchored = false;  // something in its group is pinned, so the group stays
    for (int g : Group(h))
        anchored = anchored || entries_[g].pinned;
    if (alone || anchored)
        Ungroup(h);
    move_.h = h;
    move_.windows = Group(h);
    for (int w : move_.windows)
        move_.start.push_back(entries_[w].rect);
    return true;
}

void DeskLayout::MoveTo(Vector2 topLeft)
{
    if (!Moving())
        return;
    const size_t self =
        std::find(move_.windows.begin(), move_.windows.end(), move_.h) - move_.windows.begin();
    const Vector2 d{ topLeft.x - move_.start[self].x, topLeft.y - move_.start[self].y };

    // Snap what can be seen of the moving windows to what can be seen of the rest.
    std::vector<Rectangle> moving;
    for (size_t i = 0; i < move_.windows.size(); i++)
        if (Shown(move_.windows[i]))
            moving.push_back({ move_.start[i].x + d.x, move_.start[i].y + d.y, move_.start[i].width,
                               move_.start[i].height });
    std::vector<Rectangle> others;
    const Layer            layer = entries_[move_.h].spec.layer;
    for (int i = 0; i < (int)entries_.size(); i++)
    {
        const Entry& e = entries_[i];
        if (Contains(move_.windows, i) || !Shown(i))
            continue;
        if ((Placed(e) && e.spec.layer == layer) || e.spec.snapTarget)
            others.push_back(e.rect);
    }
    const Vector2 snap = SnapOffset(moving, others, screenW_, screenH_, snapUnits_ * unit_);

    for (size_t i = 0; i < move_.windows.size(); i++)
    {
        Entry&    e = entries_[move_.windows[i]];
        Rectangle r = move_.start[i];
        r.x += d.x + snap.x;
        r.y += d.y + snap.y;
        e.rect = Reachable(r);
        Unplace(e);
    }
}

int DeskLayout::DropTarget(Vector2 mouse) const
{
    if (!Moving() || move_.windows.size() != FrameOf(move_.h).size())
        return NONE;  // a group is not stacked onto anything
    const Layer            layer = entries_[move_.h].spec.layer;
    const std::vector<int> order = Order();
    for (auto it = order.rbegin(); it != order.rend(); ++it)
    {
        const Entry&    e = entries_[*it];
        const Rectangle r = e.rect;
        if (Contains(move_.windows, *it) || !Shown(*it) || e.spec.layer == Layer::World)
            continue;
        if (mouse.x < r.x || mouse.x >= r.x + r.width || mouse.y < r.y || mouse.y >= r.y + r.height)
            continue;
        // The first thing under the cursor decides: a title bar behind another window is not
        // one the cursor is over.
        const bool title = mouse.y < r.y + TitlePx();
        return Placed(e) && e.spec.layer == layer && title ? *it : NONE;
    }
    return NONE;
}

int DeskLayout::EndMove(Vector2 mouse)
{
    if (!Moving())
        return NONE;
    const int        target = DropTarget(mouse);
    const int        h = move_.h;
    std::vector<int> moved = move_.windows;
    move_ = Move();
    if (target != NONE && Stack(h, target))
        return target;

    // Whatever it was let go against joins its group -- or the two groups become one.
    const Layer      layer = entries_[h].spec.layer;
    std::vector<int> joined = moved;
    int              group = entries_[h].group;
    for (int i = 0; i < (int)entries_.size(); i++)
    {
        const Entry& e = entries_[i];
        if (Contains(joined, i) || !Shown(i) || !Placed(e) || e.spec.layer != layer)
            continue;
        bool touches = false;
        for (int m : moved)
            touches = touches || (Shown(m) && Touching(entries_[m].rect, e.rect));
        if (!touches)
            continue;
        if (group == 0)
            group = e.group;
        for (int g : Group(i))
            if (!Contains(joined, g))
                joined.push_back(g);
    }
    if (joined.size() > moved.size())
    {
        if (group == 0)
            group = ++nextGroup_;
        for (int j : joined)
            entries_[j].group = group;
    }
    SettleAll(joined);
    Dissolve();
    return NONE;
}

// --- Saving -------------------------------------------------------------------------------

nlohmann::json DeskLayout::Save() const
{
    // A group or a stack is named by its first window's id: stable across sessions, which
    // a number handed out in the order things were dragged is not.
    auto firstId = [this](const std::vector<int>& windows)
    { return entries_[*std::min_element(windows.begin(), windows.end())].spec.id; };
    nlohmann::json out = nlohmann::json::object();
    for (int h = 0; h < (int)entries_.size(); h++)
    {
        const Entry& e = entries_[h];
        if (!e.spec.persist)
            continue;
        nlohmann::json w = { { "open", e.open || e.held },
                             { "anchor", AnchorName(e.anchor) },
                             { "offset", { e.offset.x, e.offset.y } },
                             { "size", { e.size.x, e.size.y } },
                             { "pinned", e.pinned },
                             { "collapsed", e.collapsed } };
        if (e.group != 0)
            w["group"] = firstId(Group(h));
        if (e.stack != 0)
        {
            const std::vector<int> frame = FrameOf(h);
            int                    front = frame.front();
            for (int m : frame)
                if (entries_[m].z > entries_[front].z)
                    front = m;
            w["stack"] = firstId(frame);
            w["tab"] = e.tab;
            w["front"] = front == h;
        }
        out[e.spec.id] = w;
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
    auto flag = [](const nlohmann::json& w, const char* key, bool& into)
    {
        if (w.contains(key) && w[key].is_boolean())
            into = w[key].get<bool>();
    };
    std::map<std::string, int> groups, stacks;
    std::vector<int>           fronts;
    for (int h = 0; h < (int)entries_.size(); h++)
    {
        Entry& e = entries_[h];
        if (!e.spec.persist)
            continue;
        const auto it = windows.find(e.spec.id);
        if (it == windows.end() || !it->is_object())
            continue;
        const nlohmann::json& w = *it;
        flag(w, "open", e.open);
        if (!Available(h))
        {
            e.held = e.open;
            e.open = false;
        }
        if (!Placed(e))
            continue;
        if (w.contains("anchor") && w["anchor"].is_string())
            for (Anchor a : ALL_ANCHORS)
                if (w["anchor"].get<std::string>() == AnchorName(a))
                    e.anchor = a;
        if (w.contains("offset"))
            pair(w["offset"], e.offset);
        Vector2 size;
        if (e.spec.resizable && w.contains("size") && pair(w["size"], size) && size.x > 0.0f &&
            size.y > 0.0f)
            e.size = { std::max(size.x, e.spec.minSize.x), std::max(size.y, e.spec.minSize.y) };
        flag(w, "pinned", e.pinned);
        flag(w, "collapsed", e.collapsed);
        // The file is the truth about the windows it names: no group in it is no group.
        e.group = e.stack = 0;
        if (w.contains("group") && w["group"].is_string())
        {
            int& g = groups[w["group"].get<std::string>()];
            if (g == 0)
                g = ++nextGroup_;
            e.group = g;
        }
        if (w.contains("stack") && w["stack"].is_string())
        {
            int& s = stacks[w["stack"].get<std::string>()];
            if (s == 0)
                s = ++nextStack_;
            e.stack = s;
            e.tab = w.contains("tab") && w["tab"].is_number_integer() ? w["tab"].get<int>() : 0;
            nextTab_ = std::max(nextTab_, e.tab);
            bool front = false;
            flag(w, "front", front);
            if (front)
                fronts.push_back(h);
        }
        Place(e);
    }
    Dissolve();

    // The tabs of a stack share one place: the one in front's, or the first tab's.
    for (int h = 0; h < (int)entries_.size(); h++)
    {
        if (entries_[h].stack == 0)
            continue;
        const std::vector<int> frame = FrameOf(h);
        int                    lead = frame.front();
        for (int f : fronts)
            if (Contains(frame, f))
                lead = f;
        if (h == lead)
            continue;
        Entry&       e = entries_[h];
        const Entry& t = entries_[lead];
        e.anchor = t.anchor;
        e.offset = t.offset;
        e.size = t.size;
        e.pinned = t.pinned;
        e.collapsed = t.collapsed;
        e.group = t.group;
        Place(e);
    }
    for (int f : fronts)
        if (entries_[f].stack != 0)
            Raise(f);
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
