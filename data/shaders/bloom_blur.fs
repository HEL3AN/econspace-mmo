#version 330

// Half of bloom's blur (#296): nine taps along one direction, with the gaussian the one-pass
// bloom used along each axis. Run twice, across and then down, it is that blur exactly.
//
// Not a pass of the chain: Treatment runs it for bloom, at half resolution, and bloom.fs
// composites the result. The first run also picks out what is bright enough to glow.
//
// direction  -- one step, in texture coordinates
// brightPass -- 1 on the first run: keep only what is above the threshold

in vec2 fragTexCoord;
in vec4 fragColor;

uniform sampler2D texture0;
uniform vec2 direction;
uniform float brightPass;

out vec4 finalColor;

// Where a pixel stops being ordinary and starts being a light. Below this nothing glows,
// which is what keeps a dim hull from smearing.
const float THRESHOLD = 0.55;

vec3 tap(vec2 uv)
{
    vec3 c = texture(texture0, uv).rgb;
    if (brightPass > 0.5)
    {
        float lum = dot(c, vec3(0.299, 0.587, 0.114));
        c = c * max(0.0, lum - THRESHOLD) / max(1.0 - THRESHOLD, 0.001);
    }
    return c;
}

void main()
{
    vec3 sum = vec3(0.0);
    float total = 0.0;
    for (int i = -4; i <= 4; i++)
    {
        float w = exp(-float(i * i) / 8.0);
        sum += tap(fragTexCoord + direction * float(i)) * w;
        total += w;
    }
    finalColor = vec4(sum / max(total, 0.001), 1.0);
}
