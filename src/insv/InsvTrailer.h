// SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace insv {

// Insta360's own maker-notes fields from an .insv file's proprietary trailer.
// `parameters` is the short lens calibration ("number of lenses, 6-axis
// orientation of each lens, raw resolution" per the format's public
// documentation — see ReadInsvMakerNotes below). Kept raw for reference only:
// the stitch uses the extended MEI string (ReadInsvExtendedCalibrationString).
// One entry of the per-accessory lens-profile table (0x101 field 145,
// docs/formats/insta360-x5.md §5.4): `poly` maps the off-axis angle in degrees to the
// image height on the sensor in mm (sum poly[i] * theta^i), `fov_deg` is the
// usable field of view. Names seen on the X5: "bare", "ProtectorA",
// "ProtectorS", "ProtectorAS", "InvisibleDiveWater", "InvisibleDiveAir".
struct LensProfile {
    std::string name;
    double poly[6] = {0, 0, 0, 0, 0, 0};
    double fov_deg = 0; // 0 = not listed
};

struct InsvMakerNotes {
    std::string serial_number;
    std::string model;
    std::string firmware;
    std::string parameters;
    std::vector<LensProfile> lens_profiles; // empty on files without field 145
    // Field 27 `window_crop_info` dst width: the focal reference frame the
    // extended MEI calibration's fx/fy live in (5312 on the X5, 7680 on the
    // X6; docs/formats/insta360-x5.md §5.1). 0 when absent.
    double crop_dst_w = 0;
    // Field 22 `gamma_mode` ("standard", "I_Log", "Dolby_Vision";
    // docs/formats/insta360-x6.md): the marker of log/HDR recording -- I_Log
    // clips still tag bt709 in the stream VUI, Dolby_Vision clips tag
    // bt2020 + HLG (arib-std-b67). Empty when absent.
    std::string gamma_mode;
};

// The profile named `name` in `notes`, or nullptr.
const LensProfile* FindLensProfile(const InsvMakerNotes& notes, const std::string& name);

// One inertial-measurement sample from the trailer's 0x300 record: the camera's
// accelerometer + gyroscope at a point in time. GyroStabilizer integrates
// these into a per-frame orientation; the accel gives it "down".
struct ImuSample {
    // Device monotonic clock, raw units. ExifTool displays this as
    // `timestamp / 1000.0` ("Time Code"); on an X5 the raw step between samples
    // is ~1000 (≈1 kHz IMU), so the raw unit is ~microseconds. Same clock as
    // ExposureSample::timestamp and the video-frame timestamps, so IMU samples
    // and frames can be aligned on it.
    uint64_t timestamp = 0;
    double accel[3] = {0, 0, 0};  // accelerometer, g   (x, y, z)
    double gyro[3] = {0, 0, 0};   // angular velocity, rad/s (x, y, z)
};

// One exposure sample from the trailer's 0x400 record — the rolling-shutter
// exposure time in effect at `timestamp` (needed to model motion blur / CMOS
// readout skew when de-rotating with the gyro).
struct ExposureSample {
    uint64_t timestamp = 0;    // same clock as ImuSample::timestamp
    double exposure_time = 0;  // seconds
};

// Timed telemetry recovered from the trailer. `imu` comes from record 0x300,
// `exposure` from record 0x400. Either list can be empty if the file doesn't
// carry that record (older firmware, .insp stills, a remuxed chunk that only
// had the calibration trailer spliced on).
struct InsvTelemetry {
    std::vector<ImuSample> imu;
    std::vector<ExposureSample> exposure;

    // Timing/IMU fields from the 0x101 metadata protobuf (docs/formats/insta360-x5.md
    // §5.1); 0 when the file doesn't carry them.
    // Field 24: device-clock timestamp of the first *recorded* video frame. The
    // IMU and the 0x400 exposure record start before recording (sensor
    // pre-roll), so this -- not exposure[0] -- is video t = 0 (§7.1).
    uint64_t first_frame_timestamp = 0;
    // Field 28: IMU latency, ms. Gyroflow's convention: an IMU sample's video
    // time is (ts - first_frame_timestamp)/1e6 - gyro_timestamp_ms/1e3.
    double gyro_timestamp_ms = 0;
    // Field 25: sensor rolling-shutter readout time, ms (varies per mode).
    double rolling_shutter_ms = 0;
    // Field 22: the colour profile the clip was filmed in -- "standard", or
    // "I_Log" for Insta360's log profile (§3.3; the stream itself is tagged
    // the same either way). Empty when absent.
    std::string gamma_mode;
    // Record 128 (TimeMap), written by Insta360 software on trimmed exports:
    // the original recording time (ms) of this file's video at t = 0. Field
    // 24 still names the *original* first frame, so a trimmed file's video
    // starts this much later on the device clock (2002 ms on the sample
    // "-trim" clip). 0 when absent. Only pure trims are modelled -- a
    // re-timed (speed-ramped) map is reduced to its median offset.
    double time_map_offset_ms = 0;
    // Record 0x700 (GPS, docs/formats/insta360-x5.md §8), written only when a GPS
    // source was connected. Only counted, never decoded into positions. All 0
    // when the file has no GPS record.
    std::size_t gps_records = 0; // 10 per second of video
    std::size_t gps_fixes = 0;   // records with an acquired ('A') position
    double gps_rate_hz = 0;      // how often the position updates (1 Hz on the X5)
    double gps_track_s = 0;      // seconds of track, gaps left out
};

