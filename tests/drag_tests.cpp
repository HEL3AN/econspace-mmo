#include <doctest/doctest.h>

#include "ui/Drag.h"
#include "ui/Layout.h"

#include <string>
#include <string_view>
#include <vector>

// Drag and drop between windows (#472): a mechanic of the window framework, held here without
// a window. Two layouts stand for two windows side by side: a table whose rows drag, and a box
// that takes what is dropped on it. The mouse is handed in, as the desk hands it in.

using Ui::Box;
using Ui::DragPayload;
using Ui::Layout;
using Ui::Size;
namespace Drag = Ui::Drag;

namespace
{
void Monospaced(Layout& l)
{
    l.SetMeasure(
        [](Ui::Face, float px, std::string_view text) {
            return Vector2{ 0.5f * px * (float)text.size(), px };
        });
}

Layout::Pointer At(Vector2 p, bool owned, bool down, bool pressed = false)
{
    Layout::Pointer ptr;
    ptr.owned = owned;
    ptr.pos = p;
    ptr.down = down || pressed;
    ptr.pressed = pressed;
    return ptr;
}

Vector2 Centre(Rectangle r)
{
    return { r.x + r.width * 0.5f, r.y + r.height * 0.5f };
}

const Rectangle LEFT{ 0.0f, 0.0f, 300.0f, 200.0f };     // the source window
const Rectangle RIGHT{ 400.0f, 0.0f, 300.0f, 200.0f };  // the target window

struct Windows
{
    Layout                   source, target;
    std::vector<std::string> goods{ "Iron", "Ice", "Steel" };
    std::string              kind = "goods";
    const DragPayload*       dropped = nullptr;
    DragPayload              got;
    bool                     hover = false;

    Windows()
    {
        Monospaced(source);
        Monospaced(target);
        Drag::Cancel();  // one mouse per process: nothing left over from another test
    }

    // One frame: the desk's update, then each window with the pointer as far as it owns it.
    void Frame(Vector2 mouse, bool down, bool pressed = false)
    {
        Drag::Update(mouse, down);
        const bool overSource = CheckCollisionPointRec(mouse, LEFT);
        const bool overTarget = CheckCollisionPointRec(mouse, RIGHT);
        // The desk's rule: while an item is dragged the mouse is whatever's under it;
        // otherwise a press keeps it where it began, which here is the source.
        const bool sourceOwns = Drag::Active() ? overSource : overSource || (down && !pressed);

        source.Begin(LEFT, 1.0f, At(mouse, sourceOwns, down, pressed && overSource));
        Ui::TableSpec s;
        s.id = "goods";
        s.columns = { { "good", Size::Grow(), Ui::Align::Start, false } };
        s.rows = (int)goods.size();
        s.cell = [&](int r, int) { return Ui::Cell{ goods[r], WHITE }; };
        s.drag = [&](int r, DragPayload& out)
        {
            out.kind = kind;
            out.source = "hold";
            out.item = r;
            out.amount = 5;
            out.available = 10;
            out.label = "5 " + goods[r];
            return true;
        };
        source.Table(s);
        source.End();

        target.Begin(RIGHT, 1.0f,
                     At(mouse, Drag::Active() ? overTarget : overTarget && !down, down));
        target.Column(Box().Grow(),
                      [&] {
                          target.Row(
                              Box().Id("bin").Width(Size::Fixed(200.0f)).Height(Size::Fixed(80.0f)),
                              [] {});
                      });
        hover = target.DropHover("bin", 0, "goods");
        dropped = target.Dropped("bin", 0, "goods");
        if (dropped != nullptr)
            got = *dropped;
        target.End();
    }

    Vector2 RowCentre(int r) const
    {
        // A table's rows are "<id>~r", by index.
        return Centre(source.BoxOf("goods~r", (uint32_t)r));
    }
    Vector2 BinCentre() const { return Centre(target.BoxOf("bin")); }
};
}  // namespace

TEST_CASE("Drag: a row pulled onto a target in another window is dropped there, once")
{
    Windows w;
    w.Frame({ 650.0f, 190.0f }, false);  // laid out once: the boxes are known
    const Vector2 row = w.RowCentre(1);
    REQUIRE(row.y > 0.0f);

    w.Frame(row, true, true);  // pressed on "Ice"
    CHECK(Drag::Armed());
    CHECK_FALSE(Drag::Active());

    const Vector2 bin = w.BinCentre();
    w.Frame(bin, true);  // pulled across: dragging, and the target lights up
    CHECK(Drag::Active());
    REQUIRE(Drag::Current() != nullptr);
    CHECK(Drag::Current()->label == "5 Ice");
    w.Frame(bin, true);
    CHECK(w.hover);

    w.Frame(bin, false);  // let go over it
    REQUIRE(w.dropped != nullptr);
    CHECK(w.got.item == 1);
    CHECK(w.got.amount == 5);
    CHECK(w.got.source == "hold");
    CHECK_FALSE(Drag::Dropping());  // taken: nothing else sees it

    w.Frame(bin, false);
    CHECK(w.dropped == nullptr);
    CHECK(Drag::Current() == nullptr);
}

