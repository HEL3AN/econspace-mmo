#version 330

// Noise: animated grain, the sense that the picture is being received rather than shown.
//
// scale -- how coarse the grain is; 1.0 is one grain per pixel, larger is chunkier.

in vec2 fragTexCoord;
in vec4 fragColor;

uniform sampler2D texture0;
uniform float amount;
uniform float scale;
uniform vec2 resolution;
uniform float time;

out vec4 finalColor;

// The usual hash. It does not have to be a good random number, it has to be a different
// one per pixel per frame and cost nothing.
float hash(vec2 p)
{
    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
}

void main()
{
    vec3 c = texture(texture0, fragTexCoord).rgb;
    vec2 cell = floor(fragTexCoord * resolution / max(scale, 1.0));
    // Additive and centred on zero, so grain lifts and lowers rather than only brightening
    // -- one-sided noise reads as fog over the picture instead of as noise in it.
    float n = hash(cell + fract(time) * 137.0) - 0.5;
    finalColor = vec4(clamp(c + n * amount, 0.0, 1.0), 1.0) * fragColor;
}
