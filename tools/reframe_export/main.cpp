// Render an authored reframe path over a trimmed range of an Insta360 .insv
// to a flat rectilinear H.264/H.265 file that plays in any normal video
// player.
//
// Open the clip with stitch::NativeStitchSource (trailer calibration ->
// Vulkan video decode -> Vulkan compute stitch), read the trimmed range frame
// by frame, and reproject each frame at its baked pose (on the GPU when it
// can, else the CPU) into exportpipe::FlattenWriter -> the hardware encoder
// (NVENC, Vulkan video or VAAPI, whichever this machine can run). No
// intermediate file.
//
// The --keys file stands in for a clip's ReframeTrack: it is loaded into a real
// ReframeTrack and sampled with the same ReframeTrack::sample() the viewport
// uses, one pose per output frame.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <numbers>
#include <sstream>
#include <string>
#include <vector>

#include <NativeStitchSource.h> // stitch::NativeStitchSource, stitch::Frame, stitch::PreviewStitchConfig
#include <ProbeClip.h>          // stitch::ProbeClip

extern "C" {
#include <libavutil/buffer.h> // av_buffer_unref
#include <libavutil/frame.h>  // av_frame_free
}

#include "FlatVideoReader.h" // exportpipe::FlatVideoReader::probe (--inset)
#include "InsetCompositor.h" // exportpipe::InsetCompositor (--inset)
#include "ReframeFlatten.h" // exportpipe::FlattenWriter
#include "ReframeTrack.h"

