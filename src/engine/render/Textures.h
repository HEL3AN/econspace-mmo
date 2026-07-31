#pragma once

#include "raylib.h"

// Texture store: lazy, cached loading of PNGs from data/textures/.
// If a file is missing, drawing falls back to vector shapes and the game keeps running.
namespace Tex
{
void Unload();  // unload all loaded textures; call before CloseWindow

// Draws sprite name (file data/textures/<name>.png) centered at pos,
// fit into a square of side 2*size, rotated by rotationDeg (clockwise)
// and tinted with tint. Returns false if the texture is missing — then the
// caller draws a fallback shape.
bool DrawSprite(const char* name, Vector2 pos, float size, float rotationDeg, Color tint);
}
