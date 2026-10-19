#version 450
#extension GL_GOOGLE_include_directive : require

// A video-track clip's picture-in-picture inset, drawn over the view
// with alpha blending -- or a flat main-track clip, as a full-frame one with
// its colour grade, or a title's text. The fullscreen triangle covers the output rect; this
// shader shades the pixels the inset covers and discards the rest.
//
// Coverage (rounded corners, antialiased edge, border) is inset::shade() in
// src/core/InsetGeometry.h, line for line -- the export composites with that
// C++ version, so keep the two in step.

layout(location = 0) in vec2 inNdc;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D picture;

layout(push_constant) uniform PushConstants {
    vec4 rect;        // inset x, y, w, h in output-rect pixels (inset::place)
    vec2 origin;      // the output rect's top-left in framebuffer pixels
    float radius;     // corner radius, px
    float border;     // border width, px
    vec4 borderColor; // rgb; a = opacity
    vec4 grade;       // colour grade (color_grade.glsl), all 0 = none
    vec4 lut;         // its LUT: mix (0 = none), domain min / max
} pc;

#include "color_grade.glsl"

void main() {
    vec2 p = gl_FragCoord.xy - pc.origin; // pixel centre, output-rect px
    vec2 h = pc.rect.zw * 0.5;
    vec2 q = abs(p - (pc.rect.xy + h)) - (h - pc.radius);
    float d = length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - pc.radius;
    float alpha = clamp(0.5 - d, 0.0, 1.0) * pc.borderColor.a;
    if (alpha <= 0.0)
        discard;
    float border = pc.border > 0.0 ? clamp(d + pc.border + 0.5, 0.0, 1.0) : 0.0;
    // The picture's alpha is premultiplied: a video's frame is opaque, a
    // title's text transparent around the letters.
    // The border covers whatever the picture's alpha.
    vec4 t = texture(picture, (p - pc.rect.xy) / pc.rect.zw);
    alpha *= t.a + (1.0 - t.a) * border;
    if (alpha <= 0.0)
        discard;
    vec3 c = gradeColor(t.a > 0.0 ? min(t.rgb / t.a, vec3(1.0)) : vec3(0.0), pc.grade, pc.lut, 1.0);
    outColor = vec4(mix(c, pc.borderColor.rgb, border), alpha);
}
