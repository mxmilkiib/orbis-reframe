// Playback benchmark: where does a frame's time go on today's all-GPU path,
// per clip / capture mode and per equirect size? Measure-only, no output files.
//
//   decode      insv::VulkanVideoDecoder alone: both lenses, no stitch.
//   eq N        NativeStitchSource::ReadFrame at an N x N/2 equirect: what
//               playback does today (decode -> stitch -> host-visible buffer
//               -> memcpy into the Frame).
//   flat N      ReadFlatFrame 1920x1080 NV12 through an N x N/2 equirect: the
//               export path before the fused pass (INSV_FLAT_VIA_EQUIRECT).
//   fused       ReadFlatFrame 1920x1080 NV12 straight from the fisheyes (the
//               fused pass): what export does now.
//   view        ReadViewFrame 1920x1080 RGBA, fused: a viewport-sized picture.
//   lens        ReadLensFrame: the decoded pair copied into a shareable ring
//               slot + the seam measure -- the source side of lens-frame
//               playback (the viewport then renders the view itself).
//   seek        SeekTo + first ReadFrame at spread-out positions (1920 eq).
//
//   The depth-aware seam is on unless --no-depth-seam; "seam" is its
//   cost pass + solve, inside each row's total.
//
// usage: playback_bench [--frames N] [--eq W,W,...] [--only-flat] [--no-depth-seam] <clip.insv>...

#include <LensFrame.h>
#include <NativeStitchSource.h>
#include <VulkanVideoDecoder.h>

#include <vulkan/vulkan.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

struct Stats {
    std::vector<double> v;
    void add(double x) { v.push_back(x); }
    double mean() const {
        double s = 0;
        for (double x : v)
            s += x;
        return v.empty() ? 0 : s / v.size();
    }
    double pct(double p) const {
        if (v.empty())
            return 0;
        std::vector<double> s = v;
        std::sort(s.begin(), s.end());
        return s[std::min(s.size() - 1, static_cast<size_t>(p * (s.size() - 1) + 0.5))];
    }
};

constexpr int kWarmup = 5;
bool gDepthSeam = true;

