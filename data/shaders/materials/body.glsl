// body.glsl -- what every material that paints a whole body shares (#426): noise, the
// sphere under a disc, the light, the air. Included by a material with
//
//     #include "body.glsl"
//
// after its own uniforms; the material library pastes this file in (GLSL has no include).
//
// Rules every function here keeps:
//
//   - Everything is a function of where the fragment is on the body, the body's turn
//     (`item.angle`, which the composer wraps), the seed and a clock that is only ever used
//     through periods that divide the 16384 s the backend wraps it at (#136). Nothing is
//     accumulated, so two clients see the same planet.
//   - Cost follows size on screen. `bOctaves` gives fewer octaves to a small disc: detail
//     finer than a pixel is not seen, only paid for. The most any material asks for is seven,
//     and only for a body hundreds of pixels across -- where the backend's paint budget
//     (`paintBudget` in look.json) has already bounded how many fragments there are.
//   - Value noise read from a table (`texture.noise`) rather than gradient noise or a hash:
//     four fetches, and the rotation between octaves hides its grid. This runs on an Intel UHD 620.

const float PI = 3.14159265;
const float TAU = 6.28318531;

// --- Noise ---------------------------------------------------------------------------------

// Dave Hoskins' hash without sine: the same answer on every driver, unlike sin(), whose
// precision on a large argument is the driver's choice.
float bHash(vec3 p)
{
    p = fract(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract((p.x + p.y) * p.z);
}

vec3 bHash3(vec3 p)
{
    p = fract(p * vec3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return fract((p.xxy + p.yxx) * p.zyx);
}

// The noise table (`texture.noise`): red a random byte, green the red 37 across and 17 down,
// which is the next layer up of a 3D lattice laid out in 2D. Every material that includes
// this file binds it: "noiseTex": "texture.noise".
uniform sampler2D noiseTex;

// 0..1, smooth, period-free in practice (the table repeats every 256 cells). Value noise in
// 3D from four fetches of the table -- each fetch two layers -- rather than eight hashes:
// the one function every material calls dozens of times a fragment, so this is most of
// what a planet filling the screen costs. Fetched and blended by hand rather than by the
// texture filter, whose weights have eight bits on some hardware: magnified a few hundred
// times, that is a staircase.
float bNoise(vec3 x)
{
    vec3 i = floor(x);
    vec3 f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    ivec2 c = ivec2(i.xy + vec2(37.0, 17.0) * i.z);
    vec2  a = texelFetch(noiseTex, c & 255, 0).rg;
    vec2  b = texelFetch(noiseTex, (c + ivec2(1, 0)) & 255, 0).rg;
    vec2  d = texelFetch(noiseTex, (c + ivec2(0, 1)) & 255, 0).rg;
    vec2  e = texelFetch(noiseTex, (c + ivec2(1, 1)) & 255, 0).rg;
    vec2  layer = mix(mix(a, b, f.x), mix(d, e, f.x), f.y);
    return mix(layer.x, layer.y, f.z);
}

// Each octave turned against the last, so the lattice of value noise never lines up into
// a visible grid.
const mat3 B_OCTAVE = mat3(0.00, 0.80, 0.60, -0.80, 0.36, -0.48, -0.60, -0.48, 0.64) * 2.03;

// Fractal noise, 0..1 whatever the octave count.
float bFbm(vec3 p, int octaves)
{
    float sum = 0.0, amp = 0.5, norm = 0.0;
    for (int i = 0; i < 8; i++)
    {
        if (i >= octaves)
            break;
        sum += amp * bNoise(p);
        norm += amp;
        p = B_OCTAVE * p;
        amp *= 0.5;
    }
    return sum / norm;
}

// The same, and also what the first `split` octaves alone came to: the broad shape of the
// land without its grain, for anything that wants to take a slope from it. The fine octaves
// change from one pixel to the next, and a slope taken from them is a mosaic.
float bFbmSplit(vec3 p, int octaves, int split, out float broad)
{
    float sum = 0.0, amp = 0.5, norm = 0.0;
    broad = 0.0;
    for (int i = 0; i < 8; i++)
    {
        if (i >= octaves)
            break;
        sum += amp * bNoise(p);
        norm += amp;
        if (i + 1 == split)
            broad = sum / norm;
        p = B_OCTAVE * p;
        amp *= 0.5;
    }
    if (split >= octaves)
        broad = sum / norm;
    return sum / norm;
}

// Ridged: sharp crests where the noise crosses its middle -- cracks, ridges, lanes.
float bRidge(vec3 p, int octaves)
{
    float sum = 0.0, amp = 0.5, norm = 0.0;
    for (int i = 0; i < 8; i++)
    {
        if (i >= octaves)
            break;
        sum += amp * (1.0 - abs(2.0 * bNoise(p) - 1.0));
        norm += amp;
        p = B_OCTAVE * p;
        amp *= 0.5;
    }
    return sum / norm;
}

// Cells: how far a point is from the edge between the two nearest of a jittered lattice of
// points -- 0 on an edge, more inside. Convection cells on a star, the plates of a lava
// world's crust. Twenty-seven hashes, so it is kept for bodies large on screen.
float bCells(vec3 p)
{
    vec3  i = floor(p), f = fract(p);
    float best = 8.0, second = 8.0;
    for (int z = -1; z <= 1; z++)
        for (int y = -1; y <= 1; y++)
            for (int x = -1; x <= 1; x++)
            {
                vec3  o = vec3(x, y, z);
                vec3  at = o + bHash3(i + o) - f;
                float d = dot(at, at);
                if (d < best)
                {
                    second = best;
                    best = d;
                }
                else if (d < second)
                    second = d;
            }
    return clamp(sqrt(second) - sqrt(best), 0.0, 1.0);
}

// How many octaves a body this many pixels in radius can show: one per doubling past four
// pixels, `most` at the outside. A dot on the map gets two, a planet filling the screen seven.
int bOctaves(float radiusPx, int most)
{
    return int(clamp(log2(max(radiusPx, 1.0) / 4.0), 2.0, float(most)));
}

// A slow loop through noise space, `period` seconds round: things that churn in place --
// cloud, granulation, gas -- without anything drifting off forever. The period must divide
// the 16384 s the backend wraps the clock at, or there is a jump once every four hours.
vec3 bLoop(float time, float period, float reach)
{
    float a = TAU * time / period;
    return vec3(cos(a), sin(a), cos(a * 0.5 + 1.3)) * reach;
}

// --- Craters --------------------------------------------------------------------------------

// A crater's slope at `d` crater radii from its centre: a bowl rising to a raised rim at 1,
// and nothing past about 1.6. Height is depth times this integrated; only the slope is needed.
float bCraterSlope(float d)
{
    // A floor that is flat for its middle third, the way a real crater's settles, and walls
    // that steepen toward the rim.
    float bowl = d < 1.0 ? 2.2 * smoothstep(0.3, 1.0, d) * d : 0.0;
    float rim = -0.5 * (d - 1.0) / 0.03 * exp(-(d - 1.0) * (d - 1.0) / 0.03);
    return bowl + rim;
}

// A field of small craters, one at most per cell of a lattice `freq` cells to the radius,
// each kept inside its cell so a fragment never has to look at its neighbours: one hash a
// layer, however many craters it shows. `b` is the point on the unit body. Adds the slope
// to `bend` (in the body's frame) and returns how much darker the floor is.
float bCraterCells(vec3 b, float freq, float salt, float density, inout vec3 bend)
{
    vec3 q = b * freq + salt;
    vec3 cell = floor(q);
    vec3 h = bHash3(cell + salt * 0.37);
    if (h.x > density)
        return 0.0;
    vec3  at = 0.3 + 0.4 * bHash3(cell * 1.7 + 4.1);
    float size = 0.06 + 0.1 * h.y;
    vec3  to = fract(q) - at;
    float d = length(to) / size;
    if (d > 1.6)
        return 0.0;
    vec3 dir = normalize(to - dot(to, b) * b + 0.00001);
    bend += dir * bCraterSlope(d) * 0.2;
    return (1.0 - smoothstep(0.0, 1.0, d)) * 0.12;
}

// --- The sphere under the disc -------------------------------------------------------------

// Where this fragment is on the body as a unit disc, y up; `radius` in pixels.
vec2 bLocal(vec2 centre, float radius)
{
    return (gl_FragCoord.xy - centre) / max(radius, 0.5);
}

// The surface normal, toward the viewer, of a unit sphere seen as that disc.
vec3 bNormal(vec2 local)
{
    return vec3(local, sqrt(max(0.0, 1.0 - dot(local, local))));
}

// From the view into the body's own frame: its north pole tipped `tilt` toward the viewer,
// as the composer's bands are (Shape::axisTilt), and turned `turn` radians about it.
vec3 bToBody(vec3 n, float tilt, float turn)
{
    float c = cos(tilt), s = sin(tilt);
    vec3  b = vec3(n.x, c * n.y + s * n.z, -s * n.y + c * n.z);
    float ct = cos(turn), st = sin(turn);
    return vec3(ct * b.x - st * b.z, b.y, st * b.x + ct * b.z);
}

// The body's frame back to the view: the inverse of bToBody, for a normal bent in it.
vec3 bToView(vec3 b, float tilt, float turn)
{
    float ct = cos(turn), st = sin(turn);
    vec3  u = vec3(ct * b.x + st * b.z, b.y, -st * b.x + ct * b.z);
    float c = cos(tilt), s = sin(tilt);
    return vec3(u.x, c * u.y - s * u.z, s * u.y + c * u.z);
}

// Coverage at the limb, antialiased to the pixel: the shader, not raylib's polygon, decides
// where the edge of the world is.
float bCoverage(float r, float radiusPx)
{
    return clamp((1.0 - r) * radiusPx + 0.5, 0.0, 1.0);
}

// --- Light ---------------------------------------------------------------------------------

// Toward the light, in the view. The star is in the plane of the map, a little toward the
// viewer -- less than the hull material's 0.55, so a planet shows a real terminator rather
// than a lit disc with a dark edge. No light at all is light from everywhere (#119).
vec3 bToLight(vec2 lightDir)
{
    if (dot(lightDir, lightDir) < 0.0001)
        return vec3(0.0, 0.0, 1.0);
    return normalize(vec3(lightDir, 0.38));
}

// How lit a surface is: a terminator `soft` wide rather than a cut, which is what air does
// (an airless moon passes a small number). Scaled into the scene's floor and strength.
float bDiffuse(vec3 n, vec3 l, float soft, float lightAmount, float ambient)
{
    float d = dot(n, l);
    float day = clamp((d + soft) / (1.0 + soft), 0.0, 1.0);
    day = day * day * (3.0 - 2.0 * day) * 0.35 + max(d, 0.0) * 0.65;
    // The night side keeps a share of the floor and not all of it: a world in shadow should
    // read as one, and the floor is there so a ship never vanishes, not to light planets.
    float floorLevel = ambient * 0.42;
    return floorLevel + (1.0 - floorLevel) * day * lightAmount;
}

// The light's colour on an albedo -- warmed or cooled, never replaced (#361).
vec3 bTint(vec3 col, vec4 lightSurface, float lightAmount)
{
    return mix(col, col * lightSurface.rgb, 0.6 * lightAmount);
}

// The air seen edge-on: a band of it over the lit limb inside the disc, fading round into
// the night. `density` 0 is airless.
vec3 bAirInside(vec3 col, vec3 n, vec3 l, vec3 air, float density, float lightAmount)
{
    float edge = pow(1.0 - n.z, 2.6);
    float day = smoothstep(-0.25, 0.45, dot(n, l)) * lightAmount;
    return mix(col, air, clamp(edge * density * day * 1.6, 0.0, 0.85)) +
           air * edge * day * density * 0.25;
}

// ...and outside the disc: the glow of the air past the limb, `thickness` of a radius deep.
// Returns the colour and how solid it is.
vec4 bAirOutside(vec2 local, float r, vec3 l, vec3 air, float density, float thickness,
                 float lightAmount)
{
    vec3  rim = normalize(vec3(local / max(r, 0.001), 0.0));
    float day = smoothstep(-0.35, 0.5, dot(rim, l)) * lightAmount;
    float fall = exp(-(r - 1.0) / max(thickness, 0.001) * 3.0);
    float a = clamp(fall * day * density, 0.0, 1.0);
    return vec4(air * (0.6 + 0.6 * fall), a);
}
