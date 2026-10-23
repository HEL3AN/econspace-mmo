#version 330

// star -- a sun (#426). The colour is a temperature, not a paint: a black body at
// `temperature` kelvin, pushed a little further from white than the eye would see it so a
// red dwarf and a blue giant are told apart at a glance. Then what a telescope shows of one:
// a limb darker and redder than the middle, granulation, sunspots with a dark umbra inside a
// filamented penumbra, bright faculae near the limb -- and past the edge, a thin chromosphere,
// prominences standing on it, and a corona of streamers fading out over a few radii.
//
// A star lights itself (#119), so nothing here reads the light. It moves on loops only.

in vec2 fragTexCoord;
in vec4 fragColor;

uniform sampler2D texture0;
uniform vec4 colDiffuse;

uniform vec2  centre;        // item.screenPos  -- the photosphere's centre
uniform float radius;        // item.screenSize -- the photosphere's radius, pixels
uniform float time;          // clock.time
uniform float seed;          // item.seed
uniform float turn;          // item.angle

uniform float temperature;   // kelvin
uniform float saturation;    // 1 is the black body's own colour; more is further from white
uniform float spots;         // 0..1, how spotted
uniform float corona;        // how bright the corona is
uniform float extent;        // how far the corona reaches, in radii: the material's reach
uniform vec3  palette;       // the palette's say on top of the physics: a yellow star is a white one
                             // a game shows yellow, so it reads as one at a glance

out vec4 finalColor;

#include "body.glsl"

// A black body's colour, 1000..40000 K, after Tanner Helland's fit.
vec3 blackBody(float kelvin)
{
    float t = clamp(kelvin, 1000.0, 40000.0) / 100.0;
    vec3  c;
    c.r = t <= 66.0 ? 1.0 : 1.292936 * pow(t - 60.0, -0.1332048);
    c.g = t <= 66.0 ? 0.3900816 * log(t) - 0.6318414 : 1.1298909 * pow(t - 60.0, -0.0755148);
    c.b = t >= 66.0 ? 1.0 : (t <= 19.0 ? 0.0 : 0.5432068 * log(t - 10.0) - 1.1962541);
    return clamp(c, 0.0, 1.0);
}

vec3 saturate(vec3 c, float k)
{
    float lum = dot(c, vec3(0.299, 0.587, 0.114));
    return max(mix(vec3(lum), c, k), 0.0);
}

