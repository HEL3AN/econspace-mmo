#version 330

// gatefield -- the field a built gate holds (#618). Not a wormhole and not a window: energy
// the gate's emitters hold in a lane along its axis, entering at one end and gathering into a
// lit core at the other. A gate is the one built thing that is lit from inside, and this is the
// light it makes.
//
// What is in it, and why it reads as made and not found:
//   - a lane with a hard edge: the field has a shape (a rounded lens along the axis) and a
//     bright rail where the emitters' hold ends -- a natural phenomenon fades, this stops;
//   - filaments running with the lane toward the core, fine and straight and pulled along the
//     axis, never a swirl: two copies half a loop apart, so there is no jump when it restarts;
//   - the emitters at each hoop energising it: a band across the lane at every rung, flaring in
//     sequence toward the core, with a few arcs jumping between the rail lamps. This is what
//     ties the field to the hardware around it;
//   - the core: a hot white lens where the lane ends, breathing slowly, with a ripple going out
//     of it. A thing you fly through to somewhere has a far end that is brighter than the near;
//   - for an anchored mouth, `tame` > 0 lets a faint violet through at the very edge: the
//     natural hole the gate holds, caught and made to behave.
//
// The field is a part of the gate in the grammar (a soft disc of this material, turned with
// the object): `aspect` is how much longer the lane is than wide, `rungs` where the emitters
// stand along it, in the disc's own units (-1 .. 1 along the axis), `coreAt` where it ends.
//
// f(time, seed) (#136): filaments loop every 12 s, the pulses every 4 s, the core every 16 s,
// all dividing the 16384 s clock wrap. Nothing reads the frame; a driver that cannot compile it
// draws the part's `fallback` glow (#120).

in vec2 fragTexCoord;
in vec4 fragColor;

uniform sampler2D texture0;
uniform vec4 colDiffuse;

uniform vec2  centre;        // item.screenPos
uniform float radius;        // item.screenSize -- half the lane's length
uniform float time;          // clock.time
uniform float seed;          // item.seed
uniform float turn;          // item.angle -- the object's own turn

uniform float aspect;        // how many times longer the lane is than wide
uniform vec3  rungs;         // where the first emitter stands, the step to the next, how many
uniform float coreAt;        // where the lane gathers into its core, along the axis
uniform float coreSize;      // the core's size, 0..1 of the lane's width
uniform vec3  warm;          // the field's body
uniform vec3  hot;           // its core and its brightest filaments
uniform vec3  edgeTint;      // the hint of what it holds at the edge
uniform float tame;          // how much of that shows, 0 none

out vec4 finalColor;

#include "body.glsl"

void main()
{
    vec4 texel = texture(texture0, fragTexCoord) * colDiffuse * fragColor;
    vec2 local = bLocal(centre, radius);
    // Into the object's frame: x along the axis (toward the core), y across.
    float c = cos(turn), s = sin(turn);
    vec2  p = vec2(c * local.x + s * local.y, -s * local.x + c * local.y);
    float w = 1.0 / aspect;                       // half the lane's width
    float v = p.y / w;                            // -1 .. 1 across the lane

    // The lane's shape: a lens with rounded ends, hard-edged. 0 at the centre, 1 at the rail.
    float ex = p.x / 0.96;
    float d = pow(pow(abs(ex), 3.0) + pow(abs(v), 3.0), 1.0 / 3.0);
    if (d > 1.06)
        discard;
    float inside = 1.0 - smoothstep(0.93, 1.0, d);
    int   oct = bOctaves(radius * w, 4);

    // --- Filaments: straight, pulled along the axis, running toward the core ---------------
    float T = fract(time / 12.0);
    float fil = 0.0;
    for (int k = 0; k < 2; k++)
    {
        float f = fract(T + 0.5 * float(k));
        float wt = 1.0 - abs(2.0 * f - 1.0);
        vec3  q = vec3(p.x * 2.6 - f * 5.0, v * 5.5, seed * 31.0 + float(k) * 9.0);
        float n = bRidge(q, oct);
        fil += wt * pow(n, 3.0);
    }
    // Brighter toward the middle of the lane: the rail is held, the middle is where it flows.
    fil *= 0.45 + 0.55 * (1.0 - abs(v));

    // --- The emitters: a band across the lane at every rung, flaring in sequence -----------
    float bands = 0.0, arcs = 0.0;
    float Tp = fract(time / 4.0);
    for (int k = 0; k < 8; k++)
    {
        if (float(k) >= rungs.z)
            break;
        float x = rungs.x + rungs.y * float(k);
        float dx = (p.x - x) / 0.035;
        float flare = pow(max(0.0, 0.5 + 0.5 * cos(TAU * (Tp - float(k) / max(rungs.z, 1.0)))), 3.0);
        float band = exp(-dx * dx) * (0.35 + 0.65 * flare);
        bands += band * (0.5 + 0.5 * (1.0 - abs(v) * 0.6));
        // A few arcs between the rail lamps, jumping and re-forming, thin.
        float a = bNoise(vec3(p.y * 14.0 + float(k) * 3.7, floor(time * 7.0 + float(k) * 5.0) * 0.37, seed * 17.0));
        float line = exp(-pow((a - 0.5) * 9.0, 2.0));
        arcs += band * line * flare * 1.2;
    }

    // --- The core: where the lane ends, hot and breathing ---------------------------------
    vec2  cp = vec2((p.x - coreAt) / 0.30, v / max(coreSize, 0.05));
    float cr = length(cp);
    float breath = 0.82 + 0.18 * sin(TAU * time / 16.0);
    float core = exp(-cr * cr * 1.6) * breath;
    float ripple = 0.5 + 0.5 * sin(cr * 9.0 - TAU * time / 5.0);
    float halo = exp(-cr * 0.9) * (0.55 + 0.45 * ripple) * 0.55;

    // --- The near end: dim, filaments only, so the lane has a direction --------------------
    float grow = smoothstep(-1.0, coreAt, p.x);            // dim at the entrance, full at the core
    float body = (0.42 + 0.8 * grow) * inside;

    // --- The rail: a hard bright edge where the emitters' hold ends -------------------------
    float rail = exp(-pow((d - 0.965) / 0.03, 2.0)) * (0.6 + 0.4 * grow);

    vec3 col = warm * body * (0.45 + 1.2 * fil)
             + hot * (fil * grow * 0.6 + bands * 0.55 + arcs * 0.9)
             + hot * (core * 1.35 + halo)
             + mix(warm, hot, 0.5) * rail * 0.9;
    col *= inside + rail * 0.4;

    // The hint of what an anchored mouth holds: violet only in the last tenth of the lane's
    // width, strongest at the near end where the hole is closest to being what it was.
    float edgeBand = smoothstep(0.78, 1.0, d) * (1.0 - smoothstep(1.0, 1.06, d));
    col += edgeTint * edgeBand * tame * (0.5 + 0.5 * bNoise(vec3(p.x * 4.0 - time * 0.4, v * 3.0, seed * 5.0))) * (1.0 - 0.6 * grow);

    col = vec3(1.0) - exp(-col * 1.9);
    float lum = max(col.r, max(col.g, col.b));
    float a = clamp(lum * 1.6, 0.0, 1.0) * texel.a;
    if (a <= 0.004)
        discard;
    finalColor = vec4(min(col / max(a, 0.05), vec3(1.5)), a);
}
