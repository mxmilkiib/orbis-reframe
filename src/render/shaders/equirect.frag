#version 450
#extension GL_GOOGLE_include_directive : require

// Reprojects an equirectangular 360 frame to a rectilinear "flat" viewport:
// for each pixel, reconstruct the view-space ray implied by the vertical
// FOV, rotate it by yaw/pitch to get a world-space look direction, then
// convert that direction to equirect UV coordinates.

layout(location = 0) in vec2 inNdc;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D equirectSampler;

layout(push_constant) uniform PushConstants {
    float yaw;    // radians, rotation around the vertical (Y) axis
    float pitch;  // radians, rotation around the horizontal (X) axis
    float roll;   // radians, rotation around the view (Z) axis -- horizon tilt
    float fovY;   // vertical field of view, radians
    float aspect; // viewport width / height
    // 12 bytes of std140 padding here (5 floats -> mat3 aligns at offset 32).
    mat3 stab;    // Gyro de-rotation in the sphere frame, applied to the
                  // world ray AFTER the yaw/pitch/roll reframe pose.
                  // Identity when stabilization off.
    // Colour panel: x=exposureEv (stops), y=contrast (%), z=saturation (%),
    // w=temperature (%). All 0 = neutral. Applied by color_grade.glsl.
    vec4 colorGrade;
    vec4 colorLut;   // the clip's LUT: mix, domain min / max (color_grade.glsl)
} pc;

const float PI = 3.14159265358979323846;

#include "color_grade.glsl"

void main() {
    float tanHalfFovY = tan(pc.fovY * 0.5);
    vec3 rayView = normalize(vec3(inNdc.x * tanHalfFovY * pc.aspect, -inNdc.y * tanHalfFovY, -1.0));

    float cosRoll = cos(pc.roll);
    float sinRoll = sin(pc.roll);
    float cosPitch = cos(pc.pitch);
    float sinPitch = sin(pc.pitch);
    float cosYaw = cos(pc.yaw);
    float sinYaw = sin(pc.yaw);

    // Roll around Z (view axis) first, then pitch around X, then yaw around Y.
    vec3 rayRolled = vec3(
        rayView.x * cosRoll - rayView.y * sinRoll,
        rayView.x * sinRoll + rayView.y * cosRoll,
        rayView.z
    );
    vec3 rayPitched = vec3(
        rayRolled.x,
        rayRolled.y * cosPitch - rayRolled.z * sinPitch,
        rayRolled.y * sinPitch + rayRolled.z * cosPitch
    );
    vec3 rayPosed = vec3(
        rayPitched.x * cosYaw + rayPitched.z * sinYaw,
        rayPitched.y,
        -rayPitched.x * sinYaw + rayPitched.z * cosYaw
    );
    // Stabilization lives in the camera-body (sphere) frame: C * P * ray.
    vec3 rayWorld = pc.stab * rayPosed;

    float longitude = atan(rayWorld.x, -rayWorld.z); // [-PI, PI], 0 = forward
    float latitude = asin(clamp(rayWorld.y, -1.0, 1.0)); // [-PI/2, PI/2]

    vec2 uv = vec2(longitude / (2.0 * PI) + 0.5, 0.5 - latitude / PI);
    outColor = texture(equirectSampler, uv);
    outColor.rgb = gradeColor(outColor.rgb, pc.colorGrade, pc.colorLut, 1.0);
}
