// SPDX-License-Identifier: MIT OR Apache-2.0
#include "InsvMediaInfo.h"

#include "InsvTrailer.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/pixdesc.h>
}

#include <filesystem>
#include <system_error>

namespace insv {
namespace {

// Enough IMU samples for a rate estimate without reading a long clip's whole
// 0x300 record (tens of MB per hour).
constexpr std::size_t kImuProbeSamples = 4000;

std::string profileName(const AVCodecParameters* p) {
    const char* name = avcodec_profile_name(p->codec_id, p->profile);
    return name ? name : "";
}

std::string tag(AVDictionary* dict, const char* key) {
    const AVDictionaryEntry* e = av_dict_get(dict, key, nullptr, 0);
    return e && e->value ? e->value : "";
}

// VID_<date>_<time>_00_<n>.insv -> LRV_<date>_<time>_01_<n>.lrv, as the X5
// names them on the card (docs/formats/insta360-x5.md §1).
std::string findProxy(const std::filesystem::path& insv) {
    std::string name = insv.filename().string();
    if (name.rfind("VID_", 0) != 0)
        return "";
    const auto lensPos = name.find("_00_");
    if (lensPos == std::string::npos)
        return "";
    name.replace(0, 3, "LRV");
    name.replace(lensPos, 4, "_01_");
    std::filesystem::path lrv = insv.parent_path() / name;
    lrv.replace_extension(".lrv");
    std::error_code ec;
    return std::filesystem::exists(lrv, ec) ? lrv.string() : "";
}

} // namespace

std::string MediaInfo::captureMode() const {
    if (video.size() != 2 || video[0].width != video[0].height)
        return "";
    switch (video[0].width) {
    case 3840: return "8K";
    case 2880: return "5.7K";
    case 1920: return "4K";
    default: return "";
    }
}

bool ProbeMediaInfo(const std::string& path, MediaInfo& out, std::string& err) {
    out = MediaInfo{};
    out.path = path;
    std::error_code ec;
    out.fileBytes = static_cast<int64_t>(std::filesystem::file_size(path, ec));
    if (ec) {
        err = std::filesystem::exists(path, ec) ? "cannot read the file" : "file not found";
        return false;
    }

    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) {
        err = "not a readable media file";
        return false;
    }
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        avformat_close_input(&fmt);
        err = "cannot read the stream info";
        return false;
    }
    out.container = fmt->iformat && fmt->iformat->name ? fmt->iformat->name : "";
    out.durationMs = fmt->duration > 0 ? fmt->duration / (AV_TIME_BASE / 1000) : 0;
    out.totalBitRate = fmt->bit_rate;
    out.creationTime = tag(fmt->metadata, "creation_time");

    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        const AVStream* st = fmt->streams[i];
        const AVCodecParameters* p = st->codecpar;
        if (p->codec_type == AVMEDIA_TYPE_VIDEO) {
            MediaVideoStream v;
            v.codec = avcodec_get_name(p->codec_id);
            v.profile = profileName(p);
            v.width = p->width;
            v.height = p->height;
            const AVRational r = st->avg_frame_rate.num ? st->avg_frame_rate : st->r_frame_rate;
            v.fps = r.den ? av_q2d(r) : 0.0;
            v.bitRate = p->bit_rate;
            const auto pf = static_cast<AVPixelFormat>(p->format);
            if (const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(pf)) {
                v.pixelFormat = d->name;
                v.bitDepth = d->comp[0].depth;
                if (!(d->flags & AV_PIX_FMT_FLAG_RGB) && d->nb_components >= 3)
                    v.chroma = d->log2_chroma_w == 1 ? (d->log2_chroma_h == 1 ? "4:2:0" : "4:2:2")
                                                     : "4:4:4";
            }
            // yuvj* formats are full range by definition, whatever color_range says.
            v.fullRange = p->color_range == AVCOL_RANGE_JPEG || pf == AV_PIX_FMT_YUVJ420P ||
                          pf == AV_PIX_FMT_YUVJ422P || pf == AV_PIX_FMT_YUVJ444P;
            if (p->color_space != AVCOL_SPC_UNSPECIFIED)
                v.colorSpace = av_color_space_name(p->color_space);
            v.frameCount = st->nb_frames;
            out.video.push_back(v);
        } else if (p->codec_type == AVMEDIA_TYPE_AUDIO) {
            MediaAudioStream a;
            a.codec = avcodec_get_name(p->codec_id);
            a.profile = profileName(p);
            a.sampleRate = p->sample_rate;
            a.channels = p->ch_layout.nb_channels;
            char layout[64] = {};
            if (av_channel_layout_describe(&p->ch_layout, layout, sizeof layout) > 0)
                a.channelLayout = layout;
            a.bitRate = p->bit_rate;
            out.audio.push_back(a);
        }
    }
    avformat_close_input(&fmt);

    if (const auto notes = ReadInsvMakerNotes(path)) {
        out.cameraModel = notes->model;
        out.serialNumber = notes->serial_number;
        out.firmware = notes->firmware;
    }
    if (const auto tele = ReadInsvTelemetry(path, kImuProbeSamples)) {
        if (tele->imu.size() >= 2) {
            // Raw IMU timestamps are ~microseconds (docs/formats/insta360-x5.md §6.2).
            const double spanSec =
                (tele->imu.back().timestamp - tele->imu.front().timestamp) / 1e6;
            if (spanSec > 0)
                out.imuRateHz = (tele->imu.size() - 1) / spanSec;
        }
        out.exposureRecords = static_cast<int64_t>(tele->exposure.size());
        out.rollingShutterMs = tele->rolling_shutter_ms;
        out.colorProfile = tele->gamma_mode;
        out.gpsRecords = static_cast<int64_t>(tele->gps_records);
        out.gpsFixes = static_cast<int64_t>(tele->gps_fixes);
        out.gpsRateHz = tele->gps_rate_hz;
        out.gpsTrackSec = tele->gps_track_s;
    }
    out.proxyPath = findProxy(path);
    return true;
}

} // namespace insv
