#version 330

// The one material everything uses (#121).
//
// It is not decoration. It is what stops a simple silhouette from looking flat, so that
// the shapes underneath can stay simple enough to be described in data rather than drawn
// by hand (#122). Four things, all from the game rather than from constants here:
//
//   the star's direction   ->  a lit side and a terminator
//   the star's colour      ->  what the light is made of
//   how damaged it is      ->  darker, and it flickers, with no health bar
//   the clock              ->  the flicker, and nothing else
//
// The fragment works out where it is relative to the object from gl_FragCoord and the
// object's centre and radius in pixels. That way this sits on top of whatever primitive
// the backend drew -- a disc, a hexagon, a triangle -- rather than needing its own
// geometry. Which is the point: any shader on any object.

in vec2 fragTexCoord;
in vec4 fragColor;

uniform sampler2D texture0;
uniform vec4 colDiffuse;

uniform vec2  centre;       // item.screenPos -- pixels, origin at the bottom left
uniform float radius;       // item.screenSize -- pixels
uniform vec2  axis;         // item.axis -- which way an elongated part runs; zero if round
uniform vec2  lightDir;     // light.dir -- unit vector toward the light, zero if none
uniform vec4  lightTint;    // light.tint
uniform float lightAmount;  // light.strength, 0..1
uniform float ambient;      // light.ambient -- the floor
uniform float damage;       // item.intensity, 1 = whole
uniform float time;         // clock.time

out vec4 finalColor;

// How much of the light's own colour is mixed in. Multiplied rather than replaced, so a
// red star reddens a blue hull instead of turning it red.
const float TINT = 0.6;

void main()
{
    // What raylib's default shader would have produced: the primitive's colour and its
    // coverage. Everything below only modulates this, so a fragment outside the shape
    // stays outside it.
    vec4 texel = texture(texture0, fragTexCoord) * colDiffuse * fragColor;
    if (texel.a <= 0.001)
        discard;

    // Where this fragment sits on the object, as a unit disc. Degenerate radius (an
    // object smaller than a pixel) falls back to flat, which is correct: there is no
    // surface left to shade.
    vec2 local = (radius > 0.5) ? (gl_FragCoord.xy - centre) / radius : vec2(0.0);

    // An elongated part is a cylinder, not a small sphere. Only the distance *across* its
    // axis bends the surface; measuring along it as well lights an arm as a ball and puts
    // both of its ends in shadow, which is exactly what a truss did.
    if (dot(axis, axis) > 0.0001)
    {
        vec2 across = vec2(-axis.y, axis.x);
        local = across * dot(local, across);
    }

    float r = min(length(local), 1.0);

    // Facing: 1 where the surface points at the light, 0 where it points away. The z term
    // is what makes a flat disc read as a sphere -- without it the terminator is a straight
    // line across the shape instead of a curve around it.
    float z = sqrt(max(0.0, 1.0 - r * r));
    vec3 normal = normalize(vec3(local, max(z, 0.001)));
    vec3 toLight = normalize(vec3(lightDir, 0.55));
    float facing = max(0.0, dot(normal, toLight));

    // No direction at all -- no light in the system, or an object standing between two
    // equal stars -- means lit from everywhere rather than lit from nowhere (#119).
    if (dot(lightDir, lightDir) < 0.0001)
        facing = 1.0;

    float lit = ambient + (1.0 - ambient) * facing * lightAmount;

    vec3 col = texel.rgb * lit;
    col = mix(col, col * lightTint.rgb, TINT * lightAmount);

    // The rim: where the surface turns away from the viewer but still catches the light.
    // It is the single cheapest thing that separates an object from the background, and
    // it is why a shape this simple stops reading as a sticker.
    float rim = smoothstep(0.72, 1.0, r) * facing * lightAmount;
    col += lightTint.rgb * rim * 0.55;

    // Damage, with no bar over the object. A hull at a fifth is visibly darker and
    // unsteady; an intact one is untouched, because a flicker on everything is noise.
    if (damage < 1.0)
    {
        float hurt = 1.0 - damage;
        float flicker = 0.85 + 0.15 * sin(time * (7.0 + 20.0 * hurt) + centre.x * 0.05);
        col *= mix(1.0, flicker * (0.45 + 0.55 * damage), hurt);
    }

    finalColor = vec4(col, texel.a);
}