// Parses the proprietary trailer Insta360 appends after the MP4 `moov` box in
// .insv/.insp files, to recover lens calibration without a live camera
// connection. This file-based extraction is what feeds the native stitch
// pipeline (ReadInsvExtendedCalibrationString ->
// insv::ParseExtendedCalibrationString -> insv::FisheyeStitcher).
//
// The trailer format (magic-anchored footer, backward-walked TLV records, and
// — on newer cameras including the X5 — a record-locator directory table in
// record 0x0) was reverse-engineered and is documented by the open-source
// ExifTool project (Image::ExifTool::QuickTimeStream::ProcessInsta360; the
// "Parameters" field specifically at
// https://exiftool.org/forum/index.php?msg=78942). This is an independent
// reimplementation of just the record types we need — 0x101 (maker notes),
// 0x300 (IMU), 0x400 (exposure) — from that documentation, not a port of
// ExifTool's code. GPS (0x700) is only counted (see InsvTelemetry), and the
// various unknown records are skipped.
//
// Returns std::nullopt if the file has no recognizable trailer (magic number
// mismatch) or the maker-notes record isn't found.
std::optional<InsvMakerNotes> ReadInsvMakerNotes(const std::string& file_path);

// Recovers the *extended* fisheye calibration string (the Mei/MEI model Insta360
// Studio stitches with -- see insv::LensCalibExt). Unlike the short "p2_" string
// in ReadInsvMakerNotes()->parameters, this one isn't in the 0x101 maker-notes
// record; it sits as plaintext ("2_<count>_<27 values/lens>_...") in a
// length-delimited protobuf field elsewhere in the trailer, past where the
// record walker dead-ends on the X5. This scans the raw trailer bytes for the
// richest such run (>= 55 numeric tokens). Returns std::nullopt if the file has
// no trailer or no extended string is present (older firmware / spliced
// calibration-only trailers) -- callers fall back to the short "p2_" form.
// Hand the result to insv::ParseExtendedCalibrationString.
std::optional<std::string> ReadInsvExtendedCalibrationString(const std::string& file_path);

// The camera's own preview picture from the trailer's 0x200 record
// (docs/formats/insta360-x5.md §4.3): on the X5 an already stitched equirectangular
// frame, 1280x640, NV12 (a full-size Y plane, then interleaved U/V at half
// size). Cheap to read -- only that record is loaded, not the video or the
// IMU. Returns std::nullopt when the file has no trailer, no such record, or
// one in another layout (other models store a JPEG or H.264 frame there).
struct InsvThumbnail {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> nv12; // width * height * 3 / 2 bytes
};
std::optional<InsvThumbnail> ReadInsvThumbnail(const std::string& file_path);

// Parses the trailer's timed-telemetry records (0x300 IMU, 0x400 exposure;
// 0x700 GPS is only counted).
// Returns std::nullopt only if the file has no recognizable trailer at all; a
// trailer that is present but carries neither record yields an InsvTelemetry
// with two empty lists.
//
// `max_imu_samples` caps how many IMU samples are read (0 = all). A multi-hour
// clip's 0x300 record is tens to a couple hundred MB of doubles; the cap is an
// escape hatch for callers that only need a coarse orientation track. Exposure
// records are always read in full (one per frame — small).
std::optional<InsvTelemetry> ReadInsvTelemetry(const std::string& file_path,
                                               std::size_t max_imu_samples = 0);

// Returns the raw bytes of the trailer itself -- everything from the end of
// the file's `moov` box to EOF (confirmed empirically: in a real X5 file
// this is exactly the proprietary `inst` box, contiguous through EOF; this
// function doesn't check the box's name, just that it's whatever comes after
// `moov`, since that's a more robust definition to rely on than the exact
// vendor box name if that ever changes). Returns an empty vector if no
// `moov` box is found or nothing follows it.
//
// The trailer's own internal framing is anchored purely relative to EOF (see
// ReadInsvMakerNotes's implementation), so splicing these bytes onto the end of
// ANY file makes that file's calibration parseable exactly as if it were the
// original. ReadInsvExtendedCalibrationString scans it for the extended
// calibration string.
std::vector<uint8_t> ExtractRawTrailer(const std::string& file_path);

} // namespace insv
