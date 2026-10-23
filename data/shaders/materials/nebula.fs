#version 330

// nebula -- a cloud of gas lit from inside (#426). Not a disc and not a stack of discs:
// fractal gas through a strong warp, which is what makes the filaments and the curling
// edges; two emissions mixed by the noise -- the object's own colour, hydrogen's red, and
// oxygen's teal; dark lanes of dust across it; and an edge that is wherever the gas thins
// out, not a circle. It glows -- nothing in it reads the light of a star.
//
// It churns on a long loop, so slowly that it reads as still unless watched.

in vec2 fragTexCoord;
in vec4 fragColor;

uniform sampler2D texture0;
uniform vec4 colDiffuse;

uniform vec2  centre;        // item.screenPos
uniform float radius;        // item.screenSize -- the part's radius: the furthest gas reaches
uniform float time;          // clock.time
uniform float seed;          // item.seed
uniform float turn;          // item.angle

uniform vec4  base;          // item.color -- the nebula's own hue
uniform vec3  glow;          // a second emission, hydrogen's
uniform vec3  oxygen;        // a third, oxygen's
uniform float dust;          // 0..1, how much the dark lanes take out
uniform float density;       // how solid the brightest gas is

out vec4 finalColor;

#include "body.glsl"

void main()
{
    vec4  texel = texture(texture0, fragTexCoord) * colDiffuse * fragColor;
    vec2  local = bLocal(centre, radius);
    // Turned with its part, so it turns as the composition says.
    float c = cos(turn), s = sin(turn);
    local = vec2(c * local.x - s * local.y, s * local.x + c * local.y);
    float r = length(local);
    if (r > 1.0)
        discard;

    int  oct = bOctaves(radius, 6);
    vec3 off = vec3(seed * 97.0 + 1.0, seed * 211.0 + 4.0, seed * 53.0);
    vec3 p = vec3(local * 2.3, 0.0) + off + bLoop(time, 8192.0, 0.35);

    // Two warps deep: the first bends the second, the second bends the gas.
    vec3  w1 = vec3(bFbm(p, 2), bFbm(p + 5.2, 2), 0.0) - 0.5;
    vec3  w2 = vec3(bFbm(p + w1 * 2.0 + 1.7, 2), bFbm(p + w1 * 2.0 + 9.2, 2), 0.0) - 0.5;
    float gas = bFbm(p + w2 * 2.4, oct);

    // The edge: wherever the gas thins out, pulled in by the noise -- never a circle.
    float reach = 1.0 - smoothstep(0.5, 1.05, r + 0.45 * (w1.x + w2.y));
    float d = clamp((gas - 0.32) * 2.2, 0.0, 1.0) * reach;
    d = d * d * (3.0 - 2.0 * d);

    // The colour: the object's own, shading into the other emissions where the noise says.
    float mixA = smoothstep(0.35, 0.65, bFbm(p * 0.7 + 23.0, 2));
    float mixB = smoothstep(0.55, 0.8, gas + 0.3 * w2.x);
    vec3  col = mix(base.rgb, glow, mixA * 0.7);
    col = mix(col, oxygen, mixB * 0.55);
    // The densest gas is brightest, nearly white at its heart.
    col = mix(col, vec3(1.0, 0.95, 0.92), smoothstep(0.75, 1.0, d) * 0.35) * (0.8 + 1.0 * d);

    // Dust lanes: ridged noise, dark and sharp, eating into the glow.
    float lane = smoothstep(0.78, 0.95, bRidge(p * 1.3 + w1 * 1.5 + 41.0, max(oct - 2, 2)));
    d *= 1.0 - dust * lane * 0.85;

    float a = clamp(d * density, 0.0, 1.0) * texel.a;
    if (a <= 0.003)
        discard;
    finalColor = vec4(col, a);
}
