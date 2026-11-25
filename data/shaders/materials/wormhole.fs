#version 330

// wormhole -- a tear in space, drawn as a phenomenon and not as an object (#618). There is no
// structure, no ring and no tick in it. What a viewer sees is what a throat that bends light
// would do to everything near it:
//
//   - gas falling in: one to three arms, wound tight and torn by turbulence so they read as
//     weather and not as a galaxy, drifting inward along their own spiral rather than turning
//     like a wheel, brighter on the side that is turning toward us, hot and pale inward and
//     the object's own colour outward;
//   - the sky behind it, bent: stars crowd and stretch toward the throat, as through a lens;
//   - the throat, small and dark -- not a window onto a bright place, which reads as an eye --
//     with only a thin, uneven rim of light where the bending is greatest, strongest on the
//     side turning toward us;
//   - an edge that is wherever the gas thins out, never a circle.
//
// Every hole differs: the seed picks the number of arms (mostly one or two), how tightly they
// wind, which way, and how torn they are.
//
// `life` (item.intensity) is how much of it is left: 1 is a mouth that has just opened, near 0
// one about to close -- the arms thin and shrink, the throat narrows and gutters, the rim dims
// and the gas cools to red. Nothing here reads a light of the system: it is its own light.
//
// The sky it bends is its own: a fragment shader painting one object cannot read the frame it
// is painting into, and a copy of the frame for one hole on screen is not worth a weak GPU's
// bandwidth (#618).
//
// f(time, seed) (#136): the inflow loops every 32 s and the churn on 8192 s, both of which
// divide the clock's wrap; stars are a function of the seed alone.

in vec2 fragTexCoord;
in vec4 fragColor;

uniform sampler2D texture0;
uniform vec4 colDiffuse;

uniform vec2  centre;        // item.screenPos
uniform float radius;        // item.screenSize -- how far the gas reaches
uniform float time;          // clock.time
uniform float seed;          // item.seed
uniform float life;          // item.intensity -- 1 fresh .. 0 gone

uniform vec4  base;          // item.color -- the hole's own hue
uniform vec3  hot;           // what the gas is at its hottest
uniform vec3  beyond;        // the faint tint of the sky seen through the throat
uniform float throat;        // the throat's radius, 0..1

out vec4 finalColor;

#include "body.glsl"

// A field of stars: a jittered grid, a point of light in each cell that has one. `p` is in
// field units, `dens` how many cells have a star.
float Stars(vec2 p, float dens, float sz)
{
    vec2  i = floor(p), f = fract(p);
    float h = bHash(vec3(i, 3.7));
    if (h > dens)
        return 0.0;
    vec2  at = vec2(bHash(vec3(i, 9.1)), bHash(vec3(i, 5.3))) * 0.7 + 0.15;
    float d = length(f - at);
    float b = 0.4 + 0.6 * bHash(vec3(i, 1.9));
    return b * smoothstep(sz, 0.0, d);
}

