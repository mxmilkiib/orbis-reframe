#pragma once

// "Scan clip" worker (Privacy panel): runs privacy::PlateScanner over one
// clip's trimmed range, off the GUI thread, reporting progress as it goes.
// The stitch is set up as playback / export set it up (lens guard, seam
// fine-tune, the capture-time correction when the stabilization is on), so
// the regions land where the picture has the plates; the scan records which
// (privacy::StitchTiming). The levelling for the detector is the clip's gyro
// in Horizon lock, whatever the project's stabilization. Only built with the
// plate scan (ORBIS_REFRAME_HAVE_PLATE_SCAN).

#include <BlurAddition.h> // privacy::BlurAddition
#include <PlateScanData.h>
#include <PlateScanner.h> // privacy::FollowResult

#include <IStitchSource.h> // stitch::PreviewStitchConfig

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace platescan {

struct Request {
    std::string clipPath;
    // Source time: what of the clip shows -- its trim, and the footage past
    // it a transition plays (Timeline::shownSourceRange).
    int64_t fromMs = 0;
    int64_t toMs = 0;
    double fps = 30.0;
    double syncAdjustMs = 0.0; // the clip's video <-> IMU fine-tune
    std::array<double, 3> seamAdjustDeg{0.0, 0.0, 0.0};
    stitch::PreviewStitchConfig::LensGuard lensGuard = stitch::PreviewStitchConfig::LensGuard::Premium;
    bool depthSeam = true;
    // The capture-time correction, as playback has it: on with the
    // stabilization on (when the clip has a gyro track).
    bool captureTiming = true;
};

struct Result {
    privacy::PlateScan scan; // up to where it got (a stopped scan keeps its part)
    bool stopped = false;
    std::string error;
    std::string device;  // where the detector ran
    double fpsAchieved = 0.0;
};

// Called every few frames from the worker: the frame's source time and the
// scan rate so far.
using ProgressFn = std::function<void(int64_t tMs, double fps)>;

// Blocking. `stop` is polled every frame.
Result run(const Request& req, const std::atomic<bool>& stop, const ProgressFn& progress);

// "Blur a plate…": follow the plate in `seed` (a quad in the sphere frame,
// as the frame at req.fromMs shows it) from req.fromMs on, up to req.toMs,
// then back from req.fromMs down to `backToMs` (the clip's trim-in; < 0 =
// forwards only) -- privacy::PlateScanner::follow on a StitchFrameSource,
// then on a ReverseStitchFrameSource. Same stitch set-up as run().
struct FollowResult {
    // Both ways, in time order (follow.lost: lost going forwards).
    privacy::FollowResult follow;
    bool lostBackward = false; // lost going backwards (else it reached backToMs or was stopped)
    privacy::StitchTiming timing = privacy::StitchTiming::Corrected; // the stitch it was followed in
    bool stopped = false;
    std::string error;
};
// Called every few frames: the frame's source time, and whether it is the
// backward pass.
using FollowProgressFn = std::function<void(int64_t tMs, bool backward)>;
FollowResult follow(const Request& req, const insv::BlurQuad& seed, int64_t backToMs,
                    const std::atomic<bool>& stop, const FollowProgressFn& progress);

// A followed plate as the clip stores it (ClipItem::blurAdditions): every
// region of every frame, in the stitch timing it was followed in.
privacy::BlurAddition toBlurAddition(const FollowResult& r);

} // namespace platescan
