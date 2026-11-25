#version 330

// The height map: how high the solid is at each point, seen from above. Written once per
// object per frame, read by lit.fs to march shadows and measure occlusion.

in vec3 vLocal;
out float finalHeight;

void main()
{
    finalHeight = vLocal.z;
}
