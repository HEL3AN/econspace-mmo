#version 330

// terran -- a world with seas (#426). Continents from warped fractal noise, so a coast is a
// coast at every zoom rather than a ring of discs; a shelf of shallow water along it; land
// coloured by height, latitude and how dry it is; polar ice with an edge that is torn, not
// drawn; cloud that churns in place and turns a little faster than the ground; the sun's
// glint on the sea; cities on the night side; and air on the lit limb.
//
// What is seeded: where the land is, how much of it there is (sea level), how cold the
// world is (the caps), how cloudy, and a small shift of the palette -- so two ocean worlds
// are two places. The palette itself is the material's constants.

in vec2 fragTexCoord;
in vec4 fragColor;

uniform sampler2D texture0;
uniform vec4 colDiffuse;

uniform vec2  centre;        // item.screenPos  -- the body's centre, pixels, bottom left
uniform float radius;        // item.screenSize -- the body's radius, pixels
uniform vec2  lightDir;      // light.dir
uniform vec4  lightTint;     // light.tint
uniform vec4  lightSurface;  // light.surface
uniform float lightAmount;   // light.strength
uniform float ambient;       // light.ambient
uniform float time;          // clock.time
uniform float seed;          // item.seed
uniform float turn;          // item.angle -- the planet's turn, from the part's spin

uniform float tilt;          // degrees the pole leans toward the viewer
uniform float sea;           // sea level, 0..1 of the height noise: more is more ocean
uniform float caps;          // sine of the latitude polar ice begins at
uniform float cloud;         // 0..1, how much of the sky is cloud
uniform float cities;        // 0..1, how bright the night side's cities are
uniform vec3  deep;          // open ocean
uniform vec3  shallow;       // the shelf
uniform vec3  lowland;       // wet, low land
uniform vec3  dry;           // desert
uniform vec3  highland;      // mountains
uniform vec3  ice;           // caps and snow
uniform vec3  air;           // the colour of the sky seen edge-on
uniform float airDensity;    // 0 airless .. 1 thick

out vec4 finalColor;

#include "body.glsl"

