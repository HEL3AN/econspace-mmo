#version 330

// rock -- a world of bare stone (#426): a rocky planet, a desert world, a moon, a rogue
// planet in the dark, and the rocks of an asteroid field. Mottled ground from two palettes
// the seed mixes between -- grey and cratered like Mercury, rust and dust like Mars -- with
// dark plains, craters that are bowls with lit and shadowed walls and raised rims, a few
// bright young ones with rays, optional polar frost and a thin air on the limb.
//
// A crater is not a disc of a darker colour. It is a dent, and what makes a dent read is
// light: the wall facing the star is bright, the wall facing away is in shadow, and the rim
// throws a lip of light. So the craters here bend the surface normal, and the light does the
// rest.
//
// `mode` 0 is a turning body (a planet: its turn spins it about its pole); 1 is a body that
// only turns in the picture (a moon, a rock tumbling in a field), which is also how the
// rocks of a field get a different face each.

in vec2 fragTexCoord;
in vec4 fragColor;

uniform sampler2D texture0;
uniform vec4 colDiffuse;

uniform vec2  centre;        // item.screenPos
uniform float radius;        // item.screenSize
uniform vec2  lightDir;      // light.dir
uniform vec4  lightTint;     // light.tint
uniform vec4  lightSurface;  // light.surface
uniform float lightAmount;   // light.strength
uniform float ambient;       // light.ambient
uniform float seed;          // item.seed or item.partSeed
uniform float turn;          // item.angle

uniform float mode;          // 0 a turning planet, 1 a moon or a rock (see above)
uniform float tilt;          // degrees, mode 0
uniform vec3  greyLow;       // palette A: dark and light ground
uniform vec3  greyHigh;
uniform vec3  rustLow;       // palette B
uniform vec3  rustHigh;
uniform float rustiness;     // 0..1: how far toward palette B the seed may go; 1 = all the way
uniform float maria;         // 0..1, how much of the ground is dark plains
uniform float craters;       // how many craters: 0 none .. 1 a moon's worth
uniform float frost;         // sine of the latitude polar frost begins at; 0 = none
uniform vec3  air;
uniform float airDensity;    // 0 airless
uniform float lumpy;         // 0..1, how far a rock's outline departs from round (mode 1)

out vec4 finalColor;

#include "body.glsl"

