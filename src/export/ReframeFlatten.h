#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

// The reproject + encode half of the reframe export: take equirect frames
// straight from stitch::NativeStitchSource in memory, CPU-reproject each against
// the caller-baked pose (exportpipe::RenderRectilinear) -- or take frames the
// GPU already reprojected -- and encode through the hardware encoder the
// machine has (NVENC, Vulkan video or VAAPI) + mux.
// Links system FFmpeg only.

namespace exportpipe {

// True when one of the hardware encoders FlattenWriter tries -- NVENC,
// Vulkan video or VAAPI -- exists in this FFmpeg and has a device it can
// run on. The UI gates the export on this.
bool hwEncoderAvailable();
// The encoder FlattenWriter would pick for the codec ("hevc_vulkan"),
// nullptr when none is usable. For status display.
const char* hwEncoderName(bool h265);

struct ReframeView;

// Encoder / muxer configuration, fixed for the whole output file.
struct FlattenConfig {
    std::string out_path;
    int out_w = 1920;
    int out_h = 1080;
    double fps = 30.0;
    bool h265 = true;
    int64_t bitrate_kbps = 40000;
    // Optional AAC audio stream, fed through FlattenWriter::addAudio().
    // audio_sample_rate 0 => video-only output (no audio stream at all).
    int audio_sample_rate = 0;
    int audio_channels = 2;
    int64_t audio_bitrate_kbps = 192;
};

// Feeds one or more clips' frames through a single open encoder + muxer, so a
// multi-clip timeline exports to one file with continuous output PTS and one
// GOP structure (no concat / remux, no extra generation loss). The caller
// drives it clip-by-clip -- open() once, then addEquirectFrame() /
// addNv12Frame() per frame, finish() once.
class FlattenWriter {
public:
    FlattenWriter();
    ~FlattenWriter();
    FlattenWriter(const FlattenWriter&) = delete;
    FlattenWriter& operator=(const FlattenWriter&) = delete;

    // Open the encoder + output container. False (with `err`) on failure.
    bool open(const FlattenConfig& cfg, std::string& err);

    // Reproject one in-memory RGBA equirect frame (eqW x eqH) against `pose`
    // ({yaw, pitch, roll, fovY} radians; nullptr => static forward view) and
    // encode it, advancing the shared output PTS. For callers that produce
    // equirect frames in-process (stitch::NativeStitchSource) with no
    // intermediate file. `stab` (row-major 3x3, nullptr => identity) is the
    // frame's gyro de-rotation in the sphere frame, applied after `pose`.
    // `colorGrade` ({exposureEv, contrast%, saturation%, temperature%}, nullptr
    // => neutral) is the Colour panel grade. False (with `err`) on failure.
    bool addEquirectFrame(const uint8_t* eqRgba, int eqW, int eqH, const float pose[4],
                          const float stab[9], const float colorGrade[4],
                          std::string& err);

    // Encode one already-reprojected tightly-packed NV12 frame (Y plane w*h,
    // then interleaved UV w*h/2), advancing the shared output PTS. For callers
    // that reproject on the GPU (stitch::NativeStitchSource::ReadFlatFrame) --
    // skips RenderRectilinear and the RGBA->NV12 swscale entirely. w x h must
    // equal the FlattenConfig out_w x out_h. False (with `err`) on failure.
    bool addNv12Frame(const uint8_t* nv12, int w, int h, std::string& err);

    // Called on every output frame just before it is encoded, with its index
    // (0-based, across the whole file) and its NV12 planes (BT.709 limited
    // range) -- for both addEquirectFrame() and addNv12Frame(), so one
    // overlay pass serves the CPU and the GPU paths (insets). Returning
    // false (with `err`) fails that add. Unset = no hook.
    using FrameHook = std::function<bool(int64_t index, uint8_t *y, int yStride, uint8_t *uv,
                                         int uvStride, std::string &err)>;
    void setFrameHook(FrameHook hook);

    // Append `frames` frames of interleaved float32 PCM (FlattenConfig
    // audio_channels wide, at audio_sample_rate) to the audio stream. Audio
    // has its own continuous timeline, independent of the video PTS -- the
    // caller keeps the two in step (audioFramesWritten() vs. frames written /
    // fps). Samples are buffered into whole AAC frames; the tail is flushed by
    // finish(). False (with `err`) on failure, or if the writer was opened
    // without audio.
    bool addAudio(const float* interleaved, int64_t frames, std::string& err);
    // Total audio sample frames handed to addAudio() so far.
    int64_t audioFramesWritten() const;

    // Flush the encoder(s) and write the container trailer. Returns the total
    // frames written across every addEquirectFrame() / addNv12Frame(), or -1
    // (with `err`) on failure.
    int64_t finish(std::string& err);

private:
    struct Impl;
    std::unique_ptr<Impl> d;

    // Tail of addEquirectFrame: RenderRectilinear into the output-sized
    // buffer, RGBA->NV12, encode at the next shared PTS.
    bool encodeFrame(const uint8_t* eqRgba, int eqW, int eqH, const ReframeView& view,
                     std::string& err);
};

} // namespace exportpipe
