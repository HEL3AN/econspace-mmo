#pragma once

#include "raylib.h"  // Rectangle, Vector2: plain structs, nothing here draws
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

// The half of the window manager that a test can hold (#297): which windows exist, which
// is in front of which, who owns the mouse this frame, what Esc closes, and where everything
// is kept between sessions. No raylib drawing call and no input call is made here -- the
// caller hands in the mouse and the screen size, so every rule below is a function of its
// arguments. `Desk` is the half that reads the mouse and draws.
namespace Ui
{

// Bottom to top. A window on a higher layer is in front of every window on a lower one,
// whatever order they were raised in; within a layer the last one raised is in front.
enum class Layer
{
    World,    // reserved: the world view itself is whatever no window claims
    Panels,   // the windows a player arranges: status, overview, radar...
    Screen,   // something that covers the world view: the map, the sensor screen, a station
    Modal,    // above the screens: the menu bar that opens them
    Popup,    // the context menu
    Overlay,  // F10's panel: above everything, never treated
};

// The corner or edge a window keeps its distance from. A window on the right stays on the
// right when the screen gets wider, which an absolute position cannot do.
enum class Anchor
{
    TopLeft,
    Top,  // horizontally: from the centre
    TopRight,
    BottomLeft,
    Bottom,
    BottomRight,
};

// What Esc does to a window when it is the top one open.
enum class EscRule
{
    Close,   // closes it
    Ignore,  // passes over it to the next one down (the menu bar)
    Block,   // stops there and closes nothing: leaving a station is an order to the server
};

struct WindowSpec
{
    std::string id;         // stable: the key for persistence and the menu bar
    std::string menuLabel;  // its button on the menu bar; empty for none
    Layer       layer = Layer::Panels;
    Anchor      anchor = Anchor::TopLeft;
    // Distance from the anchor (x, y) and size (width, height), in layout units: pixels at
    // a UI scale of 1 (SetUnit). Zero size means the desk does not place it: its owner says
    // where it is every frame (SetRect), in pixels.
    Rectangle place{ 0.0f, 0.0f, 0.0f, 0.0f };
    EscRule   esc = EscRule::Close;
    bool      persist = false;    // its place and open state are kept per account
    bool      resizable = false;  // a saved size is restored only if the player could make one
    Vector2   minSize{ 160.0f, 96.0f };  // units; a resizable window is never made smaller
    // While it is open, every window on a lower layer is hidden: it is drawn over the whole
    // view, and a panel showing through it is a panel nobody can use and that hides part of
    // it (the map, the sensor screen, the station).
    bool covers = false;
    // A surface whose edges a dragged window snaps to (the menu bar). A placed window always
    // is one; a surface that covers the view or comes and goes under the cursor is not.
    bool snapTarget = false;
    // Where it belongs: empty for everywhere, otherwise a context the game turns on and off
    // ("docked", "space"). Out of its context a window is closed and not on the menu bar --
    // a market is the station's, an overview is space's -- and whether it was open is
    // remembered, saved as it was, and given back when the context returns.
    std::string context;
};

// The smallest move, no longer than `reach` on either axis, that lays an edge of one of
// `moving` on an edge of the screen or of one of `others` -- beside it or in line with it.
// Each axis is decided on its own, so a window can snap into a corner. Pure: the drag and
// the tests call the same thing.
Vector2 SnapOffset(const std::vector<Rectangle>& moving, const std::vector<Rectangle>& others,
                   float screenW, float screenH, float reach);
// Two windows share an edge: one ends where the other begins, and they overlap along it.
bool Touching(Rectangle a, Rectangle b);

class DeskLayout
{
public:
    static constexpr int NONE = -1;
    static constexpr int VERSION = 1;  // of ui_layout.json; a newer one is not read

    // Registers a window and returns its handle; NONE if the id is already taken.
    int               Add(const WindowSpec& spec, bool open);
    int               Find(const std::string& id) const;
    int               Count() const { return (int)entries_.size(); }
    const WindowSpec& Spec(int h) const { return entries_[h].spec; }

    bool IsOpen(int h) const { return entries_[h].open; }
    // Out of its context, opening a window only promises to open it when the context returns.
    void SetOpen(int h, bool open);
    void Raise(int h);  // to the front of its layer