void main()
{
    vec4  texel = texture(texture0, fragTexCoord) * colDiffuse * fragColor;
    vec2  local = bLocal(centre, radius);
    float r = length(local);
    vec3  l = bToLight(lightDir);
    float sd = fract(seed * 13.37);

    if (r > 1.0)
    {
        if (airDensity <= 0.0 || mode > 0.5)
            discard;
        vec4 halo = bAirOutside(local, r, l, air, airDensity, 0.04, lightAmount);
        if (halo.a <= 0.003)
            discard;
        finalColor = vec4(bTint(halo.rgb, lightSurface, lightAmount), halo.a * texel.a);
        return;
    }

    vec3 n = bNormal(local);
    vec3 b;
    if (mode < 0.5)
        b = bToBody(n, radians(tilt), turn);
    else
    {
        // Turned in the picture only, and tipped by the seed so no two rocks show the same
        // face of the same noise.
        float c = cos(turn), s = sin(turn);
        vec3  m = vec3(c * n.x - s * n.y, s * n.x + c * n.y, n.z);
        b = bToBody(m, sd * 2.4, sd * 9.1);
    }
    int  oct = bOctaves(radius, 5);
    vec3 off = vec3(seed * 431.0 + 3.0, seed * 157.0, seed * 293.0 + 11.0);
    vec3 p = b * 2.0 + off;

    // The ground: mottled at every scale, between the dark and light of the palette.
    float g = bFbm(p, oct);
    float mix01 = clamp(rustiness * fract(seed * 4.71) * 1.25, 0.0, 1.0);
    vec3  lo = mix(greyLow, rustLow, mix01);
    vec3  hi = mix(greyHigh, rustHigh, mix01);
    vec3  albedo = mix(lo, hi, smoothstep(0.25, 0.75, g));

    // Dark plains: old floods of basalt, broad and smooth-edged.
    float plain = smoothstep(0.53, 0.57, bFbm(p * 0.7 + 17.0, 3)) * maria;
    albedo = mix(albedo, lo * 0.62, plain);

    // The light, taken into the same frame as the ground by the same turns.
    vec3 lb;
    if (mode < 0.5)
        lb = bToBody(l, radians(tilt), turn);
    else
    {
        float c = cos(turn), s = sin(turn);
        lb = bToBody(vec3(c * l.x - s * l.y, s * l.x + c * l.y, l.z), sd * 2.4, sd * 9.1);
    }
    // Craters: a dozen or two seeded bowls, large ones rare -- and under them a lattice of
    // small ones, a layer for every doubling of size on screen. Their slopes bend the normal
    // in the body's frame; the light, taken into that frame, does the rest.
    vec3  bend = vec3(0.0);
    float bright = 0.0;
    float kept = 1.0 - plain * 0.7;  // older plains have buried the smaller craters
    float pits = 0.0;
    if (craters > 0.0)
    {
        pits += bCraterCells(b, 5.0, seed * 31.0 + 1.0, 0.55 * craters * kept, bend);
        if (radius > 40.0)
            pits += bCraterCells(b, 11.0, seed * 31.0 + 2.0, 0.6 * craters * kept, bend);
        if (radius > 110.0)
            pits += bCraterCells(b, 23.0, seed * 31.0 + 3.0, 0.65 * craters * kept, bend);
        if (radius > 260.0)
            pits += bCraterCells(b, 47.0, seed * 31.0 + 4.0, 0.7 * craters * kept, bend);
    }
    albedo *= 1.0 - pits;
    int count = int(craters * clamp(radius / 10.0, 3.0, 18.0));
    for (int i = 0; i < 18; i++)
    {
        if (i >= count)
            break;
        vec3  h = bHash3(vec3(float(i) * 7.13, seed * 97.0, 3.7));
        vec3  c = normalize(h * 2.0 - 1.0 + 0.0001);
        float size = mix(0.07, 0.34, pow(fract(h.x * 5.3 + h.y), 3.0));
        vec3  to = b - c;
        float d = length(to) / size;
        if (d > 1.6)
            continue;
        vec3 dir = normalize(to - dot(to, b) * b + 0.00001);
        bend += dir * bCraterSlope(d) * 0.13;
        // A young crater is bright, its floor and a halo of thrown-out rock round it.
        if (fract(h.z * 11.0) < 0.12)
            bright += (1.0 - smoothstep(0.6, 1.6, d)) * 0.45 * kept;
        albedo *= mix(1.0, 0.93, (1.0 - smoothstep(0.5, 1.0, d)) * kept);
    }
    albedo = mix(albedo, hi * 1.25, clamp(bright, 0.0, 0.7));

    // Frost on the poles of a cold world, torn at the edge like any cap.
    if (frost > 0.0 && abs(b.y) > frost - 0.08)
    {
        float edge = frost + (bFbm(p * 3.0 + 41.0, 3) - 0.5) * 0.12;
        albedo = mix(albedo, vec3(0.9, 0.9, 0.92), smoothstep(edge - 0.02, edge + 0.02, abs(b.y)));
    }

    // A rock is not a sphere: its outline is the polygon it is drawn in, and its surface
    // lumps up and down with the same noise as its colour.
    vec3 nb = normalize(b - bend);
    if (mode > 0.5 && lumpy > 0.0)
        nb = normalize(nb + (vec3(bNoise(p * 1.7), bNoise(p * 1.7 + 5.0), bNoise(p * 1.7 + 9.0)) - 0.5) * lumpy);

    // Lit in the body's frame, with a short terminator: no air to soften it.
    float soft = airDensity > 0.0 ? 0.08 : 0.02;
    float d0 = dot(nb, lb);
    float day = clamp((d0 + soft) / (1.0 + soft), 0.0, 1.0);
    float floorLevel = ambient * 0.42;
    float lit = floorLevel + (1.0 - floorLevel) * day * lightAmount;
    // Without light from everywhere there is no shadow either.
    if (dot(lightDir, lightDir) < 0.0001)
        lit = 1.0;
    vec3 col = bTint(albedo, lightSurface, lightAmount) * lit;
    if (airDensity > 0.0)
        col = bAirInside(col, n, l, air, airDensity, lightAmount);
    else if (dot(lightDir, lightDir) > 0.0001)
    {
        // No air to scatter it, but the lit limb of bare rock still catches the light at a
        // grazing angle: the thin bright edge that tells a moon from a hole in the sky.
        float edge = smoothstep(0.8, 1.0, r) * max(dot(n, l), 0.0);
        col += lightTint.rgb * edge * 0.18 * lightAmount;
    }

    // A rock's polygon lies inside its radius, so the limb only ever cuts a round body.
    finalColor = vec4(col, texel.a * bCoverage(r, radius));
}
