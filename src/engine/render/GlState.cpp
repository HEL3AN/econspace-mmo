#include "render/GlState.h"

// raylib's own OpenGL loader, for the two queries rlgl does not wrap. Only the declarations:
// the loader itself is compiled into raylib, which has already loaded it by the time anything
// is drawn. Kept to this one file, so nothing else sees OpenGL's names.
#include "external/glad.h"
#include "rlgl.h"

namespace Render
{
namespace GlState
{

Scissor Current()
{
    Scissor s;
    s.on = glIsEnabled(GL_SCISSOR_TEST) == GL_TRUE;
    if (s.on)
        glGetIntegerv(GL_SCISSOR_BOX, s.box);
    return s;
}

void Restore(const Scissor& s)
{
    if (!s.on)
    {
        rlDisableScissorTest();
        return;
    }
    rlEnableScissorTest();
    rlScissor(s.box[0], s.box[1], s.box[2], s.box[3]);
}

}  // namespace GlState
}  // namespace Render
