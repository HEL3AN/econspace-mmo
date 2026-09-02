#include <doctest/doctest.h>

#include "core/ArchetypeEdit.h"
#include <fstream>
#include <sstream>
#include <string>

// The gallery (#118) writes a tuned look back into data/archetypes.json. None of the
// gallery itself can be tested -- there is no GPU in CI, and the thing it produces is a
// picture -- but this part is a string going into a string, and it is the part that can
// silently destroy hand-written data.

namespace
{
// Shaped like the real file: grouped with blank lines, one inline nested object, a
// nested "size" that must not be mistaken for the archetype's own.
const char* kFile = R"({
    "archetypes": [
        {
            "id": "star.yellow",
            "name": "Yellow Star",
            "color": [253, 249, 0, 255],
            "layer": 0,
            "size": 600,
            "components": {}
        },

        {
            "id": "station.trade_hub",
            "world": { "category": "stations", "subType": "TradeHub", "size": 3 },
            "name": "Trade Hub",
            "glyph": "#",
            "size": 90,
            "components": { "dockable": { "range": 120 } }
        }
    ]
}
)";

std::string Read(const std::string& path)
{
    std::ifstream      in(path, std::ios::binary);
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}
}  // namespace

TEST_CASE("a look is written back without disturbing the rest of the file")
{
    std::string t = kFile;

    REQUIRE(ArchetypeEdit::SetField(t, "star.yellow", "color", "[255, 100, 0, 255]"));

    // Everything but the one value is byte-identical. This is the whole point: a
    // parse-edit-dump would have reformatted all of it, and a two-character change would
    // arrive as a two-hundred-line diff nobody can review.
    std::string  expected = kFile;
    const size_t at = expected.find("[253, 249, 0, 255]");
    REQUIRE(at != std::string::npos);
    expected.replace(at, std::string("[253, 249, 0, 255]").size(), "[255, 100, 0, 255]");
    CHECK(t == expected);

    SUBCASE("a key of one archetype does not touch the same key of another")
    {
        REQUIRE(ArchetypeEdit::SetField(t, "star.yellow", "size", "640"));
        CHECK(t.find("\"size\": 640") != std::string::npos);
        CHECK(t.find("\"size\": 90") != std::string::npos);  // the station keeps its own
    }

    SUBCASE("a nested object of the same name is not the archetype's own field")
    {
        // "world" carries a "size" too. Editing the station's size must find the one at
        // the archetype's own level, not the first one that happens to match.
        REQUIRE(ArchetypeEdit::SetField(t, "station.trade_hub", "size", "120"));
        CHECK(t.find("\"subType\": \"TradeHub\", \"size\": 3") != std::string::npos);
        CHECK(t.find("\"size\": 120") != std::string::npos);
    }

    SUBCASE("a key the archetype does not have yet is added, indented like its neighbours")
    {
        REQUIRE(ArchetypeEdit::SetField(t, "star.yellow", "style", "\"region\""));
        CHECK(t.find("\"components\": {},\n            \"style\": \"region\"") !=
              std::string::npos);
    }

    SUBCASE("an unknown archetype changes nothing")
    {
        const std::string before = t;
        CHECK_FALSE(ArchetypeEdit::SetField(t, "station.nowhere", "glyph", "\"@\""));
        CHECK(t == before);
    }
}

TEST_CASE("the real registry survives a round trip through the writer")
{
    const std::string path = std::string(TEST_DATA_DIR) + "archetypes.json";
    const std::string original = Read(path);
    REQUIRE_FALSE(original.empty());

    // Writing a field back as the value it already holds must leave the file alone. If
    // that is not true, saving from the gallery would churn the file every time it is
    // opened, and a real change would be lost in the noise.
    std::string t = original;
    REQUIRE(ArchetypeEdit::SetField(t, "star.yellow", "color", "[253, 249, 0, 255]"));
    REQUIRE(ArchetypeEdit::SetField(t, "star.yellow", "size", "600"));
    CHECK(t == original);
}