void main()
{
    vec4 texel = texture(texture0, fragTexCoord) * colDiffuse * fragColor;
    vec2  local = bLocal(centre, radius);
    float r = length(local);
    vec3  l = bToLight(lightDir);

    // Past the limb: only the air.
    if (r > 1.0)
    {
        vec4 halo = bAirOutside(local, r, l, air, airDensity, 0.05, lightAmount);
        if (halo.a <= 0.003)
            discard;
        finalColor = vec4(bTint(halo.rgb, lightSurface, lightAmount), halo.a * texel.a);
        return;
    }

    vec3 n = bNormal(local);
    vec3 b = bToBody(n, radians(tilt), turn);
    int  oct = bOctaves(radius, 7);

    // Where this world is in noise space: its own place, far from every other's.
    vec3 off = vec3(seed * 517.0, seed * 211.0 + 13.0, seed * 389.0 + 7.0);

    // Height: fractal noise through a gentle warp, which is what turns blobs into coasts
    // with peninsulas and gulfs.
    vec3  p = b * 1.55 + off;
    vec2  w = vec2(bNoise(p * 1.3 + 3.1), bNoise(p * 1.3 + 7.7)) - 0.5;
    vec3  warp = vec3(w.x, w.x + w.y, w.y);
    float broad;
    float h = bFbmSplit(p + warp * 0.9, oct, 3, broad);
    float seaLevel = sea + (fract(seed * 7.31) - 0.5) * 0.06;

    float lat = b.y;                                      // sine of the latitude
    float coldness = abs(lat) + w.y * 0.1;
    float capEdge = caps + (fract(seed * 3.17) - 0.5) * 0.08;

    // The sea: shallow on the shelf, deep beyond it.
    float depth = clamp((seaLevel - h) / 0.12, 0.0, 1.0);
    vec3  water = mix(shallow, deep, smoothstep(0.0, 1.0, depth));

    // The coast, smoothed over one pixel of height rather than cut, so it does not stair-step.
    float edgeWidth = max(fwidth(h) * 0.75, 0.0005);

    // The land, coloured by how high and how dry it is, with grain finer than the height
    // field carries -- without it a continent is a smooth smear of two colours. Worked out
    // only where there is land: most of an ocean world is sea, and this is most of its cost.
    float grain = 0.5;
    vec3  land = water;
    if (h > seaLevel - edgeWidth * 2.0)
    {
        grain = bNoise(p * 17.0) * 0.6 + bNoise(p * 41.0) * 0.4;
        float rise = clamp((h - seaLevel) / 0.22, 0.0, 1.0);
        // Dry where the descending air of the subtropics is, and wherever the noise says.
        float moisture = bNoise(p * 2.2 + 5.0) + 0.3 * (grain - 0.5) + 0.25 -
                         0.45 * exp(-pow((abs(lat) - 0.42) / 0.16, 2.0));
        vec3 ground = mix(dry, lowland, smoothstep(0.3, 0.6, moisture));
        ground *= 0.82 + 0.36 * grain;
        // A coast is lighter for a pixel or two -- beach and surf -- which is what separates
        // land from water at a glance from far out.
        ground = mix(ground * 1.2, ground, smoothstep(0.0, 0.02, h - seaLevel));
        land = mix(ground, highland * (0.8 + 0.4 * grain), smoothstep(0.4, 0.9, rise));
        // Snow on the highest ground, reaching lower toward the poles.
        float snowLine = 0.97 - 0.4 * smoothstep(0.45, 0.9, abs(lat));
        land = mix(land, ice, smoothstep(snowLine - 0.03, snowLine + 0.03, rise + 0.08 * grain));
    }

    float isLand = smoothstep(seaLevel - edgeWidth, seaLevel + edgeWidth, h);
    vec3  albedo = mix(water, land, isLand);
    float wet = 1.0 - isLand;  // how much of the glint the surface takes: the sea, not the land

    // The caps: sea ice and land ice alike, with a torn edge and a brighter heart.
    // Only near the caps: the torn edge is all the noise is for.
    float capMask = 0.0;
    if (coldness > capEdge - 0.12)
    {
        float capEdgeNoise = capEdge + (bFbm(p * 4.0 + 19.0, min(oct, 4)) - 0.5) * 0.18;
        capMask = smoothstep(capEdgeNoise - 0.015, capEdgeNoise + 0.015, coldness);
    }
    albedo = mix(albedo, ice * mix(0.88, 1.0, grain), capMask);
    wet *= 1.0 - capMask;

    // Relief: the land's own slope from how its height changes across the screen, which
    // costs nothing more to know. Hills facing the star are lit, the far slopes shaded --
    // the difference between a map and a photograph.
    // Only on the day side, and only as a share of the light already there: a slope can
    // shade a hill, it cannot light one in the night.
    float diffuse = bDiffuse(n, l, 0.12, lightAmount, ambient);
    if (radius > 40.0)
    {
        vec2  slope = vec2(dFdx(broad), dFdy(broad)) * radius * 0.5 * isLand * (1.0 - capMask);
        vec3  relief = normalize(n - vec3(slope, 0.0));
        float day = smoothstep(0.0, 0.35, dot(n, l));
        diffuse *= clamp(1.0 + (dot(relief, l) - dot(n, l)) * 1.5 * day, 0.55, 1.35);
    }
    vec3  col = bTint(albedo, lightSurface, lightAmount) * diffuse;

    // The sun on the sea: a soft glint where the surface mirrors the light to the viewer.
    vec3  hv = normalize(l + vec3(0.0, 0.0, 1.0));
    float glint = pow(max(dot(n, hv), 0.0), 140.0) * wet * lightAmount;
    col += lightTint.rgb * glint * 0.55;

    // Cities, on land, where it is night.
    float night = 1.0 - smoothstep(-0.2, 0.08, dot(n, l));
    if (cities > 0.0 && wet < 0.5 && night > 0.0 && radius > 24.0)
    {
        float town = bNoise(p * 38.0) * bNoise(p * 9.0 + 2.0);
        float lit = smoothstep(0.42, 0.62, town) * (1.0 - capMask);
        col += vec3(1.0, 0.72, 0.38) * lit * night * cities;
    }

    // Cloud: its own fractal, warped harder and banded by latitude -- a belt over the
    // equator, the storm tracks of the middle latitudes, clear subtropics -- and churning in
    // place on a loop rather than drifting off forever.
    if (cloud > 0.0)
    {
        // Stretched along the parallels: weather is carried round a planet by its winds, so
        // a front is long east to west and short north to south.
        vec3 cb = bToBody(n, radians(tilt), turn + TAU * time / 2048.0);
        vec3 cp = vec3(cb.x, cb.y * 1.7, cb.z) * 2.6 + off.zxy + bLoop(time, 1024.0, 0.6);
        vec3  cw = vec3(bNoise(cp * 0.8), bNoise(cp * 0.8 + 4.3), bNoise(cp * 0.8 + 8.1)) - 0.5;
        float c = bFbm(cp + cw * 2.0, clamp(oct - 1, 2, 6));
        float belt = 0.55 * exp(-pow(lat / 0.12, 2.0)) +
                     0.45 * exp(-pow((abs(lat) - 0.62) / 0.16, 2.0)) - 0.25 * exp(-pow((abs(lat) - 0.38) / 0.1, 2.0));
        float cover = clamp(cloud + (fract(seed * 5.13) - 0.5) * 0.15 + belt * 0.25, 0.0, 1.0);
        float cut = 1.0 - cover;
        float a = smoothstep(cut - 0.02, cut + 0.14, c);
        // Thinner and greyer toward their own edges, thicker and whiter in their hearts.
        vec3 cloudCol = mix(vec3(0.78, 0.8, 0.84), vec3(0.97), smoothstep(cut, cut + 0.3, c));
        float cloudLight = bDiffuse(n, l, 0.18, lightAmount, ambient);
        col = mix(col, bTint(cloudCol, lightSurface, lightAmount) * cloudLight, a * 0.92);
    }

    // The air over the lit limb, and the faint blue it lays over everything seen through it.
    col = bAirInside(col, n, l, air, airDensity, lightAmount);

    finalColor = vec4(col, texel.a * bCoverage(r, radius));
}