void main()
{
    vec4  texel = texture(texture0, fragTexCoord) * colDiffuse * fragColor;
    vec2  local = bLocal(centre, radius);
    float r = length(local);
    if (r > 1.0)
        discard;

    float l = clamp(life, 0.0, 1.0);
    float flick = 1.0 - (1.0 - smoothstep(0.0, 0.35, l)) *
                            (0.5 + 0.5 * sin(time * 9.0 + seed * 40.0) * sin(time * 3.7 + seed * 13.0));
    int   oct = bOctaves(radius, 4);
    vec3  off = vec3(seed * 97.0 + 1.0, seed * 211.0 + 4.0, seed * 53.0);

    // What this hole is like, from its seed alone.
    float s1 = fract(seed * 7.31 + 0.17), s2 = fract(seed * 13.7 + 0.53), s3 = fract(seed * 3.91 + 0.71);
    float armN = s1 < 0.45 ? 1.0 : (s1 < 0.88 ? 2.0 : 3.0);
    float tight = 3.2 + 3.6 * s2;                  // how hard the arms wind
    float turb = 0.55 + 0.9 * s3;                  // how torn they are
    float hand = s3 < 0.5 ? 1.0 : -1.0;            // which way they wind

    float tr = throat * (0.6 + 0.4 * l);           // the throat narrows as it dies
    float ang = atan(local.y, local.x);
    float u = log(max(r, 0.03));

    // --- Gas: arms wound round the throat, falling in ---------------------------------------
    // Two copies of the pattern, half a loop apart, each shifted inward along the spiral by its
    // own phase and faded out as it wraps: the gas is seen to drift toward the throat and there
    // is no jump when a loop restarts. The noise is read in the wound coordinates, so the clumps
    // follow the arms; a warp of its own tears them.
    float T = fract(time / 32.0);
    float gasSum = 0.0;
    for (int k = 0; k < 2; k++)
    {
        float f = fract(T + 0.5 * float(k));
        float w = 1.0 - abs(2.0 * f - 1.0);
        float v = u + f * 1.1;
        float wind = hand * ang * armN + tight * v;
        vec3  gp = vec3(cos(wind), sin(wind), v * 1.6) * 1.7 + off + float(k) * vec3(7.0, 3.0, 1.0);
        float tear = bFbm(gp * 1.9 + bLoop(time, 8192.0, 0.4), 2) - 0.5;
        wind += turb * 3.2 * tear;
        vec3  gq = vec3(cos(wind), sin(wind), v * 1.6) * 2.2 + off + float(k) * vec3(7.0, 3.0, 1.0);
        float g = bFbm(gq, oct);
        float arm = pow(0.5 + 0.5 * cos(wind), 2.2);
        float d = arm * (0.3 + 1.3 * g) + 0.22 * g;
        gasSum += w * d;
    }
    float g2 = bFbm(vec3(local * 2.0, 1.0) + off, 2);
    float body = gasSum;

    // How far the gas reaches: far when fresh, a shrunken ragged knot when ending.
    float reachR = mix(0.36, 0.78, l) + (g2 - 0.5) * 0.25;
    float fall = 1.0 - smoothstep(tr * 1.1, reachR + 0.25, r + 0.25 * (g2 - 0.5));
    float gas = clamp((body - 0.3) * 2.0, 0.0, 1.0) * fall;
    gas = gas * gas * (3.0 - 2.0 * gas);
    gas *= smoothstep(tr * 0.9, tr * 1.5, r);       // not inside the throat

    // Beamed: the side turning toward the viewer is brighter.
    float beamA = ang - 0.6 + seed * 6.0;
    float beam = 0.55 + 0.45 * sin(beamA);
    float heat = clamp(1.0 - (r - tr) / max(0.5 * (reachR - tr), 0.05), 0.0, 1.0);
    vec3  gcol = mix(base.rgb * 1.1, hot, heat * heat * 0.75);
    gcol = mix(gcol, vec3(1.0, 0.28, 0.12), (1.0 - l) * 0.65);   // cools to red as it ends
    gcol *= (0.2 + 1.0 * gas) * gas * beam * (0.4 + 0.6 * l) * flick * 1.7;

    // --- The lens: the sky behind, bent toward the throat ----------------------------------
    float pull = 0.05 * (0.4 + 0.6 * l);
    float rs = r + pull / max(r, 0.04) - pull * 1.6;
    vec2  bent = (r > 0.0001 ? local / r : vec2(0.0)) * max(rs, 0.0);
    vec2  sky = bent * 9.0 + vec2(seed * 31.0, seed * 17.0);
    float st = Stars(sky, 0.22, 0.11) + 0.6 * Stars(sky * 2.3 + 11.0, 0.2, 0.1);
    float sm = smoothstep(0.62, 0.15, r);
    float ca = cos(0.5 * sm), sa = sin(0.5 * sm);
    st += sm * 0.9 * Stars(vec2(ca * sky.x - sa * sky.y, sa * sky.x + ca * sky.y) * 0.8, 0.22, 0.13);
    float skyMask = (1.0 - smoothstep(0.55, 1.0, r)) * smoothstep(tr * 1.0, tr * 1.8, r);
    vec3  skyCol = vec3(0.85, 0.9, 1.0) * st * skyMask * (0.35 + 0.65 * l);

    // --- The throat: dark, with a thin uneven rim ------------------------------------------
    float edgeN = (bNoise(vec3(ang * 3.0, r * 6.0, seed * 9.0 + time * 0.05)) - 0.5) * 0.1;
    float rr = r + edgeN * tr;
    float inT = 1.0 - smoothstep(tr - 0.03, tr + 0.012, rr);
    // The far side's sky: a few lensed stars, smeared, in nearly black.
    vec2  tsky = local / max(tr, 0.05) * 3.0 + vec2(seed * 7.0, seed * 3.0);
    float ts = Stars(tsky, 0.18, 0.09) * smoothstep(0.0, 0.5, r / max(tr, 0.05));
    vec3  tcol = beyond * 0.05 * (1.0 - r / max(tr, 0.05)) + vec3(0.7, 0.8, 1.0) * ts * 0.35;
    tcol *= 0.5 + 0.5 * l;

    // The rim: light that has gone round the throat, thin and uneven, strongest toward the
    // viewer; a soft halo outside it. Not a ring drawn: the gas edge's own brightness.
    float rimN = bNoise(vec3(ang * 2.0, seed * 5.0, time * 0.08 + 3.0));
    float rim = exp(-pow((rr - tr * 1.04) / (0.012 + 0.012 * tr), 2.0));
    float halo = exp(-(rr - tr) * 9.0) * step(tr, rr);
    float rimBeam = 0.18 + 0.82 * pow(0.5 + 0.5 * sin(beamA + 0.4), 2.0);
    vec3  rimCol = mix(hot, vec3(1.0, 0.3, 0.15), (1.0 - l) * 0.7) *
                   (rim * 1.5 + halo * 0.22) * rimBeam * (0.45 + 0.9 * rimN) * (0.25 + 0.75 * l) * flick;

    vec3 col = gcol + skyCol + rimCol;
    col = mix(col, tcol, inT);

    // Alpha: whatever is lit; the rest is the sky behind, which is the point. The throat is
    // nearly opaque: it is dark because it hides the sky, not because it is lit dark.
    col = vec3(1.0) - exp(-col * 1.1);
    float lum = max(col.r, max(col.g, col.b));
    float a = clamp(lum * 1.5, 0.0, 1.0);
    a = max(a, inT * 0.92);
    a *= 1.0 - smoothstep(0.5, 1.0, r);
    a *= texel.a;
    if (a <= 0.004)
        discard;
    finalColor = vec4(min(col / max(a, 0.05), vec3(1.6)), a);
}
