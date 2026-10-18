// Shared projection core for the fisheye->equirect compute stitch. The
// includer must, BEFORE #include-ing this file, declare its two lens image
// bindings (1 and 2) and define:
//
//     vec3 sampleLens(int i, vec2 uv);   // returns RGB in [0, 255]
//
// so the RGB (fisheye_stitch.comp) and NV12/ycbcr (fisheye_stitch_nv12.comp)
// variants can share everything else. Kept a faithful mirror of the CPU
// reference insv::FisheyeStitcher::renderEquirect -- constants come from
// insv::FisheyeStitcher::gpuParams().
//
// Dispatched twice per frame: pass 0 (pc.pass==0) measures the two lenses' mean
// luma in the overlap band into GainBuf; pass 1 renders, applying a per-lens
// photometric gain so the seam has no brightness step. Pass 2 (its own
// submission, before the others) is the depth-aware seam's cost volume
// (seamCostPass): one workgroup per (azimuth bin, candidate distance), NCC of
// the two lenses over the bin's patch.
//
// An includer that defines FISHEYE_STITCH_NO_MAIN (fisheye_view_nv12.comp,
// the fused fisheye -> view render) gets only the projection core and the
// per-direction helpers (eqTexelDir / lensWeights / measureAt / photoGains /
// blendLenses) and declares its own push constants and main(). One that also
// defines FISHEYE_STITCH_NO_BUFFERS (fisheye_view.frag, the viewport) gets no
// OutBuf / GainBuf and so no measureAt / photoGains -- its gains arrive
// precomputed.

#define RS_TAPS 7 // == insv::StitchGpuParams::kRsTaps
#define SEAM_BINS 128 // == insv::kSeamDepthBins
#define SEAM_CANDS 40 // == insv::kSeamDepthCands
#define SEAM_PATCH 16 // == insv::kSeamPatchAz == insv::kSeamPatchEl
#define MAX_BLUR 64 // == insv::kMaxBlurQuads
#define BLUR_TAPS 3 // == insv::kBlurTaps
#define BLUR_FEATHER 0.08 // == insv::kBlurFeather

layout(std140, binding = 0) uniform Params {
    vec4 w2l0[3];   // worldToLens[0] rows (.xyz)
    vec4 w2l1[3];   // worldToLens[1] rows
    vec4 outRot[3]; // global equirect orientation rows
    vec4 cxcy;      // cx0, cy0, cx1, cy1   (fisheye-pixel space)
    vec4 seam;      // seamRad, bandRad, thetaMaxRad, thetaRef (circle edge, rad)
    vec4 edge;      // edgeStartFrac, edgeEndFrac, photometric(0/1), gammaMode(0/1/2)
    ivec4 dims;     // fisheyeW, fisheyeH, eqW, eqH
    // Extended MEI (Mei/unified) model. See insv::LensProjection / gpuParams().
    vec4 meiA;      // xi0, xi1, fx0, fx1
    vec4 meiB;      // fy0, fy1, -, accessory(as float)
    vec4 kd0;       // lens 0 radial distortion k1..k4
    vec4 kd1;       // lens 1 radial distortion k1..k4
    vec4 rs;        // capture timing: x 1 = on, y 1 = scan along u (columns)
    vec4 rsRot[2 * RS_TAPS]; // per lens: camera rotation to row time k (rotation vector, world frame), see projectRS
    vec4 pd0;       // lens 0 tangential p1, p2, thin-prism s1, s2
    vec4 pd1;       // lens 1 likewise
    vec4 sd;        // thin-prism s3, s4 of lens 0, then of lens 1
    vec4 accA;      // accessory poly c0..c3   (theta deg -> mm, 0x101 field 145)
    vec4 accB;      // accessory poly c4, c5, bare poly c0, c1
    vec4 accC;      // bare poly c2..c5
    // Depth-aware seam (insv::StitchGpuParams::seamAxis etc.).
    vec4 seamA;     // seam axis (lens 0's way) xyz, depth active (0/1)
    vec4 seamU;     // seam-plane basis u xyz, half baseline (m; lens 0 at +h * axis)
    vec4 seamV;     // seam-plane basis v xyz, ramp past the band (rad)
    vec4 seamC;     // cost pass: candidate q min, q max (1/m), patch elevation half-range (rad), -
    vec4 seamQ[SEAM_BINS / 4]; // per azimuth bin: inverse stitch distance (1/m)
    // Privacy blur (insv::BlurQuadFrame, carried into the stitch world frame):
    // per quad 6 vec4: c.xyz + cells along s, r.xyz + cells along t, u.xyz +
    // cos radius, corners 0 / 1 (xy, xy), corners 2 / 3, (scale, -, -, -).
    vec4 blur0; // count, -, -, -
    vec4 blurQ[MAX_BLUR * 6];
} P;

