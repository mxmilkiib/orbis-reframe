// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include "BlurQuads.h"
#include "Calibration.h"
#include "Image.h"
#include "InsvTrailer.h" // insv::InsvMakerNotes, insv::LensProfile

#include <array>

// CPU fisheye -> equirectangular stitch (src/insv/).
//
// Productised from the since-removed tools/native_stitch_spike. For each
// equirect texel: reconstruct the world ray, rotate each lens's ray into its
// own frame, project it through the lens's MEI calibration, sample, and
// feather-blend the two lenses across the overlap band, with photometric seam
// compensation. With StitchParams::depthSeam each lens is sampled from its own
// centre at a per-azimuth stitch distance (insv::SeamDepth), so near content on
// the seam lines up; no dense optical-flow warp beyond that.
//
// World convention matches src/render/shaders/equirect.frag and
// src/export/ReframeRenderer: +X right, +Y up, -Z forward; equirect
// longitude = atan2(x, -z), latitude = asin(y).
//
// Dependency-free (no Qt / FFmpeg / Vulkan) -- part of the `insv`
// static lib. The .insv demux that produces the input frames is the separate
// `insv_demux` lib (it needs FFmpeg); see InsvDemux.h.

namespace insv {

// --- small linear algebra (double; public so tests can exercise it) --------

struct Vec3 {
    double x = 0, y = 0, z = 0;
};

struct Mat3 {
    // row-major; identity by default
    double m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
};

Vec3 normalize(const Vec3& v);
Vec3 apply(const Mat3& a, const Vec3& v); // a * v
Mat3 mul(const Mat3& a, const Mat3& b);   // a * b
Mat3 transpose(const Mat3& a);
Mat3 rotX(double rad);
Mat3 rotY(double rad);
Mat3 rotZ(double rad);

// Stitch parameters. The lens geometry itself comes from the calibration;
// these are the seam/blend choices plus the fixed X5 conventions.
struct StitchParams {
    // Per-lens cutoff / feather centre, and the feather half-width around it.
    // The two lenses face opposite ways, so 90 splits the sphere evenly; the
    // band is further capped by the lens field of view (fovDeg) so neither lens
    // is sampled past its usable edge.
    double seamDeg = 90.0;
    double featherDeg = 8.0;

    // Usable lens field of view, degrees: 200 for the bare X5 lens, 195 with
    // lens guards, 190 in a dive case (0x101 field 145). Sets the circle-edge
    // reference angle (fovDeg/2) and caps the seam band at
    // fovDeg/2 - seamDeg - kSeamMarginDeg.
    double fovDeg = 200.0;
    static constexpr double kSeamMarginDeg = 2.5;

    // Focal-reference width the MEI fx/fy are calibrated in (0x101 field 27
    // `window_crop_info` dst: 5312 on the X5, 7680 on the X6). Set by
    // ApplyLensProfile from the maker notes; 5312 is the X5 default for files
    // that don't carry the field.
    double cropRefW = 5312.0;

    // Accessory lens-profile correction (lens guards, dive cases). The
    // trailer's MEI calibration describes the bare lens. An accessory bends the
    // field so each angle lands further out on the sensor -- ~2.3 % at the seam
    // for the X5 lens guards, which is a ~4 deg ghost across the seam if
    // ignored. When `accessory` is set, the MEI image radius is scaled by
    // accessoryPoly(theta) / barePoly(theta), both theta-in-degrees -> mm
    // polynomials from 0x101 field 145 (docs/formats/insta360-x5.md §5.4).
    bool accessory = false;
    double barePoly[6] = {0, 0, 0, 0, 0, 0};
    double accessoryPoly[6] = {0, 0, 0, 0, 0, 0};

    // Measured correction to lens 1's extrinsic, degrees, added to its yaw
    // (about x), pitch (about y) and roll (about z) in our lens frame: the seam alignment fine-tune (insv::EstimateSeamAlignment).
    double lens1AdjustDeg[3] = {0, 0, 0};

