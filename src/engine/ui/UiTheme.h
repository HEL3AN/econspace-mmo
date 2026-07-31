#pragma once

#include "raylib.h"

// Unified UI style: colors, font, and text-drawing helpers.
namespace Ui
{
constexpr Color PANEL_BG     = { 18, 20, 28, 235 };
constexpr Color PANEL_BORDER = { 70, 80, 100, 255 };
constexpr Color TITLE_BG     = { 34, 38, 52, 255 };
constexpr Color ACCENT       = { 92, 170, 232, 255 };
constexpr Color TEXT         = { 222, 226, 234, 255 };
constexpr Color TEXT_DIM     = { 132, 142, 158, 255 };

constexpr int TITLE_HEIGHT = 26;
constexpr int PADDING      = 12;

// Font load/unload. Call LoadAssets after InitWindow,
// UnloadAssets before CloseWindow.
void LoadAssets();
void UnloadAssets();
Font GetFont();

// Draws text using the UI font.
void Text(const char* text, int x, int y, int size, Color color);
int  TextWidth(const char* text, int size);
}