namespace {

constexpr double kDegToRad = std::numbers::pi / 180.0;

[[noreturn]] void die(const std::string& what)
{
    std::fprintf(stderr, "error: %s\n", what.c_str());
    std::exit(1);
}

struct Args {
    std::string input;
    std::string output;
    int64_t trimInMs = 0;
    int64_t trimOutMs = -1; // -1 = clip end
    std::string keysPath;
    int outW = 1920;
    int outH = 1080;
    double fps = 0.0; // 0 = source fps
    bool h265 = true;
    int64_t bitrateKbps = 40000;
    int stitchW = 0; // equirect intermediate width (0 => auto)
    int stitchH = 0;
    stitch::PreviewStitchConfig::LensGuard lensGuard = stitch::PreviewStitchConfig::LensGuard::Premium;
    bool depthSeam = true;
    double staticYawDeg = 0.0;
    double staticPitchDeg = 0.0;
    double staticRollDeg = 0.0;
    double staticFovDeg = 75.0;
    // Colour panel grade, applied to the whole export. See
    // ClipItem::ColorGrade for the ranges/units.
    float exposureEv = 0.0f;
    float contrast = 0.0f;
    float saturation = 0.0f;
    float temperature = 0.0f;
    // One flat-video overlay, composited like the app's export does.
    std::string insetPath;
    int64_t insetStartMs = 0; // on the output timeline (0 = the in-point)
    inset::Look insetLook;
    inset::Frame insetFrame;
};

bool parseWxH(const std::string& s, int& w, int& h)
{
    const auto x = s.find('x');
    if (x == std::string::npos)
        return false;
    w = std::atoi(s.substr(0, x).c_str());
    h = std::atoi(s.substr(x + 1).c_str());
    return w > 0 && h > 0;
}

const char kUsage[] =
    "usage: reframe_export <clip.insv> <out.mp4> [options]\n"
    "  --trim-in MS           start of the exported range (default 0)\n"
    "  --trim-out MS          end of the exported range (default: clip end)\n"
    "  --keys FILE            reframe keyframes; lines of: time_ms yaw_rad pitch_rad fovY_rad [roll_rad]\n"
    "                        ('#' comments allowed; times are clip-absolute; roll optional, default 0)\n"
    "  --size WxH             output resolution (default 1920x1080)\n"
    "  --fps N                output frame rate (default: source fps)\n"
    "  --codec h264|h265     output codec (default h265)\n"
    "  --bitrate KBPS        output bitrate (default 40000)\n"
    "  --stitch-size WxH     equirect intermediate resolution, 2:1 (default: the full\n"
    "                        source sphere, at least 2*outW)\n"
    "  --lens-guard G        none | standard | premium (default premium)\n"
    "  --no-depth-seam       stitch at infinity (no depth-aware seam)\n"
    "  --yaw DEG --pitch DEG --roll DEG --fov DEG   static pose when --keys is absent\n"
    "  --exposure EV --contrast PCT --saturation PCT --temperature PCT\n"
    "                        colour grade (all default 0 = neutral)\n"
    "  --inset FILE[:START_MS]  a flat video overlaid from START_MS of the output\n"
    "  --inset-look CX,CY,W,OPACITY    its centre / width (fractions) and opacity\n"
    "  --inset-frame RADIUS,BORDER,RRGGBB  corner radius, border width (fractions)\n";

Args parseArgs(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            std::fputs(kUsage, stdout);
            std::exit(0);
        }
    if (argc < 3) {
        std::fputs(kUsage, stderr);
        std::exit(2);
    }
    Args a;
    a.input = argv[1];
    a.output = argv[2];
    for (int i = 3; i < argc; ++i) {
        const std::string k = argv[i];
        auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc)
                die(std::string("missing value for ") + name);
            return argv[++i];
        };
        if (k == "--trim-in")
            a.trimInMs = std::atoll(next("--trim-in").c_str());
        else if (k == "--trim-out")
            a.trimOutMs = std::atoll(next("--trim-out").c_str());
        else if (k == "--keys")
            a.keysPath = next("--keys");
        else if (k == "--size") {
            if (!parseWxH(next("--size"), a.outW, a.outH))
                die("bad --size (want WxH)");
        } else if (k == "--fps")
            a.fps = std::atof(next("--fps").c_str());
        else if (k == "--codec") {
            const std::string c = next("--codec");
            if (c == "h264")
                a.h265 = false;
            else if (c == "h265")
                a.h265 = true;
            else
                die("bad --codec (want h264 or h265)");
        } else if (k == "--bitrate")
            a.bitrateKbps = std::atoll(next("--bitrate").c_str());
        else if (k == "--no-depth-seam")
            a.depthSeam = false;
        else if (k == "--lens-guard") {
            const std::string g = next("--lens-guard");
            if (g == "none")
                a.lensGuard = stitch::PreviewStitchConfig::LensGuard::None;
            else if (g == "standard")
                a.lensGuard = stitch::PreviewStitchConfig::LensGuard::Standard;
            else if (g == "premium")
                a.lensGuard = stitch::PreviewStitchConfig::LensGuard::Premium;
            else
                die("bad --lens-guard (want none, standard or premium)");
        } else if (k == "--stitch-size") {
            if (!parseWxH(next("--stitch-size"), a.stitchW, a.stitchH))
                die("bad --stitch-size (want WxH)");
        } else if (k == "--yaw")
            a.staticYawDeg = std::atof(next("--yaw").c_str());
        else if (k == "--pitch")
            a.staticPitchDeg = std::atof(next("--pitch").c_str());
        else if (k == "--roll")
            a.staticRollDeg = std::atof(next("--roll").c_str());
        else if (k == "--fov")
            a.staticFovDeg = std::atof(next("--fov").c_str());
        else if (k == "--exposure")
            a.exposureEv = static_cast<float>(std::atof(next("--exposure").c_str()));
        else if (k == "--contrast")
            a.contrast = static_cast<float>(std::atof(next("--contrast").c_str()));
        else if (k == "--saturation")
            a.saturation = static_cast<float>(std::atof(next("--saturation").c_str()));
        else if (k == "--temperature")
            a.temperature = static_cast<float>(std::atof(next("--temperature").c_str()));
        else if (k == "--inset") {
            a.insetPath = next("--inset");
            const auto colon = a.insetPath.rfind(':');
            if (colon != std::string::npos && colon + 1 < a.insetPath.size() &&
                a.insetPath.find_first_not_of("0123456789", colon + 1) == std::string::npos) {
                a.insetStartMs = std::atoll(a.insetPath.c_str() + colon + 1);
                a.insetPath.erase(colon);
            }
        } else if (k == "--inset-look") {
            float v[4];
            if (std::sscanf(next("--inset-look").c_str(), "%f,%f,%f,%f", &v[0], &v[1], &v[2],
                            &v[3]) != 4)
                die("bad --inset-look (want CX,CY,W,OPACITY)");
            a.insetLook = inset::Look{v[0], v[1], v[2], v[3]}.clamped();
        } else if (k == "--inset-frame") {
            float r, b;
            unsigned rgb = 0xffffff;
            if (std::sscanf(next("--inset-frame").c_str(), "%f,%f,%x", &r, &b, &rgb) < 2)
                die("bad --inset-frame (want RADIUS,BORDER[,RRGGBB])");
            a.insetFrame.cornerRadius = r;
            a.insetFrame.borderWidth = b;
            a.insetFrame.borderRgb = rgb;
            a.insetFrame = a.insetFrame.clamped();
        }
        else
            die("unknown option: " + k);
    }
    return a;
}

