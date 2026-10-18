// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

// The demux / decode core shared by InsvDemux (CPU decode, RGB Image out) and
// VulkanVideoDecoder (Vulkan hwaccel, AVVkFrame out): an `.insv`'s format
// context, one persistent decoder per fisheye video stream ("lens"), a FIFO of
// decoded frames per lens, and the strictly-forward pump that fills them.
// Seeking is frame-exact, and a target a little ahead of the last frame read,
// with no keyframe in between, decodes on from there instead of seeking back
// to the keyframe (frame steps, short forward scrubs).
//
// The owner picks the decoder set-up (hwaccel or not) through open()'s hook
// and turns the AVFrames it gets back into whatever it hands out. Part of
// insv_demux (needs FFmpeg). Not thread-safe.

struct AVCodec;
struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVPacket;
struct AVStream;

namespace insv {

// The keyframe of `stream` nearest `timestampMs`, in ms rounded up (see
// InsvDemux::nearestKeyframeMs).
int64_t NearestKeyframeMs(AVFormatContext* fmt, int stream, int64_t timestampMs);
// The keyframes of `stream` at or before / at or after `timestampMs`, in ms
// rounded up as NearestKeyframeMs; -1 where the index has none.
void KeyframesAroundMs(AVFormatContext* fmt, int stream, int64_t timestampMs, int64_t& before,
                       int64_t& after);
// The last keyframe of `stream` at or before `timestampMs`, in seconds; -1 if
// the index has none (unknown).
double KeyframeAtOrBeforeSec(AVFormatContext* fmt, int stream, int64_t timestampMs);

struct InsvStreamInfo {
    int width = 0;
    int height = 0;
    double fps = 0.0;
    int64_t durationMs = 0;
};

class LensDemuxer {
public:
    // Called per video stream between avcodec_parameters_to_context and
    // avcodec_open2, to set the decoder up. False (with `err` set) fails open().
    using DecoderSetup =
        std::function<bool(const AVStream* st, const AVCodec* dec, AVCodecContext* ctx, std::string& err)>;

    LensDemuxer() = default;
    ~LensDemuxer();
    LensDemuxer(const LensDemuxer&) = delete;
    LensDemuxer& operator=(const LensDemuxer&) = delete;

    // Opens `path` and a decoder per video stream. False with `err` set on
    // failure, or if there is no video stream.
    bool open(const std::string& path, const DecoderSetup& setup, std::string& err);
    void close();

    int lensCount() const { return static_cast<int>(m_dec.size()); }
    const InsvStreamInfo& lens(int i) const { return m_lenses.at(i); }

    // Frame-exact: the next read returns the first frame at or after
    // `timestampMs` (see the class comment for the decode-on shortcut).
    bool seekTo(int64_t timestampMs, std::string& err);
    // Lens 0's index; see insv::NearestKeyframeMs / KeyframesAroundMs.
    int64_t nearestKeyframeMs(int64_t timestampMs) const;
    void keyframesAroundMs(int64_t timestampMs, int64_t& before, int64_t& after) const;

    // The next lens-0 / lens-1 frame pair in play order; the caller owns both
    // (av_frame_free). `*ptsMs` (may be null) gets lens 0's time. False with
    // `err` empty at end of stream, `err` set on error. Needs 2 lenses.
    bool readNextPair(AVFrame*& out0, AVFrame*& out1, int64_t* ptsMs, std::string& err);
    // The next frame of `lens` alone, for a single-stream file; the caller
    // owns it. False as readNextPair.
    bool readNextFrame(int lens, AVFrame*& out, std::string& err);

    // Lens `lens`'s next decoded frame without consuming it (pumps the fifo
    // as needed); nullptr at EOS or on error. For reading frame properties
    // before playback -- the frame is still handed out by the next read.
    AVFrame* peekFrame(int lens);

private:
    void clearFifos();
    void resetDecoders(); // flush + clear fifos, keep contexts
    int streamToLens(int streamIndex) const;
    void drainDecoder(int lens);     // pull ready frames into m_fifo[lens]
    bool pumpOnce(std::string& err); // read one packet / advance draining
    void dropStale(int lens);        // frames before the seek target
    AVFrame* popFront(int lens);

    AVFormatContext* m_fmt = nullptr;
    AVPacket* m_pkt = nullptr;
    std::vector<int> m_videoStreamIndex; // lens -> AVStream index
    std::vector<InsvStreamInfo> m_lenses;
    std::vector<AVCodecContext*> m_dec;       // lens -> persistent decoder
    std::vector<std::deque<AVFrame*>> m_fifo; // lens -> decoded, not yet consumed
    std::vector<char> m_streamEof;            // lens -> decoder drained (vector<bool> avoided)
    bool m_drainStarted = false;              // flush packet sent to the decoders
    double m_seekTargetSec = -1.0;            // drop frames earlier than this
    double m_lastReadSec = -1.0;              // pts of the last frame (pair) read; -1 after open / a seek
};

} // namespace insv
