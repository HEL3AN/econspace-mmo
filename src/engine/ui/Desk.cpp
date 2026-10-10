#include "ui/Desk.h"
#include "ui/Theme.h"

#include <fstream>

namespace Ui
{

namespace
{
constexpr double DOUBLE_CLICK = 0.35;  // seconds between two presses on a title
}

void Desk::AddWindow(const WindowSpec& spec, const std::string& title, bool open,
                     Window::Content content)
{
    if (layout_.Add(spec, open) == DeskLayout::NONE)
    {
        TraceLog(LOG_WARNING, "Desk: window id '%s' registered twice", spec.id.c_str());
        return;
    }
    Item it;
    it.window = std::make_unique<Window>(title, std::move(content));
    items_.push_back(std::move(it));
}

void Desk::AddSurface(const WindowSpec& spec, bool open, Surface surface)
{
    if (layout_.Add(spec, open) == DeskLayout::NONE)
    {
        TraceLog(LOG_WARNING, "Desk: window id '%s' registered twice", spec.id.c_str());
        return;
    }
    Item it;
    it.surface = std::move(surface);
    items_.push_back(std::move(it));
}

void Desk::BeginFrame()
{
    focus_.EndFrame();  // a field that was not drawn last frame lets the keyboard go
    layout_.SetTitleHeight(CurrentTheme().metrics.titleHeight);
    layout_.SetSnapDistance(CurrentTheme().metrics.snap);
    layout_.SetScreen((float)GetScreenWidth(), (float)GetScreenHeight());
    layout_.SetUnit(Scale());  // a larger interface has larger windows (#297)
    for (int h = 0; h < (int)items_.size(); h++)
    {
        const Surface& s = items_[h].surface;
        if (items_[h].window)
            continue;
        if (s.isOpen)
        {
            const bool open = s.isOpen();
            if (open && !layout_.IsOpen(h))
                layout_.Raise(h);  // what has just opened is in front of its layer
            layout_.SetOpen(h, open);
        }
        if (layout_.IsOpen(h) && s.bounds)
            layout_.SetRect(h, s.bounds());
    }
    const bool down = IsMouseButtonDown(MOUSE_BUTTON_LEFT) ||
                      IsMouseButtonDown(MOUSE_BUTTON_RIGHT) ||
                      IsMouseButtonDown(MOUSE_BUTTON_MIDDLE);
    layout_.UpdateOwner(GetMousePosition(), down);
}

void Desk::HandleMouse()
{
    const Vector2 m = GetMousePosition();
    // A press lets go of the keyboard; a press on the field that held it takes it straight
    // back while the field is drawn, and that is not counted as a loss.
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || IsMouseButtonPressed(MOUSE_BUTTON_RIGHT))
        focus_.Release(Focus::Blur::Elsewhere);
    const bool held = IsMouseButtonDown(MOUSE_BUTTON_LEFT);

    // A tab held down comes away once the cursor leaves the title bar with it: from then on
    // it is a window of its own, dragged from where its tab was grabbed.
    if (tabPress_ != DeskLayout::NONE)
    {
        const Rectangle b = layout_.Rect(tabPress_);
        const Rectangle bar = Window::TitleBar(b);
        const Rectangle zone{ bar.x, bar.y - bar.height * 0.5f, bar.width, bar.height * 2.0f };
        if (!held || !layout_.IsOpen(tabPress_))
            tabPress_ = DeskLayout::NONE;
        else if (!CheckCollisionPointRec(m, zone))
        {
            const int h = tabPress_;
            tabPress_ = DeskLayout::NONE;
            layout_.Unstack(h);
            layout_.SetRect(h, { m.x - dragOffset_.x, m.y - dragOffset_.y, b.width, b.height });
            layout_.Raise(h);
            if (layout_.BeginMove(h, true))
                dragging_ = h;
        }
    }

