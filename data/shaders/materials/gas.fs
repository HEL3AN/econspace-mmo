#version 330

// gas -- a gas giant (#426). Not stripes: belts and zones whose edges roil, streaks drawn
// out along the flow, a great oval storm that the bands bend round, white ovals strung along
// a belt's edge, mottled darker poles, limb darkening and a haze on the lit limb.
//
// The storms are not drawn on top of the bands. They twist the coordinates the bands are
// read from, so the belts are carried round a storm the way a river goes round a rock --
// which is the one thing about Jupiter a disc of colour can never fake.
//
// What is seeded: where the bands fall, where the storms are, the palette between its two
// ends (a Jupiter and a Saturn), and the turbulence. The atmosphere churns on a loop.

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
uniform vec3  zoneA;         // the bright zones and the dark belts, at one end of the palette
uniform vec3  beltA;
uniform vec3  zoneB;         // ...and at the other
uniform vec3  beltB;
uniform vec3  storm;         // the great storm's colour
uniform float bands;         // how many belt-zone pairs, pole to pole, roughly
uniform float turbulence;    // 0 laminar .. 1 Jupiter
uniform vec3  air;
uniform float airDensity;

out vec4 finalColor;

#include "body.glsl"

// Turns the point (lon, lat) round a storm at (slon, slat) of half-axes `size`: hard in the
// middle, fading out past its edge. Returns how far inside the storm the point is, 1 at the
// eye. `spin` is the turn at the eye, radians; its sign is which way it rotates.
float swirl(inout float lon, inout float lat, float slon, float slat, vec2 size, float spin)
{
    float dl = lon - slon;
    dl -= TAU * floor(dl / TAU + 0.5);
    vec2  e = vec2(dl * cos(slat) / size.x, (lat - slat) / size.y);
    float d = length(e);
    if (d > 2.2)
        return 0.0;
    float a = spin * exp(-d * d * 1.4);
    float c = cos(a), s = sin(a);
    e = vec2(c * e.x - s * e.y, s * e.x + c * e.y);
    lon = slon + e.x * size.x / cos(slat);
    lat = slat + e.y * size.y;
    return 1.0 - smoothstep(0.75, 1.05, d);
}

void main()
{
    vec4  texel = texture(texture0, fragTexCoord) * colDiffuse * fragColor;
    vec2  local = bLocal(centre, radius);
    float r = length(local);
    vec3  l = bToLight(lightDir);

    if (r > 1.0)
    {
        vec4 halo = bAirOutside(local, r, l, air, airDensity, 0.035, lightAmount);
        if (halo.a <= 0.003)
            discard;
        finalColor = vec4(bTint(halo.rgb, lightSurface, lightAmount), halo.a * texel.a);
        return;
    }

    vec3  n = bNormal(local);
    vec3  b = bToBody(n, radians(tilt), turn);
    int   oct = bOctaves(radius, 7);
    float lat = asin(clamp(b.y, -1.0, 1.0));
    float lon = atan(b.x, b.z);

    // The storms, which bend the coordinates everything else is read from.
    float s1 = fract(seed * 7.77), s2 = fract(seed * 3.31), s3 = fract(seed * 5.19);
    float great = swirl(lon, lat, s1 * TAU, -0.36 - 0.08 * s2, vec2(0.21, 0.1), 4.2);
    float ovals = 0.0;
    int   count = radius > 60.0 ? 5 : 0;
    for (int i = 0; i < 5; i++)
    {
        if (i >= count)
            break;
        float k = float(i);
        float olat = (fract(s3 * 11.0 + k * 0.37) > 0.5 ? 1.0 : -1.0) *
                     (0.5 + 0.25 * fract(s2 * 13.0 + k * 0.61));
        float olon = fract(s1 * 3.0 + k * 0.231) * TAU;
        ovals += swirl(lon, lat, olon, olat, vec2(0.06, 0.04), olat > 0.0 ? -3.0 : 3.0);
    }
    vec3 q = vec3(cos(lat) * sin(lon), sin(lat), cos(lat) * cos(lon));

    // The flow: noise squashed across the latitudes, so it is long along them, churning on
    // a loop. It shifts the latitude the bands are read at -- the roil along a belt's edge.
    vec3  off = vec3(seed * 311.0, seed * 97.0 + 5.0, seed * 173.0 + 9.0);
    vec3  fp = vec3(q.x, q.y * 4.0, q.z) * 2.2 + off + bLoop(time, 2048.0, 0.5);
    float flow = bFbm(fp, oct) - 0.5;
    float eddy = bFbm(vec3(q.x, q.y * 2.0, q.z) * 7.0 + off.yzx + flow * 1.8, max(oct - 2, 2)) - 0.5;
    float y = lat + turbulence * (0.07 * flow + 0.03 * eddy);

    // The bands: two waves of different lengths, so belts come in different widths and are
    // not a barcode, read where the flow has pushed the latitude.
    float ph = s2 * TAU;
    float wave = 0.6 * sin(y * bands + ph) + 0.4 * sin(y * bands * 2.37 + ph * 1.7 + 1.0);
    float zone = smoothstep(-0.35, 0.35, wave + 0.25 * flow);
    // Streaks drawn out along the flow, finer than the bands.
    float streak = bFbm(vec3(q.x * 1.5, y * 22.0, q.z * 1.5) + off * 1.3 + flow * 2.0, clamp(oct - 2, 3, 4));

    float t = fract(seed * 2.13);
    vec3  zc = mix(zoneA, zoneB, t);
    vec3  bc = mix(beltA, beltB, t);
    // Each belt a slightly different brown: a hue that wanders with latitude.
    float hue = bNoise(vec3(y * bands * 0.35, s1 * 9.0, 2.0));
    bc *= mix(vec3(1.08, 0.95, 0.85), vec3(0.92, 1.0, 1.1), hue);
    vec3 albedo = mix(bc, zc, zone);
    albedo *= 0.86 + 0.28 * streak;

    // The poles: no bands left, a darker, cooler mottle.
    float polar = smoothstep(0.62, 0.92, abs(sin(y)));
    albedo = mix(albedo, mix(bc, vec3(0.45, 0.47, 0.5), 0.45) * (0.8 + 0.4 * streak), polar);

    // The storms' own colour: the great one red-brown with a paler collar, the ovals white.
    albedo = mix(albedo, storm * (0.8 + 0.4 * streak), great * 0.85);
    albedo = mix(albedo, vec3(0.95, 0.93, 0.9), clamp(ovals, 0.0, 1.0) * 0.8);

    // Light: a soft terminator through deep air, darkening toward the limb.
    float diffuse = bDiffuse(n, l, 0.2, lightAmount, ambient);
    vec3  col = bTint(albedo, lightSurface, lightAmount) * diffuse;
    col *= 0.62 + 0.38 * pow(n.z, 0.45);
    col = bAirInside(col, n, l, air, airDensity, lightAmount);

    finalColor = vec4(col, texel.a * bCoverage(r, radius));
}
