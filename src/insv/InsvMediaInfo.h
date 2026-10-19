// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Everything the Media information panel shows about a source clip: container
// and stream facts from FFmpeg, camera and telemetry facts from the trailer
// (docs/formats/insta360-x5.md). No decode, no stitch.
//
// Part of insv_demux (needs FFmpeg).

namespace insv {

struct MediaVideoStream {
    std::string codec;   // "hevc"
    std::string profile; // "Main"
    int width = 0;
    int height = 0;
    double fps = 0.0;
    int64_t bitRate = 0; // bits/s, 0 if unknown
    std::string pixelFormat; // "yuvj420p"
    int bitDepth = 0;
    std::string chroma; // "4:2:0"
    bool fullRange = false;
    std::string colorSpace; // "bt709"
    int64_t frameCount = 0;
};

struct MediaAudioStream {
    std::string codec;   // "aac"
    std::string profile; // "LC"
    int sampleRate = 0;
    int channels = 0;
    std::string channelLayout; // "stereo"
    int64_t bitRate = 0;
};

struct MediaInfo {
    std::string path;
    int64_t fileBytes = 0;
    std::string container; // "mov,mp4,m4a,3gp,3g2,mj2"
    int64_t durationMs = 0;
    int64_t totalBitRate = 0;
    std::string creationTime; // ISO 8601 from the container, "" if absent

    std::vector<MediaVideoStream> video; // an X5 .insv: one per lens
    std::vector<MediaAudioStream> audio;

    // From the trailer; empty / 0 when it has none.
    std::string cameraModel;
    std::string serialNumber;
    std::string firmware;
    double imuRateHz = 0.0; // 0 if the trailer has no IMU record
    int64_t exposureRecords = 0;
    double rollingShutterMs = 0.0;
    // The colour profile it was filmed in (trailer field 22): "standard",
    // "I_Log", ...; "" when the trailer doesn't say.
    std::string colorProfile;
    // The log profile it was filmed in, by its maker's name ("I-Log"), or ""
    // when it wasn't filmed in log (or the file doesn't say). A log clip looks
    // flat until the maker's LUT for that profile is applied. Other makers'
    // profiles (D-Log M, GP-Log) go here once their files are read.
    std::string logProfile() const { return colorProfile == "I_Log" ? "I-Log" : ""; }
    // GPS track (record 0x700): presence only, never positions -- the panel is
    // what people screenshot for bug reports.
    int64_t gpsRecords = 0; // 0 if the trailer has no GPS record
    int64_t gpsFixes = 0;   // records with an acquired position
    double gpsRateHz = 0.0; // position updates per second
    double gpsTrackSec = 0.0; // seconds of track, gaps left out

    // The camera's low-res proxy (LRV_*_01_*.lrv) next to the .insv.
    std::string proxyPath; // "" if not found

    // "8K", "5.7K" or "4K" for a dual-fisheye X5 clip (from the lens size),
    // else "". 5.7K+ records the same stream shape as 5.7K, so it can't be told
    // apart (docs/formats/insta360-x5.md §3.3).
    std::string captureMode() const;
};

// Returns false with `err` set if the file can't be opened as media.
bool ProbeMediaInfo(const std::string& path, MediaInfo& out, std::string& err);

} // namespace insv