    // Turns a context on or off (WindowSpec::context). Off, its open windows close and are
    // remembered; on, they open again where they were. Nothing saved changes either way: a
    // context is where the player is, not something they arranged. Every context is off
    // until it is turned on.
    void SetContext(const std::string& name, bool active);
    bool ContextActive(const std::string& name) const;
    bool Available(int h) const;  // in a context that is on, or in none

    // Every window, bottom to top: by layer, then by when it was raised.
    std::vector<int> Order() const;
    // Open, but under something on a higher layer that covers the view: not drawn, and
    // never under the cursor.
    bool Covered(int h) const;

    // The screen the windows are placed on; a change re-places every placed window from
    // its anchor, so a resolution change keeps each one at its edge.
    void SetScreen(float width, float height);
    // How many pixels a layout unit is: the UI scale (#297). A change re-places every placed
    // window, so a larger interface has larger windows; what is saved stays in units, and
    // a layout made at one scale is the same layout at another.
    void      SetUnit(float pixels);
    float     Unit() const { return unit_; }
    Rectangle Rect(int h) const { return entries_[h].rect; }
    // Where a window is now: dragged, or drawn somewhere its owner decided. Keeps its anchor;
    // a placed window is kept reachable.
    void SetRect(int h, Rectangle r);
    // The player is dragging its corner: a new size with the top left kept, no smaller than
    // the spec's minimum and no larger than the screen.
    void Resize(int h, float width, float height);
    // A drag has ended: the window takes the anchor nearest to where it was left, so it
    // stays in that corner from then on.
    void Settle(int h);
    void Reset();  // every placed window back where its spec puts it

    // The open window in front at a point, or NONE for the world.
    int HitTest(Vector2 p) const;
    // Chooses this frame's owner of the mouse. A press keeps it until every button is up,
    // so a drag that leaves the window it started in is still that window's drag, and a
    // window opened under a held button does not take it over.
    int UpdateOwner(Vector2 mouse, bool anyButtonDown);
    int Owner() const { return owner_; }

    // Esc: the top window whose rule is Close is closed and returned. A Block stops the
    // search, Ignore is passed over, and so is a pinned window: it was put there to stay.
    // NONE if nothing was closed.
    int Escape();

    // --- Window behaviour (#297) ----------------------------------------------------------
    // Every placed window has all of it; a spec asks for none of it.
    //
    // A *frame* is what the player sees as one window: a window on its own, or a stack of
    // them sharing one place and shown as tabs. A *group* is frames snapped edge to edge,
    // which move together. Both are remembered, not worked out from where things are: a
    // group stays a group when one of its windows is collapsed and no longer touches the
    // next, and two windows that merely happen to touch are not one.

    // The title bar's height in units, for a collapsed window and for dropping onto a title;
    // how near (units) a dragged edge has to come to another to snap to it.
    void SetTitleHeight(float units) { titleUnits_ = units; }
    void SetSnapDistance(float units) { snapUnits_ = units; }

    // Pinned: not moved by a drag, not resized, not torn out of its stack, not closed by Esc.
    // Collapsed: only the title bar is shown; the size is kept for when it opens out. Both
    // belong to the frame, so they apply to every tab of a stack.
    bool Pinned(int h) const { return entries_[h].pinned; }
    void SetPinned(int h, bool pinned);
    bool Collapsed(int h) const { return entries_[h].collapsed; }
    void SetCollapsed(int h, bool collapsed);

    // The windows of h's frame, in tab order, open or not; just {h} when it is on its own.
    std::vector<int> FrameOf(int h) const;
    // The open windows of h's frame in tab order: what its tab strip shows.
    std::vector<int> Tabs(int h) const;
    // The tab in front: the open window of h's frame raised last. NONE if none is open.
    int ActiveTab(int h) const;
    // Drawn and under the cursor: open, not covered, and the tab in front of its frame.
    bool Shown(int h) const;
    // h (with its whole frame) becomes tabs of onto's frame, in front, at onto's place.
    // Refused (false) for a window that is not placed, onto another layer, or onto itself.
    bool Stack(int h, int onto);
    // h leaves its stack and its group, where it is; a stack left with one window is none.
    void Unstack(int h);
    // Every window that moves with h, h's frame included.
    std::vector<int> Group(int h) const;
    // h's frame leaves its group, where it is.
    void Ungroup(int h);

