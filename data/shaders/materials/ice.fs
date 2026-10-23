#version 330

// ice -- a frozen world (#426), after Europa and Enceladus: a shell of ice crossed by long
// stained cracks (lineae), patches of broken "chaos" ground, a few young bright craters,
// and polar caps of fresh frost that are *whiter than the rest* -- the caps only show if
// the ground between them is not already white, so the old ice is a duller blue-grey.
//
// What is seeded: the pattern of cracks, the chaos, the caps' reach and the ice's tint.

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
uniform float seed;          // item.seed
uniform float turn;          // item.angle

uniform float tilt;          // degrees
uniform float caps;          // sine of the latitude the caps begin at
uniform vec3  ice;           // young ice
uniform vec3  old;           // old, dusted ice: the ground between the caps
uniform vec3  lineae;        // the stain in the cracks
uniform vec3  cap;           // fresh polar frost
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
        vec4 halo = bAirOutside(local, r, l, air, airDensity, 0.03, lightAmount);
        if (halo.a <= 0.003)
            discard;
        finalColor = vec4(bTint(halo.rgb, lightSurface, lightAmount), halo.a * texel.a);
        return;
    }

    vec3 n = bNormal(local);
    vec3 b = bToBody(n, radians(tilt), turn);
    int  oct = bOctaves(radius, 5);
    vec3 off = vec3(seed * 271.0 + 5.0, seed * 433.0, seed * 127.0 + 17.0);
    vec3 p = b * 1.8 + off;

    // The shell: young ice and old, in broad patches.
    float age = bFbm(p * 0.9, max(oct - 2, 2));
    vec3  albedo = mix(ice, old, smoothstep(0.35, 0.7, age));
    albedo *= 0.92 + 0.16 * bNoise(p * 12.0);

    // Lineae: the crests of ridged noise, thin and long, in two sets crossing at an angle,
    // stained where they are oldest.
    // A linea is a long arc of a great circle -- the shell cracked by the tide pulling it one
    // way and then another -- so each is the set of points a seeded plane through the centre
    // passes through, kept to an arc of it, and nudged by the noise so it wanders a little.
    // A pixel or so wide whatever the zoom, with a fainter stain either side.
    float px = 1.0 / max(radius, 1.0);
    float wander = (bNoise(p * 2.5) - 0.5) * 0.03;
    float lines = 0.0, stain = 0.0;
    int   count = radius > 60.0 ? 18 : 9;
    for (int i = 0; i < 18; i++)
    {
        if (i >= count)
            break;
        vec3  h = bHash3(vec3(float(i) * 5.31, seed * 71.0, 8.3));
        vec3  plane = normalize(h * 2.0 - 1.0 + 0.0001);
        vec3  along = normalize(bHash3(h * 13.1 + 2.0) * 2.0 - 1.0 + 0.0001);
        float arc = smoothstep(0.1, 0.35, dot(b, along) + 0.55 * h.z);  // only part of the circle
        float dist = abs(dot(b, plane) + wander);
        float width = mix(0.6, 1.4, h.x) * max(px * 1.1, 0.0025);
        lines = max(lines, (1.0 - smoothstep(width * 0.5, width * 1.5, dist)) * arc);
        stain = max(stain, (1.0 - smoothstep(width * 1.5, width * 7.0, dist)) * arc);
    }
    // Fainter on a small disc, where a line a pixel wide is a large part of the picture.
    float reads = 0.45 + 0.55 * smoothstep(40.0, 200.0, radius);
    albedo = mix(albedo, lineae, clamp(lines * 0.8 + stain * 0.3, 0.0, 1.0) * (0.6 + 0.4 * age) * reads);

    // Chaos terrain: broken, darker, mottled ice, in a few places.
    float chaos = smoothstep(0.66, 0.72, bFbm(p * 1.3 + 31.0, 3));
    albedo = mix(albedo, mix(old, lineae, 0.25) * (0.75 + 0.5 * bNoise(p * 24.0)), chaos * 0.55);

    // A few young craters, bright and rayed: the only thing on the plains that is new.
    vec3  bend = vec3(0.0);
    float pits = bCraterCells(b, 6.0, seed * 17.0 + 1.0, 0.25, bend);
    if (radius > 80.0)
        pits += bCraterCells(b, 14.0, seed * 17.0 + 2.0, 0.3, bend);
    albedo *= 1.0 - pits * 0.5;

    // The caps: fresh frost, a torn edge, and real contrast with the ground they cover.
    // The noise is only worked out near the caps, which is all it is for.
    float edge = caps + (fract(seed * 6.1) - 0.5) * 0.1;
    float capMask = 0.0;
    if (abs(b.y) > edge - 0.1)
    {
        edge += (bFbm(p * 2.5 + 51.0, min(oct, 4)) - 0.5) * 0.16;
        capMask = smoothstep(edge - 0.02, edge + 0.02, abs(b.y));
    }
    albedo = mix(albedo, cap * (0.95 + 0.05 * bNoise(p * 8.0)), capMask);

    vec3  lb = bToBody(l, radians(tilt), turn);
    vec3  nb = normalize(b - bend * (1.0 - capMask));
    float d = dot(nb, lb);
    float day = clamp((d + 0.03) / 1.03, 0.0, 1.0);
    float floorLevel = ambient * 0.42;
    float lit = dot(lightDir, lightDir) < 0.0001 ? 1.0 : floorLevel + (1.0 - floorLevel) * day * lightAmount;
    vec3 col = bTint(albedo, lightSurface, lightAmount) * lit;

    // Ice is a mirror at a low angle: a broad sheen where it faces between the star and us.
    vec3  hv = normalize(l + vec3(0.0, 0.0, 1.0));
    col += lightTint.rgb * pow(max(dot(n, hv), 0.0), 18.0) * 0.12 * lightAmount;
    col = bAirInside(col, n, l, air, airDensity, lightAmount);

    finalColor = vec4(col, texel.a * bCoverage(r, radius));
}
