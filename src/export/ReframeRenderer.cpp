#include "ReframeRenderer.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <thread>
#include <vector>

namespace exportpipe {

namespace {

constexpr float kPi = std::numbers::pi_v<float>;

int wrapIndex(int v, int n)
{
    // Positive-result modulo, for the horizontal equirect seam.
    int m = v % n;
    return m < 0 ? m + n : m;
}

// Bilinear RGBA fetch: U wraps, V clamps. `out[4]` receives the sample.
void sampleEquirect(const uint8_t* eq, int w, int h, float u, float v, float out[4])
{
    const float fx = u * static_cast<float>(w) - 0.5f;
    const float fy = v * static_cast<float>(h) - 0.5f;
    const float x0f = std::floor(fx);
    const float y0f = std::floor(fy);
    const float tx = fx - x0f;
    const float ty = fy - y0f;
    const int x0 = static_cast<int>(x0f);
    const int y0 = static_cast<int>(y0f);

    const int xs[2] = {wrapIndex(x0, w), wrapIndex(x0 + 1, w)};
    const int ys[2] = {std::clamp(y0, 0, h - 1), std::clamp(y0 + 1, 0, h - 1)};

    for (int c = 0; c < 4; ++c) {
        const float p00 = eq[(static_cast<size_t>(ys[0]) * w + xs[0]) * 4 + c];
        const float p10 = eq[(static_cast<size_t>(ys[0]) * w + xs[1]) * 4 + c];
        const float p01 = eq[(static_cast<size_t>(ys[1]) * w + xs[0]) * 4 + c];
        const float p11 = eq[(static_cast<size_t>(ys[1]) * w + xs[1]) * 4 + c];
        const float top = p00 + (p10 - p00) * tx;
        const float bot = p01 + (p11 - p01) * tx;
        out[c] = top + (bot - top) * ty;
    }
}

// Colour grade, applied in [0,255] rgb space -- the C++ twin of
// src/render/shaders/color_grade.glsl's gradeColor() (which can't be included
// here): change both together, constants included. Order: the LUT (mixed
// with the input by lutMix), temperature, exposure, contrast, saturation.
// Leaves alpha untouched.
constexpr float kGradeTemperaturePush = 40.0f; // levels at temperature +-100%
constexpr float kGradeContrastPivot = 128.0f;
constexpr float kGradeWhite = 255.0f;

// A grade's factors, worked out once per picture rather than per pixel.
struct Grade {
    float t, ef, cf, sf;
    const color::Lut3D* lut; // nullptr = none
    float lutMix;
    explicit Grade(const color::Grade& g)
        : t(g.adjust[3] / 100.0f), ef(std::pow(2.0f, g.adjust[0])),
          cf(1.0f + g.adjust[1] / 100.0f), sf(1.0f + g.adjust[2] / 100.0f),
          lut(g.hasLut() ? g.lut.get() : nullptr), lutMix(std::min(g.lutMix, 1.0f))
    {
    }
};

inline void gradeRgb(float rgba[4], const Grade &g)
{
    if (g.lut) {
        float in[3], out[3];
        for (int c = 0; c < 3; ++c)
            in[c] = std::clamp(rgba[c] / kGradeWhite, 0.0f, 1.0f);
        g.lut->apply(in, out);
        for (int c = 0; c < 3; ++c)
            rgba[c] += (std::clamp(out[c], 0.0f, 1.0f) * kGradeWhite - rgba[c]) * g.lutMix;
    }
    float r = rgba[0] + g.t * kGradeTemperaturePush;
    float gr = rgba[1];
    float b = rgba[2] - g.t * kGradeTemperaturePush;

    const float ef = g.ef;
    r *= ef;
    gr *= ef;
    b *= ef;

    const float cf = g.cf;
    r = (r - kGradeContrastPivot) * cf + kGradeContrastPivot;
    gr = (gr - kGradeContrastPivot) * cf + kGradeContrastPivot;
    b = (b - kGradeContrastPivot) * cf + kGradeContrastPivot;

    const float luma = 0.299f * r + 0.587f * gr + 0.114f * b;
    const float sf = g.sf;
    r = luma + (r - luma) * sf;
    gr = luma + (gr - luma) * sf;
    b = luma + (b - luma) * sf;

    rgba[0] = std::clamp(r, 0.0f, kGradeWhite);
    rgba[1] = std::clamp(gr, 0.0f, kGradeWhite);
    rgba[2] = std::clamp(b, 0.0f, kGradeWhite);
}

void renderRows(const uint8_t* eq, int eqW, int eqH, uint8_t* out, int outW, int outH,
                const ReframeView& view, int rowBegin, int rowEnd)
{
    const Grade grade(view.colorGrade);
    const float tanHalfFovY = std::tan(view.fovY * 0.5f);
    const float cosR = std::cos(view.roll);
    const float sinR = std::sin(view.roll);
    const float cosP = std::cos(view.pitch);
    const float sinP = std::sin(view.pitch);
    const float cosY = std::cos(view.yaw);
    const float sinY = std::sin(view.yaw);

    for (int py = rowBegin; py < rowEnd; ++py) {
        const float ndcY = (static_cast<float>(py) + 0.5f) / static_cast<float>(outH) * 2.0f - 1.0f;
        for (int px = 0; px < outW; ++px) {
            const float ndcX =
                (static_cast<float>(px) + 0.5f) / static_cast<float>(outW) * 2.0f - 1.0f;

            // View-space ray (matches equirect.frag: +Y up, -Z forward).
            float vx = ndcX * tanHalfFovY * view.aspect;
            float vy = -ndcY * tanHalfFovY;
            float vz = -1.0f;
            const float invLen = 1.0f / std::sqrt(vx * vx + vy * vy + vz * vz);
            vx *= invLen;
            vy *= invLen;
            vz *= invLen;

            // Roll about Z (view axis), then pitch about X, then yaw about Y.
            const float rx = vx * cosR - vy * sinR;
            const float ry = vx * sinR + vy * cosR;
            const float px_ = rx;
            const float py_ = ry * cosP - vz * sinP;
            const float pz_ = ry * sinP + vz * cosP;
            const float qx = px_ * cosY + pz_ * sinY;
            const float qy = py_;
            const float qz = -px_ * sinY + pz_ * cosY;
            // Stabilization in the sphere frame, after the pose.
            const float* m = view.stab;
            const float wx = m[0] * qx + m[1] * qy + m[2] * qz;
            const float wy = m[3] * qx + m[4] * qy + m[5] * qz;
            const float wz = m[6] * qx + m[7] * qy + m[8] * qz;

            const float longitude = std::atan2(wx, -wz);          // [-pi, pi]
            const float latitude = std::asin(std::clamp(wy, -1.0f, 1.0f)); // [-pi/2, pi/2]
            const float u = longitude / (2.0f * kPi) + 0.5f;
            const float v = 0.5f - latitude / kPi;

            float rgba[4];
            sampleEquirect(eq, eqW, eqH, u, v, rgba);
            gradeRgb(rgba, grade);

            uint8_t* dst = out + (static_cast<size_t>(py) * outW + px) * 4;
            for (int c = 0; c < 4; ++c)
                dst[c] = static_cast<uint8_t>(std::clamp(rgba[c] + 0.5f, 0.0f, 255.0f));
        }
    }
}

} // namespace

void RenderRectilinear(const uint8_t* eqRgba, int eqW, int eqH, uint8_t* outRgba, int outW, int outH,
                       const ReframeView& view)
{
    if (eqW <= 0 || eqH <= 0 || outW <= 0 || outH <= 0)
        return;

    unsigned hw = std::thread::hardware_concurrency();
    int workers = static_cast<int>(hw == 0 ? 4u : hw);
    workers = std::clamp(workers, 1, outH);
    // Small frames aren't worth the thread spin-up.
    if (outH < 64)
        workers = 1;

    if (workers == 1) {
        renderRows(eqRgba, eqW, eqH, outRgba, outW, outH, view, 0, outH);
        return;
    }

    std::vector<std::thread> pool;
    pool.reserve(static_cast<size_t>(workers - 1));
    const int band = (outH + workers - 1) / workers;
    for (int w = 0; w < workers; ++w) {
        const int begin = w * band;
        const int end = std::min(begin + band, outH);
        if (begin >= end)
            break;
        if (w == workers - 1) {
            renderRows(eqRgba, eqW, eqH, outRgba, outW, outH, view, begin, end);
        } else {
            pool.emplace_back(renderRows, eqRgba, eqW, eqH, outRgba, outW, outH, view, begin, end);
        }
    }
    for (auto& t : pool)
        t.join();
}

void GradeRgba(uint8_t* rgba, size_t pixels, const color::Grade& grade)
{
    if (grade.isNeutral())
        return;
    const Grade g(grade);
    auto run = [rgba, &g](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            uint8_t* p = rgba + i * 4;
            float c[4] = {float(p[0]), float(p[1]), float(p[2]), float(p[3])};
            gradeRgb(c, g);
            // lround(), exactly (graded values are >= 0), without the call.
            for (int k = 0; k < 3; ++k)
                p[k] = static_cast<uint8_t>(static_cast<double>(c[k]) + 0.5);
        }
    };
    // Across the cores: a flat clip's every frame goes through here.
    const unsigned hw = std::thread::hardware_concurrency();
    const size_t workers = pixels < 64 * 1024 ? 1 : std::max(1u, hw == 0 ? 4u : hw);
    const size_t band = (pixels + workers - 1) / workers;
    std::vector<std::thread> pool;
    for (size_t b = band; b < pixels; b += band)
        pool.emplace_back(run, b, std::min(b + band, pixels));
    run(0, std::min(band, pixels));
    for (std::thread& t : pool)
        t.join();
}

} // namespace exportpipe
