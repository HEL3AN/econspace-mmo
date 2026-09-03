#version 330

// Fringe: colour separation and a little barrel, the two things that say "lens" rather
// than "framebuffer".
//
// scale -- how much the image bows outward. 0 is flat.
//
// The two belong in one pass because they are the same idea measured from the centre: the
// further out a pixel is, the more the glass moved it, and the more the three channels
// disagree about where it went.

in vec2 fragTexCoord;
in vec4 fragColor;

uniform sampler2D texture0;
uniform float amount;
uniform float scale;
uniform vec2 resolution;

out vec4 finalColor;

void main()
{
    vec2 uv = fragTexCoord;
    vec2 centred = uv - 0.5;
    float r2 = dot(centred, centred);

    // Barrel first, so the separation below is measured on the bowed image and the two
    // agree at the edges instead of fighting.
    vec2 bowed = 0.5 + centred * (1.0 + 0.12 * scale * r2);

    // Outside the frame there is nothing to sample; black is honest and a clamped smear is
    // not, because a smear looks like part of the picture.
    if (bowed.x < 0.0 || bowed.x > 1.0 || bowed.y < 0.0 || bowed.y > 1.0)
    {
        finalColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec2 shift = centred * amount * 0.006 * (0.4 + r2 * 3.0);
    float r = texture(texture0, bowed + shift).r;
    float g = texture(texture0, bowed).g;
    float b = texture(texture0, bowed - shift).b;
    finalColor = vec4(r, g, b, 1.0) * fragColor;
}
