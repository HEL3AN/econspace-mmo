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
    // agree at the edges instead of fighting. Normalised so that a corner (r2 = 0.5) lands
    // on the corner: the bow pulls the middle of each edge inward and never reaches outside
    // the frame. Unnormalised, every edge sampled past the picture and the screen wore a
    // black curved border fifteen to forty pixels wide (#426).
    const float k = 0.12;
    vec2 bowed = 0.5 + centred * (1.0 + k * scale * r2) / (1.0 + k * scale * 0.5);

    // The channels are pulled apart along the radius, and at the edge one of them points
    // outside the frame. Clamped, so it repeats the edge pixel rather than reading the far
    // side of the texture: wrapped, the top row of the screen took its red from the bottom
    // one and the picture had a thin red and cyan line along the top (#426).
    vec2 shift = centred * amount * 0.006 * (0.4 + r2 * 3.0);
    float r = texture(texture0, clamp(bowed + shift, 0.0, 1.0)).r;
    float g = texture(texture0, clamp(bowed, 0.0, 1.0)).g;
    float b = texture(texture0, clamp(bowed - shift, 0.0, 1.0)).b;
    finalColor = vec4(r, g, b, 1.0) * fragColor;
}