    if (dragging_ != DeskLayout::NONE)
    {
        if (!held || !layout_.IsOpen(dragging_))
        {
            if (resizing_)
                layout_.Settle(dragging_);
            else
                layout_.EndMove(m);  // stacked, grouped or left alone
            dragging_ = DeskLayout::NONE;
            resizing_ = false;
        }
        else if (resizing_)
        {
            // dragOffset_ is where the press was inside the grip, from the bottom right.
            const Rectangle r = layout_.Rect(dragging_);
            layout_.Resize(dragging_, m.x + dragOffset_.x - r.x, m.y + dragOffset_.y - r.y);
        }
        else
            layout_.MoveTo({ m.x - dragOffset_.x, m.y - dragOffset_.y });
    }

    const int owner = layout_.Owner();
    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || owner == DeskLayout::NONE ||
        !items_[owner].window)
        return;
    layout_.Raise(owner);
    const Rectangle b = layout_.Rect(owner);
    const bool      pinned = layout_.Pinned(owner);
    if (CheckCollisionPointRec(m, Window::CloseButton(b)))
        Close(owner);
    else if (CheckCollisionPointRec(m, Window::PinButton(b)))
        layout_.SetPinned(owner, !pinned);
    else if (CheckCollisionPointRec(m, Window::CollapseButton(b)))
        layout_.SetCollapsed(owner, !layout_.Collapsed(owner));
    else if (layout_.Spec(owner).resizable && !pinned && !layout_.Collapsed(owner) &&
             CheckCollisionPointRec(m, Window::ResizeGrip(b)))
    {
        dragging_ = owner;
        resizing_ = true;
        dragOffset_ = { b.x + b.width - m.x, b.y + b.height - m.y };
    }
    else if (CheckCollisionPointRec(m, Window::TitleBar(b)))
    {
        // A tab: brought to the front, and held -- pulled far enough, it comes away.
        const std::vector<int> tabs = layout_.Tabs(owner);
        if (tabs.size() >= 2)
            for (int i = 0; i < (int)tabs.size(); i++)
            {
                const Rectangle tr = Window::TabRect(b, i, (int)tabs.size());
                if (!CheckCollisionPointRec(m, tr))
                    continue;
                layout_.Raise(tabs[i]);
                if (!pinned)
                {
                    tabPress_ = tabs[i];
                    dragOffset_ = { m.x - tr.x, m.y - b.y };
                }
                return;
            }
        // The rest of the bar: a double click folds the window to its title, a drag moves it.
        const double now = GetTime();
        if (lastTitle_ == owner && now - lastTitleTime_ < DOUBLE_CLICK)
        {
            layout_.SetCollapsed(owner, !layout_.Collapsed(owner));
            lastTitle_ = DeskLayout::NONE;
            return;
        }
        lastTitle_ = owner;
        lastTitleTime_ = now;
        const bool alone = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
        if (layout_.BeginMove(owner, alone))  // a pinned window does not move
        {
            dragging_ = owner;
            dragOffset_ = { m.x - b.x, m.y - b.y };
        }
    }
}

void Desk::Toggle(const std::string& id)
{
    const int h = layout_.Find(id);
    if (h == DeskLayout::NONE)
        return;
    // Open behind another tab of its stack: the player cannot see it, so the button brings
    // it forward rather than closing what they were looking for.
    if (layout_.IsOpen(h) && !layout_.Shown(h) && !layout_.Covered(h))
    {
        layout_.Raise(h);
        return;
    }
    SetOpen(id, !layout_.IsOpen(h));
}

void Desk::Close(int h)
{
    if (!layout_.IsOpen(h))
        return;
    layout_.SetOpen(h, false);
    focus_.ReleaseOwner(layout_.Spec(h).id, Focus::Blur::Elsewhere);
    if (items_[h].surface.onClose)
        items_[h].surface.onClose();
}

bool Desk::Escape()
{
    if (focus_.Taken())
    {
        focus_.Release(Focus::Blur::Cancel);
        return true;
    }
    const int h = layout_.Escape();
    if (h == DeskLayout::NONE)
        return false;
    focus_.ReleaseOwner(layout_.Spec(h).id, Focus::Blur::Elsewhere);
    if (items_[h].surface.onClose)
        items_[h].surface.onClose();
    return true;
}