#ifndef FISHEYE_STITCH_NO_BUFFERS
layout(std430, binding = 3) writeonly buffer OutBuf { uint px[]; };
layout(std430, binding = 4) buffer GainBuf { uint gsum[3]; }; // luma0*4, luma1*4, count
// Pass 2 output: per (bin, candidate) = bin * SEAM_CANDS + k: (NCC, texture),
// as insv::SeamCosts.
layout(std430, binding = 5) writeonly buffer SeamBuf { vec2 seamCost[]; };
#endif

#ifndef FISHEYE_STITCH_NO_MAIN
layout(push_constant) uniform PC { int pass; } pc;
#endif

const float PI = 3.14159265358979323846;
const float GAIN_FP = 4.0;

// Transfer decodes for StitchParams::gammaMode (P.edge.w). The June-2026
// white-paper I-Log curve maps a code value to linear reflectance on BT.2020
// primaries; "Dolby_Vision" clips carry HLG (arib-std-b67) on the same
// primaries. Both end in a gamut matrix and the BT.709 OETF producing
// ordinary display-encoded RGB, so the rest of the pipeline (gains, seam
// costs, blending, grading) is unaffected.
float ilogDecode(float v)
{
    return v < 0.154402 ? (v - 0.09055934) / 5.77837328
                        : pow(10.0, (v - 0.623992) / 0.280055) - 0.01;
}
// HLG inverse OETF: signal -> scene light, a = 0.17883277, b = 0.28466892,
// c = 0.55991073.
float hlgDecode(float e)
{
    return e <= 0.5 ? (e * e) / 3.0
                    : (exp((e - 0.55991073) / 0.17883277) + 0.28466892) / 12.0;
}
float oetf709(float l)
{
    return l < 0.018 ? 4.5 * l : 1.099 * pow(l, 0.45) - 0.099;
}
// BT.2020 -> BT.709 primaries (both D65) then the BT.709 OETF.
vec3 lin2020To709(vec3 l)
{
    vec3 d = vec3(dot(vec3(1.660491, -0.587641, -0.072850), l),
                  dot(vec3(-0.124550, 1.132900, -0.008349), l),
                  dot(vec3(-0.018151, -0.100579, 1.118730), l));
    return vec3(oetf709(max(d.r, 0.0)), oetf709(max(d.g, 0.0)), oetf709(max(d.b, 0.0)));
}
vec3 ilogTo709(vec3 c)
{
    return lin2020To709(max(vec3(ilogDecode(c.r), ilogDecode(c.g), ilogDecode(c.b)),
                          vec3(0.0)));
}
// HLG: decode to scene light, then the BT.2100 OOTF evaluated for the SDR
// target -- the system gamma is 1.2 + 0.42*log10(Lw/1000), 0.78 at 100 nits
// -- so channels scale by Ys^(gamma-1) = Ys^-0.22, then encode.
vec3 hlgTo709(vec3 c)
{
    vec3 l = vec3(hlgDecode(c.r), hlgDecode(c.g), hlgDecode(c.b));
    float ys = max(dot(l, vec3(0.2627, 0.6780, 0.0593)), 1e-6);
    return lin2020To709(l * pow(ys, -0.22));
}
// The lens pixel at uv in [0,255], decoded to BT.709 for log/HDR sources.
vec3 lensColor(int i, vec2 uv)
{
    vec3 c = sampleLens(i, uv);
    if (P.edge.w == 1.0)
        c = ilogTo709(c / 255.0) * 255.0;
    else if (P.edge.w == 2.0)
        c = hlgTo709(c / 255.0) * 255.0;
    return c;
}