TEST_CASE("Drag: a press that does not move is a click, not a drag")
{
    Windows w;
    w.Frame({ 650.0f, 190.0f }, false);
    const Vector2 row = w.RowCentre(0);
    w.Frame(row, true, true);
    w.Frame({ row.x + 2.0f, row.y }, true);  // within the threshold
    CHECK(Drag::Armed());
    w.Frame({ row.x + 2.0f, row.y }, false);
    CHECK_FALSE(Drag::Armed());
    CHECK_FALSE(Drag::Active());
    CHECK(w.dropped == nullptr);
}

TEST_CASE("Drag: let go anywhere but a target, nothing is dropped")
{
    Windows w;
    w.Frame({ 650.0f, 190.0f }, false);
    w.Frame(w.RowCentre(2), true, true);
    const Vector2 beside{ 680.0f, 190.0f };  // in the target window, not on the bin
    w.Frame(beside, true);
    CHECK(Drag::Active());
    CHECK_FALSE(w.hover);
    w.Frame(beside, false);
    CHECK(w.dropped == nullptr);
    CHECK(Drag::Dropping());  // offered for this frame; nobody took it
    w.Frame(beside, false);
    CHECK_FALSE(Drag::Dropping());
}

TEST_CASE("Drag: a target takes only the kind it accepts")
{
    Windows w;
    w.kind = "ship";
    w.Frame({ 650.0f, 190.0f }, false);
    w.Frame(w.RowCentre(0), true, true);
    const Vector2 bin = w.BinCentre();
    w.Frame(bin, true);
    w.Frame(bin, true);
    CHECK_FALSE(w.hover);
    w.Frame(bin, false);
    CHECK(w.dropped == nullptr);
    // ...and the world, which takes anything, still could.
    REQUIRE(Drag::Take() != nullptr);
}

TEST_CASE("Drag: a target in a window that does not own the mouse never sees the drop")
{
    Windows w;
    w.Frame({ 650.0f, 190.0f }, false);
    w.Frame(w.RowCentre(0), true, true);
    const Vector2 bin = w.BinCentre();
    w.Frame(bin, true);
    // A window in front of the target: the desk hands the target a pointer it does not own.
    Drag::Update(bin, false);
    w.target.Begin(RIGHT, 1.0f, At(bin, false, false));
    w.target.Row(Box().Id("bin").Width(Size::Fixed(200.0f)).Height(Size::Fixed(80.0f)), [] {});
    CHECK(w.target.Dropped("bin", 0, "goods") == nullptr);
    w.target.End();
    CHECK(Drag::Dropping());
}

TEST_CASE("Drag: Cancel drops nothing")
{
    Windows w;
    w.Frame({ 650.0f, 190.0f }, false);
    w.Frame(w.RowCentre(0), true, true);
    const Vector2 bin = w.BinCentre();
    w.Frame(bin, true);
    REQUIRE(Drag::Active());
    Drag::Cancel();  // Esc
    w.Frame(bin, false);
    CHECK(w.dropped == nullptr);
    CHECK_FALSE(Drag::Dropping());
}

TEST_CASE("Drag: a source of any element arms on its press")
{
    Drag::Cancel();
    Layout l;
    Monospaced(l);
    auto frame = [&](Vector2 at, bool pressed)
    {
        l.Begin(LEFT, 1.0f, At(at, true, pressed, pressed));
        l.Row(Box().Id("chip").Width(Size::Fixed(60.0f)).Height(Size::Fixed(20.0f)), [] {});
        DragPayload p;
        p.kind = "entity";
        p.item = 42;
        l.DragSource("chip", 0, p);
        l.End();
    };
    frame({ 250.0f, 150.0f }, false);
    const Vector2 c = Centre(l.BoxOf("chip"));
    frame(c, true);
    CHECK(Drag::Armed());
    Drag::Update({ c.x + 20.0f, c.y }, true);
    REQUIRE(Drag::Active());
    CHECK(Drag::Current()->item == 42);
    Drag::Cancel();
}
