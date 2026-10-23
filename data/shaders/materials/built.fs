#version 330

// built -- the surface of anything somebody made (#516): plating, armour, radiators, solar
// cells, glass, trusses, nozzles. The natural bodies are painted whole (#426); a built thing
// keeps the shape its data gives it (#122) and this only finishes the surface of each part.
//
// One shader, many materials: `surface` (a constant in materials.json) says which, and an
// object's material maps its roles onto them (`roles`), so a station's panels are recessed
// plating and its struts truss without a line of C++ knowing either.
//
// Rules this file keeps:
//
//   - **Patterns fit the part.** Everything is laid out in the part's own frame -- along its
//     axis, across it, round its centre -- and in the object's radii (`item.partShape`),
//     never in raw world or screen units. Panel rows divide a part's width into a whole
//     number, so a seam meets its edge rather than being cut by it, and a hexagonal core
//     is plated in rings parallel to its sides.
//   - **Detail follows size.** A seam is drawn once the panel it bounds is several pixels
//     across, and fades in rather than popping. A ship forty pixels long is lit plating and
//     its livery; the same ship filling the screen has seams, bevels and chipped edges.
//     Grime takes its octaves from the object's size on screen, as body.glsl's bodies do.
//   - **f(time, seed)** (#136): every pattern comes from the part's and the object's seeds;
//     the clock only drives loops whose periods divide the 16384 s wrap.
//   - **Colour is art** (#117): the livery is a people's palette, never a side.

in vec2 fragTexCoord;
in vec4 fragColor;

uniform sampler2D texture0;
uniform vec4 colDiffuse;

uniform vec2  centre;       // item.screenPos -- this part's centre, pixels, origin bottom left
uniform float radius;       // item.screenSize -- this part's cross section, pixels
uniform vec2  axis;         // item.axis -- which way it runs; zero if round
uniform float turn;         // item.angle -- the part's own turn, radians, OpenGL's sense
uniform float partSeed;     // item.partSeed
uniform float seed;         // item.seed
uniform vec4  partShape;    // item.partShape -- half length, half width, sides, form
uniform vec4  frame;        // item.frame -- the object's centre (px), radius (px), heading
uniform float objectSize;   // item.size -- world units: a station has decks, a fighter has not
uniform vec2  lightDir;     // light.dir
uniform vec4  lightTint;    // light.tint
uniform vec4  lightSurface; // light.surface
uniform float lightAmount;  // light.strength
uniform float ambient;      // light.ambient
uniform float damage;       // item.intensity, 1 = whole
uniform float time;         // clock.time
uniform float thrusting;    // item.thrusting
uniform vec4  accent;       // livery.accent
uniform vec4  livery;       // livery.params: wear, grime, stripes, panel scale
uniform vec4  style;        // livery.style: panel aspect, patchwork, window warmth, windows

// The material's own constants (materials.json).
uniform float surface;  // 0 plating, 1 recessed, 2 trim, 3 truss, 4 armour, 5 radiator,
                        // 6 solar, 7 glass, 8 nozzle
uniform float pitch;    // a panel's size, in the object's radii
uniform float relief;   // how far neighbouring panels tilt against each other
uniform float shine;    // 0 matt .. 1 glass
uniform float paint;    // 1: the livery's stripes are painted on it
uniform float windows;  // how many lit windows it has, times the livery's
uniform float heat;     // how much it glows on its own: a radiator, a nozzle

out vec4 finalColor;

#include "body.glsl"

// The forms, as Render::Form numbers them.
const int DISC = 0;
const int RING = 1;
const int POLYGON = 2;
const int CHEVRON = 4;
const int BAR = 5;
const int ARC = 8;

const vec3 BARE = vec3(0.80, 0.81, 0.83);  // metal where the paint is gone
const vec3 SOOT = vec3(0.045, 0.04, 0.035);

// A smooth band from `a` to `b` in `x`, `w` wide at each edge.
float Band(float x, float a, float b, float w)
{
    return smoothstep(a - w, a + w, x) * (1.0 - smoothstep(b - w, b + w, x));
}

// A turn of a 2D vector by `a` radians.
vec2 Turn(vec2 v, float a)
{
    float c = cos(a), s = sin(a);
    return vec2(c * v.x - s * v.y, s * v.x + c * v.y);
}