vec3 mul3(vec4 r0, vec4 r1, vec4 r2, vec3 v)
{
    return vec3(dot(r0.xyz, v), dot(r1.xyz, v), dot(r2.xyz, v));
}

// Rotationally-symmetric part of the MEI model: sampled radius (units of fx)
// vs. off-axis angle.  rho = sin/(cos+xi), then the radial polynomial.
float meiRadial(float theta, float xi, vec4 k)
{
    float denom = cos(theta) + xi;
    if (denom < 1e-6)
        return 1e9;
    float rho = sin(theta) / denom;
    float r2 = rho * rho, r4 = r2 * r2;
    return rho * (1.0 + k.x * r2 + k.y * r4 + k.z * r2 * r4 + k.w * r4 * r4);
}

// Accessory (lens guard / dive case) radial scale: accessoryPoly(deg) /
// barePoly(deg). 1 without an accessory. See insv::StitchParams::accessory.
float accessoryScale(float theta)
{
    if (P.meiB.w == 0.0)
        return 1.0;
    float t = max(degrees(theta), 1e-3);
    float pa = ((((P.accB.y * t + P.accB.x) * t + P.accA.w) * t + P.accA.z) * t + P.accA.y) * t + P.accA.x;
    float pb = ((((P.accC.w * t + P.accC.z) * t + P.accC.y) * t + P.accC.x) * t + P.accB.w) * t + P.accB.z;
    return (pb > 1e-9 && pa > 0.0) ? pa / pb : 1.0;
}

// Normalised so it is ~1 at thetaRef, the FOV edge (what circleEdgeWeight
// expects).
float radiusForTheta(float theta)
{
    float tref = max(P.seam.w, 1e-3);
    float ref = max(meiRadial(tref, P.meiA.x, P.kd0) * accessoryScale(tref), 1e-9);
    return meiRadial(theta, P.meiA.x, P.kd0) * accessoryScale(theta) / ref;
}

// world dir -> (u, v, theta); theta < 0 signals "reject".
vec3 project(int i, vec3 world)
{
    vec3 d;
    if (i == 0)
        d = normalize(mul3(P.w2l0[0], P.w2l0[1], P.w2l0[2], world));
    else
        d = normalize(mul3(P.w2l1[0], P.w2l1[1], P.w2l1[2], world));

    float theta = acos(clamp(d.z, -1.0, 1.0));
    if (theta > P.seam.z)
        return vec3(0.0, 0.0, -1.0);
    float cx = (i == 0) ? P.cxcy.x : P.cxcy.z;
    float cy = (i == 0) ? P.cxcy.y : P.cxcy.w;
    float xi = (i == 0) ? P.meiA.x : P.meiA.y;
    float fx = (i == 0) ? P.meiA.z : P.meiA.w;
    float fy = (i == 0) ? P.meiB.x : P.meiB.y;
    vec4 k = (i == 0) ? P.kd0 : P.kd1;
    vec4 pd = (i == 0) ? P.pd0 : P.pd1;
    vec2 s34 = (i == 0) ? P.sd.xy : P.sd.zw;
    float denom = d.z + xi;
    if (denom < 1e-6)
        return vec3(0.0, 0.0, -1.0);
    // Calibration image axes (u right, v down): our lens frame has both
    // flipped. See insv::LensProjection.
    float x = -d.x / denom, y = -d.y / denom;
    float r2 = x * x + y * y, r4 = r2 * r2;
    float radial = 1.0 + k.x * r2 + k.y * r4 + k.z * r2 * r4 + k.w * r4 * r4;
    float xd = x * radial + 2.0 * pd.x * x * y + pd.y * (r2 + 2.0 * x * x) + pd.z * r2 + pd.w * r4;
    float yd = y * radial + pd.x * (r2 + 2.0 * y * y) + 2.0 * pd.y * x * y + s34.x * r2 + s34.y * r4;
    float acc = accessoryScale(theta);
    return vec3(cx + fx * xd * acc, cy + fy * yd * acc, theta);
}

// Rotate v by -|r| about r, i.e. apply exp(r)^T.
vec3 rotateBack(vec3 v, vec3 r)
{
    float a = length(r);
    if (a < 1e-9)
        return v;
    vec3 ax = -r / a;
    return v * cos(a) + cross(ax, v) * sin(a) + ax * dot(ax, v) * (1.0 - cos(a));
}