    // Depth-aware seam. The two lenses sit `baselineM` apart along their common
    // axis (the calibration's lens-1 tz, ~32 mm on the X5), so an object at
    // distance D on the seam is seen ~baselineM/D rad apart by the two lenses.
    // With `depthSeam` each lens is projected from its own centre at a
    // per-azimuth stitch distance (the table passed to renderEquirect /
    // StitchGpuParams::seamQ, measured per frame by insv::SeamDepthTracker),
    // fading back to infinity over `depthRampDeg` beyond the blend band. Off =
    // both lenses at one point.
    bool depthSeam = false;
    // 0 = from the calibration (|lens-1 tz|); negative swaps which lens is
    // in front (a diagnostic for the sign, tools/insv_stitch --baseline).
    double baselineM = 0.0;
    double depthRampDeg = 20.0;

    // Circle-edge cutoff: a lens contributes fully while its sample radius
    // (normalised to 1 at fovDeg/2, see radiusForTheta) <= edgeStartFrac,
    // then feathers to 0 by edgeEndFrac. Stops the wide-angle rays near the poles from sampling the black/vignette ring
    // outside the imaged circle (the "pole smear"). The other lens still covers
    // that direction (combined FOV > 360 on the X5's ~200 deg lenses).
    double edgeStartFrac = 0.90;
    double edgeEndFrac = 1.00;

    // Photometric seam compensation: match the two lenses' mean brightness in
    // the overlap band (sqrt-split gain, clamped) so the seam has no exposure
    // step. Per-frame; off = raw cross-fade.
    bool photometric = true;

    // The X5 stores roll ~ 90 for both lenses -- a fixed sensor-frame clock,
    // not a per-unit correction. Subtracted from each lens's stored roll so
    // only the small real deviation is applied.
    double tzNominalDeg = 90.0;

    // Nominal hemisphere yaw per lens (lens 0 forward-ish, lens 1 ~180 apart).
    double nominalYaw0Deg = 0.0;
    double nominalYaw1Deg = 180.0;

    // Global equirect orientation applied to the sampling ray (Ry(yaw) Rx(pitch)
    // Rz(roll)) -- levels the sensors' common mount clock and matches
    // Insta360's "forward". 180 = Insta360 forward vs our lens 0.
    double outYawDeg = 180.0;
    double outPitchDeg = 0.0;
    double outRollDeg = 0.0;
};

// Sets `params` up for the lens accessory named `accessoryName` (a field-145
// profile name such as "ProtectorA"; empty = bare lens): the usable FOV, and
// the accessory radial scale against the file's "bare" profile. Returns false
// (and leaves the bare-lens setup) when the file lacks the needed profiles.
bool ApplyLensProfile(StitchParams& params, const InsvMakerNotes& notes,
                      const std::string& accessoryName);

// Built per-lens projection data, in decoded per-stream pixels.
struct LensProjection {
    Mat3 worldToLens; // R_lens^T

