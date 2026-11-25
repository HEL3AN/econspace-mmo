#pragma once

namespace Render
{
namespace GlState
{

// The scissor rectangle, as OpenGL has it: whether it is on, and its box in the current
// target's pixels, origin bottom left.
//
// raylib sets it (BeginScissorMode) and has no way to ask what it is. Something that has to
// draw into a target of its own in the middle of a frame -- a body painted at reduced
// resolution (#426) -- must switch it off there, where the box means nothing, and put back
// exactly what the caller had. Asked of OpenGL itself, through the loader raylib already uses.
struct Scissor
{
    bool on = false;
    int  box[4] = { 0, 0, 0, 0 };
};

Scissor Current();
void    Restore(const Scissor& s);

// The viewport of the target being drawn into, in its pixels, origin bottom left.
void Viewport(int box[4]);

// Clears the depth of a box of the current target, leaving its colour: what a solid drawn
// over a 2D scene needs before it starts (Built3D), so an earlier solid's depth under it is
// not read as in front of it.
void ClearDepth(int x, int y, int w, int h);

}  // namespace GlState
}  // namespace Render