// project() with the capture-time correction: lens i's row at fraction s of
// the sensor was captured when the camera had turned by R = exp(r(s)) from the
// frame time (r interpolated from lens i's rsRot taps, tap k at s =
// k/(RS_TAPS-1)), so the scene direction sat at R^T * world in the frame-time
// body frame. The row comes from a first projection with the lens's centre
// tap; one fixed-point step (the rotations are well under a degree).
vec3 projectRS(int i, vec3 world)
{
    if (P.rs.x == 0.0)
        return project(i, world);
    int base = i * RS_TAPS;
    vec3 pr = project(i, rotateBack(world, P.rsRot[base + RS_TAPS / 2].xyz));
    if (pr.z < 0.0)
        return pr;
    float s = (P.rs.y != 0.0) ? pr.x / float(P.dims.x) : pr.y / float(P.dims.y);
    float f = clamp(s, 0.0, 1.0) * float(RS_TAPS - 1);
    int k = min(int(f), RS_TAPS - 2);
    vec3 r = mix(P.rsRot[base + k].xyz, P.rsRot[base + k + 1].xyz, f - float(k));
    return project(i, rotateBack(world, r));
}

float seamWeight(float theta)
{
    float seamRad = P.seam.x;
    float bandRad = P.seam.y;
    if (theta <= seamRad - bandRad)
        return 1.0;
    if (theta >= seamRad + bandRad)
        return 0.0;
    float t = (theta - (seamRad - bandRad)) / (2.0 * bandRad);
    return 0.5 * (1.0 + cos(PI * t));
}

// Fade a lens out as its sample radius nears the imaged-circle edge (r/radius),
// so near-pole wide-angle rays don't pull in the black/vignette ring.
float circleEdgeWeight(float rr)
{
    float s = P.edge.x, e = P.edge.y;
    if (rr <= s)
        return 1.0;
    if (rr >= e || e <= s)
        return (rr >= e) ? 0.0 : 1.0;
    float t = (rr - s) / (e - s);
    return 0.5 * (1.0 + cos(PI * t));
}

float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

// Equirect texel (x, y) of a W x H equirect -> stitch-world direction.
vec3 eqTexelDir(ivec2 t, int W, int H)
{
    float lat = (0.5 - (float(t.y) + 0.5) / float(H)) * PI;
    float lon = ((float(t.x) + 0.5) / float(W) - 0.5) * 2.0 * PI;
    float cl = cos(lat), sl = sin(lat);
    return mul3(P.outRot[0], P.outRot[1], P.outRot[2], vec3(cl * sin(lon), sl, -cl * cos(lon)));
}

// --- depth-aware seam (insv::FisheyeStitcher::seamQAt / lensRay) ----------

float seamQBin(int b) { return P.seamQ[b >> 2][b & 3]; }

// Inverse stitch distance for stitch-world direction `dir`: the per-bin table,
// linear in azimuth, faded to 0 over the ramp past the blend band.
float seamQAt(vec3 dir)
{
    if (P.seamA.w == 0.0)
        return 0.0;
    float e = abs(asin(clamp(dot(dir, P.seamA.xyz), -1.0, 1.0)));
    float band = P.seam.y, ramp = P.seamV.w;
    if (e >= band + ramp)
        return 0.0;
    float r = e > band ? 0.5 * (1.0 + cos(PI * (e - band) / ramp)) : 1.0;
    float phi = atan(dot(dir, P.seamV.xyz), dot(dir, P.seamU.xyz));
    if (phi < 0.0)
        phi += 2.0 * PI;
    float f = phi / (2.0 * PI) * float(SEAM_BINS) - 0.5;
    float fl = floor(f);
    int i0 = (int(fl) + SEAM_BINS) % SEAM_BINS;
    int i1 = (i0 + 1) % SEAM_BINS;
    return r * mix(seamQBin(i0), seamQBin(i1), f - fl);
}

// The direction lens i sees the point at distance 1/q along `dir` in.
vec3 lensRay(int i, vec3 dir, float q)
{
    if (q == 0.0)
        return dir;
    float h = i == 0 ? P.seamU.w : -P.seamU.w;
    return normalize(dir - (q * h) * P.seamA.xyz);
}

