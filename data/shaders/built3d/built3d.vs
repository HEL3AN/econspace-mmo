#version 330

// built3d.vs -- a built object seen from straight above, one vertex shader for both passes
// (#618 spike): `uMode` 0 draws into the scene through the game's own 2D camera, 1 draws the
// object's heights into the height map the lighting reads its shadows and occlusion from.
//
// The mesh is built once, object-local, heading 0, time 0 (Built3DMesh). What moves is done
// here from the clock, so nothing is rebuilt per frame (#136): the object's heading, and each
// part's own spin about the object's centre.

layout(location = 0) in vec3 aPos;     // x, y, height
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;      // the part's own frame, in the object's radii
layout(location = 3) in vec2 aSeed;    // part seed, solidity
layout(location = 4) in vec4 aMotion;  // spin (deg/s), blink period, blink phase, role
layout(location = 5) in vec4 aTint;
layout(location = 6) in float aEdge;   // inside the part's outline, world units

uniform int   uMode;
uniform float uHeading;   // radians
uniform vec2  uClock;     // whole seconds (wrapped), fraction
uniform float uMaxH;      // the tallest thing, world units
uniform float uReach;     // the furthest from the centre, world units
uniform vec3  uTilt;      // the camera's lean: tan, cos, sin; (0, 1, 0) is straight down

// Scene pass.
uniform vec2 uObj;        // the object's centre relative to the camera's target, world units
uniform vec4 uCam;        // zoom, rotation (radians), offset x, offset y (pixels)
uniform vec2 uViewport;   // pixels

// Height pass.
uniform float uHExtent;   // half the map's width, world units
uniform float uHFill;     // the share of the map's texture in use

out vec3  vLocal;         // x, y rotated into the object's turned frame; z the height
out vec3  vNormal;
out vec2  vUv;
out vec2  vSeed;
flat out vec4 vMotion;
out vec4  vTint;
out float vEdge;

void main()
{
    float spin = mod(aMotion.x * uClock.x, 360.0) + aMotion.x * uClock.y;
    float a = radians(spin) + uHeading;
    float c = cos(a), s = sin(a);
    vec2  p = vec2(c * aPos.x - s * aPos.y, s * aPos.x + c * aPos.y);
    vec3  n = vec3(c * aNormal.x - s * aNormal.y, s * aNormal.x + c * aNormal.y, aNormal.z);

    vLocal = vec3(p, aPos.z);
    vNormal = n;
    vUv = aUv;
    vSeed = aSeed;
    vMotion = aMotion;
    vTint = aTint;
    vEdge = aEdge;

    if (uMode == 0)
    {
        vec2  w = (uObj + p) * uCam.x;
        float cr = cos(uCam.y), sr = sin(uCam.y);
        w = vec2(cr * w.x - sr * w.y, sr * w.x + cr * w.y) + uCam.zw;
        // The camera leans toward the viewer's side: what is high is carried up the screen,
        // and the ground stays exactly where the 2D world puts it (an oblique projection, so
        // an object's footprint is the same at any tilt and picking needs no change).
        w.y -= uTilt.x * aPos.z * uCam.x;
        // Higher is nearer the viewer, and so is what lies further down the screen.
        float down = sr * p.x + cr * p.y;
        float t = (aPos.z * uTilt.y + (down + uReach) * uTilt.z) /
                  max(uMaxH * uTilt.y + 2.0 * uReach * uTilt.z, 0.0001);
        float depth = 1.0 - 1.96 * clamp(t, 0.0, 1.0);
        gl_Position = vec4(w.x / uViewport.x * 2.0 - 1.0, 1.0 - w.y / uViewport.y * 2.0, depth, 1.0);
    }
    else
    {
        float depth = 1.0 - 1.96 * clamp(aPos.z / max(uMaxH, 0.0001), 0.0, 1.0);
        gl_Position = vec4(p / uHExtent, depth, 1.0);
    }
}
