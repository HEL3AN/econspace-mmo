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
};

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
    void SetOpen(int h, bool open);
    void Raise(int h);  // to the front of its layer
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
    // search, Ignore is passed over. NONE if nothing was closed.
    int Escape();

    // Something kept per account moved, opened or closed since the last call.
    bool TakeChanged();

    // One account's windows: { id: { open, anchor, offset, size } }. Load ignores an
    // unknown id or a field of the wrong type, and keeps every window reachable.
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
    };

    bool      Placed(const Entry& e) const { return e.spec.place.width > 0.0f; }
    void      Place(Entry& e) const;    // rect from anchor, offset and size, kept reachable
    void      Unplace(Entry& e) const;  // offset from rect, for the anchor it has
    Rectangle Reachable(Rectangle r) const;

    std::vector<Entry> entries_;
    int                nextZ_ = 0;
    float              screenW_ = 1280.0f, screenH_ = 720.0f;
    float              unit_ = 1.0f;
    int                owner_ = NONE;
    bool               captured_ = false;
    bool               changed_ = false;
};

}  // namespace Ui