void Desk::Draw(Layer layer)
{
    const int owner = layout_.Owner();
    for (int h : layout_.Order())
    {
        if (layout_.Spec(h).layer != layer || layout_.Covered(h))
            continue;
        const Item& it = items_[h];
        // A popup opened during this frame's input is drawn this frame, not the next.
        if (!(it.surface.isOpen ? it.surface.isOpen() : layout_.IsOpen(h)))
            continue;
        if (it.window)
        {
            if (!layout_.Shown(h))
                continue;  // a tab behind the one in front of its stack
            Window::Chrome c;
            c.owner = owner == h;
            c.resizable = layout_.Spec(h).resizable;
            c.pinned = layout_.Pinned(h);
            c.collapsed = layout_.Collapsed(h);
            const std::vector<int> tabs = layout_.Tabs(h);
            if (tabs.size() >= 2)
                for (int t : tabs)
                {
                    if (t == h)
                        c.activeTab = (int)c.tabs.size();
                    c.tabs.push_back(items_[t].window ? items_[t].window->Title() : "");
                }
            c.dropTarget = layout_.Moving() && layout_.DropTarget(GetMousePosition()) == h;
            it.window->Draw(layout_.Rect(h), c, &focus_, layout_.Spec(h).id);
        }
        else if (it.surface.draw)
        {
            MouseScope scope(owner == h);
            it.surface.draw();
        }
    }
}

bool Desk::IsOpen(const std::string& id) const
{
    const int h = layout_.Find(id);
    return h != DeskLayout::NONE && layout_.IsOpen(h);
}

void Desk::SetOpen(const std::string& id, bool open)
{
    const int h = layout_.Find(id);
    if (h == DeskLayout::NONE)
        return;
    if (!open)
    {
        Close(h);
        return;
    }
    if (!layout_.IsOpen(h))
        layout_.Raise(h);
    layout_.SetOpen(h, true);
}

Frame Desk::SurfaceFrame(const std::string& id, Rectangle area)
{
    const int              h = layout_.Find(id);
    const std::string_view key = h == DeskLayout::NONE ? std::string_view() : layout_.Spec(h).id;
    return Frame(area, Owns(id), &focus_, key);
}

bool Desk::Owns(const std::string& id) const
{
    const int h = layout_.Owner();
    return h != DeskLayout::NONE && layout_.Spec(h).id == id;
}

std::vector<Desk::MenuSlot> Desk::MenuSlots() const
{
    std::vector<MenuSlot> slots;
    for (int h = 0; h < layout_.Count(); h++)
        if (!layout_.Spec(h).menuLabel.empty())
            slots.push_back({ layout_.Spec(h).id, layout_.Spec(h).menuLabel, layout_.IsOpen(h) });
    return slots;
}

void Desk::UseLayoutFile(const std::string& path, const std::string& account)
{
    file_ = path;
    account_ = account;
    canSave_ = true;
    std::ifstream in(path);
    if (!in)
        return;  // nothing saved yet
    nlohmann::json file = nlohmann::json::parse(in, nullptr, false);
    if (file.is_discarded())
    {
        // A layout is a preference, not a save: an unreadable one is replaced.
        TraceLog(LOG_WARNING, "UI layout: %s is not valid JSON; it will be rewritten",
                 path.c_str());
        return;
    }
    std::string error;
    if (!DeskLayout::ReadFile(file, account, layout_, error))
    {
        TraceLog(LOG_WARNING, "UI layout: %s: %s; not used, not overwritten", path.c_str(),
                 error.c_str());
        canSave_ = false;
    }
    layout_.TakeChanged();  // what was just read is not a change to write back
}

void Desk::Persist()
{
    if (!layout_.TakeChanged() || !canSave_ || file_.empty())
        return;
    // Read again rather than kept: another client on this machine may have written its
    // own account since.
    nlohmann::json file;
    {
        std::ifstream in(file_);
        if (in)
            file = nlohmann::json::parse(in, nullptr, false);
    }
    if (file.is_discarded() || !file.is_object())
        file = nlohmann::json::object();
    DeskLayout::WriteFile(file, account_, layout_);
    std::ofstream out(file_);
    if (!out)
    {
        TraceLog(LOG_WARNING, "UI layout: cannot write %s", file_.c_str());
        return;
    }
    out << file.dump(2) << '\n';
}

}  // namespace Ui
