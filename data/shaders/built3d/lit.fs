#version 330

// lit.fs -- a built object as a solid (#618 spike): lit by the system's star, shadowed by what
// stands between a point and the light, darkened where parts meet, bevels catching the light.
//
// Shadows and occlusion come from one thing: the height map (height.fs). A top-down object is a
// heightfield -- slabs on slabs, nothing overhangs -- so a shadow is a ray marched from a point
// toward the star until it passes under something, and occlusion is how much of the ground
// around a point stands above it. No shadow map, no light-space matrices, any number of parts.
//
// Colour is art (#117): the object's own colour and its livery, never a side.

in vec3  vLocal;
in vec3  vNormal;
in vec2  vUv;
in vec2  vSeed;
flat in vec4 vMotion;
in vec4  vTint;
in float vEdge;

uniform sampler2D uHeight;

uniform vec4  uBase;          // the object's colour, livery applied
uniform vec4  uAccent;        // livery.accent
uniform vec4  uLivery;        // wear, grime, stripes, panel scale
uniform vec4  uStyle;         // aspect, patchwork, window warmth, windows
uniform vec3  uLightDir;      // toward the star, unit; (0,0,1) with no star
uniform vec4  uLightSurface;  // rgb: the star's light on a surface; a: strength 0..1
uniform vec3  uLightTint;     // the star's own colour, for glints
uniform float uAmbient;
uniform float uHasLight;      // 0: lit from everywhere, no shadows
uniform float uMaxH;
uniform float uHExtent;
uniform float uHFill;
uniform vec2  uClock;
uniform float uPixelsPerUnit; // for how fine a seam may be
uniform float uRadius;        // the object's radius, world units
uniform float uDamage;        // 1 whole
uniform vec2  uQuality;       // shadow steps, occlusion on
uniform float uDome;          // how domed the whole object reads, 0 none

out vec4 finalColor;

float hash(vec2 p)
{
    vec3 q = fract(vec3(p.xyx) * 0.1031);
    q += dot(q, q.yzx + 33.33);
    return fract((q.x + q.y) * q.z);
}

float H(vec2 p)
{
    return texture(uHeight, (p / uHExtent * 0.5 + 0.5) * uHFill).r;
}

// Interleaved gradient noise: a fixed pattern that hides banding without looking like grain.
float Ign()
{
    return fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
}

