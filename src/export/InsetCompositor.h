#pragma once

#include "ColorLut.h" // color::Grade
#include "InsetGeometry.h"
#include "PictureSource.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Composites the flat main-track clips and the video tracks' insets into the
// export's output frames. Runs on the NV12 frame just before it's encoded, so
// the GPU export path (frames reprojected straight to NV12) and the CPU
// fallback (RGBA -> NV12 through swscale) share one implementation -- the same
// way the colour grade is applied in both. Placement and edge / corner /
// border shading are inset::place / inset::shade, exactly what the viewport
// draws. A layer that covers its rect fully on whole pixels -- a flat clip
// filling the frame, mostly -- is decoded to NV12 at its size and copied in;
// the rest are blended per pixel, across the cores.
//
// Each clip's frames come from its own FlatVideoReader (or PictureSource,
// for a 360 clip's view), opened on first use and closed once the export is
// past the clip.

namespace exportpipe {

class FlatVideoReader;

struct InsetClipJob {
    std::string path;
    int64_t startMs = 0; // timeline position
    int64_t trimInMs = 0; // source time
    int64_t trimOutMs = 0;
    double speed = 1.0;
    double aspect = 16.0 / 9.0; // display width / height
    int64_t fadeInMs = 0;
    int64_t fadeOutMs = 0;
    // Opacity over the clip, on top of the look's and the fades: a straight
    // ramp from rampFrom at its start to rampTo at its end (a transition's
    // other clip).
    float rampFrom = 1.0f;
    float rampTo = 1.0f;
    inset::Frame frame;
    // The look at a source time (keyframes sampled; fades not applied).
    // Called on the export thread.
    std::function<inset::Look(int64_t sourceMs)> lookAt;
    // A flat main-track clip's colour grade (ReframeView::colorGrade);
    // default = none.
    color::Grade colorGrade;
    // A title: this picture (premultiplied RGBA, stillW x stillH) for the
    // whole clip instead of a file's frames (`path` unused). Blended by its
    // alpha, like every picture.
    std::shared_ptr<const std::vector<uint8_t>> still;
    int stillW = 0, stillH = 0;
    // A 360 clip's view (a transition's other clip): its frames from this
    // instead of a file's (`path` names it in messages). It carries its own
    // grade.
    PictureSource::Factory picture;

    int64_t spanMs() const { return static_cast<int64_t>(std::llround((trimOutMs - trimInMs) / speed)); }
    int64_t endMs() const { return startMs + spanMs(); }
};

class InsetCompositor {
public:
    // `jobs` bottom-most first (drawn in that order).
    InsetCompositor(std::vector<InsetClipJob> jobs, int outW, int outH);
    ~InsetCompositor();

    bool empty() const { return m_jobs.empty(); }

    // Blend every inset showing at timeline time `timelineMs` into an NV12
    // frame (BT.709 limited range, like the export's). False (with `err`)
    // when a clip's file can't be read.
    bool composite(int64_t timelineMs, uint8_t *y, int yStride, uint8_t *uv, int uvStride,
                   std::string &err);
    // The same into a packed RGB (bytesPerPixel 3) or RGBA (4) picture of
    // the output size -- the screenshot button's still.
    bool compositeRgb(int64_t timelineMs, uint8_t *rgb, int stride, int bytesPerPixel,
                      std::string &err);

    // Blend one RGBA picture (picW x picH, already the rect's size or any
    // size -- it is sampled by position; alpha premultiplied, so a video's
    // opaque frame and a title's transparent one go the same way) into NV12
    // at `rect` with `opacity`.
    // Exposed for tests.
    static void blendNv12(const uint8_t *rgba, int picW, int picH, const inset::Rect &rect,
                          float opacity, uint32_t borderRgb, uint8_t *y, int yStride,
                          uint8_t *uv, int uvStride, int outW, int outH);

    static void blendRgb(const uint8_t *rgba, int picW, int picH, const inset::Rect &rect,
                         float opacity, uint32_t borderRgb, uint8_t *rgb, int stride,
                         int bytesPerPixel, int outW, int outH);

private:
    struct State;
    // The picture to draw for job `i` at `timelineMs` and where: false if
    // it isn't showing (or on error: `err` set). With `nv12` (the output
    // is), one that's a plain copy (opaque, square, on whole even pixels)
    // comes as NV12 of the rect's size, to copy; else RGBA, to blend.
    struct Layer {
        const uint8_t *rgba = nullptr;
        const uint8_t *nv12 = nullptr;
        int w = 0, h = 0;
        inset::Rect rect;
        float opacity = 0.0f;
        uint32_t borderRgb = 0;
    };
    bool layerAt(size_t i, int64_t timelineMs, bool nv12, Layer &layer, std::string &err);
    std::vector<InsetClipJob> m_jobs;
    std::vector<std::unique_ptr<State>> m_states;
    int m_outW, m_outH;
};

} // namespace exportpipe
