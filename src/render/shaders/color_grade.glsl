// Colour grade (Colour panel), shared by every shader that grades:
// equirect.frag and fisheye_view.frag ([0,1] space), reframe_reproject.comp
// and fisheye_view_nv12.comp ([0,255] space). The CPU export fallback,
// exportpipe::gradeRgb() in src/export/ReframeRenderer.cpp, is the C++ twin --
// it can't include GLSL, so change both together (see ClipItem::ColorGrade).
//
// g = {exposureEv (stops), contrast%, saturation%, temperature%}; all 0 =
// neutral. lut = {mix 0..1, domainMin, domainMax, unused}: the clip's 3D LUT
// (gradeLut, set 1 -- render::VulkanLutStore) mixed in by lut.x; 0 = none
// (the set then holds a tiny identity LUT). white = the value of full white
// in c's space: 1.0 or 255.0. The constants are in 8-bit levels, scaled by
// white / 255. Order: the LUT, temperature, exposure, contrast, saturation.
layout(set = 1, binding = 0) uniform sampler3D gradeLut;

vec3 gradeColor(vec3 c, vec4 g, vec4 lut, float white)
{
    if (lut.x > 0.0) {
        // Texel centres: entry 0 at 0.5 / n, entry n-1 at 1 - 0.5 / n.
        float n = float(textureSize(gradeLut, 0).x);
        vec3 x = clamp((clamp(c / white, 0.0, 1.0) - lut.y) / (lut.z - lut.y), 0.0, 1.0);
        vec3 l = texture(gradeLut, x * ((n - 1.0) / n) + 0.5 / n).rgb;
        c = mix(c, clamp(l, 0.0, 1.0) * white, lut.x);
    }
    float level = white / 255.0;
    float t = g.w / 100.0;
    c.r += t * 40.0 * level; // push toward warm(+)/cool(-)
    c.b -= t * 40.0 * level;
    c *= pow(2.0, g.x);
    float mid = 128.0 * level; // contrast pivot
    c = (c - mid) * (1.0 + g.y / 100.0) + mid;
    float luma = dot(c, vec3(0.299, 0.587, 0.114));
    c = luma + (c - luma) * (1.0 + g.z / 100.0);
    return clamp(c, 0.0, white);
}
