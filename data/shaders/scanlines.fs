#version 330

// Scanlines: the horizontal lines of a phosphor display.
//
// scale -- how far apart the lines sit, in screen pixels.
//
// The darkening is a raised cosine rather than every other row switched off: a hard
// alternation aliases into moire the moment the window is resized, and this is a pass that
// has to survive any window size a player picks.

in vec2 fragTexCoord;
in vec4 fragColor;

uniform sampler2D texture0;
uniform float amount;
uniform float scale;
uniform vec2 resolution;

out vec4 finalColor;

const float PI = 3.14159265;

void main()
{
    vec3 c = texture(texture0, fragTexCoord).rgb;
    float pitch = max(scale, 1.0);
    float phase = fragTexCoord.y * resolution.y / pitch;
    float line = 0.5 + 0.5 * cos(phase * 2.0 * PI);
    finalColor = vec4(c * (1.0 - amount * line), 1.0) * fragColor;
}