void main()
{
    vec4 texel = texture(texture0, fragTexCoord) * colDiffuse * fragColor;
    if (texel.a <= 0.001)
        discard;

    int   kind = int(surface + 0.5);
    int   form = int(partShape.w + 0.5);
    float R = max(frame.z, 1.0);  // the object's radius, pixels
    float hw = max(partShape.y, 0.0001);
    bool  elong = dot(axis, axis) > 0.0001;
    bool  noLight = dot(lightDir, lightDir) < 0.0001;

    // Where this fragment is on the part: `local` in its cross sections, `pf` turned into its
    // own frame (x along it, y across), `q` the same in the object's radii.
    vec2  local = (radius > 0.5) ? (gl_FragCoord.xy - centre) / radius : vec2(0.0);
    vec2  ax = elong ? axis : vec2(cos(turn), sin(turn));
    vec2  ac = vec2(-ax.y, ax.x);
    vec2  pf = vec2(dot(local, ax), dot(local, ac));
    vec2  q = pf * hw;
    float rr = length(local);

    // --- The shape under the surface -------------------------------------------------------
    // What the part is, seen from above: a cylinder (a capsule, a strut), a box with
    // chamfered edges (a bar, a wedge), a tube round a centre (a ring), a plate with a
    // chamfered rim (a polygon), a ball (a disc). The hull material lights everything as a
    // cylinder or a ball; a plate lit as a ball is a dome, which is what a hexagonal core
    // looked like.
    vec3  n;
    float rimAt = 0.0;  // how near the part's own edge, for the rim light
    float polyR = rr;   // a polygon's radius measured to its sides rather than its corners
    float seg = 0.0, mid = 0.0;
    if (form == POLYGON && partShape.z >= 3.0)
    {
        seg = TAU / partShape.z;
        float a = atan(pf.y, pf.x);
        mid = (floor(a / seg) + 0.5) * seg;  // the middle of the side this fragment faces
        polyR = rr * cos(a - mid) / cos(seg * 0.5);
    }
    if (elong)
    {
        float v = clamp(pf.y, -1.0, 1.0);
        if (form == BAR || form == CHEVRON)
        {
            // Gently rounded across, chamfered at its long edges.
            float ch = smoothstep(0.72, 1.0, abs(v));
            n = normalize(vec3(ac * (v * 0.35 + sign(v) * ch * 1.3), 1.0));
        }
        else
            n = vec3(ac * v, sqrt(max(0.0, 1.0 - v * v)));
        rimAt = abs(v);
    }
    else if (form == RING || form == ARC)
    {
        float th = clamp(partShape.x / hw, 0.02, 1.0);
        float v = clamp((rr - (1.0 - th * 0.5)) / (th * 0.5), -1.0, 1.0);
        vec2  rad = rr > 0.0001 ? local / rr : vec2(0.0);
        n = vec3(rad * v, sqrt(max(0.0, 1.0 - v * v)));
        rimAt = abs(v);
    }
    else if (form == POLYGON)
    {
        float ch = smoothstep(0.82, 1.0, polyR);
        vec2  rad = rr > 0.0001 ? local / rr : vec2(0.0);
        n = normalize(vec3(rad * (0.3 * polyR + ch * 1.4), 1.0));
        rimAt = polyR;
    }
    else
    {
        vec2 l2 = rr > 1.0 ? local / rr : local;
        n = vec3(l2, sqrt(max(0.0, 1.0 - dot(l2, l2))));
        rimAt = min(rr, 1.0);
    }

    // --- Panels ----------------------------------------------------------------------------
    // A cell of the plating: which one (`id`), where in it (`f`, 0..1 each way), how big
    // (`size`, object radii) and which way its sides run on screen (`ex`, `ey`).
    // The pitch is a fraction of the object's radius, finer on a bigger object -- a station
    // is plated in more panels than a fighter, though not in proportion to its size, or a
    // station's plates would be specks and a fighter's a handful of slabs.
    float pitchE = max(pitch * livery.w * pow(max(objectSize, 1.0) / 100.0, -0.35), 0.002);
    vec2  id = vec2(0.0), f = vec2(0.5), size = vec2(1.0);
    vec2  ex = ax, ey = ac;
    if (elong)
    {
        // Rows along the part, a whole number of them across its width. On a flat part -- a
        // hull plate, a wedge -- the panels are laid like a hull's, each row its own length
        // and offset. A round one -- a capsule, a strut -- is a barrel: its sections run
        // all the way round, so the seams across it line up from row to row, as the frames
        // of a pressurised module do.
        bool  barrel = form != BAR && form != CHEVRON;
        float rows = max(1.0, floor(2.0 * hw / pitchE + 0.5));
        float rowH = 2.0 * hw / rows;
        float yy = (q.y + hw) / rowH;
        float iy = floor(yy);
        float lane = barrel ? 0.0 : iy;
        float L = (barrel ? 2.0 * hw * 0.8 : rowH * style.x) *
                  (0.7 + 0.8 * bHash(vec3(lane, 3.1, partSeed * 41.0)));
        float xx = (q.x + partShape.x) / L + bHash(vec3(lane, 7.7, partSeed * 43.0));
        id = vec2(floor(xx), iy);
        f = vec2(fract(xx), fract(yy));
        size = vec2(L, rowH);
    }
    else if (form == RING || form == ARC)
    {
        // Segments round the ring, each about as long as `aspect` panels are wide, and a
        // whole number of bands across its thickness.
        float th = max(partShape.x, 0.0001);
        float rq = rr * hw;
        float bands = max(1.0, floor(th / pitchE + 0.5));
        float L = pitchE * style.x;
        // Counted at the middle of the band, so every fragment across it agrees on how many:
        // counted where each one is, the seams ran across the ring at a slant.
        float segs = max(3.0, floor(TAU * (hw - th * 0.5) / L + 0.5));
        float a = atan(pf.y, pf.x) / TAU + 0.5;
        float yy = clamp((hw - rq) / th, 0.0, 0.999) * bands;
        id = vec2(floor(a * segs), floor(yy));
        f = vec2(fract(a * segs), fract(yy));
        size = vec2(TAU * rq / segs, th / bands);
        vec2 rad = rr > 0.0001 ? local / rr : vec2(1.0, 0.0);
        ex = vec2(-rad.y, rad.x);
        ey = -rad;
    }
    else
    {
        // Rings of panels from the middle out -- parallel to a polygon's sides, round a
        // disc -- and each ring cut along its length into panels.
        float rq = polyR * hw;
        float rings = max(1.0, floor(hw / (pitchE * (form == POLYGON ? 1.0 : 1.6)) + 0.5));
        float ringH = hw / rings;
        float yy = rq / ringH;
        float iy = floor(yy);
        float L = ringH * style.x * 0.7;
        if (form == POLYGON && seg > 0.0)
        {
            float a = atan(pf.y, pf.x);
            float u = rr * hw * sin(a - mid);  // along the side, from its middle
            float xx = u / (L * 1.6) + 0.5;
            id = vec2(floor(xx) + 31.0 * floor(a / seg), iy);
            f = vec2(fract(xx), fract(yy));
            vec2 side = Turn(ax, mid);
            ex = vec2(-side.y, side.x);
            ey = side;
        }
        else
        {
            float a = atan(pf.y, pf.x) / TAU + 0.5;
            float segs = iy < 0.5 ? 1.0 : max(3.0, floor(TAU * (iy + 0.5) * ringH / L + 0.5));
            id = vec2(floor(a * segs), iy);
            f = vec2(fract(a * segs), fract(yy));
            vec2 rad = rr > 0.0001 ? local / rr : vec2(1.0, 0.0);
            ex = vec2(-rad.y, rad.x);
            ey = rad;
            if (iy < 0.5)
                f.x = 0.5;  // the cap in the middle has no seam across it
        }
        size = vec2(L, ringH);
    }
    if (kind == 6)
    {
        // Solar cells are small and square whatever the panel pitch.
        float cells = max(2.0, floor(2.0 * hw / (pitchE * 0.45) + 0.5));
        float c = 2.0 * hw / cells;
        vec2  g = (q + vec2(partShape.x, hw)) / c;
        id = floor(g);
        f = fract(g);
        size = vec2(c);
        ex = ax;
        ey = ac;
    }

    vec2  cellPx = size * R;
    float cellMin = min(cellPx.x, cellPx.y);
    vec2  edgePx = min(f, 1.0 - f) * cellPx;  // pixels to the nearer side, each way
    float edge = min(edgePx.x, edgePx.y);
    // How much of the panelling this size on screen can show: none at a few pixels a panel,
    // all of it by a dozen. Trusses have none.
    float detail = kind == 3 ? 0.0 : smoothstep(3.5, 11.0, cellMin);
    vec3  ph = bHash3(vec3(id, partSeed * 61.0 + seed * 7.0));  // this panel's own roll

    float seam = 0.0;
    float inset = 0.0;  // an access panel set into the plate
    if (detail > 0.0)
    {
        bool plate = kind <= 2 || kind == 4;
        float sw = (kind == 4 ? 0.8 : 0.35) + 0.004 * cellMin;
        seam = (1.0 - smoothstep(sw, sw + 0.8, edge)) * detail;
        // Each panel falls away toward its seams over a pixel or two, and sits at a slight
        // tilt of its own: the bevel catches the light on one side and not the other, which
        // is most of what makes plating read as plating rather than as lines drawn on it.
        float bw = 0.6 + 0.02 * cellMin;
        vec2  slope = vec2((f.x > 0.5 ? 1.0 : -1.0) * (1.0 - smoothstep(0.0, bw, edgePx.x)),
                           (f.y > 0.5 ? 1.0 : -1.0) * (1.0 - smoothstep(0.0, bw, edgePx.y)));
        vec2  tilt = (ex * slope.x + ey * slope.y) * (kind == 4 ? 0.9 : 0.35);
        tilt += (ph.xy - 0.5) * relief;
        if (plate)
        {
            // A third of the plates are cut again into two or three: a hull is laid in big
            // structural plates, and only some of them are worked finer. A finer seam.
            float split = ph.x < 0.33 ? (ph.y < 0.5 ? 2.0 : 3.0) : 1.0;
            float fs = fract(f.x * split);
            float subPx = min(fs, 1.0 - fs) * cellPx.x / split;
            if (split > 1.0)
                seam = max(seam, (1.0 - smoothstep(0.3, 1.0, subPx)) * 0.55 * detail *
                                     smoothstep(10.0, 18.0, cellMin));
            // One plate in eight carries an access panel: a darker inset with its own edge.
            if (ph.z > 0.88 && kind <= 1)
            {
                vec2  c = abs(f - 0.5) * cellPx;
                vec2  hb = cellPx * vec2(0.28, 0.24);
                float inside = max(c.x - hb.x, c.y - hb.y);
                inset = (1.0 - smoothstep(-0.5, 0.5, inside)) * smoothstep(12.0, 22.0, cellMin);
                float lip = 1.0 - smoothstep(0.0, 1.2, abs(inside));
                vec2  out2 = vec2(c.x - hb.x > c.y - hb.y ? sign(f.x - 0.5) : 0.0,
                                  c.x - hb.x > c.y - hb.y ? 0.0 : sign(f.y - 0.5));
                tilt -= (ex * out2.x + ey * out2.y) * lip * 0.6 * inset;
                seam = max(seam, lip * 0.5 * smoothstep(12.0, 22.0, cellMin));
            }
        }
        n = normalize(n + vec3(tilt * detail, 0.0));
    }

    // --- Light -------------------------------------------------------------------------------
    // A little lower than the hull material's 0.55, so the light rakes across the plating and
    // its bevels show; and a floor a little under the scene's, so a shadow is a shadow --
    // never so far under it that a ship on its night side is lost (#119).
    vec3  toLight = normalize(vec3(lightDir, 0.45));
    float facing = noLight ? 1.0 : max(0.0, dot(n, toLight));
    float floorLevel = ambient * 0.75;
    float lit = floorLevel + (1.0 - floorLevel) * facing * lightAmount;
    // Night: the half of the whole object turned from the light, measured across it from its
    // centre as the CPU's `onlyDark` parts are -- every part is lit a little from above, so a
    // part's own shading never has a night side. A lit window shows there, and is lost in
    // the glare wherever the sun is full on it.
    float across = dot(gl_FragCoord.xy - frame.xy, lightDir) / R;
    float night = noLight ? 1.0
                          : mix(1.0, smoothstep(0.25, -0.35, across), lightAmount) *
                                (1.0 - 0.5 * smoothstep(0.45, 0.9, facing * lightAmount));

    // --- The object's frame: grime that runs on from part to part ---------------------------
    vec2  d = gl_FragCoord.xy - frame.xy;
    float hc = cos(frame.w), hs = sin(frame.w);
    vec2  oc = vec2(hc * d.x + hs * d.y, -hs * d.x + hc * d.y) / R;
    int   oct = bOctaves(R, 4);

    // --- Albedo ------------------------------------------------------------------------------
    vec3 alb = texel.rgb;
    alb *= 0.96 + 0.06 * ph.z * detail + 0.03 * (1.0 - detail);
    alb *= 1.0 - 0.28 * inset;
    // Each part a shade of its own: modules from different batches, sections painted years
    // apart. Small, but it is what stops a station reading as one casting.
    alb *= 0.94 + 0.12 * bHash(vec3(partSeed * 113.0, seed * 7.0, 4.2));
    // Brushed: a faint grain along the part, which is what tells metal from stone.
    if (detail > 0.0 && kind <= 4)
        alb *= 0.975 + 0.05 * bNoise(vec3(q.x / pitchE * 0.6, q.y / pitchE * 22.0, partSeed * 9.0)) *
                           detail;
    // A panel replaced from another batch -- primer, a different alloy: the patchwork of a
    // people who mend rather than refit.
    if (kind <= 1 || kind == 4)
    {
        float swap = step(ph.x, style.y) * detail;
        vec3  other = mix(vec3(0.62, 0.6, 0.55), vec3(0.5, 0.52, 0.56), ph.y);
        alb = mix(alb, other * (0.6 + 0.5 * ph.z) * max(max(texel.r, texel.g), texel.b) * 1.2,
                  swap * 0.75);
    }

    // The livery's stripes: a band near each end of a long part, a line down its spine, or
    // hazard chevrons -- and on a round part a ring, two spokes, or a hazard ring. Measured in
    // the part's width, so a stripe on a station's arm and one on a ship's hull are the same
    // mark at two sizes.
    int   stripes = int(livery.z + 0.5);
    float stripe = 0.0;
    // Only on the parts that make up the body of the thing -- a stripe on every hatch and
    // nozzle is a zebra, not a livery.
    float major = elong ? step(0.2, partShape.x) * step(0.045, hw) : step(0.12, hw);
    if (paint > 0.5 && stripes > 0 && major > 0.0)
    {
        float aa = 1.0 / max(R, 1.0);  // a pixel, in the object's radii
        if (form == RING || form == ARC)
        {
            // Round a ring: every sixth segment in the accent, a pinstripe down both of its
            // edges, or hazard segments.
            // Only once a segment is big enough to be one: smaller, a pattern round a ring
            // is a moire, a barber's pole.
            float big = smoothstep(6.0, 14.0, cellPx.x);
            if (stripes == 1)
                stripe = step(mod(id.x, 8.0), 0.5) * big;
            else if (stripes == 2)
            {
                // Across the whole band, not each row of panels in it.
                float th = max(partShape.x, 0.0001);
                float v = clamp((hw - rr * hw) / th, 0.0, 1.0);
                stripe = (1.0 - smoothstep(0.04, 0.07, abs(abs(v - 0.5) - 0.36))) *
                         smoothstep(8.0, 16.0, th * R);
            }
            else
                stripe = step(mod(id.x, 2.0), 0.5) * step(0.5, fract(f.x * 2.0 + f.y)) * big;
        }
        else if (elong)
        {
            // From the part's forward end (+x along it): a ship's bow, an arm's outer end --
            // a radial part points away from the centre it is repeated round.
            float fromEnd = partShape.x - q.x;
            if (stripes == 1)
                stripe = Band(fromEnd, 0.4 * hw, 0.8 * hw, aa);
            else if (stripes == 2)
                stripe = (1.0 - smoothstep(0.16 * hw - aa, 0.16 * hw + aa, abs(q.y))) *
                         smoothstep(0.5 * hw, 0.5 * hw + aa, partShape.x - abs(q.x));
            else
                stripe = Band(fromEnd, 0.25 * hw, 1.0 * hw, aa) *
                         step(0.5, fract((q.x + q.y) / (0.45 * hw)));
        }
        else
        {
            float rq = polyR * hw;
            if (stripes == 1)
                stripe = Band(rq, 0.6 * hw, 0.72 * hw, aa);
            else if (stripes == 2)
                stripe = (1.0 - smoothstep(0.1 * hw - aa, 0.1 * hw + aa, abs(q.y))) *
                         step(0.3 * hw, rq);
            else
                stripe = Band(rq, 0.55 * hw, 0.8 * hw, aa) *
                         step(0.5, fract(atan(pf.y, pf.x) / TAU * 12.0));
        }
        alb = mix(alb, accent.rgb * (0.95 + 0.1 * ph.z), stripe * 0.92);
    }

    // --- What each kind is -----------------------------------------------------------------
    vec3  emit = vec3(0.0);
    float gloss = shine;
    if (kind == 3)
    {
        // A strut: dark, worked metal with ribs across it.
        float rib = step(0.72, fract(q.x / (hw * 1.6)));
        alb *= 0.75 - 0.2 * rib;
    }
    else if (kind == 5)
    {
        // A radiator fin: pale ceramic with heat pipes down its length, glowing dull red
        // where it sheds heat -- a slow breath, on a loop that divides the clock's wrap.
        float pipe = smoothstep(0.75, 0.95, fract((pf.y + 1.0) * 1.5));
        alb = mix(alb * 1.05, alb * 0.6, pipe);
        float breath = 0.75 + 0.25 * sin(TAU * time / 64.0 + partSeed * TAU);
        float glow = heat * breath * (0.35 + 0.65 * night) * (0.5 + 0.5 * pipe);
        emit += vec3(1.0, 0.32, 0.1) * glow;
    }
    else if (kind == 6)
    {
        // Solar cells: dark blue-black under a thin silver grid; they live by the light, so
        // they glint where it strikes them and go black where it does not.
        float grid = (1.0 - smoothstep(0.4, 1.2, edge)) * smoothstep(2.5, 6.0, cellMin);
        alb = mix(alb * (0.55 + 0.15 * ph.z), vec3(0.75, 0.78, 0.82), grid * 0.8);
        alb += vec3(0.02, 0.03, 0.08) * (1.0 - grid);
    }
    else if (kind == 7)
    {
        // Glass: dark by day with the light's sheen on it; lit from inside at night with the
        // colour the part was given -- a cockpit's glow, a row of cabin windows.
        vec3 inside = texel.rgb;
        alb = vec3(0.05, 0.07, 0.1) + vec3(0.06, 0.09, 0.14) * smoothstep(0.2, 1.0, rimAt);
        emit += inside * (0.25 + 0.95 * night) * heat;
        gloss = max(gloss, 0.9);
    }
    else if (kind == 8)
    {
        // A nozzle: the throat at the part's wide end (-x: exhaust leaves along -x). Heat
        // tints the metal toward the throat -- straw, then blue -- and the throat glows, a
        // little at idle and white-hot under thrust.
        float t = clamp(-q.x / max(partShape.x, 0.0001), -1.0, 1.0);  // 1 at the throat
        float hot = smoothstep(-0.2, 1.0, t);
        alb = mix(alb * 0.55, mix(vec3(0.62, 0.5, 0.3), vec3(0.32, 0.36, 0.6), hot), hot * 0.6);
        float core = smoothstep(0.55, 1.0, t) * (1.0 - smoothstep(0.55, 1.0, abs(pf.y)));
        emit += mix(vec3(1.0, 0.45, 0.15), vec3(0.65, 0.85, 1.0), thrusting) * core * core *
                heat * (0.25 + 1.6 * thrusting);
    }

    // --- Lit windows -------------------------------------------------------------------------
    // Stations have decks and decks have windows; a fighter has a cockpit and no more. One row
    // along the middle of each panel row, some lit and some not, by the panel's own roll.
    // Too small to see one by one, they are a faint glow across the night side, the way a
    // city is seen from orbit.
    float rate = windows * style.w * smoothstep(120.0, 400.0, objectSize);
    if (rate > 0.0 && (kind <= 1))
    {
        // Only some rows are decks with a view; the rest are tanks, machinery, hold.
        float deck = step(bHash(vec3(id.y, partSeed * 29.0, 2.3)), 0.4);
        float k = max(1.0, floor(size.x / (size.y * 0.5)));
        float wx = fract(f.x * k);
        float on = (1.0 - smoothstep(0.07, 0.11, abs(f.y - 0.5))) *
                   (1.0 - smoothstep(0.16, 0.22, abs(wx - 0.5))) * deck * (1.0 - inset);
        float lamp = step(bHash(vec3(id * 1.7 + floor(f.x * k) * 0.37, seed * 19.0 + 5.0)), rate);
        float sharp = smoothstep(0.8, 2.5, size.y * R * 0.18);
        float glow = mix(rate * 0.5, on * lamp * 1.6, sharp);
        // By day a window is a dark pane with the sky in it, not a hole.
        alb = mix(alb, vec3(0.16, 0.19, 0.24), on * sharp * 0.6);
        vec3 warm = mix(vec3(0.75, 0.88, 1.0), vec3(1.0, 0.74, 0.42), style.z);
        emit += warm * glow * night * 1.25;
    }

    // --- Grime and wear ----------------------------------------------------------------------
    // Grime in the object's own frame, so a stain runs on across the joint between two parts
    // and turns with the ship, gathered in the seams and streaked along each part's length --
    // what leaks and exhaust leave behind.
    float grime = 0.0;
    if (livery.y > 0.0 && kind != 7)
    {
        float g = R > 10.0 ? bFbm(vec3(oc * 2.6, seed * 31.0 + 1.7), oct) : 0.5;
        float streak = bNoise(vec3(q.x / pitchE * 0.18, q.y / pitchE * 2.4, partSeed * 23.0));
        grime = clamp((g - 0.42) * 2.6, 0.0, 1.0) * (0.55 + 0.45 * streak);
        grime = clamp(grime * livery.y * 1.6 + seam * 0.35 * livery.y, 0.0, 1.0);
        alb = mix(alb, alb * vec3(0.45, 0.41, 0.36), grime * 0.7);
    }
    // Wear: paint chipped back to bare metal at the panels' edges, more on an old hull.
    if (livery.x > 0.0 && detail > 0.0 && kind != 6 && kind != 7)
    {
        float chip = bNoise(vec3(q / pitchE * 7.0, seed * 11.0 + partSeed * 3.0));
        float nearEdge = 1.0 - smoothstep(0.0, 0.16, min(min(f.x, 1.0 - f.x) * size.x,
                                                           min(f.y, 1.0 - f.y) * size.y) /
                                                       pitchE);
        float worn = step(1.0 - livery.x * 0.75, chip) * nearEdge * detail;
        alb = mix(alb, BARE * (0.75 + 0.25 * ph.y), worn * 0.8);
    }
    alb *= 1.0 - seam * (kind == 4 ? 0.6 : 0.32);

    // --- Shade ---------------------------------------------------------------------------------
    vec3 col = alb * lit;
    col = mix(col, col * lightSurface.rgb, 0.6 * lightAmount);
    if (!noLight)
    {
        // A highlight off the panels -- broad on paint, tight on cells and glass -- and dulled
        // where it is dirty.
        vec3  h = normalize(toLight + vec3(0.0, 0.0, 1.0));
        float spec = pow(max(0.0, dot(n, h)), mix(10.0, 90.0, gloss)) * gloss * lightAmount;
        col += lightTint.rgb * spec * 0.55 * (1.0 - grime * 0.8);
        float rim = smoothstep(0.75, 1.0, rimAt) * facing * lightAmount;
        col += lightTint.rgb * rim * 0.3;
    }

    // --- Damage --------------------------------------------------------------------------------
    // Scorch where it was hit, in the object's frame so a burn crosses parts, with embers at
    // its edge when the damage is heavy. Parts taken off are #138's; this only marks them.
    if (damage < 1.0)
    {
        float hurt = 1.0 - damage;
        float sc = bFbm(vec3(oc * 5.5, seed * 5.0 + 9.0), max(oct, 2));
        float cut = 1.0 - hurt * 0.75;
        float burn = smoothstep(cut - 0.05, cut + 0.05, sc);
        col = mix(col, SOOT * lit, burn * 0.9);
        float ember = Band(sc, cut - 0.03, cut + 0.01, 0.015) * smoothstep(0.4, 0.8, hurt);
        float flicker = 0.7 + 0.3 * sin(time * 9.0 + oc.x * 40.0 + seed * 20.0);
        emit += vec3(1.0, 0.42, 0.12) * ember * flicker;
        emit *= 1.0 - burn * 0.8;
        col *= mix(1.0, 0.85 + 0.15 * sin(time * (7.0 + 20.0 * hurt) + centre.x * 0.05), hurt);
    }

    finalColor = vec4(col + emit, texel.a);
}
