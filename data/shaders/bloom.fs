#version 330

// Bloom: bright parts bleed into what is around them.
//
// scale -- how far the glow reaches, in source pixels per step.
//
// One pass rather than the usual bright-pass plus two separable blurs. It is the only
// effect here that needs the picture as it stood before it, which is why this is the one
// shader given a second sampler.

in vec2 fragTexCoord;
in vec4 fragColor;

uniform sampler2D texture0;    // what the chain has produced so far
uniform sampler2D scene;       // the world as it was drawn, before any pass
uniform float amount;
uniform float scale;
uniform vec2 resolution;

out vec4 finalColor;

// Where a pixel stops being ordinary and starts being a light. Below this nothing glows,
// which is what keeps a dim hull from smearing.
const float THRESHOLD = 0.55;

vec3 bright(vec2 uv)
{
    vec3 c = texture(scene, uv).rgb;
    float lum = dot(c, vec3(0.299, 0.587, 0.114));
    return c * max(0.0, lum - THRESHOLD) / max(1.0 - THRESHOLD, 0.001);
}

void main()
{
    vec2 step = scale / resolution;
    vec3 sum = vec3(0.0);
    float total = 0.0;

    // A 9x9 tap grid with a gaussian falloff. Coarse enough to be cheap, wide enough that
    // a star reads as a light source rather than as a disc with a halo stuck on it.
    for (int y = -4; y <= 4; y++)
    {
        for (int x = -4; x <= 4; x++)
        {
            float d2 = float(x * x + y * y);
            float w = exp(-d2 / 8.0);
            sum += bright(fragTexCoord + vec2(float(x), float(y)) * step) * w;
            total += w;
        }
    }

    vec3 glow = sum / max(total, 0.001);
    vec3 base = texture(texture0, fragTexCoord).rgb;
    finalColor = vec4(base + glow * amount, 1.0) * fragColor;
}