    // MEI (Mei/unified omnidirectional) model, from the extended calibration
    // (insv::LensCalibExt). project() maps the lens-frame ray d to the
    // calibration's image axes (u right, v down -- our lens frame has both
    // flipped):
    //   x = -d.x/(d.z+xi), y = -d.y/(d.z+xi),  r^2 = x^2+y^2
    //   radial = 1 + k1 r^2 + k2 r^4 + k3 r^6 + k4 r^8
    //   xd = x radial + 2 p1 x y + p2 (r^2 + 2x^2) + s1 r^2 + s2 r^4
    //   yd = y radial + p1 (r^2 + 2y^2) + 2 p2 x y + s3 r^2 + s4 r^4
    //   u = cx + fx*xd*acc(theta),  v = cy + fy*yd*acc(theta)
    // acc() is the accessory scale (StitchParams::accessory, 1 without).
    // The tangential/thin-prism terms move the rim by up to ~9 px; including
    // them in these axes measurably improves the seam match.
    double cx = 0, cy = 0;
    double xi = 0, fx = 0, fy = 0;
    double k[4] = {0, 0, 0, 0};
    double p[2] = {0, 0};       // tangential
    double s[4] = {0, 0, 0, 0}; // thin-prism
};

// Depth-aware seam table: the seam circle is split into this many
// azimuth bins, each with its own inverse stitch distance q (1/m; 0 =
// infinity). Must match SEAM_BINS in fisheye_stitch_common.glsl.
constexpr int kSeamDepthBins = 128;

// Everything a GPU port of renderEquirect() needs, as plain floats -- one
// source of truth for the geometry so the compute shader stays a faithful
// mirror of the CPU path. Populated by FisheyeStitcher::gpuParams(). All
// angles in radians; matrices row-major 3x3.
struct StitchGpuParams {
    float worldToLens[2][9];
    float outRot[9];
    // MEI model (see LensProjection).
    float cx[2] = {0, 0};
    float cy[2] = {0, 0};
    float xi[2] = {0, 0};
    float fx[2] = {0, 0};
    float fy[2] = {0, 0};
    float kdist[2][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}};
    float pdist[2][2] = {{0, 0}, {0, 0}};             // tangential p1, p2
    float sdist[2][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}}; // thin-prism s1..s4
    // Accessory scale acc(theta) = accessoryPoly(deg) / barePoly(deg); off
    // when accessory == 0. See StitchParams::accessory.
    int accessory = 0;
    float barePoly[6] = {0, 0, 0, 0, 0, 0};
    float accessoryPoly[6] = {0, 0, 0, 0, 0, 0};
    float thetaRef = 0; // circle-edge angle (fovDeg/2), radians
    float seamRad = 0;
    float bandRad = 0;
    float thetaMaxRad = 0;
    float edgeStartFrac = 0.90f;
    float edgeEndFrac = 1.00f;
    int photometric = 1;
    int fisheyeW = 0;
    int fisheyeH = 0;
    // Per-frame capture-time correction (GPU stitch only; rsActive 0 = off).
    // Lens i's fisheye row v was captured at the frame time + lens offset +
    // (v/fisheyeH - 0.5) * that lens's readout (stitch::CaptureTiming); the
    // shader rotates the sampled ray back by how far the camera turned by
    // then. rsRot[i][k] is that rotation (rotation vector, rad, equirect /
    // sphere frame) at row fraction k/(kRsTaps-1), interpolated per row -- a
    // table rather than one rate because the stick mount's ~11 Hz swing
    // changes direction within a readout. rsUseColumns scans along u.
    static constexpr int kRsTaps = 7;
    float rsRot[2][kRsTaps][3] = {};
    int rsActive = 0;
    int rsUseColumns = 0;
    // Depth-aware seam (StitchParams::depthSeam). The seam frame: `seamAxis`
    // is the lenses' common axis (lens 0's way), `seamU`/`seamV` span the
    // seam plane (azimuth phi = atan2(v, u)). Lens 0 sits at
    // +halfBaseline * seamAxis, lens 1 at -halfBaseline * seamAxis (metres).
    // Per frame: seamQ[b] is the inverse stitch distance of azimuth bin b
    // (bin centres at (b + 0.5) * 2pi / kSeamDepthBins).
    int depthActive = 0;
    float seamAxis[3] = {0, 0, 1};
    float seamU[3] = {1, 0, 0};
    float seamV[3] = {0, 1, 0};
    float halfBaseline = 0;
    float depthRampRad = 0;
    float seamQ[kSeamDepthBins] = {};
    // Privacy blur (BlurQuads.h): regions mosaicked in this frame, equirect
    // (sphere) frame, prepared. Per frame, like the RS taps.
    int blurCount = 0;
    BlurQuadFrame blur[kMaxBlurQuads];
};

