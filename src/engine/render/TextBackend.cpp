#include "render/TextBackend.h"

#include <cmath>
#include <cstdio>

// No raylib drawing calls in this file, on purpose: econagent is headless and the tests
// have no window. Only raylib's plain-old-data types (Vector2, Color) appear here.

namespace Render
{

namespace
{
// Eight-point compass. World y grows downward, so north is -y — the same convention the
// agent's text projection uses, and worth stating because getting it backwards produces
// directions that are wrong in a way nothing crashes on.
const char* Compass(float dx, float dy)
{
    if (std::fabs(dx) < 1.0f && std::fabs(dy) < 1.0f)
        return "here";
    float              angle = std::atan2(-dy, dx);  // -y is north
    static const char* names[8] = { "E", "NE", "N", "NW", "W", "SW", "S", "SE" };
    int                sector = (int)std::lround(angle / (3.14159265f / 4.0f));
    if (sector < 0)
        sector += 8;
    return names[sector % 8];
}
}  // namespace

void TextBackend::Begin()
{
    lines_.clear();
}

void TextBackend::Draw(const Item& item)
{
    const float dx = item.pos.x - origin_.x;
    const float dy = item.pos.y - origin_.y;
    const float dist = std::sqrt(dx * dx + dy * dy);

    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s %-24s %6.0fm %-2s", item.glyph.c_str(),
                  item.label.empty() ? "(unnamed)" : item.label.c_str(), dist, Compass(dx, dy));
    std::string line = buf;

    // Only worth saying when it is not the default: a full belt and an intact ship are
    // the uninteresting case, and a line per object is already long.
    if (item.intensity < 0.999f)
    {
        std::snprintf(buf, sizeof(buf), " [%d%%]", (int)std::lround(item.intensity * 100.0f));
        line += buf;
    }

    lines_.push_back(line);
}

std::string TextBackend::Text() const
{
    std::string out;
    for (const std::string& l : lines_)
    {
        out += l;
        out += '\n';
    }
    return out;
}

GridBackend::GridBackend(int width, int height, float span, Vector2 center)
    : width_(width < 1 ? 1 : width), height_(height < 1 ? 1 : height),
      span_(span > 0.0f ? span : 1.0f), center_(center)
{
    cells_.assign((size_t)width_ * (size_t)height_, ' ');
}

void GridBackend::Begin()
{
    cells_.assign((size_t)width_ * (size_t)height_, ' ');
}

void GridBackend::Draw(const Item& item)
{
    // Square cells: the vertical scale follows the horizontal one rather than stretching
    // to fill the height, or a circular orbit would come out as an ellipse.
    const float unitsPerCell = span_ / (float)width_;

    const float dx = item.pos.x - center_.x;
    const float dy = item.pos.y - center_.y;

    const int cx = (int)std::lround(dx / unitsPerCell) + width_ / 2;
    const int cy = (int)std::lround(dy / unitsPerCell) + height_ / 2;

    if (cx < 0 || cx >= width_ || cy < 0 || cy >= height_)
        return;  // off-screen

    // Later items win the cell. Present() has already sorted by layer, so this is the
    // higher layer overwriting the lower one — a station in front of a nebula.
    cells_[(size_t)cy * (size_t)width_ + (size_t)cx] = item.glyph.empty() ? '?' : item.glyph[0];
}

char GridBackend::At(int x, int y) const
{
    if (x < 0 || x >= width_ || y < 0 || y >= height_)
        return ' ';
    return cells_[(size_t)y * (size_t)width_ + (size_t)x];
}

std::string GridBackend::Row(int y) const
{
    std::string row;
    for (int x = 0; x < width_; x++)
        row += At(x, y);
    return row;
}

std::string GridBackend::Text() const
{
    std::string out;
    for (int y = 0; y < height_; y++)
    {
        out += Row(y);
        out += '\n';
    }
    return out;
}

}  // namespace Render
