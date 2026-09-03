#version 330

// Vignette: the corners fall off.
//
// scale -- how far in the darkening reaches; larger pulls it toward the centre.
//
// Last in the default chain on purpose. It is the one pass that should see everything the
// others did, including the glow, or a bright corner survives a treatment meant to have
// darkened it.

in vec2 fragTexCoord;
in vec4 fragColor;

uniform sampler2D texture0;
uniform float amount;
uniform float scale;

out vec4 finalColor;

void main()
{
    vec3 c = texture(texture0, fragTexCoord).rgb;
    vec2 centred = fragTexCoord - 0.5;
    // Aspect is deliberately ignored: a vignette that follows the window shape reads as a
    // property of the window, and this one is meant to read as a property of the lens.
    float d = length(centred) * 1.414;
    float fall = smoothstep(0.35, 1.0, d * max(scale, 0.1));
    finalColor = vec4(c * (1.0 - fall * amount), 1.0) * fragColor;
}
