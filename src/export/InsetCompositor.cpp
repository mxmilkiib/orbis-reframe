#include "InsetCompositor.h"

#include "FlatVideoReader.h"
#include "ReframeRenderer.h" // GradeRgba

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>

namespace exportpipe {

namespace {

// BT.709 limited range, the export's encoding.
constexpr float kKr = 0.2126f, kKb = 0.0722f, kKg = 1.0f - kKr - kKb;

struct Yuv {
    float y, u, v;
};

Yuv toYuv(float r, float g, float b) // [0,1] rgb
{
    const float l = kKr * r + kKg * g + kKb * b;
    return {16.0f + 219.0f * l, 128.0f + 224.0f * (b - l) / (2.0f * (1.0f - kKb)),
            128.0f + 224.0f * (r - l) / (2.0f * (1.0f - kKr))};
}

uint8_t clamp8(float v)
{
    return static_cast<uint8_t>(std::clamp(std::lround(v), 0L, 255L));
}

// Bilinear sample of a premultiplied RGBA picture at pixel coordinates
// (texel centres at +0.5, edges clamped) -- what the viewport's linear
// sampler does. `out` is the straight colour; returns the alpha.
float sampleBilinear(const uint8_t *rgba, int w, int h, double fx, double fy, float out[3])
{
    fx = std::clamp(fx - 0.5, 0.0, double(w - 1));
    fy = std::clamp(fy - 0.5, 0.0, double(h - 1));
    const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
    const int x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
    const float tx = static_cast<float>(fx - x0), ty = static_cast<float>(fy - y0);
    const uint8_t *p00 = rgba + (size_t(y0) * w + x0) * 4, *p10 = rgba + (size_t(y0) * w + x1) * 4;
    const uint8_t *p01 = rgba + (size_t(y1) * w + x0) * 4, *p11 = rgba + (size_t(y1) * w + x1) * 4;
    float v[4];
    for (int c = 0; c < 4; ++c) {
        const float top = p00[c] + (p10[c] - p00[c]) * tx;
        const float bot = p01[c] + (p11[c] - p01[c]) * tx;
        v[c] = (top + (bot - top) * ty) / 255.0f;
    }
    const float a = v[3];
    for (int c = 0; c < 3; ++c)
        out[c] = a >= 1.0f ? v[c] : a > 0.0f ? std::min(1.0f, v[c] / a) : 0.0f;
    return a;
}

// fn(begin, end) over rows [y0, y1) in bands of whole `step`s, across the
// cores; on this thread alone for a small area.
template <typename Fn>
void parallelRows(int y0, int y1, int step, int64_t pixels, Fn fn)
{
    const unsigned hw = std::thread::hardware_concurrency();
    int workers = static_cast<int>(hw == 0 ? 4u : hw);
    if (pixels < 64 * 1024 || y1 - y0 < 2 * step)
        workers = 1;
    const int steps = (y1 - y0 + step - 1) / step;
    workers = std::clamp(workers, 1, std::max(1, steps));
    if (workers == 1) {
        fn(y0, y1);
        return;
    }
    const int band = (steps + workers - 1) / workers * step;
    std::vector<std::thread> pool;
    for (int b = y0 + band; b < y1; b += band)
        pool.emplace_back(fn, b, std::min(b + band, y1));
    fn(y0, std::min(y0 + band, y1));
    for (std::thread &t : pool)
        t.join();
}

// Whether `r` at `opacity` covers its pixels exactly as a copy of a picture
// of its own size would: opaque, square corners, no border, on whole pixels
// -- even ones, for NV12's chroma, in an output of even size. inset::shade
// then gives every pixel inside full coverage and every one outside none.
bool copyable(const inset::Rect &r, float opacity, int outW, int outH)
{
    const auto even = [](double v) {
        return std::abs(v - std::round(v)) < 1e-3 && (std::llround(v) & 1) == 0;
    };
    return opacity >= 1.0f && r.radius <= 0.0 && r.border <= 0.0 && !((outW | outH) & 1) &&
           even(r.x) && even(r.y) && even(r.w) && even(r.h);
}

// Copy an NV12 picture w x h (even) into the output at (rx, ry) (even),
// cropped to it.
void copyNv12(const uint8_t *pic, int w, int h, int rx, int ry, uint8_t *yPlane, int yStride,
              uint8_t *uvPlane, int uvStride, int outW, int outH)
{
    const int x0 = std::max(0, rx), x1 = std::min(outW, rx + w);
    const int y0 = std::max(0, ry), y1 = std::min(outH, ry + h);
    if (x0 >= x1 || y0 >= y1)
        return;
    const uint8_t *picUv = pic + size_t(w) * h;
    parallelRows(y0, y1, 2, int64_t(x1 - x0) * (y1 - y0), [&](int b0, int b1) {
        for (int py = b0; py < b1; ++py)
            std::memcpy(yPlane + size_t(py) * yStride + x0, pic + size_t(py - ry) * w + (x0 - rx),
                        size_t(x1 - x0));
        for (int cy = b0 / 2; cy < b1 / 2; ++cy)
            std::memcpy(uvPlane + size_t(cy) * uvStride + x0,
                        picUv + size_t(cy - ry / 2) * w + (x0 - rx), size_t(x1 - x0));
    });
}

} // namespace

struct InsetCompositor::State {
    std::unique_ptr<FlatVideoReader> reader;
    std::unique_ptr<PictureSource> picture;
    bool failed = false;
    // The decoded frame graded (InsetClipJob::colorGrade), when it has one
    // (Nv12Converter::kPadding longer), and that as NV12 when it's copied.
    std::vector<uint8_t> graded;
    const VideoFrame *gradedFrom = nullptr;
    int64_t gradedPts = -1;
    std::vector<uint8_t> gradedNv12;
    bool gradedNv12Done = false;
    Nv12Converter toNv12;
};

InsetCompositor::InsetCompositor(std::vector<InsetClipJob> jobs, int outW, int outH)
    : m_jobs(std::move(jobs)), m_outW(outW), m_outH(outH)
{
    for (size_t i = 0; i < m_jobs.size(); ++i)
        m_states.push_back(std::make_unique<State>());
}

InsetCompositor::~InsetCompositor() = default;

void InsetCompositor::blendNv12(const uint8_t *rgba, int picW, int picH, const inset::Rect &r,
                                float opacity, uint32_t borderRgb, uint8_t *yPlane, int yStride,
                                uint8_t *uvPlane, int uvStride, int outW, int outH)
{
    if (r.empty() || opacity <= 0.0f || picW <= 0 || picH <= 0)
        return;
    const Yuv border = toYuv(((borderRgb >> 16) & 0xff) / 255.0f, ((borderRgb >> 8) & 0xff) / 255.0f,
                             (borderRgb & 0xff) / 255.0f);
    // Chroma blocks (2x2 luma) touching the rect.
    const int x0 = std::max(0, static_cast<int>(std::floor(r.x)) & ~1);
    const int y0 = std::max(0, static_cast<int>(std::floor(r.y)) & ~1);
    const int x1 = std::min(outW, static_cast<int>(std::ceil(r.x + r.w)) + 1);
    const int y1 = std::min(outH, static_cast<int>(std::ceil(r.y + r.h)) + 1);
    const double sx = picW / r.w, sy = picH / r.h;
    parallelRows(y0, y1, 2, int64_t(x1 - x0) * (y1 - y0), [&](int b0, int b1) {
        for (int by = b0; by < b1; by += 2) {
            for (int bx = x0; bx < x1; bx += 2) {
                float aSum = 0.0f, uSum = 0.0f, vSum = 0.0f;
                for (int dy = 0; dy < 2; ++dy) {
                    const int py = by + dy;
                    if (py >= outH)
                        continue;
                    for (int dx = 0; dx < 2; ++dx) {
                        const int px = bx + dx;
                        if (px >= outW)
                            continue;
                        const inset::Coverage c = inset::shade(px + 0.5, py + 0.5, r);
                        float a = c.alpha * opacity;
                        if (a <= 0.0f)
                            continue;
                        // The picture fills the rect.
                        float rgb[3];
                        // The border covers whatever the picture's alpha.
                        const float pa = sampleBilinear(rgba, picW, picH, (px + 0.5 - r.x) * sx,
                                                        (py + 0.5 - r.y) * sy, rgb);
                        a *= pa + (1.0f - pa) * c.border;
                        if (a <= 0.0f)
                            continue;
                        Yuv col = toYuv(rgb[0], rgb[1], rgb[2]);
                        if (c.border > 0.0f) {
                            col.y += (border.y - col.y) * c.border;
                            col.u += (border.u - col.u) * c.border;
                            col.v += (border.v - col.v) * c.border;
                        }
                        uint8_t &yv = yPlane[size_t(py) * yStride + px];
                        yv = clamp8(yv + (col.y - yv) * a);
                        aSum += a;
                        uSum += a * col.u;
                        vSum += a * col.v;
                    }
                }
                if (aSum <= 0.0f)
                    continue;
                // The block's chroma: the average coverage, premultiplied.
                const float a = aSum / 4.0f;
                uint8_t *uv = uvPlane + size_t(by / 2) * uvStride + bx;
                uv[0] = clamp8(uv[0] * (1.0f - a) + uSum / 4.0f);
                uv[1] = clamp8(uv[1] * (1.0f - a) + vSum / 4.0f);
            }
        }
    });
}

bool InsetCompositor::layerAt(size_t i, int64_t timelineMs, bool nv12, Layer &layer,
                              std::string &err)
{
    const InsetClipJob &j = m_jobs[i];
    State &s = *m_states[i];
    if (timelineMs >= j.endMs()) {
        s.reader.reset(); // past it: free the decoder
        s.picture.reset();
        return false;
    }
    if (timelineMs < j.startMs || s.failed)
        return false;
    const int64_t localMs = timelineMs - j.startMs;
    const int64_t srcMs = j.trimInMs + static_cast<int64_t>(std::llround(localMs * j.speed));
    inset::Look look = j.lookAt ? j.lookAt(srcMs) : inset::Look{};
    look.opacity *= inset::fadeGain(localMs, j.spanMs(), j.fadeInMs, j.fadeOutMs) *
                    inset::rampGain(localMs, j.spanMs(), j.rampFrom, j.rampTo);
    const inset::Rect rect = inset::place(look, j.frame, j.aspect, m_outW, m_outH);
    if (rect.empty() || look.opacity <= 0.0f)
        return false;
    if (j.still) { // a title: blended, never copied
        layer = {j.still->data(), nullptr, j.stillW, j.stillH, rect, look.opacity, 0};
        return true;
    }
    if (j.picture) { // a 360 clip's view, rendered at the rect's size
        if (!s.picture) {
            s.picture = j.picture();
            if (!s.picture || !s.picture->open(err)) {
                err = j.path + ": " + (err.empty() ? std::string("can't be read") : err);
                s.picture.reset();
                s.failed = true;
                return false;
            }
        }
        s.picture->setOutputSize(std::max(4, static_cast<int>(std::lround(rect.w)) & ~3),
                                 std::max(2, static_cast<int>(std::lround(rect.h)) & ~1));
        const VideoFrame *f = s.picture->frameAt(srcMs);
        if (!f) {
            if (!s.picture->lastError().empty())
                err = j.path + ": " + s.picture->lastError();
            return false;
        }
        layer = {f->rgba.data(), nullptr, f->width, f->height, rect, look.opacity, j.frame.borderRgb};
        return true;
    }
    if (!s.reader) {
        s.reader = std::make_unique<FlatVideoReader>();
        if (!s.reader->open(j.path, err)) {
            s.failed = true;
            return false;
        }
    }
    // A full-frame flat clip (or an inset like one) is copied into the
    // output as NV12, decoded straight to it at the rect's size; anything
    // else is blended from RGBA.
    const bool copy = nv12 && copyable(rect, look.opacity, m_outW, m_outH);
    const bool graded = !j.colorGrade.isNeutral();
    s.reader->setOutputFormat(copy && !graded ? FlatVideoReader::Format::Nv12
                                              : FlatVideoReader::Format::Rgba);
    if (copy) {
        s.reader->setOutputSize(static_cast<int>(std::lround(rect.w)),
                                static_cast<int>(std::lround(rect.h)));
    } else {
        // Decode at the rect's size (at most the source's): sampling is then
        // ~1:1 -- no aliasing from a big source shrunk by a bilinear tap.
        const auto &info = s.reader->info();
        s.reader->setOutputSize(
            std::min(info.width, std::max(2, static_cast<int>(std::lround(rect.w)))),
            std::min(info.height, std::max(2, static_cast<int>(std::lround(rect.h)))));
    }
    const VideoFrame *f = s.reader->frameAt(srcMs);
    if (!f) {
        if (!s.reader->lastError().empty())
            err = j.path + ": " + s.reader->lastError();
        return false;
    }
    layer = {nullptr, nullptr, f->width, f->height, rect, look.opacity, j.frame.borderRgb};
    if (copy && !graded) {
        layer.nv12 = f->nv12.data();
        return true;
    }
    const uint8_t *rgba = f->rgba.data();
    if (graded) {
        const size_t bytes = size_t(f->width) * f->height * 4;
        if (s.gradedFrom != f || s.gradedPts != f->ptsMs ||
            s.graded.size() != bytes + Nv12Converter::kPadding) {
            s.graded.resize(bytes + Nv12Converter::kPadding);
            std::memcpy(s.graded.data(), f->rgba.data(), bytes);
            GradeRgba(s.graded.data(), size_t(f->width) * f->height, j.colorGrade);
            s.gradedFrom = f;
            s.gradedPts = f->ptsMs;
            s.gradedNv12Done = false;
        }
        rgba = s.graded.data();
        if (copy) {
            if (!s.gradedNv12Done && !s.toNv12.convert(rgba, f->width, f->height, s.gradedNv12)) {
                err = j.path + ": can't convert the graded picture";
                return false;
            }
            s.gradedNv12Done = true;
            layer.nv12 = s.gradedNv12.data();
            return true;
        }
    }
    layer.rgba = rgba;
    return true;
}

bool InsetCompositor::composite(int64_t timelineMs, uint8_t *y, int yStride, uint8_t *uv,
                                int uvStride, std::string &err)
{
    for (size_t i = 0; i < m_jobs.size(); ++i) {
        Layer l;
        if (!layerAt(i, timelineMs, true, l, err)) {
            if (!err.empty())
                return false;
        } else if (l.nv12) {
            copyNv12(l.nv12, l.w, l.h, static_cast<int>(std::lround(l.rect.x)),
                     static_cast<int>(std::lround(l.rect.y)), y, yStride, uv, uvStride, m_outW,
                     m_outH);
        } else {
            blendNv12(l.rgba, l.w, l.h, l.rect, l.opacity, l.borderRgb, y, yStride, uv, uvStride,
                      m_outW, m_outH);
        }
    }
    return true;
}

bool InsetCompositor::compositeRgb(int64_t timelineMs, uint8_t *rgb, int stride,
                                   int bytesPerPixel, std::string &err)
{
    for (size_t i = 0; i < m_jobs.size(); ++i) {
        Layer l;
        if (layerAt(i, timelineMs, false, l, err))
            blendRgb(l.rgba, l.w, l.h, l.rect, l.opacity, l.borderRgb, rgb, stride, bytesPerPixel,
                     m_outW, m_outH);
        else if (!err.empty())
            return false;
    }
    return true;
}

void InsetCompositor::blendRgb(const uint8_t *rgba, int picW, int picH, const inset::Rect &r,
                               float opacity, uint32_t borderRgb, uint8_t *out, int stride,
                               int bpp, int outW, int outH)
{
    if (r.empty() || opacity <= 0.0f || picW <= 0 || picH <= 0)
        return;
    const float border[3] = {((borderRgb >> 16) & 0xff) / 255.0f, ((borderRgb >> 8) & 0xff) / 255.0f,
                             (borderRgb & 0xff) / 255.0f};
    const int x0 = std::max(0, static_cast<int>(std::floor(r.x)));
    const int y0 = std::max(0, static_cast<int>(std::floor(r.y)));
    const int x1 = std::min(outW, static_cast<int>(std::ceil(r.x + r.w)) + 1);
    const int y1 = std::min(outH, static_cast<int>(std::ceil(r.y + r.h)) + 1);
    const double sx = picW / r.w, sy = picH / r.h;
    parallelRows(y0, y1, 1, int64_t(x1 - x0) * (y1 - y0), [&](int b0, int b1) {
        for (int py = b0; py < b1; ++py) {
            uint8_t *row = out + size_t(py) * stride;
            for (int px = x0; px < x1; ++px) {
                const inset::Coverage c = inset::shade(px + 0.5, py + 0.5, r);
                float a = c.alpha * opacity;
                if (a <= 0.0f)
                    continue;
                float col[3];
                const float pa = sampleBilinear(rgba, picW, picH, (px + 0.5 - r.x) * sx,
                                                (py + 0.5 - r.y) * sy, col);
                a *= pa + (1.0f - pa) * c.border;
                if (a <= 0.0f)
                    continue;
                uint8_t *p = row + size_t(px) * bpp;
                for (int k = 0; k < 3; ++k) {
                    const float v = col[k] + (border[k] - col[k]) * c.border;
                    p[k] = clamp8(p[k] + (v * 255.0f - p[k]) * a);
                }
            }
        }
    });
}

} // namespace exportpipe
