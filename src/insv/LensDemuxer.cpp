// SPDX-License-Identifier: MIT OR Apache-2.0
#include "LensDemuxer.h"

#include <algorithm>
#include <cmath>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
#include <libavutil/rational.h>
}

namespace insv {

namespace {
// A seek keeps the first frame within this of the target: targets are whole
// ms, frame times aren't, and a keyframe target (nearestKeyframeMs, rounded up)
// must not drop the keyframe itself.
constexpr double kSeekSlackSec = 0.001;

double ptsSec(const AVFrame* f, AVRational tb)
{
    const int64_t p = (f->best_effort_timestamp == AV_NOPTS_VALUE) ? f->pts : f->best_effort_timestamp;
    return (p == AV_NOPTS_VALUE) ? 0.0 : p * av_q2d(tb);
}
} // namespace

void KeyframesAroundMs(AVFormatContext* fmt, int stream, int64_t timestampMs, int64_t& before,
                       int64_t& after)
{
    before = after = -1;
    if (!fmt || stream < 0 || stream >= static_cast<int>(fmt->nb_streams))
        return;
    AVStream* st = fmt->streams[stream];
    const int64_t ts = av_rescale_q(timestampMs, AVRational{1, 1000}, st->time_base);
    auto ms = [&](const AVIndexEntry* e) {
        return e ? static_cast<int64_t>(std::ceil(e->timestamp * av_q2d(st->time_base) * 1000.0 - 1e-9))
                 : int64_t(-1);
    };
    before = ms(avformat_index_get_entry_from_timestamp(st, ts, AVSEEK_FLAG_BACKWARD));
    after = ms(avformat_index_get_entry_from_timestamp(st, ts, 0));
}

int64_t NearestKeyframeMs(AVFormatContext* fmt, int stream, int64_t timestampMs)
{
    if (!fmt || stream < 0 || stream >= static_cast<int>(fmt->nb_streams))
        return timestampMs;
    AVStream* st = fmt->streams[stream];
    const int64_t ts = av_rescale_q(timestampMs, AVRational{1, 1000}, st->time_base);
    // The keyframe at-or-before and the one after; keyframes only (no
    // AVSEEK_FLAG_ANY).
    const AVIndexEntry* before = avformat_index_get_entry_from_timestamp(st, ts, AVSEEK_FLAG_BACKWARD);
    const AVIndexEntry* after = avformat_index_get_entry_from_timestamp(st, ts, 0);
    const AVIndexEntry* best = before;
    if (!best || (after && after->timestamp - ts < ts - before->timestamp))
        best = after;
    if (!best)
        return timestampMs;
    const double ms = best->timestamp * av_q2d(st->time_base) * 1000.0;
    return static_cast<int64_t>(std::ceil(ms - 1e-9));
}

double KeyframeAtOrBeforeSec(AVFormatContext* fmt, int stream, int64_t timestampMs)
{
    if (!fmt || stream < 0 || stream >= static_cast<int>(fmt->nb_streams))
        return -1.0;
    AVStream* st = fmt->streams[stream];
    const int64_t ts = av_rescale_q(timestampMs, AVRational{1, 1000}, st->time_base);
    const AVIndexEntry* e = avformat_index_get_entry_from_timestamp(st, ts, AVSEEK_FLAG_BACKWARD);
    return e ? e->timestamp * av_q2d(st->time_base) : -1.0;
}

LensDemuxer::~LensDemuxer() { close(); }

void LensDemuxer::clearFifos()
{
    for (auto& q : m_fifo) {
        while (!q.empty()) {
            AVFrame* f = q.front();
            av_frame_free(&f);
            q.pop_front();
        }
    }
}

void LensDemuxer::close()
{
    clearFifos();
    for (auto*& d : m_dec)
        avcodec_free_context(&d);
    m_dec.clear();
    m_fifo.clear();
    m_streamEof.clear();
    if (m_pkt)
        av_packet_free(&m_pkt);
    if (m_fmt)
        avformat_close_input(&m_fmt);
    m_videoStreamIndex.clear();
    m_lenses.clear();
    m_drainStarted = false;
    m_seekTargetSec = -1.0;
    m_lastReadSec = -1.0;
}

bool LensDemuxer::open(const std::string& path, const DecoderSetup& setup, std::string& err)
{
    close();
    if (avformat_open_input(&m_fmt, path.c_str(), nullptr, nullptr) < 0) {
        err = "avformat_open_input failed for " + path;
        return false;
    }
    if (avformat_find_stream_info(m_fmt, nullptr) < 0) {
        err = "avformat_find_stream_info failed";
        close();
        return false;
    }
    for (unsigned i = 0; i < m_fmt->nb_streams; ++i) {
        AVStream* st = m_fmt->streams[i];
        if (st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO)
            continue;
        InsvStreamInfo info;
        info.width = st->codecpar->width;
        if (const AVPixFmtDescriptor* d =
                av_pix_fmt_desc_get(static_cast<AVPixelFormat>(st->codecpar->format)))
            info.bitDepth = d->comp[0].depth;
        info.height = st->codecpar->height;
        const AVRational fr = st->avg_frame_rate;
        if (fr.num <= 0 || fr.den <= 0) {
            err = "no frame rate in video stream " + std::to_string(i) + " of " + path;
            close();
            return false;
        }
        info.fps = av_q2d(fr);
        if (const AVPixFmtDescriptor* pd =
                av_pix_fmt_desc_get(static_cast<AVPixelFormat>(st->codecpar->format)))
            info.bitDepth = pd->comp[0].depth;
        if (st->duration != AV_NOPTS_VALUE)
            info.durationMs = static_cast<int64_t>(st->duration * av_q2d(st->time_base) * 1000.0);
        else if (m_fmt->duration != AV_NOPTS_VALUE)
            info.durationMs = m_fmt->duration / (AV_TIME_BASE / 1000);

        const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
        if (!dec) {
            err = "no decoder for video stream " + std::to_string(i);
            close();
            return false;
        }
        AVCodecContext* ctx = avcodec_alloc_context3(dec);
        avcodec_parameters_to_context(ctx, st->codecpar);
        if (setup && !setup(st, dec, ctx, err)) {
            avcodec_free_context(&ctx);
            close();
            return false;
        }
        if (avcodec_open2(ctx, dec, nullptr) < 0) {
            avcodec_free_context(&ctx);
            err = "avcodec_open2 failed for video stream " + std::to_string(i);
            close();
            return false;
        }
        m_videoStreamIndex.push_back(static_cast<int>(i));
        m_lenses.push_back(info);
        m_dec.push_back(ctx);
    }
    if (m_dec.empty()) {
        err = "no video streams in " + path;
        close();
        return false;
    }
    m_fifo.resize(m_dec.size());
    m_streamEof.assign(m_dec.size(), 0);
    m_pkt = av_packet_alloc();
    return true;
}

int LensDemuxer::streamToLens(int streamIndex) const
{
    for (size_t l = 0; l < m_videoStreamIndex.size(); ++l)
        if (m_videoStreamIndex[l] == streamIndex)
            return static_cast<int>(l);
    return -1;
}

void LensDemuxer::drainDecoder(int lens)
{
    for (;;) {
        AVFrame* frm = av_frame_alloc();
        const int r = avcodec_receive_frame(m_dec[lens], frm);
        if (r < 0) { // EAGAIN, EOF or an error
            av_frame_free(&frm);
            if (r == AVERROR_EOF)
                m_streamEof[lens] = 1;
            return;
        }
        m_fifo[lens].push_back(frm);
    }
}

bool LensDemuxer::pumpOnce(std::string& err)
{
    if (m_drainStarted) {
        for (size_t l = 0; l < m_dec.size(); ++l)
            drainDecoder(static_cast<int>(l));
        return true;
    }
    int r = av_read_frame(m_fmt, m_pkt);
    if (r < 0) {
        for (auto* d : m_dec)
            avcodec_send_packet(d, nullptr);
        m_drainStarted = true;
        for (size_t l = 0; l < m_dec.size(); ++l)
            drainDecoder(static_cast<int>(l));
        return true;
    }
    const int lens = streamToLens(m_pkt->stream_index);
    if (lens >= 0) {
        int s = avcodec_send_packet(m_dec[lens], m_pkt);
        if (s == AVERROR(EAGAIN)) {
            drainDecoder(lens); // decoder full -- make room, then retry once
            s = avcodec_send_packet(m_dec[lens], m_pkt);
        }
        if (s < 0 && s != AVERROR(EAGAIN)) {
            av_packet_unref(m_pkt);
            err = "avcodec_send_packet failed on lens " + std::to_string(lens);
            return false;
        }
        drainDecoder(lens);
    }
    av_packet_unref(m_pkt);
    return true;
}

void LensDemuxer::resetDecoders()
{
    for (auto* d : m_dec)
        avcodec_flush_buffers(d);
    clearFifos();
    m_drainStarted = false;
    std::fill(m_streamEof.begin(), m_streamEof.end(), 0);
}

void LensDemuxer::keyframesAroundMs(int64_t timestampMs, int64_t& before, int64_t& after) const
{
    KeyframesAroundMs(m_fmt, m_videoStreamIndex.empty() ? -1 : m_videoStreamIndex[0], timestampMs,
                      before, after);
}

int64_t LensDemuxer::nearestKeyframeMs(int64_t timestampMs) const
{
    return NearestKeyframeMs(m_fmt, m_videoStreamIndex.empty() ? -1 : m_videoStreamIndex[0],
                             timestampMs);
}

bool LensDemuxer::seekTo(int64_t timestampMs, std::string& err)
{
    if (m_dec.empty()) {
        err = "seekTo before open";
        return false;
    }
    // Ahead of the last frame read with no keyframe in between: the seek would
    // land on the keyframe this GOP started from and decode more frames than
    // carrying on from here (frame steps, short forward scrubs).
    const double targetSec = timestampMs / 1000.0;
    if (m_lastReadSec >= 0 && targetSec > m_lastReadSec + kSeekSlackSec) {
        const double kf = KeyframeAtOrBeforeSec(m_fmt, m_videoStreamIndex[0], timestampMs);
        if (kf >= 0 && kf <= m_lastReadSec + kSeekSlackSec) {
            m_seekTargetSec = targetSec;
            return true;
        }
    }
    m_lastReadSec = -1.0;
    const int64_t targetUs = timestampMs > 0 ? timestampMs * (AV_TIME_BASE / 1000) : 0;
    if (av_seek_frame(m_fmt, -1, targetUs, AVSEEK_FLAG_BACKWARD) < 0) {
        err = "av_seek_frame failed at " + std::to_string(timestampMs) + " ms";
        return false;
    }
    resetDecoders();
    // Frame-exact from 1 ms on (frame stepping reaches frame 1 at ~34 ms).
    m_seekTargetSec = timestampMs > 0 ? timestampMs / 1000.0 : -1.0;
    return true;
}

void LensDemuxer::dropStale(int lens)
{
    const AVRational tb = m_fmt->streams[m_videoStreamIndex[lens]]->time_base;
    while (!m_fifo[lens].empty() && m_seekTargetSec >= 0 &&
           ptsSec(m_fifo[lens].front(), tb) + kSeekSlackSec < m_seekTargetSec) {
        AVFrame* f = popFront(lens);
        av_frame_free(&f);
    }
}

AVFrame* LensDemuxer::popFront(int lens)
{
    AVFrame* f = m_fifo[lens].front();
    m_fifo[lens].pop_front();
    return f;
}

bool LensDemuxer::readNextPair(AVFrame*& out0, AVFrame*& out1, int64_t* ptsMs, std::string& err)
{
    if (m_dec.size() < 2) {
        err = "readNextPair requires 2 lens streams, have " + std::to_string(m_dec.size());
        return false;
    }
    for (;;) {
        dropStale(0);
        dropStale(1);
        if (!m_fifo[0].empty() && !m_fifo[1].empty())
            break;
        const bool ex0 = m_streamEof[0] && m_fifo[0].empty();
        const bool ex1 = m_streamEof[1] && m_fifo[1].empty();
        if (ex0 || ex1) {
            err.clear(); // clean end of stream
            return false;
        }
        if (!pumpOnce(err))
            return false;
    }
    out0 = popFront(0);
    out1 = popFront(1);
    m_seekTargetSec = -1.0; // satisfied by this pair
    m_lastReadSec = ptsSec(out0, m_fmt->streams[m_videoStreamIndex[0]]->time_base);
    if (ptsMs)
        *ptsMs = static_cast<int64_t>(std::llround(m_lastReadSec * 1000.0));
    return true;
}

bool LensDemuxer::readNextFrame(int lens, AVFrame*& out, std::string& err)
{
    if (lens < 0 || lens >= static_cast<int>(m_dec.size())) {
        err = "lens index " + std::to_string(lens) + " out of range";
        return false;
    }
    for (;;) {
        dropStale(lens);
        if (!m_fifo[lens].empty())
            break;
        if (m_streamEof[lens]) {
            err.clear();
            return false;
        }
        if (!pumpOnce(err))
            return false;
    }
    out = popFront(lens);
    m_seekTargetSec = -1.0;
    // The shortcut in seekTo reads lens 0's keyframes.
    m_lastReadSec = lens == 0 ? ptsSec(out, m_fmt->streams[m_videoStreamIndex[0]]->time_base) : -1.0;
    return true;
}

} // namespace insv