void main()
{
    // The whole object is a little domed, so it has a day side and a night side as a planet has:
    // the parts' own normals are tilted outward by how far from the centre they are.
    vec3  N = normalize(vNormal + vec3(vLocal.xy / uRadius * uDome, 0.0));
    float h = vLocal.z;
    vec2  P = vLocal.xy;
    int   role = int(mod(vMotion.w, 16.0) + 0.5);
    bool  nightOnly = vMotion.w >= 16.0;
    bool  hasLight = uHasLight > 0.5;
    vec3  L = hasLight ? uLightDir : vec3(0.0, 0.0, 1.0);
    float strength = hasLight ? uLightSurface.a : 0.55;

    // --- What the part is made of ------------------------------------------------------------
    vec3  base = vTint.a > 0.0 ? vTint.rgb : uBase.rgb;
    vec3  albedo = base;
    float shine = 0.35, glossy = 14.0;
    if (role == 1) { albedo = base * 0.55; shine = 0.12; }                                  // panel
    else if (role == 2) { albedo = mix(base, vec3(1.0), 0.22); shine = 0.45; glossy = 40.0; } // trim
    else if (role == 4) { albedo = base * 0.75; shine = 0.5; glossy = 28.0; }               // antenna
    if (role == 2 && uLivery.z > 0.0)
        albedo = mix(albedo, uAccent.rgb, 0.35 * uLivery.z);

    // Panels: seams every `pitch` radii, laid out in the part's own frame, and a tone that
    // differs from panel to panel. Fades out when a panel is only a few pixels across, so a
    // station at a distance is lit plating and the one filling the screen has seams.
    float pitch = 0.045 * max(uLivery.w, 0.2);
    vec2  cell = vUv / pitch;
    vec2  id = floor(cell);
    vec2  f = fract(cell);
    float cellPx = pitch * uRadius * uPixelsPerUnit;
    float seamW = clamp(1.2 / max(cellPx, 1.0), 0.015, 0.2);
    float seam = 1.0 - smoothstep(0.0, seamW, min(min(f.x, 1.0 - f.x), min(f.y, 1.0 - f.y)));
    float fade = smoothstep(4.0, 9.0, cellPx);
    float tone = hash(id + vSeed.x * 91.7);
    if (role != 3)
    {
        albedo *= 1.0 + (tone - 0.5) * 0.12 * fade * (0.4 + uLivery.x);
        albedo *= 1.0 - 0.12 * seam * fade;
    }

    // --- Light -------------------------------------------------------------------------------
    float ndl = max(dot(N, L), 0.0);
    float shadow = 1.0;
    float ao = 1.0;

    if (uQuality.x > 0.5 && role != 3)
    {
        float jit = Ign();
        float eps = 0.004 * uHExtent;
        if (hasLight)
        {
            vec2  ld = normalize(L.xy + vec2(0.00001));
            float tanE = L.z / max(length(L.xy), 0.0001);
            float smax = min((uMaxH - h) / max(tanE, 0.05), uHExtent * 1.2);
            float occ = 0.0;
            int   steps = int(uQuality.x);
            for (int i = 0; i < steps; i++)
            {
                float s = smax * (float(i) + 0.3 + 0.7 * jit) / float(steps);
                float rayH = h + s * tanE;
                float hs = H(P + ld * s);
                float soft = 0.006 * uHExtent + 0.10 * s;
                occ = max(occ, clamp((hs - rayH - eps) / soft, 0.0, 1.0));
            }
            shadow = 1.0 - occ;
        }
        if (uQuality.y > 0.5)
        {
            float acc = 0.0;
            for (int k = 0; k < 8; k++)
            {
                float t = 6.2831853 * (float(k) + jit) / 8.0;
                vec2  d = vec2(cos(t), sin(t));
                float r1 = 0.012 * uHExtent, r2 = 0.045 * uHExtent;
                acc += clamp((H(P + d * r1) - h) / r1 * 0.5, 0.0, 1.0) * 0.55;
                acc += clamp((H(P + d * r2) - h) / r2 * 1.2, 0.0, 1.0) * 0.45;
            }
            ao = 1.0 - 0.7 * (acc / 8.0);
        }
    }

    // A star is bright and the dark side of a thing next to it is dark: ambient is only the
    // sky, so a shadow is allowed to read.
    float amb = uAmbient * (0.55 + 0.45 * N.z) * ao;
    vec3  lightCol = hasLight ? uLightSurface.rgb : vec3(1.0);
    vec3  col = albedo * (amb + 0.78 * strength * ndl * shadow * lightCol);

    // Glints: the star in the bevels and on glossy parts, never in a shadow.
    vec3  Hh = normalize(L + vec3(0.0, 0.0, 1.0));
    float spec = pow(max(dot(N, Hh), 0.0), glossy) * shine * strength * shadow;
    col += uLightTint * spec;

    // --- Light the object makes --------------------------------------------------------------
    if (role == 3)
    {
        float k = vSeed.y;
        if (vMotion.y > 0.0)
        {
            float t = fract((uClock.x + uClock.y) / vMotion.y + vMotion.z);
            k *= 0.25 + 0.75 * (0.5 + 0.5 * cos(t * 6.2831853));
        }
        if (nightOnly)
            k *= 1.0 - clamp(ndl * strength * 3.0, 0.0, 1.0);
        vec3 lamp = vTint.a > 0.0 ? vTint.rgb : base;
        // A lamp is light, not a surface: translucent where its solidity is, so a gate's field
        // is a glow over what is beneath.
        finalColor = vec4(lamp * 1.25 * (0.25 + 0.75 * uDamage),
                          k * clamp(vEdge * uPixelsPerUnit + 0.5, 0.0, 1.0));
        return;
    }
    else if (role == 0 && N.z > 0.9 && uStyle.w > 0.05 && cellPx > 6.0)
    {
        // Lit windows on the side of a hull the star does not reach: a station's night is a
        // scatter of warm lights, as a world's is a scatter of cities.
        float night = 1.0 - clamp(ndl * strength * shadow * 3.0, 0.0, 1.0);
        float lit = step(0.84, hash(id * 1.7 + vSeed.x * 13.0 + 3.0));
        vec2  w = abs(f - 0.5);
        float win = (1.0 - smoothstep(0.12, 0.2, w.x)) * (1.0 - smoothstep(0.08, 0.14, w.y));
        vec3  warm = mix(vec3(1.0, 0.9, 0.7), vec3(0.85, 0.95, 1.0), clamp(1.0 - uStyle.z, 0.0, 1.0));
        col += warm * lit * win * night * uStyle.w * fade * 1.4;
    }

    col = col / (1.0 + 0.30 * col) * 1.25;  // a soft shoulder, so a lit slab does not clip

    // Coverage from the distance to the outline, one pixel across: the part's own edge is
    // soft, with no multisampling and nothing to resolve.
    finalColor = vec4(col, clamp(vEdge * uPixelsPerUnit + 0.5, 0.0, 1.0));
}
