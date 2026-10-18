#pragma once

// Where a video-track clip (a picture-in-picture "inset") sits on the
// output frame, and how each output pixel is shaded by it. The one place the
// placement math lives: the viewport (src/render/shaders/inset.frag, which
// mirrors shade() below line for line) and the export compositor
// (src/export/InsetCompositor) both go through it, so what plays is what
// exports.
//
// Header-only and free of Qt / FFmpeg / Vulkan, so src/export can use it.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace inset {

// The animatable part (InsetKeyTrack keyframes it). Positions and sizes are
// fractions of the output frame, so they survive a change of output size.
struct Look {
    float centerX = 0.78f; // inset centre, fraction of the output width
    float centerY = 0.75f; // ... of the output height
    float width = 0.30f;   // inset width, fraction of the output width
    float opacity = 1.0f;  // 0..1

    static constexpr float kMinWidth = 0.02f;
    static constexpr float kMaxWidth = 1.0f;

    Look clamped() const
    {
        Look l = *this;
        l.centerX = std::clamp(l.centerX, -0.5f, 1.5f); // may hang off an edge
        l.centerY = std::clamp(l.centerY, -0.5f, 1.5f);
        l.width = std::clamp(l.width, kMinWidth, kMaxWidth);
        l.opacity = std::clamp(l.opacity, 0.0f, 1.0f);
        return l;
    }
    bool operator==(const Look &o) const
    {
        return centerX == o.centerX && centerY == o.centerY && width == o.width &&
               opacity == o.opacity;
    }
    bool operator!=(const Look &o) const { return !(*this == o); }
};

inline Look lerp(const Look &a, const Look &b, float t)
{
    auto l = [t](float x, float y) { return x + (y - x) * t; };
    return {l(a.centerX, b.centerX), l(a.centerY, b.centerY), l(a.width, b.width),
            l(a.opacity, b.opacity)};
}

// The static frame around it (not keyframed).
struct Frame {
    float cornerRadius = 0.0f; // fraction of the inset's shorter side, 0..0.5
    float borderWidth = 0.0f;  // fraction of the output height, 0..kMaxBorder
    uint32_t borderRgb = 0xffffff;

    static constexpr float kMaxBorder = 0.05f;

    Frame clamped() const
    {
        Frame f = *this;
        f.cornerRadius = std::clamp(f.cornerRadius, 0.0f, 0.5f);
        f.borderWidth = std::clamp(f.borderWidth, 0.0f, kMaxBorder);
        f.borderRgb &= 0xffffff;
        return f;
    }
    bool operator==(const Frame &o) const
    {
        return cornerRadius == o.cornerRadius && borderWidth == o.borderWidth &&
               borderRgb == o.borderRgb;
    }
    bool operator!=(const Frame &o) const { return !(*this == o); }
};

// The inset's rectangle in output pixels (x, y = top-left; may lie partly
// outside the frame), plus its corner radius and border width in pixels.
struct Rect {
    double x = 0, y = 0, w = 0, h = 0;
    double radius = 0; // px
    double border = 0; // px
    bool empty() const { return w < 1.0 || h < 1.0; }
};

// `aspect` = the inset video's width / height (display aspect).
inline Rect place(const Look &look, const Frame &frame, double aspect, int outW, int outH)
{
    Rect r;
    if (outW <= 0 || outH <= 0)
        return r;
    if (!(aspect > 0.0))
        aspect = 16.0 / 9.0;
    r.w = double(look.width) * outW;
    r.h = r.w / aspect;
    r.x = look.centerX * outW - r.w / 2.0;
    r.y = look.centerY * outH - r.h / 2.0;
    r.radius = frame.cornerRadius * std::min(r.w, r.h);
    r.border = std::min(double(frame.borderWidth) * outH, std::min(r.w, r.h) / 2.0);
    return r;
}

// A flat main-track clip (ClipItem::flat) as a full-frame layer: centred,
// the whole picture inside the frame (`fill` false: bars on two sides), or
// covering it (`fill` true: the overhang cropped). Not clamped -- a fill's
// width can exceed the frame's.
inline Look fitted(double aspect, int outW, int outH, bool fill)
{
    Look l;
    l.centerX = 0.5f;
    l.centerY = 0.5f;
    l.width = 1.0f;
    if (outW > 0 && outH > 0 && aspect > 0.0) {
        const double rel = aspect / (double(outW) / outH); // picture vs frame
        l.width = static_cast<float>(fill ? std::max(1.0, rel) : std::min(1.0, rel));
    }
    return l;
}

// How the output pixel whose centre is at (px, py) is covered: `alpha` is the
// inset's coverage there (antialiased over one pixel at the edge and the
// rounded corners), `border` how much of it is the border colour rather than
// the picture. The picture fills the whole rect; the border is drawn inside
// it, over the picture's edge. The caller multiplies alpha by the opacity.
struct Coverage {
    float alpha = 0.0f;
    float border = 0.0f;
};

inline Coverage shade(double px, double py, const Rect &r)
{
    // Signed distance to the rounded rectangle (negative inside).
    const double hx = r.w / 2.0, hy = r.h / 2.0;
    const double qx = std::abs(px - (r.x + hx)) - (hx - r.radius);
    const double qy = std::abs(py - (r.y + hy)) - (hy - r.radius);
    const double ox = std::max(qx, 0.0), oy = std::max(qy, 0.0);
    const double d = std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0) - r.radius;
    Coverage c;
    c.alpha = static_cast<float>(std::clamp(0.5 - d, 0.0, 1.0));
    if (r.border > 0.0)
        c.border = static_cast<float>(std::clamp(d + r.border + 0.5, 0.0, 1.0));
    return c;
}

// Opacity multiplier of a linear fade in / out at clip-local timeline time
// `localMs` of a clip `spanMs` long (the audio fades' shape).
inline float fadeGain(int64_t localMs, int64_t spanMs, int64_t fadeInMs, int64_t fadeOutMs)
{
    double g = 1.0;
    if (fadeInMs > 0 && localMs < fadeInMs)
        g *= std::max(0.0, double(localMs) / double(fadeInMs));
    if (fadeOutMs > 0 && localMs > spanMs - fadeOutMs)
        g *= std::max(0.0, double(spanMs - localMs) / double(fadeOutMs));
    return static_cast<float>(std::clamp(g, 0.0, 1.0));
}

// Opacity multiplier going in a straight line from `from` at the start of a
// clip `spanMs` long to `to` at its end, at clip-local timeline time
// `localMs`: a transition's other clip blending in or out.
inline float rampGain(int64_t localMs, int64_t spanMs, float from, float to)
{
    if (spanMs <= 0)
        return from;
    const double t = std::clamp(double(localMs) / double(spanMs), 0.0, 1.0);
    return static_cast<float>(from + (to - from) * t);
}

} // namespace inset
