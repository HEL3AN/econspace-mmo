#version 330

// Bloom: bright parts bleed into what is around them.
//
// scale -- how far the glow reaches, in source pixels per step.
//
// The glow itself is made before this runs (#296): the bright parts of the world as it was
// drawn, blurred across and then down at half the screen's resolution by bloom_blur.fs. That
// is two passes of nine taps over a quarter of the pixels, where this used to be one pass of
// eighty-one taps over all of them -- the same gaussian, because exp(-(x*x + y*y) / 8) is
// exp(-x*x / 8) times exp(-y*y / 8). On a laptop's integrated graphics the old pass alone was
// most of a frame. What is left here is to lay the glow over the picture.

in vec2 fragTexCoord;
in vec4 fragColor;

uniform sampler2D texture0;    // what the chain has produced so far
uniform sampler2D glow;        // the bright parts of the scene, blurred
uniform float amount;

out vec4 finalColor;

void main()
{
    vec3 base = texture(texture0, fragTexCoord).rgb;
    vec3 g = texture(glow, fragTexCoord).rgb;
    finalColor = vec4(base + g * amount, 1.0) * fragColor;
}
