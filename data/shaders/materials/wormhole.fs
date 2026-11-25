#version 330

// wormhole -- a tear in space, drawn as a phenomenon and not as an object (#618). There is no
// structure, no ring and no tick in it. What a viewer sees is what a throat that bends light
// would do to everything near it:
//
//   - the sky behind it, bent: stars crowd and stretch toward the throat, and the ones nearest
//     are smeared round it, as a lens does;
//   - gas falling in, in arms that wind the way a spiral does, brighter on the side that is
//     turning toward us, lit by nothing but its own heat -- hot and pale inward, the object's
//     own colour outward;
//   - the throat itself, where the far side's sky shows through -- a different, denser sky
//     tinted by where it leads -- and a dark seam where the bending is greatest;
//   - an edge that is wherever the gas thins out, never a circle.
//
// `life` (item.intensity) is how much of it is left: 1 is a mouth just opened, near 0 one that
// is about to close -- the arms thin out, the throat narrows and gutters, and the gas cools
// to red. Nothing here reads a light of the system: it is its own light.
//
// f(time, seed) (#136): the swirl turns on a 64 s loop (which divides the clock's wrap), the
// gas churns on bLoop, stars are a function of the seed alone.

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
uniform vec3  beyond;        // the tint of the sky seen through the throat
uniform float throat;        // the throat's radius, 0..1
uniform float arms;          // how many arms of the spiral

out vec4 finalColor;

#include "body.glsl"

const float TAU2 = 6.2831853;

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
    float ph = fract(time / 64.0);
    float flick = 1.0 - (1.0 - smoothstep(0.0, 0.35, l)) *
                            (0.5 + 0.5 * sin(time * 9.0 + seed * 40.0) * sin(time * 3.7 + seed * 13.0));
    int   oct = bOctaves(radius, 5);
    vec3  off = vec3(seed * 97.0 + 1.0, seed * 211.0 + 4.0, seed * 53.0);

    float tr = throat * (0.55 + 0.45 * l);               // the throat narrows as it dies
    float ang = atan(local.y, local.x);

    // --- Gas: arms wound round the throat --------------------------------------------------
    // The angle is shifted by the log of the radius, which is what makes a spiral, and the
    // whole turns once a loop. The noise is read in those wound coordinates, so the clumps
    // follow the arms; a second, slower read breaks them up.
    float wind = ang * arms + 3.2 * log(max(r, 0.02)) * arms * 0.5 - ph * TAU2 * 2.0;
    vec3  gp = vec3(cos(wind) * r * 5.0, sin(wind) * r * 5.0, r * 4.0) + off + bLoop(time, 8192.0, 0.3);
    float g1 = bFbm(gp, oct);
    float g2 = bFbm(gp * 0.5 + vec3(5.0, 1.0, 3.0), 2);
    float armWave = 0.5 + 0.5 * cos(wind);
    float fil = bRidge(gp * 2.3 + 17.0, 4);
    float body = mix(g1, armWave, 0.25) * (0.5 + 0.9 * g2) * (0.55 + 0.9 * fil) + 0.06;

    // How far the gas reaches: far when fresh, a shrunken ragged knot when ending.
    float reachR = mix(0.4, 0.8, l) + (g2 - 0.5) * 0.3;
    float fall = 1.0 - smoothstep(tr * 1.2, reachR + 0.3, r + 0.3 * (g1 - 0.5));
    float gas = clamp((body - 0.3) * 2.2, 0.0, 1.0) * fall;
    gas = gas * gas * (3.0 - 2.0 * gas);
    gas *= smoothstep(tr * 0.7, tr * 1.35, r);           // not inside the throat

    // Beamed: the side turning toward the viewer is brighter.
    float beam = 0.62 + 0.38 * sin(ang - 0.6 + seed * 6.0);
    float heat = clamp(1.0 - (r - tr) / max(0.5 * (reachR - tr), 0.05), 0.0, 1.0);
    vec3  gcol = mix(base.rgb * 1.25, hot, heat * heat * heat * 0.8);
    gcol = mix(gcol, vec3(1.0, 0.3, 0.15), (1.0 - l) * 0.6);   // cools to red as it ends
    gcol *= (0.25 + 1.1 * gas) * gas * beam * (0.5 + 0.5 * l) * flick * 2.0;

    // --- The lens: the sky behind, bent toward the throat ----------------------------------
    // Sampled at a radius pulled inward as 1/r, so the stars nearest the throat are the ones
    // that were far out, crowded, and stretched round it.
    float pull = 0.045 * (0.4 + 0.6 * l);
    float rs = r + pull / max(r, 0.04) - pull * 1.6;
    vec2  bent = (r > 0.0001 ? local / r : vec2(0.0)) * max(rs, 0.0);
    vec2  sky = bent * 9.0 + vec2(seed * 31.0, seed * 17.0);
    float st = Stars(sky, 0.22, 0.11) + 0.6 * Stars(sky * 2.3 + 11.0, 0.2, 0.1);
    // The stars are smeared tangentially close in: a second read, rotated a little.
    float sm = smoothstep(0.62, 0.15, r);
    float ca = cos(0.5 * sm), sa = sin(0.5 * sm);
    st += sm * 0.9 * Stars(vec2(ca * sky.x - sa * sky.y, sa * sky.x + ca * sky.y) * 0.8, 0.22, 0.13);
    float skyMask = (1.0 - smoothstep(0.55, 1.0, r)) * smoothstep(tr * 0.8, tr * 1.6, r);
    vec3  skyCol = vec3(0.85, 0.9, 1.0) * st * skyMask * (0.4 + 0.6 * l);

    // --- The throat: another sky, denser, tinted ------------------------------------------
    float edgeN = (bNoise(vec3(ang * 3.0, r * 6.0, seed * 9.0 + time * 0.05)) - 0.5) * 0.12;
    float inT = 1.0 - smoothstep(tr - 0.16, tr + 0.02, r + edgeN * 2.0);
    vec2  tsky = local / max(tr, 0.05) * 5.0 + vec2(seed * 7.0, seed * 3.0);
    float ts = Stars(tsky * 1.0, 0.3, 0.1) + Stars(tsky * 2.7 + 4.0, 0.25, 0.09);
    float tglow = 0.05 + 0.32 * pow(1.0 - r / max(tr, 0.05), 2.0);
    vec3  tcol = mix(beyond, base.rgb, 0.35) * (tglow + 0.4 * bFbm(vec3(tsky * 0.5, 2.0) + off, 3)) + vec3(1.0) * ts * 0.9;
    tcol *= (0.55 + 0.45 * l) * (1.0 - 0.6 * smoothstep(0.5, 1.0, r / max(tr, 0.05)));

    // The seam where the bending is greatest: a dark, not a line -- the gas's own shadow.
    float seam = 1.0 - 0.7 * exp(-pow((r - tr * 1.25) / (0.05 + 0.03 * tr), 2.0));

    vec3  col = (gcol + skyCol) * seam;
    col = mix(col, tcol, inT);

    // Alpha: whatever is lit; the rest is the sky behind, which is the point.
    col = vec3(1.0) - exp(-col * 1.1);
    float lum = max(col.r, max(col.g, col.b));
    float a = clamp(lum * 1.5, 0.0, 1.0);
    a = max(a, inT * 0.85);
    a *= 1.0 - smoothstep(0.5, 1.0, r);
    a *= texel.a;
    if (a <= 0.004)
        discard;
    // Light adds: the colour is what it would be over black, so over the sky it is `col` plus
    // the sky showing through (1 - a).
    finalColor = vec4(min(col / max(a, 0.05), vec3(1.6)), a);
}