// Seam feather of lens i for `dir`, from dir's own off-axis angle whatever
// the depth (so the blend stays put when the rays shift).
float seamBlendWeight(int i, vec3 dir)
{
    vec3 z = i == 0 ? P.w2l0[2].xyz : P.w2l1[2].xyz; // the lens's optical axis
    return seamWeight(acos(clamp(dot(z, dir), -1.0, 1.0)));
}

// Per-lens sample uv + blend weight for stitch-world direction `dir` (a
// weight of 0 means that lens doesn't contribute).
void lensWeights(vec3 dir, out vec2 uv0, out vec2 uv1, out float w0, out float w1)
{
    float fw = float(P.dims.x), fh = float(P.dims.y);
    uv0 = vec2(0.0);
    uv1 = vec2(0.0);
    w0 = 0.0;
    w1 = 0.0;
    float q = seamQAt(dir);
    for (int i = 0; i < 2; ++i) {
        float ws = seamBlendWeight(i, dir);
        if (ws <= 0.0)
            continue;
        vec3 pr = projectRS(i, lensRay(i, dir, q));
        if (pr.z < 0.0)
            continue;
        float u = pr.x, v = pr.y;
        if (u < 0.0 || v < 0.0 || u > fw - 1.0 || v > fh - 1.0)
            continue;
        float wt = ws * circleEdgeWeight(radiusForTheta(pr.z));
        if (wt <= 0.0)
            continue;
        vec2 uv = (vec2(u, v) + 0.5) / vec2(fw, fh);
        if (i == 0) { uv0 = uv; w0 = wt; }
        else        { uv1 = uv; w1 = wt; }
    }
}

#ifndef FISHEYE_STITCH_NO_BUFFERS
// Measure pass body for one direction: where both lenses are solidly present,
// add their luma to the exposure match.
void measureAt(vec3 dir)
{
    vec2 uv0, uv1;
    float w0, w1;
    lensWeights(dir, uv0, uv1, w0, w1);
    if (w0 > 0.25 && w1 > 0.25) {
        atomicAdd(gsum[0], uint(luma(lensColor(0, uv0)) * GAIN_FP));
        atomicAdd(gsum[1], uint(luma(lensColor(1, uv1)) * GAIN_FP));
        atomicAdd(gsum[2], 1u);
    }
}

// Photometric gain: pull each lens toward the geometric-mean brightness.
void photoGains(out float g0, out float g1)
{
    g0 = 1.0;
    g1 = 1.0;
    if (int(P.edge.z) != 0 && gsum[2] > 64u) {
        float m0 = float(gsum[0]) / (GAIN_FP * float(gsum[2]));
        float m1 = float(gsum[1]) / (GAIN_FP * float(gsum[2]));
        if (m0 > 1.0 && m1 > 1.0) {
            float r = sqrt(m1 / m0);
            g0 = clamp(r, 0.8, 1.25);
            g1 = clamp(1.0 / r, 0.8, 1.25);
        }
    }
}

// Pass 2: the depth-aware seam's cost volume. Workgroup (bin, k) of 64
// invocations samples the bin's SEAM_PATCH x SEAM_PATCH patch (4 samples
// each) from both lenses, each from its own centre at candidate distance k,
// and writes the grey NCC + texture -- insv::MeasureSeamCosts on the GPU
// (same sample pattern, insv::SeamPatchSample).
shared float seamRed[6][64];