    // A drag of h's title bar. `alone` takes h's frame out of its group first (the player
    // held Shift); a group with a pinned window in it does not move, so h leaves it the same
    // way. False if h is pinned: nothing moves.
    bool BeginMove(int h, bool alone);
    bool Moving() const { return move_.h != NONE; }
    int  MovingWindow() const { return move_.h; }
    // h's top left would be here: everything moving goes by as much, snapped to an edge
    // within reach.
    void MoveTo(Vector2 topLeft);
    // The drag is let go with the cursor at `mouse`. Over another window's title bar, a
    // single frame is stacked onto it (and that window is returned); otherwise whatever it
    // now touches joins its group, and the group takes one anchor so that it keeps together
    // when the screen changes. NONE when nothing was stacked.
    int EndMove(Vector2 mouse);
    // The window whose title bar the cursor is over, that the frame being moved would be
    // stacked onto if it were let go here; NONE if none, or if a whole group is moving.
    int DropTarget(Vector2 mouse) const;

    // Something kept per account moved, opened or closed since the last call.
    bool TakeChanged();

    // One account's windows: { id: { open, anchor, offset, size, pinned, collapsed, group,
    // stack, tab, front } }; a group or a stack is named by the id of one of its windows.
    // Load ignores an unknown id or a field of the wrong type, keeps every window reachable,
    // and forgets a group or a stack that comes back with one window in it.
    nlohmann::json Save() const;
    void           Load(const nlohmann::json& windows);

    // The file holds every account that played on this machine. Read refuses a file from
    // a newer build (and says so), and is otherwise content with nothing to read; Write
    // replaces one account and keeps the rest.
    static bool ReadFile(const nlohmann::json& file, const std::string& account, DeskLayout& into,
                         std::string& error);
    static void WriteFile(nlohmann::json& file, const std::string& account, const DeskLayout& from);

    static const char* AnchorName(Anchor a);

private:
    struct Entry
    {
        WindowSpec spec;
        bool       open = false;
        int        z = 0;
        Anchor     anchor = Anchor::TopLeft;
        Vector2    offset{ 0.0f, 0.0f };
        Vector2    size{ 0.0f, 0.0f };
        Rectangle  rect{ 0.0f, 0.0f, 0.0f, 0.0f };
        int        stack = 0;  // 0: alone; otherwise every window with the same number
        int        tab = 0;    // its place in the stack's tab strip
        int        group = 0;  // 0: in no group
        bool       pinned = false;
        bool       collapsed = false;
        bool       held = false;  // open, but out of its context: opens when it returns
    };

    bool      Placed(const Entry& e) const { return e.spec.place.width > 0.0f; }
    void      Place(Entry& e) const;    // rect from anchor, offset and size, kept reachable
    void      Unplace(Entry& e) const;  // offset from rect, for the anchor it has
    Rectangle Reachable(Rectangle r) const;
    float     TitlePx() const { return titleUnits_ * unit_; }
    Anchor    AnchorFor(Rectangle r) const;
    void      SettleAll(const std::vector<int>& windows);  // one anchor for all of them
    void      Dissolve();                                  // a stack or a group of one is none
    void      Touched(const Entry& e);  // something kept per account may have changed

    struct Move
    {
        int                    h = NONE;
        std::vector<int>       windows;  // everything that moves, h's frame included
        std::vector<Rectangle> start;    // where each was when the drag began
    };

    std::vector<Entry>       entries_;
    int                      nextZ_ = 0;
    float                    screenW_ = 1280.0f, screenH_ = 720.0f;
    float                    unit_ = 1.0f;
    int                      owner_ = NONE;
    bool                     captured_ = false;
    bool                     changed_ = false;
    float                    titleUnits_ = 26.0f;
    float                    snapUnits_ = 12.0f;
    int                      nextStack_ = 0, nextTab_ = 0, nextGroup_ = 0;
    Move                     move_;
    std::vector<std::string> contexts_;  // the ones that are on
};

}  // namespace Ui
