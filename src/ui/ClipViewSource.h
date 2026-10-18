#pragma once

#include <PictureSource.h> // exportpipe::PictureSource

#include "ReframeExportController.h" // ReframeExportController::ClipJob

#include <GyroStabilizer.h> // insv::StabConfig

#include <memory>
#include <string>

namespace stitch {
class NativeStitchSource;
}

// A 360 clip's view as a layer's pictures (exportpipe::PictureSource): the
// other clip of a transition, drawn over the main picture in playback, the
// export and a screenshot. It renders the clip the way the export does --
// its own stitch (lens guards, depth-aware seam, seam fine-tune, privacy
// blur, capture-time correction), its reframe path and stabilization at
// each frame's own time, its colour grade -- through the fused GPU pass
// (else an equirect reprojected on the CPU).
class ClipViewSource : public exportpipe::PictureSource {
public:
    struct Spec {
        ReframeExportController::ClipJob job;
        // The stitch's equirect width (2:1); 0 = the preview's automatic
        // size. Only the CPU fallback reprojects from it.
        int stitchWidth = 0;
        // When job.stab is null: build the stabilizer on open() (reads the
        // clip's trailer -- not on the GUI thread) under this setting, the
        // clip's frame rate and its sync fine-tune. Off = none.
        insv::StabConfig buildStab;
        double fps = 0.0;
        double syncMs = 0.0;
    };

    explicit ClipViewSource(Spec spec);
    ~ClipViewSource() override;

    bool open(std::string &err) override;
    double fps() const override { return m_fps; }
    void setOutputSize(int width, int height) override;
    const exportpipe::VideoFrame *frameAt(int64_t ms) override;
    bool readNext(exportpipe::VideoFrame &out) override;
    const std::string &lastError() const override { return m_error; }

private:
    // Render the next frame into `out`. False at the end or on error.
    bool read(exportpipe::VideoFrame &out);

    Spec m_spec;
    std::unique_ptr<stitch::NativeStitchSource> m_src;
    double m_fps = 30.0;
    int m_w = 1280, m_h = 720;
    exportpipe::VideoFrame m_cur;  // what frameAt() handed out last
    bool m_haveCur = false;
    exportpipe::VideoFrame m_next; // read ahead of it, not handed out yet
    bool m_haveNext = false;
    std::string m_error;
};