class FisheyeStitcher {
public:
    // `cal` must carry the extended MEI calibration for exactly 2 lenses;
    // `streamW/streamH` are the decoded per-lens fisheye frame dimensions
    // (3840x3840 on the X5). Returns false otherwise. Cheap; safe to call again on a params change.
    bool configure(const InsvCalibration& cal, int streamW, int streamH, const StitchParams& params);

    bool ready() const { return m_ready; }

    // Reproject `lens0`/`lens1` into `out`, which the caller has already
    // alloc()'d to the desired 2:1 equirect size. No-op (leaves `out` as given)
    // when !ready(). Multithreaded over scanlines. `seamQ` (kSeamDepthBins
    // values, nullptr = all 0) is the depth-aware seam's per-azimuth inverse
    // stitch distance; used only with StitchParams::depthSeam.
    // `blur` (`blurCount` prepared quads, BlurQuads.h) are mosaicked.
    void renderEquirect(const Image& lens0, const Image& lens1, Image& out,
                        const float* seamQ = nullptr, const BlurQuadFrame* blur = nullptr,
                        int blurCount = 0) const;

    // Convenience: allocate a `width x width/2` equirect and render into it.
    Image renderEquirect(const Image& lens0, const Image& lens1, int width,
                         const float* seamQ = nullptr) const;

    // Flattened geometry for a GPU port (see StitchGpuParams). Valid after a
    // successful configure().
    StitchGpuParams gpuParams() const;

    // --- introspection, for tests / debugging -----------------------------
    const LensProjection& lensProjection(int i) const { return m_lens[i]; }
    const Mat3& outRotation() const { return m_outRot; }

    // world dir -> (in front of the lens?, pixel u,v, off-axis angle theta).
    bool project(int lens, const Vec3& world, double& u, double& v, double& theta) const;

    // Image radius for an off-axis angle (radians), normalised to 1 at the
    // circle edge (fovDeg/2). Drives the edge feather.
    double radiusForTheta(double thetaRad) const;

    // Accessory radial scale at an off-axis angle (radians); 1 without one.
    double accessoryScale(double thetaRad) const;

    // --- depth-aware seam (StitchParams::depthSeam) -----------------------
    bool depthSeam() const { return m_params.depthSeam; }
    // The seam frame (see StitchGpuParams::seamAxis) and lens i's centre.
    const Vec3& seamAxis() const { return m_seamAxis; }
    const Vec3& seamU() const { return m_seamU; }
    const Vec3& seamV() const { return m_seamV; }
    Vec3 lensCentre(int lens) const;
    // Direction on the seam frame at azimuth phi, elevation e (towards lens 0).
    Vec3 seamDir(double phi, double e) const;
    // The inverse stitch distance for world dir `dir` from the per-bin table:
    // linear in azimuth, faded to 0 past the blend band. 0 when off / null.
    double seamQAt(const Vec3& dir, const float* seamQ) const;
    // The direction lens i sees the point at distance 1/q along `dir` in.
    Vec3 lensRay(int lens, const Vec3& dir, double q) const;
    // Blend weight of lens i for world dir `dir` (seam feather only, no
    // circle edge): from dir's own off-axis angle, whatever the depth.
    double seamBlendWeight(int lens, const Vec3& dir) const;
    // Circle-edge weight for an off-axis angle (radians) actually sampled.
    double edgeWeight(double thetaRad) const;

private:
    bool m_ready = false;
    bool m_accessory = false; // accessory radial scale active
    StitchParams m_params;
    LensProjection m_lens[2];
    Mat3 m_outRot;
    int m_streamW = 0;
    int m_streamH = 0;
    double m_thetaRefRad = 0;
    double m_thetaSeamRad = 0;
    double m_bandRad = 0;
    double m_thetaMaxRad = 0;
    Vec3 m_seamAxis{0, 0, 1};
    Vec3 m_seamU{1, 0, 0};
    Vec3 m_seamV{0, 1, 0};
    double m_halfBaseline = 0;
};

} // namespace insv