ReframeTrack loadKeys(const std::string& path, qint64 frameDurMs)
{
    ReframeTrack track;
    std::ifstream in(path);
    if (!in)
        die("cannot open --keys file: " + path);
    std::string line;
    int lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        const auto hash = line.find('#');
        if (hash != std::string::npos)
            line.erase(hash);
        std::istringstream ls(line);
        long long tms;
        float yaw, pitch, fovY;
        if (!(ls >> tms >> yaw >> pitch >> fovY)) {
            if (line.find_first_not_of(" \t\r\n") == std::string::npos)
                continue;
            die("bad --keys line " + std::to_string(lineno) + ": " + line);
        }
        float roll = 0.0f;
        ls >> roll; // optional 5th column
        track.upsert(static_cast<qint64>(tms), ReframePose{yaw, pitch, roll, fovY}, frameDurMs);
    }
    if (track.isEmpty())
        die("--keys file has no keyframes: " + path);
    return track;
}

} // namespace

int main(int argc, char** argv)
{
    const Args a = parseArgs(argc, argv);

    std::string err;
    stitch::ClipProbe probe;
    if (!stitch::ProbeClip(a.input, probe, err))
        die(err);

    const double fps = a.fps > 0.0 ? a.fps : (probe.fps > 0.0 ? probe.fps : 30.0);
    const int64_t trimIn = std::max<int64_t>(0, a.trimInMs);
    const int64_t trimOut =
        a.trimOutMs < 0 ? probe.duration_ms : std::min<int64_t>(a.trimOutMs, probe.duration_ms);
    if (trimOut - trimIn < 100)
        die("trimmed range shorter than 100 ms");

    int eqW = a.stitchW > 0 ? a.stitchW : stitch::ExportStitchWidth(probe.width, a.outW);
    eqW &= ~1;
    const int eqH = (a.stitchH > 0 ? a.stitchH : eqW / 2) & ~1;

    std::printf("source: %dx%d @ %.3f fps, %lld ms\n", probe.width, probe.height, probe.fps,
                static_cast<long long>(probe.duration_ms));
    std::printf("export: [%lld, %lld) ms -> %s  %dx%d %s @ %.3f fps (equirect %dx%d)\n",
                static_cast<long long>(trimIn), static_cast<long long>(trimOut), a.output.c_str(),
                a.outW, a.outH, a.h265 ? "h265" : "h264", fps, eqW, eqH);

    // Bake one pose per output frame from a real ReframeTrack.
    const qint64 frameDurMs = static_cast<qint64>(std::llround(1000.0 / fps));
    const int64_t frameCount =
        static_cast<int64_t>(std::llround((trimOut - trimIn) / 1000.0 * fps));

    std::vector<float> poses;
    poses.reserve(static_cast<size_t>(frameCount) * 4);
    if (!a.keysPath.empty()) {
        const ReframeTrack track = loadKeys(a.keysPath, frameDurMs);
        std::printf("keys:   %d from %s\n", track.count(), a.keysPath.c_str());
        for (int64_t i = 0; i < frameCount; ++i) {
            const qint64 tAbs = trimIn + static_cast<qint64>(std::llround(i * 1000.0 / fps));
            const ReframePose p = track.sample(tAbs);
            poses.push_back(p.yaw);
            poses.push_back(p.pitch);
            poses.push_back(p.roll);
            poses.push_back(p.fovY);
        }
    } else {
        std::printf("keys:   none (static yaw=%.1f pitch=%.1f roll=%.1f fov=%.1f deg)\n",
                    a.staticYawDeg, a.staticPitchDeg, a.staticRollDeg, a.staticFovDeg);
        poses = {static_cast<float>(a.staticYawDeg * kDegToRad),
                 static_cast<float>(a.staticPitchDeg * kDegToRad),
                 static_cast<float>(a.staticRollDeg * kDegToRad),
                 static_cast<float>(a.staticFovDeg * kDegToRad)};
    }
    const int poseCount = static_cast<int>(poses.size() / 4);
    const float colorGrade[4] = {a.exposureEv, a.contrast, a.saturation, a.temperature};
    if (a.exposureEv != 0.0f || a.contrast != 0.0f || a.saturation != 0.0f ||
        a.temperature != 0.0f) {
        std::printf("colour: exposure=%.2fEV contrast=%.0f%% saturation=%.0f%% temperature=%.0f%%\n",
                    a.exposureEv, a.contrast, a.saturation, a.temperature);
    }

    // --- open the native stitch source, seek to the in-point ---
    stitch::PreviewStitchConfig sc;
    sc.width = eqW;
    sc.height = eqH;
    sc.lensGuard = a.lensGuard;
    sc.depthSeam = a.depthSeam;
    auto src = std::make_unique<stitch::NativeStitchSource>();
    if (!src->Open(a.input, sc))
        die(src->GetLastError());
    if (!src->SeekTo(trimIn))
        die(src->GetLastError());

    // --- reproject + NVENC encode, frame by frame ---
    exportpipe::FlattenConfig fc;
    fc.out_path = a.output;
    fc.out_w = a.outW;
    fc.out_h = a.outH;
    fc.fps = fps;
    fc.h265 = a.h265;
    fc.bitrate_kbps = a.bitrateKbps;
    // A 10-bit source (the X6's HEVC Main 10) gets a Main 10 export; the
    // writer falls back to 8-bit when the encoder can't take P010.
    fc.ten_bit = a.h265 && src->SourceBitDepth() > 8;
    fc.vulkan_device = src->vulkanHwDevice(); // share the decode device if one

    exportpipe::FlattenWriter writer;
    if (!writer.open(fc, err)) {
        av_buffer_unref(&fc.vulkan_device);
        die(err);
    }
    av_buffer_unref(&fc.vulkan_device); // the encoder holds its own reference

    // --inset: output frame k is at k / fps on the output timeline.
    std::shared_ptr<exportpipe::InsetCompositor> insets;
    if (!a.insetPath.empty()) {
        exportpipe::FlatVideoReader::Info info;
        if (!exportpipe::FlatVideoReader::probe(a.insetPath, info, err))
            die(err);
        exportpipe::InsetClipJob j;
        j.path = a.insetPath;
        j.startMs = a.insetStartMs;
        j.trimOutMs = info.durationMs;
        j.aspect = double(info.width) / info.height;
        j.frame = a.insetFrame;
        j.lookAt = [look = a.insetLook](int64_t) { return look; };
        insets = std::make_shared<exportpipe::InsetCompositor>(
            std::vector<exportpipe::InsetClipJob>{j}, a.outW, a.outH);
        writer.setFrameHook([insets, fps](int64_t k, uint8_t* y, int ys, uint8_t* uv, int uvs,
                                          std::string& e) {
            return insets->composite(std::llround(k * 1000.0 / fps), y, ys, uv, uvs, e);
        });
        std::printf("inset:  %s (%dx%d, %lld ms) from %lld ms\n", a.insetPath.c_str(),
                    info.width, info.height, static_cast<long long>(info.durationMs),
                    static_cast<long long>(a.insetStartMs));
    }

    // GPU reproject: when the all-GPU stitch path is active, reproject the
    // equirect to NV12 on the GPU and hand it straight to the encoder -- no
    // CPU equirect->rectilinear pass, no RGBA->NV12 swscale. Needs
    // outW % 4 == 0 and outH % 2 == 0 (the shader's NV12 packing).
    const bool gpuFlat =
        src->GpuReframeAvailable() && (a.outW % 4) == 0 && (a.outH % 2) == 0;
    // No host round-trip at all when the encoder shares the stitch device
    // (fused view -> encoder-frame images on-device). Off when --inset set a
    // frame hook, which needs CPU pixels.
    const bool gpuVk = gpuFlat && writer.vulkanEncodeAvailable() && src->GpuViewAvailable();
    const char* encName = exportpipe::hwEncoderName(a.h265);
    std::printf("reproject: %s, encoder: %s%s\n",
                gpuVk ? "GPU (YUV straight into the encoder's frames)"
                      : gpuFlat ? "GPU (YUV straight to the encoder)"
                                : "CPU (multithreaded scanlines)",
                encName ? encName : "none",
                writer.tenBit() ? ", main10" : "");

    const double halfFrameMs = 500.0 / fps;
    int lastPct = -1;
    int64_t written = 0;
    while (written < frameCount) {
        const float* pose =
            poseCount > 0
                ? &poses[static_cast<size_t>(std::min<int64_t>(written, poseCount - 1)) * 4]
                : nullptr;

        if (gpuVk) {
            AVFrame* vkFrame = writer.acquireVulkanFrame();
            int64_t ptsMs = 0;
            const bool got = vkFrame &&
                src->ReadFlatFrameVk(a.outW, a.outH,
                                     [pose](int64_t, float p[4], float[9]) {
                                         if (pose)
                                             std::copy(pose, pose + 4, p);
                                     },
                                     colorGrade, vkFrame, ptsMs);
            if (!got) {
                av_frame_free(&vkFrame);
                if (!src->GetLastError().empty())
                    die(src->GetLastError());
                break; // clean end-of-clip
            }
            if (ptsMs + halfFrameMs < trimIn) {
                av_frame_free(&vkFrame);
                continue; // straggler ahead of the in-point
            }
            if (!writer.addVulkanFrame(vkFrame, err))
                die(err);
        } else if (gpuFlat) {
            stitch::FlatFrame ff = src->ReadFlatFrame(a.outW, a.outH, pose, nullptr, colorGrade);
            if (ff.nv12.empty()) {
                if (!src->GetLastError().empty())
                    die(src->GetLastError());
                break; // clean end-of-clip
            }
            if (ff.timestamp_ms + halfFrameMs < trimIn)
                continue; // straggler ahead of the in-point
            if (!writer.addNv12Frame(ff.nv12.data(), ff.width, ff.height, err))
                die(err);
        } else {
            stitch::Frame eq = src->ReadFrame();
            if (eq.rgba.empty()) {
                if (!src->GetLastError().empty())
                    die(src->GetLastError());
                break; // clean end-of-clip
            }
            if (eq.timestamp_ms + halfFrameMs < trimIn)
                continue; // straggler ahead of the in-point
            if (!writer.addEquirectFrame(eq.rgba.data(), eq.width, eq.height, pose, nullptr,
                                         colorGrade, err))
                die(err);
        }
        ++written;
        const int pct = frameCount > 0 ? static_cast<int>(written * 100 / frameCount) : 100;
        if (pct != lastPct) {
            lastPct = pct;
            std::printf("\r  render %3d%%", pct);
            std::fflush(stdout);
        }
    }
    std::printf("\n");

    const int64_t total = writer.finish(err);
    if (total < 0)
        die(err);

    std::printf("done: %lld frames -> %s\n", static_cast<long long>(total), a.output.c_str());
    return 0;
}
