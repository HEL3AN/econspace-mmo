#version 330

// lava -- a world still molten (#426), after Io and the young Earth: a black crust broken
// into plates, glowing seams between them, seas of open lava in the lows, ash around the
// vents. The glow is light of its own, not light from the star, so the night side is where
// a lava world is most itself: the seams burn on in the dark.
//
// What is seeded: where the plates crack, how much of it is open, the churn of the seas.

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
uniform float time;          // clock.time
uniform float seed;          // item.seed
uniform float turn;          // item.angle

uniform float tilt;          // degrees
uniform vec3  crust;         // cold rock
uniform vec3  ash;           // grey fall-out round the vents
uniform vec3  hot;           // lava, as it glows
uniform vec3  white;         // the hottest of it
uniform float molten;        // 0..1, how much of the world is open lava
uniform vec3  air;
uniform float airDensity;

out vec4 finalColor;

#include "body.glsl"

void main()
{
    vec4  texel = texture(texture0, fragTexCoord) * colDiffuse * fragColor;
    vec2  local = bLocal(centre, radius);
    float r = length(local);
    vec3  l = bToLight(lightDir);

    if (r > 1.0)
    {
        vec4 halo = bAirOutside(local, r, l, air, airDensity, 0.04, lightAmount);
        if (halo.a <= 0.003)
            discard;
        finalColor = vec4(bTint(halo.rgb, lightSurface, lightAmount), halo.a * texel.a);
        return;
    }

    vec3 n = bNormal(local);
    vec3 b = bToBody(n, radians(tilt), turn);
    int  oct = bOctaves(radius, 5);
    vec3 off = vec3(seed * 353.0 + 2.0, seed * 191.0 + 8.0, seed * 239.0);
    vec3 p = b * 1.7 + off;

    // Low ground and high: the lows are where the lava pools.
    float h = bFbm(p, oct);
    float sea = smoothstep(1.0 - molten - 0.03, 1.0 - molten + 0.03, 1.0 - h);

    // The crust is plates -- cells -- and the seams between them are where it glows. Bent by
    // a little noise so the plates are not a honeycomb; churned on a loop so they drift.
    vec3  warp = vec3(bNoise(p * 2.0), bNoise(p * 2.0 + 3.3), bNoise(p * 2.0 + 6.1)) - 0.5;
    vec3  cp = p * 1.9 + warp * 0.8 + bLoop(time, 4096.0, 0.15);
    float edge = radius > 90.0 ? bCells(cp) : 1.0 - bRidge(cp, 2);
    // A seam is a pixel or two of white heat, with a red halo in the rock either side.
    float px = max(fwidth(edge), 0.004);
    float seam = 1.0 - smoothstep(px * 0.5, px * 2.5 + 0.02, edge);
    float halo = 1.0 - smoothstep(0.0, 0.16, edge);
    // Only some seams are open: where the crust is thin, which the height says.
    float open = smoothstep(0.66, 0.82, bNoise(p * 3.1 + 17.0) + (1.0 - h) * 0.35);
    seam *= open;
    halo *= open;

    // The rock: basalt, cracked and grained, greyed with ash on the heights.
    float grain = bNoise(p * 15.0) * 0.6 + bNoise(p * 37.0) * 0.4;
    vec3  rock = mix(crust, ash, smoothstep(0.55, 0.85, h + 0.1 * grain)) * (0.7 + 0.6 * grain);

    // Light on the rock only: the lava lights itself.
    float diffuse = bDiffuse(n, l, 0.06, lightAmount, ambient);
    vec3  col = bTint(rock, lightSurface, lightAmount) * diffuse;

    // The seas: open lava, crusted over in drifting plates of its own with bright gaps.
    float gap = 0.0;
    if (sea > 0.0)
    {
        float skin = radius > 90.0 ? bCells(p * 9.0 + warp + bLoop(time, 2048.0, 0.2)) : bNoise(p * 9.0);
        gap = 1.0 - smoothstep(0.02, 0.12, skin);
    }
    float pool = sea * (0.55 + 0.45 * gap);

    float heat = clamp(max(pool, seam), 0.0, 1.0);
    vec3  glow = mix(hot * 0.6, white, smoothstep(0.6, 1.0, max(seam, gap * sea) * (0.8 + 0.3 * grain)));
    col = mix(col, glow, heat);
    // ...and the rock beside the heat dull red, so a seam is not a cut-out.
    col += hot * 0.22 * halo * (1.0 - heat);

    col = bAirInside(col, n, l, air, airDensity, lightAmount);
    finalColor = vec4(col, texel.a * bCoverage(r, radius));
}