void benchDecode(const std::string& path, int frames, double fps) {
    insv::VulkanVideoDecoder dec;
    std::string err;
    if (!dec.open(path, err)) {
        std::printf("  decode      FAILED: %s\n", err.c_str());
        return;
    }
    VkInstance inst;
    VkPhysicalDevice phys;
    VkDevice dev;
    uint32_t qf;
    VkDeviceQueueCreateFlags qflags;
    dec.deviceHandles(inst, phys, dev, qf, qflags);

    int n = 0;
    Clock::time_point t0;
    for (int i = 0; i < frames + kWarmup; ++i) {
        if (i == kWarmup)
            t0 = Clock::now();
        AVFrame* f0 = nullptr;
        AVFrame* f1 = nullptr;
        int64_t pts = 0;
        if (!dec.readNextPair(f0, f1, &pts, err))
            break;
        const bool last = i == frames + kWarmup - 1;
        if (last) { // make sure the GPU has actually finished decoding
            insv::VulkanLensFrame l0, l1;
            dec.lockFrame(f0, l0);
            dec.lockFrame(f1, l1);
            const VkSemaphore sems[2] = {l0.semaphore, l1.semaphore};
            const uint64_t vals[2] = {l0.semWaitValue, l1.semWaitValue};
            VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
            wi.semaphoreCount = 2;
            wi.pSemaphores = sems;
            wi.pValues = vals;
            vkWaitSemaphores(dev, &wi, 5'000'000'000ULL);
            dec.unlockFrame(f0, {l0.semWaitValue, l0.layout});
            dec.unlockFrame(f1, {l1.semWaitValue, l1.layout});
        }
        dec.freeFrame(f0);
        dec.freeFrame(f1);
        if (i >= kWarmup)
            ++n;
    }
    const double ms = msSince(t0);
    std::printf("  decode      %4d pairs  %6.2f ms/pair  %6.1f fps  (%.2fx real time)\n", n,
                ms / n, 1000.0 * n / ms, 1000.0 * n / ms / fps);
}

std::unique_ptr<stitch::NativeStitchSource> openSource(const std::string& path, int eqW) {
    auto src = std::make_unique<stitch::NativeStitchSource>();
    stitch::PreviewStitchConfig cfg;
    cfg.width = eqW;
    cfg.height = eqW / 2;
    cfg.depthSeam = gDepthSeam;
    if (!src->Open(path, cfg)) {
        std::printf("  open FAILED: %s\n", src->GetLastError().c_str());
        return nullptr;
    }
    if (!src->GpuReframeAvailable()) {
        std::printf("  not on the all-GPU path -- numbers would not be comparable\n");
        return nullptr;
    }
    return src;
}

void benchEquirect(const std::string& path, int eqW, int frames, double fps) {
    auto src = openSource(path, eqW);
    if (!src)
        return;
    Stats total, dec, wait, gpu, copy, seam;
    for (int i = 0; i < frames + kWarmup; ++i) {
        const auto t0 = Clock::now();
        const stitch::Frame f = src->ReadFrame();
        const double ms = msSince(t0);
        if (f.rgba.empty())
            break;
        if (i < kWarmup)
            continue;
        const stitch::ReadTiming& t = src->LastReadTiming();
        total.add(ms);
        dec.add(t.decodeMs);
        wait.add(t.fenceWaitMs);
        gpu.add(t.stitchGpuMs);
        copy.add(t.copyMs);
        seam.add(t.seamMs);
    }
    const double mb = eqW * (eqW / 2) * 4 / 1e6;
    std::printf("  eq %4d     %4zu frames  %6.2f ms/frame (p95 %6.2f)  %6.1f fps  |  decode %5.2f  "
                "seam %5.2f  wait %5.2f  stitch-gpu %5.2f  copy %5.2f (%.0f MB, %.1f GB/s)%s\n",
                eqW, total.v.size(), total.mean(), total.pct(0.95), 1000.0 / total.mean(),
                dec.mean(), seam.mean(), wait.mean(), gpu.mean(), copy.mean(), mb,
                copy.mean() > 0 ? mb / copy.mean() : 0.0,
                1000.0 / total.mean() >= fps ? "" : "  < real time");
}

enum class FlatKind { ViaEquirect, FusedNv12, FusedRgba };

void benchFlat(const std::string& path, int eqW, int frames, double fps, FlatKind kind) {
    if (kind == FlatKind::ViaEquirect)
        setenv("INSV_FLAT_VIA_EQUIRECT", "1", 1);
    else
        unsetenv("INSV_FLAT_VIA_EQUIRECT");
    auto src = openSource(path, eqW);
    if (!src)
        return;
    if (kind != FlatKind::ViaEquirect && !src->GpuViewAvailable()) {
        std::printf("  fused view pass unavailable\n");
        return;
    }
    const float pose[4] = {0.0f, 0.0f, 0.0f, 1.309f};
    const auto fixedPose = [&pose](int64_t, float p[4], float[9]) { std::copy(pose, pose + 4, p); };
    Stats total, dec, wait, gpu, rf, copy, seam;
    for (int i = 0; i < frames + kWarmup; ++i) {
        const auto t0 = Clock::now();
        const bool got = kind == FlatKind::FusedRgba
                             ? !src->ReadViewFrame(1920, 1080, fixedPose).rgba.empty()
                             : !src->ReadFlatFrame(1920, 1080, pose).nv12.empty();
        const double ms = msSince(t0);
        if (!got)
            break;
        if (i < kWarmup)
            continue;
        const stitch::ReadTiming& t = src->LastReadTiming();
        total.add(ms);
        dec.add(t.decodeMs);
        wait.add(t.fenceWaitMs);
        gpu.add(t.stitchGpuMs);
        rf.add(t.reframeGpuMs);
        copy.add(t.copyMs);
        seam.add(t.seamMs);
    }
    const char* label = kind == FlatKind::ViaEquirect ? "flat" : kind == FlatKind::FusedNv12
                                                                     ? "fused"
                                                                     : "view";
    char name[32];
    if (kind == FlatKind::ViaEquirect)
        std::snprintf(name, sizeof name, "%s %4d", label, eqW);
    else
        std::snprintf(name, sizeof name, "%-9s", label);
    std::printf("  %-10s %4zu frames  %6.2f ms/frame (p95 %6.2f)  %6.1f fps  |  decode %5.2f  "
                "seam %5.2f  wait %5.2f  gpu %5.2f  reframe-gpu %5.2f  copy %5.2f%s\n",
                name, total.v.size(), total.mean(), total.pct(0.95), 1000.0 / total.mean(),
                dec.mean(), seam.mean(), wait.mean(), gpu.mean(), kind == FlatKind::ViaEquirect ? rf.mean() : 0.0,
                copy.mean(), 1000.0 / total.mean() >= fps ? "" : "  < real time");
}

void benchLens(const std::string& path, int frames, double fps) {
    auto src = openSource(path, 0); // Auto: the measure grid of the preview
    if (!src)
        return;
    if (!src->LensFramesAvailable()) {
        std::printf("  lens frames unavailable\n");
        return;
    }
    Stats total, dec, wait, seam;
    std::vector<render::LensFrame> held; // hold a few, as a playback queue would
    for (int i = 0; i < frames + kWarmup; ++i) {
        const auto t0 = Clock::now();
        render::LensFrame lf;
        const bool got = src->ReadLensFrame(lf);
        const double ms = msSince(t0);
        if (!got) {
            if (!src->GetLastError().empty())
                std::printf("  lens: %s\n", src->GetLastError().c_str());
            break;
        }
        held.push_back(std::move(lf));
        if (held.size() > 6)
            held.erase(held.begin());
        if (i < kWarmup)
            continue;
        const stitch::ReadTiming& t = src->LastReadTiming();
        total.add(ms);
        dec.add(t.decodeMs);
        wait.add(t.fenceWaitMs);
        seam.add(t.seamMs);
    }
    std::printf("  lens       %4zu frames  %6.2f ms/frame (p95 %6.2f)  %6.1f fps  |  decode %5.2f  "
                "seam %5.2f  wait %5.2f%s\n",
                total.v.size(), total.mean(), total.pct(0.95), 1000.0 / total.mean(), dec.mean(),
                seam.mean(), wait.mean(), 1000.0 / total.mean() >= fps ? "" : "  < real time");
}

void benchSeek(const std::string& path, int64_t durationMs, int eqW) {
    auto src = openSource(path, eqW);
    if (!src)
        return;
    src->ReadFrame(); // warm the pipeline
    // Exact seeks decode from the previous keyframe; scrub seeks go to the
    // nearest keyframe (PlaybackController::scrub) and decode just that frame.
    Stats exact, scrub;
    int offKey = 0; // scrub frames that didn't land on the keyframe itself
    // Spread over the clip, deliberately off keyframe boundaries.
    constexpr int kSeeks = 10;
    for (int i = 0; i < kSeeks; ++i) {
        const int64_t t = (durationMs - 1500) * (i * 7 % kSeeks) / kSeeks + 733;
        auto t0 = Clock::now();
        if (src->SeekTo(t) && !src->ReadFrame().rgba.empty())
            exact.add(msSince(t0));
        t0 = Clock::now();
        const int64_t kf = src->NearestKeyframeMs(t);
        if (!src->SeekTo(kf))
            continue;
        const stitch::Frame f = src->ReadFrame();
        if (f.rgba.empty())
            continue;
        scrub.add(msSince(t0));
        if (std::llabs(f.timestamp_ms - kf) > 1)
            ++offKey;
    }
    std::printf("  seek %4d   exact mean %6.1f ms  max %6.1f  |  scrub (keyframe) mean %5.1f ms  "
                "max %5.1f%s\n",
                eqW, exact.mean(), exact.pct(1.0), scrub.mean(), scrub.pct(1.0),
                offKey ? "  (some scrubs missed the keyframe!)" : "");
}

const char kUsage[] =
    "usage: playback_bench [--frames N] [--eq W,W,...] [--only-flat] [--no-depth-seam] <clip.insv>...\n";

} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            std::fputs(kUsage, stdout);
            return 0;
        }
    int frames = 240;
    bool onlyFlat = false; // skip decode / eq / seek: the flat + fused rows only
    std::vector<int> eqWidths = {1920, 3840, 7680};
    std::vector<std::string> clips;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--frames" && i + 1 < argc) {
            frames = std::atoi(argv[++i]);
        } else if (a == "--no-depth-seam") {
            gDepthSeam = false;
        } else if (a == "--only-flat") {
            onlyFlat = true;
        } else if (a == "--eq" && i + 1 < argc) {
            eqWidths.clear();
            std::stringstream ss(argv[++i]);
            std::string w;
            while (std::getline(ss, w, ','))
                eqWidths.push_back(std::atoi(w.c_str()));
        } else {
            clips.push_back(a);
        }
    }
    if (clips.empty()) {
        std::fputs(kUsage, stderr);
        return 2;
    }

    for (const std::string& path : clips) {
        stitch::NativeStitchSource probe;
        if (!probe.Open(path, stitch::PreviewStitchConfig{})) {
            std::printf("%s: open failed: %s\n", path.c_str(), probe.GetLastError().c_str());
            continue;
        }
        const stitch::ClipInfo info = probe.GetInfo();
        const double budget = 1000.0 / info.fps;
        std::printf("\n%s\n  %dx%d per lens @ %.2f fps, %.1f s -- budget %.2f ms/frame\n",
                    path.c_str(), info.width, info.height, info.fps, info.duration_ms / 1000.0,
                    budget);
        std::fflush(stdout);
        if (!onlyFlat) {
            benchDecode(path, frames, info.fps);
            for (int w : eqWidths) {
                benchEquirect(path, w, frames, info.fps);
                std::fflush(stdout);
            }
        }
        for (int w : {1920, 7680}) {
            benchFlat(path, w, frames, info.fps, FlatKind::ViaEquirect);
            std::fflush(stdout);
        }
        // Fused: the measure grid is the full sphere, as export uses it.
        const int sphere = stitch::FullSphereWidth(info.width);
        benchFlat(path, sphere, frames, info.fps, FlatKind::FusedNv12);
        benchFlat(path, sphere, frames, info.fps, FlatKind::FusedRgba);
        benchLens(path, frames, info.fps);
        std::fflush(stdout);
        if (onlyFlat)
            continue;
        benchSeek(path, info.duration_ms, 1920);
        benchSeek(path, info.duration_ms, 0); // Auto preview size
        std::fflush(stdout);
    }
    return 0;
}