void seamCostPass(uvec3 wg, uint lid)
{
    int bin = int(wg.x), k = int(wg.y);
    if (bin >= SEAM_BINS || k >= SEAM_CANDS)
        return;
    float q = mix(P.seamC.x, P.seamC.y, float(k) / float(SEAM_CANDS - 1));
    float fw = float(P.dims.x), fh = float(P.dims.y);
    float sx = 0.0, sy = 0.0, sxx = 0.0, syy = 0.0, sxy = 0.0, n = 0.0;
    for (int j = 0; j < SEAM_PATCH * SEAM_PATCH / 64; ++j) {
        int s = int(lid) + 64 * j;
        int az = s % SEAM_PATCH, el = s / SEAM_PATCH;
        float phi = (float(bin) + (float(az) + 0.5) / float(SEAM_PATCH)) * (2.0 * PI / float(SEAM_BINS));
        float e = (-1.0 + (2.0 * float(el) + 1.0) / float(SEAM_PATCH)) * P.seamC.z;
        vec3 dir = cos(e) * (cos(phi) * P.seamU.xyz + sin(phi) * P.seamV.xyz) + sin(e) * P.seamA.xyz;
        vec3 p0 = projectRS(0, lensRay(0, dir, q));
        vec3 p1 = projectRS(1, lensRay(1, dir, q));
        if (p0.z < 0.0 || p1.z < 0.0)
            continue;
        if (p0.x < 0.0 || p0.y < 0.0 || p0.x > fw - 1.0 || p0.y > fh - 1.0 ||
            p1.x < 0.0 || p1.y < 0.0 || p1.x > fw - 1.0 || p1.y > fh - 1.0)
            continue;
        float x = luma(lensColor(0, (p0.xy + 0.5) / vec2(fw, fh)));
        float y = luma(lensColor(1, (p1.xy + 0.5) / vec2(fw, fh)));
        sx += x; sy += y; sxx += x * x; syy += y * y; sxy += x * y; n += 1.0;
    }
    seamRed[0][lid] = sx;
    seamRed[1][lid] = sy;
    seamRed[2][lid] = sxx;
    seamRed[3][lid] = syy;
    seamRed[4][lid] = sxy;
    seamRed[5][lid] = n;
    barrier();
    for (uint stride = 32u; stride > 0u; stride >>= 1) {
        if (lid < stride)
            for (int c = 0; c < 6; ++c)
                seamRed[c][lid] += seamRed[c][lid + stride];
        barrier();
    }
    if (lid != 0u)
        return;
    n = seamRed[5][0];
    vec2 res = vec2(-2.0, 0.0);
    if (n >= float(SEAM_PATCH * SEAM_PATCH / 4)) {
        float mx = seamRed[0][0] / n, my = seamRed[1][0] / n;
        float vx = max(seamRed[2][0] / n - mx * mx, 0.0);
        float vy = max(seamRed[3][0] / n - my * my, 0.0);
        float cxy = seamRed[4][0] / n - mx * my;
        res.y = sqrt(min(vx, vy));
        res.x = (vx > 1e-6 && vy > 1e-6) ? cxy / sqrt(vx * vy) : 0.0;
    }
    seamCost[bin * SEAM_CANDS + k] = res;
}
#endif

// The stitched colour for `dir`, RGB in [0, 255] (unquantised); black outside
// both imaged circles.
vec3 blendLenses(vec3 dir, float g0, float g1)
{
    vec2 uv0, uv1;
    float w0, w1;
    lensWeights(dir, uv0, uv1, w0, w1);
    vec3 acc = vec3(0.0);
    float wsum = 0.0;
    if (w0 > 0.0) { acc += w0 * g0 * lensColor(0, uv0); wsum += w0; }
    if (w1 > 0.0) { acc += w1 * g1 * lensColor(1, uv1); wsum += w1; }
    return wsum > 1e-6 ? acc / wsum : vec3(0.0);
}

// Privacy blur (insv/BlurQuads.h, mirrored there and in
// FisheyeStitcher::renderEquirect): inside a blur quad the colour is the
// quad's mosaic cell -- the mean of BLUR_TAPS^2 directions spread over the
// cell -- faded into the picture over the outer BLUR_FEATHER of (s, t).
float cross2(vec2 a, vec2 b) { return a.x * b.y - a.y * b.x; }