void main()
{
    vec4  texel = texture(texture0, fragTexCoord) * colDiffuse * fragColor;
    vec2  local = bLocal(centre, radius);
    float r = length(local);
    vec3  hotCol = saturate(blackBody(temperature), saturation) * palette;
    vec3  coolCol = saturate(blackBody(temperature * 0.72), saturation) * palette;
    vec3  off = vec3(seed * 211.0, seed * 97.0 + 3.0, seed * 157.0 + 7.0);
    float ang = atan(local.y, local.x);
    vec3  ring = vec3(cos(ang), sin(ang), 0.0);

    if (r > 1.0)
    {
        // The corona: streamers -- brighter in some directions than others -- over a glow that
        // falls off fast near the limb and slowly further out, gone by `extent`.
        float streak = bFbm(ring * 2.2 + off + vec3(0.0, 0.0, r * 0.6) + bLoop(time, 4096.0, 0.3), 4);
        float fall = 0.5 * exp(-(r - 1.0) * 6.0) + 0.5 * exp(-(r - 1.0) * 2.4);
        float glow = corona * fall * (0.45 + 1.1 * streak) * (1.0 - smoothstep(extent * 0.55, extent, r));
        vec3  haze = mix(hotCol, vec3(1.0), 0.2);
        float hazeA = clamp(glow, 0.0, 1.0);

        // The chromosphere and its prominences: a thin rim, and plumes standing on it where
        // the noise round the limb is high -- the colour of hydrogen, a deep pink.
        float plume = bFbm(ring * 5.0 + off.zxy + bLoop(time, 2048.0, 0.4), 4);
        float height = 0.004 + 0.1 * smoothstep(0.64, 0.92, plume);
        float rise = (r - 1.0) / height;
        // Too fine to see on a small disc, where it is a ring of sparks instead.
        float body = (1.0 - smoothstep(0.55, 1.0, rise)) *
                     (0.5 + 0.5 * bNoise(ring * 40.0 + vec3(0.0, 0.0, r * 60.0 + seed * 50.0))) *
                     smoothstep(30.0, 90.0, radius);
        vec3  pink = mix(vec3(1.0, 0.32, 0.36), hotCol, 0.2);
        float plumeA = clamp(body * 0.9, 0.0, 1.0);

        // The plume in front of the haze, both over space.
        float a = plumeA + hazeA * (1.0 - plumeA);
        if (a <= 0.003)
            discard;
        vec3 col = (pink * plumeA + haze * hazeA * (1.0 - plumeA)) / a;
        finalColor = vec4(col, a * texel.a);
        return;
    }

    vec3  n = bNormal(local);
    vec3  b = bToBody(n, radians(10.0), turn);
    float mu = n.z;

    // Limb darkening, the standard quadratic law: the edge is half as bright as the middle
    // and the colour of a cooler star, because what is seen there is higher and cooler gas.
    float limb = 1.0 - 0.45 * (1.0 - mu) - 0.15 * (1.0 - mu) * (1.0 - mu);
    vec3  col = mix(coolCol, hotCol, pow(mu, 0.6)) * limb;
    // The middle of the disc is as near white as the star gets: what makes it a light and
    // not a ball painted the star's colour.
    col = mix(col, vec3(1.0, 0.98, 0.94), 0.3 * mu * mu) * 1.12;

    // Granulation, and the larger supergranules under it.
    vec3  gp = b * 4.0 + off + bLoop(time, 512.0, 0.35);
    float super = bFbm(gp, 3);
    // Granules a few pixels across however near: larger, and a star is a cobbled ball.
    float gran = radius > 160.0 ? bCells(b * clamp(radius / 5.0, 30.0, 140.0) + off + bLoop(time, 256.0, 0.5))
                                : bNoise(b * 30.0 + off);
    col *= 0.9 + 0.1 * super + 0.06 * gran;

    // Faculae: brighter patches that only show near the limb, against the darkening.
    float fac = smoothstep(0.55, 0.75, bFbm(b * 6.0 + off.yzx, 3)) * smoothstep(0.35, 0.05, mu);
    col += hotCol * fac * 0.25;

    // Sunspots, at the latitudes where spots are: a dark umbra in a filamented penumbra.
    int count = int(spots * 7.0 + 0.5);
    for (int i = 0; i < 7; i++)
    {
        if (i >= count)
            break;
        vec3  h = bHash3(vec3(float(i) * 3.7, seed * 61.0, 1.9));
        float lat = (h.x > 0.5 ? 1.0 : -1.0) * (0.12 + 0.45 * h.y);
        float lon = h.z * TAU;
        vec3  c = vec3(cos(lat) * sin(lon), sin(lat), cos(lat) * cos(lon));
        float size = 0.025 + 0.06 * fract(h.y * 7.3 + h.z);
        vec3  to = b - c;
        float d = length(to) / size;
        if (d > 1.0)
            continue;
        float fil = 0.75 + 0.25 * bNoise(vec3(atan(dot(to, vec3(0, 1, 0)), dot(to, vec3(1, 0, 0))) * 6.0, d * 3.0, h.x * 9.0));
        float penumbra = 1.0 - smoothstep(0.75, 1.0, d);
        float umbra = 1.0 - smoothstep(0.3, 0.45, d);
        col *= mix(1.0, 0.55 * fil, penumbra);
        col *= mix(1.0, 0.35, umbra);
    }

    // Solid to the limb: the corona begins where this ends, and an antialiased edge here
    // would let the black behind show through as a dark ring between them.
    finalColor = vec4(col, texel.a);
}
