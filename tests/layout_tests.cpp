#include <doctest/doctest.h>

#include "ui/Layout.h"

#include <string>
#include <string_view>

// Ui::Layout (#297): rows and columns solved by Clay, held here without a window. Text is
// measured by a stand-in -- half the font size per character, the font size tall -- since
// a real font needs a GPU to load, and a CI runner has none.

using Ui::Align;
using Ui::Box;
using Ui::DrawCommand;
using Ui::Layout;
using Ui::Size;
using Ui::TextStyle;

namespace
{
void Monospaced(Layout& l)
{
    l.SetMeasure([](Ui::Face, float px, std::string_view text)
                 { return Vector2{ 0.5f * px * (float)text.size(), px }; });
}

int Count(const std::vector<DrawCommand>& cmds, DrawCommand::Kind kind)
{
    int n = 0;
    for (const DrawCommand& c : cmds)
        n += c.kind == kind ? 1 : 0;
    return n;
}

const Rectangle AREA{ 100.0f, 50.0f, 300.0f, 200.0f };
}  // namespace

TEST_CASE("growing siblings share what their row has left")
{
    Layout l;
    Monospaced(l);
    l.Begin(AREA, 1.0f);
    l.Row(Box().GrowX().Gap(10.0f),
          [&]
          {
              l.Open(Box().Id("fixed").Width(Size::Fixed(40.0f)).Height(Size::Fixed(20.0f)));
              l.Close();
              l.Open(Box().Id("a").GrowX().Height(Size::Fixed(20.0f)));
              l.Close();
              l.Open(Box().Id("b").GrowX().Height(Size::Fixed(20.0f)));
              l.Close();
          });
    l.End();
    CHECK_MESSAGE(l.LastError().empty(), l.LastError());

    const Rectangle f = l.BoxOf("fixed"), a = l.BoxOf("a"), b = l.BoxOf("b");
    // In screen coordinates: the layout starts at the area's corner.
    CHECK(f.x == doctest::Approx(100.0f));
    CHECK(f.y == doctest::Approx(50.0f));
    CHECK(f.width == doctest::Approx(40.0f));
    // 300 - 40 - two gaps of 10, halved.
    CHECK(a.width == doctest::Approx(120.0f));
    CHECK(b.width == doctest::Approx(120.0f));
    CHECK(b.x == doctest::Approx(100.0f + 40.0f + 10.0f + 120.0f + 10.0f));
}

TEST_CASE("sizes, padding and gaps are in units, and a unit is the scale in pixels")
{
    Layout l;
    Monospaced(l);
    l.Begin({ 0.0f, 0.0f, 600.0f, 400.0f }, 1.5f);
    l.Column(Box().Id("outer").Pad(10.0f).Gap(4.0f),
             [&]
             {
                 l.Open(Box().Id("one").Width(Size::Fixed(100.0f)).Height(Size::Fixed(20.0f)));
                 l.Close();
                 l.Open(Box().Id("two").Width(Size::Fixed(100.0f)).Height(Size::Fixed(20.0f)));
                 l.Close();
             });
    l.End();
    const Rectangle one = l.BoxOf("one"), two = l.BoxOf("two"), outer = l.BoxOf("outer");
    CHECK(one.x == doctest::Approx(15.0f));
    CHECK(one.width == doctest::Approx(150.0f));
    CHECK(two.y == doctest::Approx(15.0f + 30.0f + 6.0f));
    // The column fits what is in it: two rows, a gap, and the padding round them.
    CHECK(outer.height == doctest::Approx(15.0f + 30.0f + 6.0f + 30.0f + 15.0f));
}

TEST_CASE("text is measured in the layout's font size and drawn at the scaled one")
{
    Layout l;
    Monospaced(l);
    l.Begin(AREA, 2.0f);
    l.Row(Box().Id("row"), [&] { l.Text("abcd", TextStyle::Body()); });
    const std::vector<DrawCommand>& cmds = l.End();
    REQUIRE(Count(cmds, DrawCommand::Kind::Text) == 1);
    for (const DrawCommand& c : cmds)
        if (c.kind == DrawCommand::Kind::Text)
        {
            const float px = Ui::CurrentTheme().fontSize.body * 2.0f;
            CHECK(c.px == doctest::Approx(px));
            CHECK(c.text == "abcd");
            CHECK(c.box.width == doctest::Approx(0.5f * px * 4.0f));
        }
}

