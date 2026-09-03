#version 330

// Pixelate: the image is resolved on a coarser grid.
//
// scale -- how large one pixel is, in screen pixels. 1.0 is no change.
//
// amount blends between the sharp image and the coarse one, so the effect can be dialled
// down rather than only switched off. Half a pixel is not a thing, but half of the way to
// a coarse pixel is, and it is what makes this usable as a subtle treatment.

in vec2 fragTexCoord;
in vec4 fragColor;

uniform sampler2D texture0;
uniform float amount;
uniform float scale;
uniform vec2 resolution;

out vec4 finalColor;

void main()
{
    float size = max(scale, 1.0);
    vec2 grid = resolution / size;
    // The half-step lands the sample in the middle of the block rather than on its corner,
    // which is the difference between a coarse image and a coarse image shifted up-left.
    vec2 snapped = (floor(fragTexCoord * grid) + 0.5) / grid;

    vec3 sharp = texture(texture0, fragTexCoord).rgb;
    vec3 blocky = texture(texture0, snapped).rgb;
    finalColor = vec4(mix(sharp, blocky, clamp(amount, 0.0, 1.0)), 1.0) * fragColor;
}
