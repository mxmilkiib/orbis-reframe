#pragma once

#include <cstddef>
#include <cstdint>

#include "ColorLut.h" // color::Grade

// CPU equirectangular -> rectilinear ("flat") reprojection for the export
// pipeline. A direct port of the interactive viewport's fragment shader
// (src/render/shaders/equirect.frag): for each output pixel, reconstruct the
// view-space ray implied by the vertical FOV, rotate it by yaw/pitch to a
// world-space look direction, convert that to equirect UV, and bilinearly
// sample the source frame.
//
// Deliberately Qt-free and Vulkan-free: it is the reproject half of the export,
// linked into reframe_flatten alongside system FFmpeg only. Export is offline,
// so a multithreaded CPU pass is the right tool.

namespace exportpipe {

struct ReframeView {
    float yaw = 0.0f;     // radians, rotation about the vertical (Y) axis
    float pitch = 0.0f;   // radians, rotation about the horizontal (X) axis
    float roll = 0.0f;    // radians, rotation about the view (Z) axis -- horizon tilt
    float fovY = 1.309f;  // vertical field of view, radians (~75 deg)
    float aspect = 1.0f;  // output width / height
    // Gyro de-rotation (row-major 3x3) in the sphere frame, applied to the
    // world ray after the pose -- same as equirect.frag's pc.stab.
    float stab[9] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    // Colour panel: the LUT and the four adjustments; default = neutral.
    // Same formula as the shaders' color_grade.glsl -- see
    // ClipItem::ColorGrade.
    color::Grade colorGrade;
};

// Reproject an interleaved 8-bit RGBA equirect frame (eqW x eqH) into an
// interleaved 8-bit RGBA rectilinear frame (outW x outH). `outRgba` must have
// room for outW*outH*4 bytes. The equirect seam wraps horizontally (U repeat);
// the poles clamp vertically (V clamp-to-edge) -- matching the sampler in
// src/render/EquirectViewportWindow.cpp.
void RenderRectilinear(const uint8_t* eqRgba, int eqW, int eqH,
                       uint8_t* outRgba, int outW, int outH,
                       const ReframeView& view);

// Apply a colour grade (as ReframeView::colorGrade) to `pixels` interleaved
// 8-bit RGBA pixels in place -- the same formula RenderRectilinear grades
// with. A neutral grade is a no-op.
void GradeRgba(uint8_t* rgba, size_t pixels, const color::Grade& grade);

} // namespace exportpipe