// insv::blurQuadST: where `dir` falls in quad k, (s, t) in [0, 1]^2 inside.
bool blurST(int k, vec3 dir, out vec2 st)
{
    st = vec2(-1.0);
    vec4 A = P.blurQ[k * 6], B = P.blurQ[k * 6 + 1], C = P.blurQ[k * 6 + 2];
    float z = dot(dir, A.xyz);
    if (z < C.w || z <= 0.0)
        return false;
    vec4 D = P.blurQ[k * 6 + 3], E = P.blurQ[k * 6 + 4];
    float scale = P.blurQ[k * 6 + 5].x;
    vec2 p = vec2(dot(dir, B.xyz), dot(dir, C.xyz)) / (z * scale);
    vec2 a = D.xy;
    vec2 e = D.zw - a, f = E.zw - a, g = a - D.zw + E.xy - E.zw, h = p - a;
    float k2 = cross2(g, f);
    float k1 = cross2(e, f) + cross2(h, g);
    float k0 = cross2(h, e);
    float t0, t1;
    if (abs(k2) < 1e-7) {
        if (abs(k1) < 1e-12)
            return false;
        t0 = t1 = -k0 / k1;
    } else {
        float w = k1 * k1 - 4.0 * k0 * k2;
        if (w < 0.0)
            return false;
        w = sqrt(w);
        t0 = (-k1 - w) * 0.5 / k2;
        t1 = (-k1 + w) * 0.5 / k2;
    }
    for (int i = 0; i < 2; ++i) {
        float t = i == 0 ? t0 : t1;
        vec2 dd = e + g * t;
        float s = abs(dd.x) > abs(dd.y) ? (h.x - f.x * t) / dd.x : (h.y - f.y * t) / dd.y;
        if (s >= 0.0 && s <= 1.0 && t >= 0.0 && t <= 1.0) {
            st = vec2(s, t);
            return true;
        }
    }
    return false;
}

// insv::blurQuadDir: (s, t) in quad k -> its direction.
vec3 blurDir(int k, vec2 st)
{
    vec4 A = P.blurQ[k * 6], B = P.blurQ[k * 6 + 1], C = P.blurQ[k * 6 + 2];
    vec4 D = P.blurQ[k * 6 + 3], E = P.blurQ[k * 6 + 4];
    float scale = P.blurQ[k * 6 + 5].x;
    vec2 top = mix(D.xy, D.zw, st.x), bot = mix(E.zw, E.xy, st.x);
    vec2 p = mix(top, bot, st.y) * scale;
    return normalize(A.xyz + B.xyz * p.x + C.xyz * p.y);
}

// blendLenses() with the privacy blur applied: what every render path calls.
vec3 blurredLenses(vec3 dir, float g0, float g1)
{
    vec3 rgb = blendLenses(dir, g0, g1);
    int n = int(P.blur0.x);
    for (int k = 0; k < n; ++k) {
        vec2 st;
        if (!blurST(k, dir, st))
            continue;
        vec2 cells = vec2(P.blurQ[k * 6].w, P.blurQ[k * 6 + 1].w);
        vec2 cell = min(floor(st * cells), cells - 1.0);
        vec3 mos = vec3(0.0);
        for (int i = 0; i < BLUR_TAPS; ++i)
            for (int j = 0; j < BLUR_TAPS; ++j)
                mos += blendLenses(blurDir(k, (cell + (vec2(i, j) + 0.5) / float(BLUR_TAPS)) / cells), g0, g1);
        mos /= float(BLUR_TAPS * BLUR_TAPS);
        float edge = min(min(st.x, 1.0 - st.x), min(st.y, 1.0 - st.y));
        return mix(rgb, mos, smoothstep(0.0, BLUR_FEATHER, edge));
    }
    return rgb;
}

#ifndef FISHEYE_STITCH_NO_MAIN
void main()
{
    if (pc.pass == 2) {
        seamCostPass(gl_WorkGroupID, gl_LocalInvocationIndex);
        return;
    }
    ivec2 gid = ivec2(gl_GlobalInvocationID.xy);
    int W = P.dims.z, H = P.dims.w;
    if (gid.x >= W || gid.y >= H)
        return;
    // Measure pass touches every 8th texel only.
    if (pc.pass == 0 && ((gid.x & 7) != 0 || (gid.y & 7) != 0))
        return;

    vec3 dir = eqTexelDir(gid, W, H);
    if (pc.pass == 0) {
        measureAt(dir);
        return;
    }

    float g0, g1;
    photoGains(g0, g1);
    uvec3 q = uvec3(clamp(blurredLenses(dir, g0, g1) + 0.5, vec3(0.0), vec3(255.0)));
    px[gid.y * W + gid.x] = q.r | (q.g << 8) | (q.b << 16) | (255u << 24);
}
#endif