TEST_CASE("wrapped text takes more lines in a narrower window")
{
    const std::string words = "the quick brown fox jumps over the lazy dog";
    auto              lines = [&](float width)
    {
        Layout l;
        Monospaced(l);
        l.Begin({ 0.0f, 0.0f, width, 400.0f }, 1.0f);
        l.Text(words, TextStyle::Body().Wrap());
        return Count(l.End(), DrawCommand::Kind::Text);
    };
    CHECK(lines(1000.0f) == 1);
    CHECK(lines(150.0f) > 1);
    CHECK(lines(80.0f) > lines(150.0f));
}

TEST_CASE("a field puts its value at the far end of the row")
{
    Layout l;
    Monospaced(l);
    l.Begin(AREA, 1.0f);
    l.Field("Speed", "120", WHITE);
    const std::vector<DrawCommand>& cmds = l.End();
    float                           valueRight = 0.0f, labelLeft = 1e9f;
    for (const DrawCommand& c : cmds)
    {
        if (c.kind != DrawCommand::Kind::Text)
            continue;
        if (c.text == "120")
            valueRight = c.box.x + c.box.width;
        if (c.text == "Speed")
            labelLeft = c.box.x;
    }
    CHECK(labelLeft == doctest::Approx(AREA.x));
    CHECK(valueRight == doctest::Approx(AREA.x + AREA.width));
}

TEST_CASE("a click lands on what the previous frame put under the mouse, and only if owned")
{
    Layout l;
    Monospaced(l);
    auto frame = [&](Layout::Pointer p)
    {
        l.Begin(AREA, 1.0f, p);
        const bool clicked = l.Button("ok", "OK");
        l.End();
        return clicked;
    };
    Layout::Pointer none;
    CHECK_FALSE(frame(none));  // the first frame places the button
    const Rectangle b = l.BoxOf("ok");
    REQUIRE(b.width > 0.0f);

    Layout::Pointer p;
    p.owned = true;
    p.pos = { b.x + b.width * 0.5f, b.y + b.height * 0.5f };
    p.pressed = true;
    p.down = true;
    CHECK(frame(p));

    p.owned = false;  // another window is in front
    CHECK_FALSE(frame(p));

    p.owned = true;
    p.pos = { b.x - 5.0f, b.y };  // beside it
    CHECK_FALSE(frame(p));
}

TEST_CASE("content taller than its scrolling box is clipped, not spilled")
{
    Layout l;
    Monospaced(l);
    l.Begin({ 0.0f, 0.0f, 200.0f, 100.0f }, 1.0f);
    l.Column(Box().Id("list").Grow().ScrollY(),
             [&]
             {
                 for (int i = 0; i < 20; i++)
                     l.Text("row", TextStyle::Body());
             });
    const std::vector<DrawCommand>& cmds = l.End();
    CHECK(Count(cmds, DrawCommand::Kind::ClipBegin) == 1);
    CHECK(Count(cmds, DrawCommand::Kind::ClipEnd) == 1);
    CHECK(l.BoxOf("list").height == doctest::Approx(100.0f));
}

TEST_CASE("two elements with one id are reported, not silently merged")
{
    Layout l;
    Monospaced(l);
    l.Begin(AREA, 1.0f);
    l.Open(Box().Id("same"));
    l.Close();
    l.Open(Box().Id("same"));
    l.Close();
    l.End();
    CHECK_FALSE(l.LastError().empty());
}

TEST_CASE("two layouts keep their own state")
{
    Layout a, b;
    Monospaced(a);
    Monospaced(b);
    a.Begin(AREA, 1.0f);
    a.Open(Box().Id("x").Width(Size::Fixed(10.0f)).Height(Size::Fixed(10.0f)));
    a.Close();
    a.End();
    b.Begin(AREA, 1.0f);
    b.Open(Box().Id("x").Width(Size::Fixed(50.0f)).Height(Size::Fixed(10.0f)));
    b.Close();
    b.End();
    CHECK(a.BoxOf("x").width == doctest::Approx(10.0f));
    CHECK(b.BoxOf("x").width == doctest::Approx(50.0f));
}
