// SPDX-License-Identifier: MIT OR Apache-2.0
#include "FisheyeProjection.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <numbers>
#include <thread>
#include <vector>

namespace insv {

namespace {
constexpr double kPi = std::numbers::pi;
constexpr double kDeg = kPi / 180.0;
} // namespace

// --- linear algebra -------------------------------------------------------

Vec3 normalize(const Vec3& v)
{
    const double n = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return n > 1e-12 ? Vec3{v.x / n, v.y / n, v.z / n} : Vec3{0, 0, 1};
}

Vec3 apply(const Mat3& a, const Vec3& v)
{
    return {a.m[0][0] * v.x + a.m[0][1] * v.y + a.m[0][2] * v.z,
            a.m[1][0] * v.x + a.m[1][1] * v.y + a.m[1][2] * v.z,
            a.m[2][0] * v.x + a.m[2][1] * v.y + a.m[2][2] * v.z};
}

Mat3 mul(const Mat3& a, const Mat3& b)
{
    Mat3 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
    return r;
}

Mat3 transpose(const Mat3& a)
{
    Mat3 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            r.m[i][j] = a.m[j][i];
    return r;
}

Mat3 rotX(double a)
{
    const double c = std::cos(a), s = std::sin(a);
    return Mat3{{{1, 0, 0}, {0, c, -s}, {0, s, c}}};
}
Mat3 rotY(double a)
{
    const double c = std::cos(a), s = std::sin(a);
    return Mat3{{{c, 0, s}, {0, 1, 0}, {-s, 0, c}}};
}
Mat3 rotZ(double a)
{
    const double c = std::cos(a), s = std::sin(a);
    return Mat3{{{c, -s, 0}, {s, c, 0}, {0, 0, 1}}};
}

// --- lens profiles ------------------------------------------------------

bool ApplyLensProfile(StitchParams& params, const InsvMakerNotes& notes,
                      const std::string& accessoryName)
{
    params.accessory = false;
    params.logInput = notes.gamma_mode == "I_Log";
    if (notes.crop_dst_w > 0)
        params.cropRefW = notes.crop_dst_w;
    const LensProfile* bare = FindLensProfile(notes, "bare");
    if (bare && bare->fov_deg > 0)
        params.fovDeg = bare->fov_deg;
    if (accessoryName.empty() || accessoryName == "bare")
        return bare != nullptr;
    const LensProfile* acc = FindLensProfile(notes, accessoryName);
    if (!bare || !acc || bare->poly[1] <= 0 || acc->poly[1] <= 0)
        return false;
    params.accessory = true;
    for (int i = 0; i < 6; ++i) {
        params.barePoly[i] = bare->poly[i];
        params.accessoryPoly[i] = acc->poly[i];
    }
    if (acc->fov_deg > 0)
        params.fovDeg = acc->fov_deg;
    return true;
}

// --- FisheyeStitcher ----------------------------------------------------

bool FisheyeStitcher::configure(const InsvCalibration& cal, int streamW, int streamH,
                                const StitchParams& params)
{
    m_ready = false;
    if (cal.lensesExt.size() != 2 || streamW <= 0 || streamH <= 0)
        return false;

    m_params = params;
    m_streamW = streamW;
    m_streamH = streamH;
    // The circle edge sits at half the usable FOV, and the seam band must end
    // inside it (with lens guards past ~97.5 deg is the guard's rim).
    m_thetaRefRad = std::max(0.5 * params.fovDeg * kDeg, 1e-3);
    const double featherDeg =
        std::clamp(0.5 * params.fovDeg - params.seamDeg - StitchParams::kSeamMarginDeg, 1.0,
                   params.featherDeg);
    m_accessory = params.accessory;
    m_thetaSeamRad = params.seamDeg * kDeg;
    m_bandRad = std::max(featherDeg * kDeg, 1e-4);
    m_thetaMaxRad = (params.seamDeg + featherDeg + 2.0) * kDeg;

    // sensor -> video scaling for the MEI intrinsics (insv-stitch
    // x5_pipeline.py `_sensor_to_video`): fx/fy divide by the crop dst width,
    // cx/cy by the full single-lens sensor width. The crop dst width comes
    // from the trailer's field 27 via params.cropRefW (5312 on the X5,
    // 7680 on the X6).
    const double kCropDstW = params.cropRefW;

    for (int i = 0; i < 2; ++i) {
        const LensCalibExt& e = cal.lensesExt[i];

        // Extrinsic, stored as yaw/pitch/roll (deg), roll ~ 90 being a fixed
        // sensor clock. The calibration's image axes are ours with x and y
        // negated (a 180 deg turn about the optical axis, see LensProjection),
        // so its yaw/pitch (about x / y) flip sign too; roll (about z) doesn't.
        // Measured: the seam match improves on every sample clip.
        double ax = -e.yaw * kDeg;
        double ay = -e.pitch * kDeg;
        double az = (e.roll - params.tzNominalDeg) * kDeg;
        if (i == 1) {
            ax += params.lens1AdjustDeg[0] * kDeg;
            ay += params.lens1AdjustDeg[1] * kDeg;
            az += params.lens1AdjustDeg[2] * kDeg;
        }
        const Mat3 corr = mul(mul(rotZ(az), rotY(ay)), rotX(ax)); // Rz Ry Rx
        const double nominalYaw =
            (i == 0 ? params.nominalYaw0Deg : params.nominalYaw1Deg) * kDeg;
        m_lens[i].worldToLens = transpose(mul(rotY(nominalYaw), corr));

        const double halfW = e.w * 0.5; // side-by-side combined frame
        const double cxLocal = e.cx - (i == 0 ? 0.0 : halfW);
        const double sVideoPerSensor = static_cast<double>(streamW) / halfW;
        const double sVideoPerCrop = static_cast<double>(streamW) / kCropDstW;
        m_lens[i].xi = e.xi;
        m_lens[i].fx = e.fx * sVideoPerCrop;
        m_lens[i].fy = e.fy * sVideoPerCrop;
        m_lens[i].cx = cxLocal * sVideoPerSensor;
        m_lens[i].cy = e.cy * sVideoPerSensor;
        for (int k = 0; k < 4; ++k) {
            m_lens[i].k[k] = e.k[k];
            m_lens[i].s[k] = e.s[k];
        }
        m_lens[i].p[0] = e.p[0];
        m_lens[i].p[1] = e.p[1];
    }

    m_outRot = mul(mul(rotY(params.outYawDeg * kDeg), rotX(params.outPitchDeg * kDeg)),
                   rotZ(params.outRollDeg * kDeg));

    // Seam frame: the lenses' common axis (lens 0's optical axis, the third
    // row of its worldToLens, against lens 1's), and a basis of the seam
    // plane with u as close to world up as the axis allows.
    {
        const double* z0 = m_lens[0].worldToLens.m[2];
        const double* z1 = m_lens[1].worldToLens.m[2];
        m_seamAxis = normalize(Vec3{z0[0] - z1[0], z0[1] - z1[1], z0[2] - z1[2]});
        const Vec3& a = m_seamAxis;
        Vec3 ref{0, 1, 0};
        if (std::fabs(a.y) > 0.9)
            ref = Vec3{1, 0, 0};
        const double d = ref.x * a.x + ref.y * a.y + ref.z * a.z;
        m_seamU = normalize(Vec3{ref.x - d * a.x, ref.y - d * a.y, ref.z - d * a.z});
        const Vec3& u = m_seamU;
        m_seamV = Vec3{a.y * u.z - a.z * u.y, a.z * u.x - a.x * u.z, a.x * u.y - a.y * u.x};
    }
    // Lens 1 sits |tz| behind lens 0 along the axis (docs/formats/insta360-x5.md,
    // offset_v3 fields 8-10); the lateral tx/ty (< 1 mm) are ignored.
    double baseline = std::fabs(params.baselineM);
    if (baseline == 0.0)
        baseline = std::fabs(cal.lensesExt[1].tz - cal.lensesExt[0].tz);
    if (baseline <= 0.0 || baseline > 0.2)
        baseline = 0.032;
    m_halfBaseline = (params.baselineM < 0.0 ? -0.5 : 0.5) * baseline;
    // The depth shift pushes a lens's ray outward by up to a few degrees;
    // let it sample up to the circle edge (the edge weight fades it there).
    if (params.depthSeam)
        m_thetaMaxRad = std::max(m_thetaMaxRad, m_thetaRefRad);

    m_ready = true;
    return true;
}

Vec3 FisheyeStitcher::lensCentre(int lens) const
{
    const double h = lens == 0 ? m_halfBaseline : -m_halfBaseline;
    return {h * m_seamAxis.x, h * m_seamAxis.y, h * m_seamAxis.z};
}

Vec3 FisheyeStitcher::seamDir(double phi, double e) const
{
    const double ce = std::cos(e), se = std::sin(e);
    const double cp = std::cos(phi), sp = std::sin(phi);
    return {ce * (cp * m_seamU.x + sp * m_seamV.x) + se * m_seamAxis.x,
            ce * (cp * m_seamU.y + sp * m_seamV.y) + se * m_seamAxis.y,
            ce * (cp * m_seamU.z + sp * m_seamV.z) + se * m_seamAxis.z};
}

double FisheyeStitcher::seamQAt(const Vec3& dir, const float* seamQ) const
{
    if (!m_params.depthSeam || !seamQ)
        return 0.0;
    const double sa = dir.x * m_seamAxis.x + dir.y * m_seamAxis.y + dir.z * m_seamAxis.z;
    const double e = std::fabs(std::asin(std::clamp(sa, -1.0, 1.0)));
    const double rampRad = m_params.depthRampDeg * kDeg;
    double ramp = 1.0;
    if (e >= m_bandRad + rampRad)
        return 0.0;
    if (e > m_bandRad)
        ramp = 0.5 * (1.0 + std::cos(kPi * (e - m_bandRad) / rampRad));
    const double pu = dir.x * m_seamU.x + dir.y * m_seamU.y + dir.z * m_seamU.z;
    const double pv = dir.x * m_seamV.x + dir.y * m_seamV.y + dir.z * m_seamV.z;
    double phi = std::atan2(pv, pu);
    if (phi < 0)
        phi += 2 * kPi;
    const double f = phi / (2 * kPi) * kSeamDepthBins - 0.5;
    const double fl = std::floor(f);
    const double t = f - fl;
    const int i0 = (static_cast<int>(fl) + kSeamDepthBins) % kSeamDepthBins;
    const int i1 = (i0 + 1) % kSeamDepthBins;
    return ramp * (seamQ[i0] * (1.0 - t) + seamQ[i1] * t);
}

Vec3 FisheyeStitcher::lensRay(int lens, const Vec3& dir, double q) const
{
    if (q == 0.0)
        return dir;
    const Vec3 c = lensCentre(lens);
    return normalize(Vec3{dir.x - q * c.x, dir.y - q * c.y, dir.z - q * c.z});
}

StitchGpuParams FisheyeStitcher::gpuParams() const
{
    StitchGpuParams p{};
    for (int i = 0; i < 2; ++i) {
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                p.worldToLens[i][r * 3 + c] = static_cast<float>(m_lens[i].worldToLens.m[r][c]);
        p.cx[i] = static_cast<float>(m_lens[i].cx);
        p.cy[i] = static_cast<float>(m_lens[i].cy);
        p.xi[i] = static_cast<float>(m_lens[i].xi);
        p.fx[i] = static_cast<float>(m_lens[i].fx);
        p.fy[i] = static_cast<float>(m_lens[i].fy);
        for (int k = 0; k < 4; ++k) {
            p.kdist[i][k] = static_cast<float>(m_lens[i].k[k]);
            p.sdist[i][k] = static_cast<float>(m_lens[i].s[k]);
        }
        p.pdist[i][0] = static_cast<float>(m_lens[i].p[0]);
        p.pdist[i][1] = static_cast<float>(m_lens[i].p[1]);
    }
    p.accessory = m_accessory ? 1 : 0;
    for (int k = 0; k < 6; ++k) {
        p.barePoly[k] = static_cast<float>(m_params.barePoly[k]);
        p.accessoryPoly[k] = static_cast<float>(m_params.accessoryPoly[k]);
    }
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            p.outRot[r * 3 + c] = static_cast<float>(m_outRot.m[r][c]);
    p.thetaRef = static_cast<float>(m_thetaRefRad);
    p.seamRad = static_cast<float>(m_thetaSeamRad);
    p.bandRad = static_cast<float>(m_bandRad);
    p.thetaMaxRad = static_cast<float>(m_thetaMaxRad);
    p.edgeStartFrac = static_cast<float>(m_params.edgeStartFrac);
    p.edgeEndFrac = static_cast<float>(m_params.edgeEndFrac);
    p.photometric = m_params.photometric ? 1 : 0;
    p.fisheyeW = m_streamW;
    p.fisheyeH = m_streamH;
    p.depthActive = m_params.depthSeam ? 1 : 0;
    const Vec3* basis[3] = {&m_seamAxis, &m_seamU, &m_seamV};
    float* dst[3] = {p.seamAxis, p.seamU, p.seamV};
    for (int b = 0; b < 3; ++b) {
        dst[b][0] = static_cast<float>(basis[b]->x);
        dst[b][1] = static_cast<float>(basis[b]->y);
        dst[b][2] = static_cast<float>(basis[b]->z);
    }
    p.halfBaseline = static_cast<float>(m_halfBaseline);
    p.depthRampRad = static_cast<float>(m_params.depthRampDeg * kDeg);
    p.logInput = m_params.logInput ? 1 : 0;
    return p;
}

namespace {
// MEI radial term g(theta) = [sin(theta) / (cos(theta) + xi)] * radialPoly, i.e.
// the sampled radius (in units of fx) as a function of off-axis angle for the
// rotationally-symmetric part of the Mei model.
double meiRadial(double theta, double xi, const double k[4])
{
    const double st = std::sin(theta), ct = std::cos(theta);
    const double denom = ct + xi;
    if (denom < 1e-6)
        return 1e9;
    const double rho = st / denom;         // = sqrt(x^2 + y^2) before distortion
    const double r2 = rho * rho, r4 = r2 * r2;
    const double radial = 1.0 + k[0] * r2 + k[1] * r4 + k[2] * r2 * r4 + k[3] * r4 * r4;
    return rho * radial;
}
} // namespace

double FisheyeStitcher::accessoryScale(double theta) const
{
    if (!m_accessory)
        return 1.0;
    const double* b = m_params.barePoly;
    const double* a = m_params.accessoryPoly;
    const double t = std::max(theta / kDeg, 1e-3);
    double pb = 0, pa = 0;
    for (int i = 5; i >= 0; --i) {
        pb = pb * t + b[i];
        pa = pa * t + a[i];
    }
    return pb > 1e-9 && pa > 0 ? pa / pb : 1.0;
}

double FisheyeStitcher::radiusForTheta(double theta) const
{
    // Normalised so it returns ~1 at thetaRef (the FOV edge), which is what
    // circleEdgeWeight() expects. Uses lens 0's intrinsics (the two lenses
    // differ only marginally, and this only drives edge feather).
    const double tref = std::max(m_thetaRefRad, 1e-3);
    const double ref = meiRadial(tref, m_lens[0].xi, m_lens[0].k) * accessoryScale(tref);
    return ref > 1e-9 ? meiRadial(theta, m_lens[0].xi, m_lens[0].k) * accessoryScale(theta) / ref
                      : 0.0;
}

bool FisheyeStitcher::project(int i, const Vec3& world, double& u, double& v, double& theta) const
{
    const Vec3 d = normalize(apply(m_lens[i].worldToLens, world));
    theta = std::acos(std::clamp(d.z, -1.0, 1.0));
    if (theta > m_thetaMaxRad)
        return false;

    const LensProjection& L = m_lens[i];
    const double denom = d.z + L.xi;
    if (denom < 1e-6)
        return false;
    // Calibration image axes (u right, v down): our lens frame has both
    // flipped. See LensProjection.
    const double x = -d.x / denom, y = -d.y / denom;
    const double r2 = x * x + y * y, r4 = r2 * r2;
    const double* k = L.k;
    const double radial = 1.0 + k[0] * r2 + k[1] * r4 + k[2] * r2 * r4 + k[3] * r4 * r4;
    const double xd = x * radial + 2 * L.p[0] * x * y + L.p[1] * (r2 + 2 * x * x) +
                      L.s[0] * r2 + L.s[1] * r4;
    const double yd = y * radial + L.p[0] * (r2 + 2 * y * y) + 2 * L.p[1] * x * y +
                      L.s[2] * r2 + L.s[3] * r4;
    const double acc = accessoryScale(theta);
    u = L.cx + L.fx * xd * acc;
    v = L.cy + L.fy * yd * acc;
    return true;
}

namespace {
// Feather weight for a lens near the seam: full up to (seam - band), ramping to
// 0 at (seam + band).
double seamWeight(double theta, double seamRad, double bandRad)
{
    if (theta <= seamRad - bandRad)
        return 1.0;
    if (theta >= seamRad + bandRad)
        return 0.0;
    const double t = (theta - (seamRad - bandRad)) / (2 * bandRad);
    return 0.5 * (1.0 + std::cos(kPi * t));
}

// Weight for a lens as its sample radius approaches the imaged circle edge:
// full up to `startFrac`, cosine ramp to 0 by `endFrac` (r/radius units).
double circleEdgeWeight(double rr, double startFrac, double endFrac)
{
    if (rr <= startFrac)
        return 1.0;
    if (rr >= endFrac || endFrac <= startFrac)
        return rr >= endFrac ? 0.0 : 1.0;
    const double t = (rr - startFrac) / (endFrac - startFrac);
    return 0.5 * (1.0 + std::cos(kPi * t));
}
} // namespace

double FisheyeStitcher::seamBlendWeight(int lens, const Vec3& dir) const
{
    const double* z = m_lens[lens].worldToLens.m[2]; // the lens's optical axis
    const double c = z[0] * dir.x + z[1] * dir.y + z[2] * dir.z;
    return seamWeight(std::acos(std::clamp(c, -1.0, 1.0)), m_thetaSeamRad, m_bandRad);
}

namespace {
// I-Log -> BT.709 for StitchParams::logInput (maker notes gamma_mode "I_Log").
// The June-2026 Insta360 white-paper curve maps a code value to linear
// reflectance on BT.2020 primaries; a gamut matrix and the BT.709 OETF bring
// it to ordinary display-encoded RGB. In/out are [0,255]-scaled channels.
float decodeILog(float v01)
{
    // decode: v < 0.154402 ? (v - 0.09055934)/5.77837328
    //                     : pow(10, (v - 0.623992)/0.280055) - 0.01
    const float l = v01 < 0.154402f ? (v01 - 0.09055934f) / 5.77837328f
                                  : std::pow(10.0f, (v01 - 0.623992f) / 0.280055f) - 0.01f;
    return std::max(l, 0.0f);
}
float oetf709(float l)
{
    return l < 0.018f ? 4.5f * l : 1.099f * std::pow(l, 0.45f) - 0.099f;
}
void ilogTo709(std::array<float, 3>& rgb)
{
    float l[3] = {decodeILog(rgb[0] / 255.0f), decodeILog(rgb[1] / 255.0f),
                  decodeILog(rgb[2] / 255.0f)};
    // BT.2020 -> BT.709 primaries (both D65).
    const float r = 1.660491f * l[0] - 0.587641f * l[1] - 0.072850f * l[2];
    const float g = -0.124550f * l[0] + 1.132900f * l[1] - 0.008349f * l[2];
    const float b = -0.018151f * l[0] - 0.100579f * l[1] + 1.118730f * l[2];
    rgb = {oetf709(std::max(r, 0.0f)) * 255.0f, oetf709(std::max(g, 0.0f)) * 255.0f,
           oetf709(std::max(b, 0.0f)) * 255.0f};
}
} // namespace

double FisheyeStitcher::edgeWeight(double thetaRad) const
{
    return circleEdgeWeight(radiusForTheta(thetaRad), m_params.edgeStartFrac, m_params.edgeEndFrac);
}

void FisheyeStitcher::renderEquirect(const Image& lens0, const Image& lens1, Image& out,
                                     const float* seamQ, const BlurQuadFrame* blur,
                                     int blurCount) const
{
    if (!m_ready || out.w <= 0 || out.h <= 0)
        return;

    const int W = out.w, H = out.h;
    const Image* lim[2] = {&lens0, &lens1};

    // Lens i's sample position and blend weight for world dir `dir`: the
    // seam feather from dir itself, the circle edge from the ray actually
    // sampled (shifted by the depth-aware seam). False = no contribution.
    auto lensSample = [&](int i, const Vec3& dir, double q, double& u, double& v,
                          double& w) {
        w = seamBlendWeight(i, dir);
        if (w <= 0.0)
            return false;
        double theta;
        if (!project(i, lensRay(i, dir, q), u, v, theta))
            return false;
        if (u < 0 || v < 0 || u > lim[i]->w - 1.0 || v > lim[i]->h - 1.0)
            return false;
        w *= edgeWeight(theta);
        return w > 0.0;
    };

    // Lens pixel at (u,v), decoded to BT.709 when the clip is I-Log.
    auto pix = [&](int i, double u, double v) {
        auto s = lim[i]->sample(u, v);
        if (m_params.logInput)
            ilogTo709(s);
        return s;
    };

    // --- photometric prepass: mean luma of each lens across the overlap band,
    // on a coarse grid. gain[i] pulls lens i toward the geometric-mean level so
    // the seam has no exposure step. Mirrors fisheye_stitch_common.glsl pass 0.
    double gain[2] = {1.0, 1.0};
    if (m_params.photometric) {
        double sum0 = 0, sum1 = 0;
        long cnt = 0;
        for (int y = 0; y < H; y += 8) {
            const double lat = (0.5 - (y + 0.5) / H) * kPi;
            const double cl = std::cos(lat), sl = std::sin(lat);
            for (int x = 0; x < W; x += 8) {
                const double lon = ((x + 0.5) / W - 0.5) * 2.0 * kPi;
                const Vec3 dir =
                    apply(m_outRot, Vec3{cl * std::sin(lon), sl, -cl * std::cos(lon)});
                const double q = seamQAt(dir, seamQ);
                double u0, v0, w0, u1, v1, w1;
                if (!lensSample(0, dir, q, u0, v0, w0) || !lensSample(1, dir, q, u1, v1, w1))
                    continue;
                if (w0 <= 0.25 || w1 <= 0.25)
                    continue;
                const auto s0 = pix(0, u0, v0);
                const auto s1 = pix(1, u1, v1);
                sum0 += 0.299 * s0[0] + 0.587 * s0[1] + 0.114 * s0[2];
                sum1 += 0.299 * s1[0] + 0.587 * s1[1] + 0.114 * s1[2];
                ++cnt;
            }
        }
        if (cnt > 64 && sum0 > cnt && sum1 > cnt) {
            const double r = std::sqrt((sum1 / cnt) / (sum0 / cnt));
            gain[0] = std::clamp(r, 0.8, 1.25);
            gain[1] = std::clamp(1.0 / r, 0.8, 1.25);
        }
    }

    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    std::atomic<int> nextRow{0};

    // The blended lenses at stitch-world direction `dir`, [0, 255].
    auto shade = [&](const Vec3& dir, double rgb[3]) {
        const double q = seamQAt(dir, seamQ);
        double acc[3] = {0, 0, 0};
        double wsum = 0;
        for (int i = 0; i < 2; ++i) {
            double u, v, w;
            if (!lensSample(i, dir, q, u, v, w))
                continue;
            const auto s = pix(i, u, v);
            acc[0] += w * gain[i] * s[0];
            acc[1] += w * gain[i] * s[1];
            acc[2] += w * gain[i] * s[2];
            wsum += w;
        }
        for (int c = 0; c < 3; ++c)
            rgb[c] = wsum > 1e-6 ? acc[c] / wsum : 0.0;
    };

    auto worker = [&] {
        for (int y = nextRow.fetch_add(1); y < H; y = nextRow.fetch_add(1)) {
            const double lat = (0.5 - (y + 0.5) / H) * kPi;
            const double cl = std::cos(lat), sl = std::sin(lat);
            for (int x = 0; x < W; ++x) {
                const double lon = ((x + 0.5) / W - 0.5) * 2.0 * kPi;
                const Vec3 sphere{cl * std::sin(lon), sl, -cl * std::cos(lon)};
                const Vec3 dir = apply(m_outRot, sphere);
                double rgb[3];
                shade(dir, rgb);
                // Privacy blur: the quads are in the sphere frame, so test
                // the sphere direction and render the taps through outRot
                // (fisheye_stitch_common.glsl blurredLenses).
                const double sd[3] = {sphere.x, sphere.y, sphere.z};
                for (int k = 0; k < blurCount; ++k) {
                    double s, t;
                    if (!blurQuadST(blur[k], sd, s, t))
                        continue;
                    double mos[3] = {0, 0, 0};
                    for (int i = 0; i < kBlurTaps; ++i)
                        for (int j = 0; j < kBlurTaps; ++j) {
                            double td[3], c[3];
                            blurTapDir(blur[k], s, t, i, j, td);
                            shade(apply(m_outRot, Vec3{td[0], td[1], td[2]}), c);
                            for (int ch = 0; ch < 3; ++ch)
                                mos[ch] += c[ch];
                        }
                    const double w = blurQuadWeight(s, t);
                    for (int ch = 0; ch < 3; ++ch)
                        rgb[ch] += (mos[ch] / (kBlurTaps * kBlurTaps) - rgb[ch]) * w;
                    break;
                }
                uint8_t* px = &out.rgb[(static_cast<std::size_t>(y) * W + x) * 3];
                for (int c = 0; c < 3; ++c)
                    px[c] = static_cast<uint8_t>(std::clamp(rgb[c] + 0.5, 0.0, 255.0));
            }
        }
    };

    std::vector<std::thread> pool;
    pool.reserve(hw);
    for (unsigned i = 0; i < hw; ++i)
        pool.emplace_back(worker);
    for (auto& t : pool)
        t.join();
}

Image FisheyeStitcher::renderEquirect(const Image& lens0, const Image& lens1, int width,
                                      const float* seamQ) const
{
    Image out;
    out.alloc(width, width / 2);
    renderEquirect(lens0, lens1, out, seamQ);
    return out;
}

} // namespace insv
