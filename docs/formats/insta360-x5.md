# Insta360 X5 file format — `.insv` / `.lrv` reference

Everything this project knows about the Insta360 X5's recorded video files: the
MP4 container, the video/audio streams, the proprietary `inst` trailer, the
telemetry in it (gyro, accelerometer, exposure, GPS), and the lens calibration.
It collects the findings from `src/insv/`, the spikes/tools, the project's
development notes, a direct re-inspection of the sample footage, and a
cross-check against the external sources in §13 (2026-09-24).

The other cameras' formats are in their own files next to this one, indexed
in [`README.md`](README.md); they describe how each differs from this one.

**Confidence markers** used below:
- ✅ **verified**: parsed by our code and cross-checked (ExifTool byte-for-byte,
  ffprobe, several clips agreeing, and/or an independent implementation)
- 📖 **documented externally**: stated by an independent source (§13) and
  consistent with our files, but not yet exercised by our code
- ⚠️ **inferred**: strong evidence, but not validated end to end
- ❓ **unknown**

The main external sources are Gyroflow's **telemetry-parser**
(`src/insta360/`), which has a full reader for this trailer including
**the protobuf schema of the metadata record**; **ExifTool**
(`QuickTimeStream.pl`, `ProcessInsta360`); BenjaminHenriksson's
**insv-stitch**; the Insta360 X5 online manual; and the TDK ICM-40609-D
datasheet. We use only interoperability documentation and our own files. No
Insta360 SDK disassembly was used, and none should be.

Reference clips (`sample_footage/`, gitignored), all from one camera, firmware
`v1.5.6_build1`.

| clip | length | notes |
|---|---|---|
| `Old/VID_20250904_193105_00_002.insv` (+ `LRV_…_01_002.lrv`) | 42.8 s | stick mount, GPS, main test clip |
| `Old/VID_20250828_192900_00_001.insv` | ~9.5 min | curves clip, **two AAC streams** (camera mic + Mic Air), no GPS |
| `Old/VID_20250828_201257_00_006.insv` | ~10.7 min | 15 GB, GPS for the first ~48 s only (§8), different rolling-shutter value |
| `Analysis/VID_20250927_144221_00_019-trim.insv` / `_020-trim` | 30 min + 7.4 min | one recording split into two files, re-written by Insta360 software, handlebar mount |

---

## 1. Files on the card

```
DCIM/Camera01/VID_20250904_193105_00_002.insv   full-res dual-fisheye master
DCIM/Camera01/LRV_20250904_193105_01_002.lrv    low-res proxy of the same take
MISC/Camera01/VID_20250904_193105_00_002.insv.pb   protobuf sidecar (§9)
```

- **Name pattern** 📖 ✅ (Insta360 X5 manual): *prefix + date/time +
  lens-direction digit + master/proxy digit + sequence number*.
  - Lens-direction digit: `0` = back lens (away from the screen) and `1` =
    screen-side lens. Master/proxy digit: `0` = master and `1` = proxy.
  - So `_00_` is the master and `_01_` the proxy. `_10_`/`_11_` exist for
    single-lens and photo modes.
  - The X5 (like the X4) writes **one** `.insv` holding both lenses. The X3
    wrote one file per lens.
  - FreeFrame recordings use a `PRO_VID` prefix. Photos are `IMG_…` (`.insp`,
    `.jpg`, `.dng`).
  - The date/time is the recording start in camera-local time. `NNN` is the
    camera's running counter.
- **Long recordings are split** 📖 ✅: every **29 min 59 s**, per Insta360
  support (X5/X4/X3). The recording continues in a new file with the same
  date/time prefix and the next counter: our 37-minute take became `…_019`
  (1797.8 s, 42 GB) and `…_020` (442.6 s). Files aren't limited to 4 GB; 13–42
  GB single files exist. The continuation's metadata names the take's first
  file (§5.1 field 26).
- The metadata also records the file's own card path
  (`/DCIM/Camera01/VID_….insv`).

## 2. MP4 container

Both `.insv` and `.lrv` are plain ISO-BMFF / QuickTime files that FFmpeg opens
without special handling. After the standard boxes, the camera appends one
proprietary box. Top-level layout, as camera-written ✅:

```
ftyp  (28 B)   major_brand "avc1", compatible "avc1isom" (even for HEVC)
wide  (8 B)
mdat           all A/V samples
moov           track tables; udta holds a ThumbnailTIFF (~1.38 MB)
inst           Insta360 trailer, runs to EOF (12.5 MB on a 43 s clip)
```

- `moov` comes **after** `mdat`, so the file isn't "fast-start".
- A file re-written by Insta360 software (the `_020-trim` clip) had no `wide`
  box but still carried the trailer after `moov`. `ExtractRawTrailer()`
  therefore defines the trailer as "everything after `moov`".
- Container timescale is 30000. Track handlers are named `INS.HVC`, `INS.AVC`
  and `INS.AAC`.

## 3. Streams

### 3.1 `.insv` (master) ✅

| index | what | details |
|---|---|---|
| 0 | lens 0 fisheye video | HEVC Main, `hvc1`, **3840×3840**, 29.97 fps (30000/1001), ~94 Mb/s, no B-frames, GOP 60 (keyframe every 2 s) |
| 1 | lens 1 fisheye video | identical parameters to stream 0 |
| 2 | audio | AAC-LC, 48 kHz stereo, ~189 kb/s, `default` |
| 3 | *(only with an external mic)* | a second AAC-LC 48 kHz stereo stream, not `default` |

- **Pixel format** ✅: `yuvj420p` — 8-bit 4:2:0, **full range**
  (`color_range=pc`), BT.709 primaries/transfer/matrix. Treat it as full range:
  feeding it to swscale as limited range lifts the blacks and crushes the
  whites (a 3.5 dB PSNR fix, 2026-09-01). The GPU path
  samples it as NV12 through a `VkSamplerYcbcrConversion` (BT.709, ITU_FULL).
- Each lens stream is a single fisheye circle filling the square frame, with
  radius ~1920 stream px. The two streams are frame-locked.
- insv-stitch calls stream 0 the "front" lens and stream 1 the "back" lens.
  The Insta360 naming (§1) calls lens `0` the back lens. Which lens counts as
  "front" is only a convention.
- The "8K" figure (7680×3840) is 2 × 3840². No stream has that size.
- **Two audio streams** (`VID_20250828_192900`): the camera mic and an
  Insta360 Mic Air external mic. They're separate sources, not duplicates
  (−1.3 dB vs −8.9 dB peak). `InsvAudioDemux::open` takes the stream's
  ordinal among the audio streams; orbis-reframe puts each stream on its
  own track, *Camera mic* or *External mic*, by the order below
  (`Track::roleOfStream`). Nothing in the container (`handler_name` =
  `INS.AAC` on both) says which stream is which mic, but on this clip the
  **first (`default`)
  audio stream is the Mic Air** and the second is the camera's own mic --
  identified by the user by ear (the Mic Air was mounted by the exhaust),
  and it fits the data: stream 0 carries by far the clearest engine
  harmonics (`tools/audio_filter_bench`, 2026-09-29). The `.lrv` proxy carries
  the same two streams in the same order.
  0x101 field 61 (`audio_mode`) is 9 on these clips; its value table only
  goes up to 6. It's 9 on the single-mic clips too, so it doesn't flag the
  external mic. 0x101 fields 120 (track count) and 121 (one entry per
  track) do show a second track, but don't say which track is which mic
  (§5.1).

### 3.2 `.lrv` (proxy) ✅

| index | what | details |
|---|---|---|
| 0 | video | H.264 High, `avc1`, **1664×832**, 29.97 fps, ~4 Mb/s, B-frames, GOP 30 |
| 1 | audio | AAC-LC 48 kHz stereo |

- The LRV is **not stitched**. It holds both fisheyes side by side, each in an
  832×832 half. Our lens 0 (`.insv` stream 0) is the **right** half.
- Same full-range BT.709 `yuvj420p` as the master.
- It can be a frame or two shorter than the master: 1280 vs 1282 frames. Its
  own metadata gives its own frame count.
- **It carries the full trailer**: the same IMU, exposure, GPS and
  calibration, byte-identical in content. Stabilization or calibration work can
  run from the LRV alone.
- Per Insta360, the LRV is for "playback, preview, AI recognition, and scene
  analysis".
- Not used by the editor yet (proxy playback is planned).

### 3.3 Capture modes ✅

The camera app (firmware v1.11.10) offers these 360 video modes:

| Resolution | Frame rates | HDR |
|---|---|---|
| 8K | 30 / 25 / 24 | on or off (off adds an "AdaptiveTone" on/off) |
| 5.7K+ | 30 / 25 / 24 | on or off |
| 5.7K | 60 / 50 / 48 / 30 / 25 / 24 | available at every rate |
| 4K | 120 / 100, and 60 / 50 / 48 / 30 / 25 / 24 | not at 120 / 100; available at 60 and below |

Measured from `sample_footage/Modes/` (2026-09-27, one clip per mode):

| Clip | Mode | Per lens | fps | Mb/s per lens | GOP |
|---|---|---|---|---|---|
| `102745_00_001` | 8K HDR | 3840×3840 | 29.97 | ~94 | 60 (2 s) |
| `102815_00_002` | 8K, HDR off | 3840×3840 | 29.97 | ~94 | 60 (2 s) |
| `102900_00_003` | 5.7K 60 HDR | 2880×2880 | 59.94 | ~104 | 120 (2 s) |
| `102925_00_004` | 4K 120 | 1920×1920 | 119.88 | ~94 | 120 (1 s) |
| `104454_00_005` | 5.7K+ 30 HDR | 2880×2880 | 29.97 | ~63 | 60 (2 s) |
| `185907_00_006` (`sample_footage/ilog/`, 2026-10-18) | 5.7K 30 I-Log | 2880×2880 | 29.97 | ~56 | 60 (2 s) |

- **Every mode is 8-bit** HEVC Main, full-range BT.709 `yuvj420p`, no
  B-frames. HDR is tone-mapped in the camera; nothing in the stream says
  HDR, and the HDR / non-HDR 8K clips probe identically. 5.7K+ has the same stream shape as 5.7K; only the
  bitrate differs. No 10-bit path is
  needed for these modes.
- **I-Log** (5.7K 60 and below, Insta360's manual) ✅ on `185907_00_006`:
  **8-bit** HEVC Main too, and tagged exactly like a standard clip
  (full-range BT.709 `yuvj420p`, BT.709 transfer) -- the stream doesn't say
  log; the trailer does: metadata field 22 `gamma_mode` is `I_Log` instead
  of `standard` (§4; read by `ReadInsvTelemetry` as `gamma_mode`, shown
  in Media information, and the Colour section asks for the LUT). The
  rest of the trailer (calibration, IMU, exposure) has the same shape. The
  picture is flat as log should be: on a 1280×720 forward view, luma
  53–125 (average 97), mean saturation ~8.5 -- after the X5 LUT 28–99
  (66), saturation ~19; the back view 73–214 -> 39–213. No visible
  banding through the LUT at 8 bits (indoor, smooth ceiling).
  📖 Insta360's I-Log download page offers the X5 a LUT of its own, as the
  one for **8-bit** I-Log (the other, for newer cameras' 10-bit I-Log, is
  the X6 set, X6 document). The LUT is
  `X5_I-Log_To_Rec.709_V1.0.cube`: a 65³ 3D `.cube`, `TITLE "X5 ILog to
  Rec709 3DLUT v1.0"`, no `DOMAIN` lines (so input 0–1), I-Log to Rec.709.
  The app applies it per clip (Colour ▸ LUT); the curve itself isn't
  modelled.
- The trailer is identical in shape across modes: the same calibration
  strings (stated at the 10752×5376 reference size and scaled to the
  stream), the same lens profiles, ~1006 Hz IMU, and one exposure record per
  frame (so 2× / 4× as many at 60 / 120 fps). The native stitch works
  unchanged at 2880² and 1920².
- The `.lrv` is always 1664×832 at **29.97 fps**, even for 60 / 120 fps
  masters (377 LRV frames vs 754 master frames on the 60 fps clip). A proxy
  can't stand in for high-fps playback frame for frame.
- Decode load is similar across modes: ~0.88 / ~1.0 / ~0.88 Gpx/s for
  8K30 / 5.7K60 / 4K120, but the per-frame budget is 33 / 16.7 / 8.3 ms.
  Worst-case seek decodes up to a whole GOP: 60 frames at 8K, 120 at 5.7K60
  and 4K120.

## 4. The `inst` trailer — framing

The trailer is anchored at **EOF**. Everything is little-endian.
Implemented in `src/insv/InsvTrailer.cpp` (`WalkTrailerRecords`). The
structure below is confirmed independently by ExifTool and telemetry-parser.

### 4.1 Footer (last 78 bytes) ✅

| offset | size | field |
|---|---|---|
| 0 | 6 | header of the last record (see §4.2) |
| 6 | 32 | padding (zero) |
| 38 | 4 | **trailer length** in bytes from trailer start to EOF (u32). This is the `inst` box size minus its 8-byte box header. |
| 42 | 4 | **trailer version**: `3` on the X5 📖 |
| 46 | 32 | magic, the ASCII text `8db42d694ccc418790edff439fe026bf` |

### 4.2 Records ✅ 📖

Each record is `payload` followed by a 6-byte header. The header comes
**after** the payload, so you walk the records backward from EOF. The header
is **`{format: u8, type: u8, size: u32}`**. Our code reads the first two bytes
as one LE u16 "id", which gives `type << 8 | format`: `0x0300` means type 3 in
format 0. `format` is `0` = binary, `1` = protobuf, `2` = JSON
(telemetry-parser `RecordFormat`). So **`0x101` = type 1 (Metadata) in
protobuf format**.

- **Sequential walk**: from the footer's header, the payload starts at
  `-78 - size` from EOF. The previous record's header sits in the 6 bytes
  before that payload, and so on.
- **Offsets table (type 0)**: on the X5 the footer's header points first to a
  type-0 "Offsets" record: a table of 10-byte entries
  `{type: u8, format: u8, size: u32, offset: u32}`. `offset` is where that
  record's payload starts, from the trailer start, and its header follows at
  `offset + size`. Note the byte order is `type, format` here but
  `format, type` in a record header. That explains the "swapped ids" we saw
  (header `0x0300` appears in the table as `0x0003`).
  - On the X5 the sequential chain dead-ends after type 2 in a run of empty
    records. **The telemetry records can only be reached through this table.**
  - telemetry-parser checks each record header against its table entry
    (matching type and size). Our walker just uses the table for position.
  - Empty entries are placeholders for types the clip doesn't have.

### 4.3 Record types

Type names are from telemetry-parser's `RecordType` / `ExtraType` 📖. Sizes
are from the 42.8 s clip `193105` (1282 video frames).

| hdr id | type | name | X5 size | contents | status |
|---|---|---|---|---|---|
| `0x0000` | 0 | Offsets | 310 | locator table (§4.2) | ✅ |
| `0x0101` | 1 | **Metadata** (protobuf) | ~4 KB | serial, model, firmware, calibration, timing, IMU config… (§5) | ✅ |
| `0x0200` | 2 | Thumbnail | 1 228 840 | 40-byte header (`u32 1, u32 record size, u32 1, u32 0, u32 width, u32 height`, zeros) + **1280×640 NV12**, an already **stitched equirectangular** frame from the start of the recording (ExifTool: `PreviewTIFF`; other models store JPEG or H.264 here). Read by `ReadInsvThumbnail` for the project library's cards | ✅ |
| `0x0300` | 3 | **Gyro** (IMU) | 877 440 | accel + gyro, ~1 kHz (§6) | ✅ |
| `0x0400` | 4 | **Exposure** | 20 928 | one per sensor frame (§7) | ✅ |
| — | 5 | ThumbnailExt | — | not on the X5 samples | |
| — | 6 | FramePts / TimelapseTimestamp | — | u64 per frame; timelapse only (ExifTool: `VideoTimeStamp`) | |
| `0x0700` | 7 | **GPS** | 22 790 | 53 B × 10 Hz, fix updated at 1 Hz (§8); only with a GPS source | ✅ |
| `0x0900` | 9 | **AAAData** (auto-exposure / white-balance / focus) | 61 536 | **48 B per recorded frame** (§4.4) | 📖 |
| `0x0a00` | 10 | Anchors / highlights | 35 | `{type u8, count u32}` lists (+ u64 timestamps). Here 7 types (1,2,3,4,16,17,18), all with count 0 | 📖 ✅ |
| `0x0b00` | 11 | AAASimulation | 342 294 | format unknown | 📖❓ |
| — | 12–18 | ExposureSecondary, Magnetic, Euler, SecGyro, Speed, TBox, Quaternions | — | not on the X5 samples (other models) | |
| `0x1600` | 22 | ? | 8 306 836 | 148-byte header (mentions 1664/832) + 3 × 1664×832×2 bytes | ❓ |
| `0x1c00` | 28 | ? | 12 793 | contains 0..255 byte ramps (tone curve / LUT?) | ❓ |
| `0x1d00` | 29 | ? | 1 953 | 9-byte `{u64 µs device timestamp, u8}`, ~5 Hz | ❓ |
| `0x3900` | 57 | ? | — | only in the software-rewritten `-trim` clip | ❓ |
| `0x8000` | 128 | **TimeMap** | 862 136 (`144221_019-trim`) | written by Insta360 software on trimmed/re-timed exports. Decoded ✅: header `{u32 32 (header size), u32 trimCount, u32 pairBytes, u32 frameCount, 16 B zero}`, then `trimCount × {f64 duration, source start, source end}` (ms), then `(video ms, original ms)` f64 pairs, one per frame. On the sample: 1 trim, source 2002.0 → 1 799 764.6 ms, pairs `(0, 2002.0), (33.37, 2035.37)…`. **Field 24 keeps naming the original first frame**, so sync must add the source offset (§7.1) | ✅ |

ExifTool decodes types 1 (partly), 2, 3, 4, 6 and 7. Its source notes that
0x900/0xa00/0xb00 also occur on X3 / ONE RS / Ace Pro. Our parser reads types
1, 2, 3 and 4 (and counts 7).

### 4.4 Type 9 — AAAData (48 B per frame) 📖

Layout per telemetry-parser: `u32 timestamp` (ms), `f32 ev_target`,
`f32 exp_time`, `u32 data_stat`, `u32 luma_struct`, then `7 × u32` temperature
or spare data. `luma_struct` packs:
- `luma_wg_grid` = bits 0–6
- `luma_wg_y` = bits 7–13
- `sum_wg_y` = bits 14–18
- `iso = (100 · (luma_struct >> 19)) >> 6`

On `193105`: ev/exp read 0, and `luma_struct = 0x02000000` gives **ISO 100**.
The first u32 of the trailing data counts down slowly (2297, 2296, …). Its
ms clock sits ~208 ms after the video device clock ❓, so don't use it for
sync.

## 5. Type 1 — Metadata (protobuf `ExtraMetadata`)

The payload is a protobuf message. telemetry-parser publishes its schema
(`extra_info.rs`, fields 1–65) 📖. Fields above 65 are newer and unnamed there;
their meanings below come from our own decoding ⚠️.

Our `ParseMakerNotes()` reads the first four length-delimited fields
(1, 2, 3, 5) by hand. That works because they come first; field 4
(`file_type`) is absent on the X5.

### 5.1 Field map (values from `193105`)

| # | name (telemetry-parser) | X5 value | notes |
|---|---|---|---|
| 1 | `serial_number` | `IAHEA25xxxxxxx` (14 characters) | ✅ |
| 2 | `camera_type` | `Insta360 X5` | ✅ |
| 3 | `fw_version` | `v1.5.6_build1` | ✅ |
| 5 | `offset` | 16-token short calibration | §5.2 ✅ |
| 7 | `creation_time` | `20250904193105` | local `YYYYMMDDhhmmss` |
| 9 | `file_size` | 1012672185 | size of the file *before* the trailer body (= `inst` box offset + 8) ✅ |
| 10 | `total_time` | 42 | seconds |
| 11 | `gps` | 3 doubles | start fix: lat, lon, alt |
| 14 | `gyro` | empty | (older models: one sample) |
| 17 | `original_offset` | = field 5 | factory copy. The non-"original" variants may be adjusted for accessories (`OffsetConvertState`: WaterProof, DivingWater/Air, Protect, …) |
| 18 | `trigger_source` | 2 = RemoteControl (4 = BtRemote on `192900`) | 📖 |
| 19 | `dimension` | {3840, 3840} | per-lens stream size ✅ |
| 20 | `frame_rate` | 30 | nominal |
| 22 | `gamma_mode` | `standard`; `I_Log` in I-Log | colour profile ✅ (both values seen) |
| **24** | **`first_frame_timestamp`** | 2468081984 | **device µs of the first recorded video frame**; the sync anchor (§7.1) ✅ 📖 |
| **25** | **`rolling_shutter_time`** | **21.244** (11.837 on `201257`) | **frame readout time, ms**. Gyroflow's `frame_readout_time` reads it directly ✅ 📖 |
| 26 | `file_group_info` | {type 20, index 0, identify "/DCIM/Camera01/…_019.insv", total 0} | split-recording group. `index` is non-zero on continuations (2 on `_020`), and `identify` is the first file's path. Type 20 is past the published enum (max 17). |
| 27 | `window_crop_info` | {src 5376×5376, dst 5312×5312} | sensor crop window. `dst` is the focal reference: **`fx_video = fx · 3840/5312`**. Gyroflow computes `fx / (dst_w / dimension_w)` ✅ 📖 |
| **28** | **`gyro_timestamp`** | **1.6** | **IMU timing offset, ms**. Gyroflow subtracts it from every gyro/accel time (§6.2) 📖 |
| 29 | `is_has_gyro_timestamp` | true | enables field 28 |
| 31 | `gyro_calib` | 6 × f64 + u64 unix ts | per-unit IMU calibration: 6 small values (−0.0069 0.0025 0.0041 −0.0055 −0.0088 0.0043), likely gyro + accel bias; ts 0. Identical on every clip. Whether they're already applied to type 3 data is unknown ❓ |
| 36 | `first_gps_timestamp` | 1757003465191 | Unix ms |
| 41 | `is_selfie` | 0 | |
| 42 | `is_flowstate_online` | 0 | in-camera FlowState off |
| 44 | `resolution_size` | {3840, 3840} | |
| 46 | `cam_posture` | 0 | CameraRotate0 (0/90/180/270) 📖 |
| 47 / 48 / 49 | `fov_type` / `distance` / `fov` | 15 = Mega / 0.6 / 56.8° (`192900`: 3 = Ultrawide / 1.0 / 40°) | the in-camera or app **default reframe view** |
| 51 | `gyro_type` | **1 = `InsdevImuType40609`** | **the IMU is a TDK ICM-40609(-D)** 📖 |
| 53 | `offset_v2` | 34-token calibration | §5.3 |
| 54 | `offset_v3` | 40-token MEI calibration | §5.3. **This is what Gyroflow uses** |
| 55 / 56 | `original_offset_v2/_v3` | = 53 / 54 | |
| 57 | `focus_sensor` | 3 = All | |
| 61 | `audio_mode` | 9 | beyond the published enum |
| **62** | **`is_raw_gyro`** | **true** | **type 3 holds raw integer IMU counts** (§6.1) 📖 |
| 64 | `pts_type` | **2 = `VideoPtsEexposureFile`** | the video PTS derive from the exposure record, which confirms §7.1 📖 |
| **65** | **`gyro_cfg_info`** | **{acc_range: 32, gyro_range: 2000}** | **±32 g, ±2000 °/s**: the scale for raw counts (§6.1) 📖 |
| 93 | — | 42776 | duration, ms |
| 98 | — | {2468081984, 2510815317, 1757003465870, 1757003508603, 1282, 0, 20} | **first/last frame device µs, first/last frame Unix ms, frame count** (= ffprobe `nb_frames`). Absent in the rewritten `-trim` clips ✅ |
| 111 / 112 | — | 56-token calibration | full MEI with 13 distortion terms (§5.3). Probably "offset_v4 / original_offset_v4" |
| 120 | — | 1 (2 on `192900`) | **probably the number of audio tracks**: 2 only on the clip with the Mic Air, matching its two AAC streams (§3.1). 1 on every other sample, `.lrv` included |
| 121 | — | {1: 0, 2: 460} | repeated, **one entry per audio track** (the count in field 120). Field 2 is a signed varint: +378 to +460 on the camera-mic-only clips, −121 on *both* entries of `192900`. Meaning unknown ❓. Field 1 is 0 on every entry, so it doesn't say which track is which mic |
| 145 | — | named lens-profile table | per-accessory profiles `bare`, `ProtectorA`, `ProtectorS`, `ProtectorAS`, `InvisibleDiveWater`, `InvisibleDiveAir`: θ(deg)→image height(mm) polynomial + FOV each (**bare 200°**, protectors 195°, dive 190°). §5.4 ✅ |
| 143, 135 (=180), 146–163 | — | | ❓ |

The "bare 200°" profile matches our empirical finding that the fisheye
`radius` maps to θ ≈ 100°.

> **Code-comment correction:** `InsvTrailer.h` says the extended `2_…` string
> is "not in the 0x101 maker-notes record". It **is** in 0x101, as fields
> 53–56 and 111–112. `ReadInsvExtendedCalibrationString()` finds field 111
> anyway, because it scans for the longest `2_` run.

### 5.2 Short calibration string (`offset`, field 5)

```
2  2663.208 2705.380 2687.820 0.326 -0.017 89.997   2648.963 8071.630 2687.090 -0.283 -0.065 88.577   10752 5376 1137
N  |------------ lens 0 (6 values) ------------|   |------------ lens 1 (6 values) ------------|
```

**Not parsed any more** (2026-09-26): the stitch uses only the extended MEI
string (§5.3), which every X5 clip carries. The removed legacy parser read it
as per lens `cx cy radius tx ty tz`, then `rawH rawW extra`, with the circles
stacked vertically, plus empirical fixes: a ~90° `tz` "sensor clock", mirroring
both lenses, and θ_ref ≈ 100°. The 90° clock and the mirror are also needed on
the MEI path, so they're real coordinate conventions, not artefacts of that
reading.

**⚠️→📖 Better-supported reading:** per lens `radius cx cy yaw pitch roll`, in
a **side-by-side 10752 (W) × 5376 (H)** frame.
- Our evidence: the MEI strings (§5.3) put lens 0 at `cx=2705.380
  cy=2687.820` and lens 1 at `cx=8071.630`, which are the short string's
  2nd/3rd values per lens.
- insv-stitch's fallback path indexes it the same way. It reads `offset[2]`
  and `offset[3]` as cx/cy, takes `offset[7]/offset[1]` as the lens-1/lens-0
  focal ratio, and computes lens-1 cx as `offset[8] − offset[14]`
  (8071.63 − 5376).
- Moot for the app since the parser was removed; kept here as format
  reference.

The trailing token (`1137`) is unknown.

### 5.3 Extended calibration strings

All start with `2_` (the lens count) and end with one trailing integer: 132096
= 0x20400, 197632 = 0x30400, 394240 = 0x60400. telemetry-parser calls this
trailing token `flag`. Each lens block ends in `w h lensType` (`10752 5376
113`).

| field | name | tokens | per lens (telemetry-parser / insv-stitch layout) |
|---|---|---|---|
| 53/55 | `offset_v2` | 34 | 16: `radius cx cy yaw pitch roll tx ty tz` + 4 poly coefficients (`1.0 -0.189 0.274 -0.090`) + `w h type` ⚠️ |
| 54/56 | `offset_v3` | 40 | 19: **`xi fx fy cx cy yaw pitch roll tx ty tz k1 k2 k3 p1 p2 w h lensType`** 📖, then a trailing `flag` |
| 111/112 | (v4?) | 56 | 27: the full MEI model below ✅ 📖 (insv-stitch). Slightly different values (fx 4294.03 vs 4297.15), so probably a newer or refined calibration |

**56-token MEI layout** (`insv::LensCalibExt`, matching insv-stitch):

| idx | field | lens 0 value | notes |
|---|---|---|---|
| 0 | `xi` | 2.000000 | MEI mirror parameter |
| 1–2 | `fx fy` | 4294.03 4293.86 | sensor px, then `· 3840/5312` (field 27) |
| 3–4 | `cx cy` | 2703.31 2684.03 | in the 10752×5376 side-by-side frame; lens 1 `cx −= 5376`, then `· 3840/5376` |
| 5–7 | `yaw pitch roll` | 0.241 0.068 89.993 | degrees (see below: also the lens↔IMU rotation) |
| 8–10 | `tx ty tz` | 0 0 0; lens 1: 0.00077 0.00017 **−0.0318** | lens 1 is **~32 mm** behind lens 0 (the stereo baseline). Sign checked on seam parallax: lens 0's centre is the one ahead along lens 0's own axis; the flipped model matches nothing |
| 11–14 | `k1..k4` | 0.2297 1.602 −1.242 −3.170 | radial |
| 15 | reserved | 0 | |
| 16–17 | `p1 p2` | −0.00145 0.00040 | tangential |
| 18–21 | `s1..s4` | … | thin-prism |
| 22–23 | reserved | | |
| 24–26 | `w h type` | 10752 5376 113 | |

- The MEI forward model is implemented in `FisheyeProjection.cpp` and
  `fisheye_stitch_common.glsl`: `x = Xs/(Zs+xi)`, then distortion (radial
  k1–k4, tangential p1/p2, thin-prism s1–s4 in the OpenCV-omnidir form), then
  `u = fx·xd + cx`, `v = fy·yd + cy`, with (Xs, Ys) in the calibration's own
  image axes (u right, v down; our lens frame has both negated). Including
  p/s in these axes measurably improves the seam match;
  the other sign choices make it worse. It's the only stitch model (the
  legacy short-string path and `INSV_NO_MEI` were removed 2026-09-26).
- The extrinsic `yaw pitch` are rotations about the calibration's x / y
  axes. In a frame with x and y negated (ours), they flip sign; `roll` doesn't.
  Getting this wrong misaligns the seam by up to ~0.8°.
  Even with it right, the sample unit shows a further lens-1 yaw of
  −0.3 to −0.7° that the calibration doesn't carry. The app measures it per
  clip from the video.
- The MEI calibration is the **bare-lens** model. With a lens guard or dive
  case fitted, scale its image radius by the accessory profile (§5.4).
- Gyroflow's X5 path multiplies `cx` by 2 ("`cx_fix`", because of the
  side-by-side frame) and passes `[k1 k2 k3 p1 p2 xi]` as an "insta360"
  distortion model.

**Per-unit variance:** these values differ per camera, and X5 lenses are
user-replaceable. Always read calibration per file; never hardcode it.

### 5.4 Lens-accessory profiles (field 145) ✅

```
145 {
  1 { 1: "bare"        2: 0  2: 0.03159       2: -8.415e-05   2: 2.201e-06  2: -1.284e-08 2: 0 }
  1 { 1: "ProtectorA"  2: 0  2: 0.0321906505  2: -9.37245e-05 2: 2.5248e-06 2: -1.5e-08   2: 0 }
  1 { "ProtectorS" / "ProtectorAS": same as ProtectorA }
  1 { 1: "InvisibleDiveWater" 2: 0.000238 2: 0.03112 2: 7.306e-06 2: 4.427e-07 2: -3.892e-09 2: 0 }
  1 { 1: "InvisibleDiveAir"   2: 0 2: 0.0299338566 2: 4.86874e-05 2: -6.784e-07 2: 1.2e-09 2: 0 }
  2 { 1: "bare" 2: 200.0 }  2 { "Protector*": 195.0 }  2 { "InvisibleDive*": 190.0 }
  3 { 1: "invisibleDive" 2: 2 }  3 { 1: "heat_bare" 2: 10 }  3 { 1: "heat_protector" 2: 12 }
}
```

- Values are doubles (wire type 1). Identical on every clip in the sample set.
- Each `1 {}` entry is a polynomial `h(θ) = Σ cᵢ θⁱ`, **θ in degrees → image
  height in mm**. Evidence: `bare`'s h(θ) is proportional to the MEI model's
  sensor-px radius at a constant ~820 px/mm (≈1.22 µm pitch) from 10° to
  105°, within ±1 %.
- `2 {}` is the usable FOV per profile. `3 {}` is unknown (a dive-mode id and
  thermal thresholds?).
- A **guard pushes each angle further out**: `Protector(θ)/bare(θ)` = 1.017
  at the centre, rising to ~1.024 at 70–95°. The image circle stays the same,
  so the guarded lens fits 195° into it.
- **The camera does not record which accessory is fitted.** Fields 5/17,
  53/55, 54/56 and 111/112 (the "may be adjusted for accessories" pairs) are
  identical on guarded footage, and `ExtraUserOptions.offset_convert_states`
  (field 13.10, telemetry-parser) is absent; it is written by Insta360
  software after an edit. So the user has to choose it, as in Studio.
- Which of `ProtectorA`/`S`/`AS` is the Premium or Standard guard is a guess
  (A = premium, S = standard). They carry identical data, so it doesn't
  matter on the X5.
- **Measured effect**: ignoring it on guarded footage
  offsets the two lenses by ~4° across the seam; scaling the MEI radius by
  `Protector(θ)/bare(θ)` brings that to ≤ 0.5° (the rest is parallax). A
  constant 1.025 fits almost as well.

## 6. Type 3 — IMU (gyro + accelerometer)

### 6.1 Encoding and scaling

The payload is a flat array of fixed-size samples, each starting with a `u64`
device timestamp. Value order is **accel x, y, z, then gyro x, y, z** ✅.
ExifTool, telemetry-parser and our data all agree on that order. Ignore
insv-stitch's `x5_pipeline.md` table, which lists gyro first, 56-byte samples,
~200 Hz and m/s²; it contradicts its own code and the files.

| layout | size | after the timestamp | when |
|---|---|---|---|
| **raw counts** (X5) | **20 B** | 6 × `u16`, value = `raw − 32768` (signed count) | `is_raw_gyro` (field 62) true |
| doubles | 56 B | 6 × `f64` LE, already in g and rad/s | older models / `is_raw_gyro` false |

- **Pick the layout from field 62**. That is what telemetry-parser does. Our
  code instead guesses from `len % 20` / `len % 56` and a byte peek (the
  ExifTool method). The guess works for the X5, but the flag is authoritative.
- **Scaling raw counts** 📖 (telemetry-parser, from field 65 `gyro_cfg_info`):
  - **gyro** in °/s = count / (32768 / `gyro_range`) = count / **16.384**
    (±2000 °/s), so rad/s = count · **0.0010653**
  - **accel** in g = count / (32768 / `acc_range`) = count / **1024**
    (±32 g)
  - Defaults when field 65 is absent: 2000 °/s and 16 g.
  - These are the TDK **ICM-40609-D**'s maximum full-scale ranges (datasheet:
    gyro ±15.6…±2000 °/s, accel ±4…±32 g). Field 51 names that part.
- **⚠️ Our parser (and ExifTool) use `count / 1000` for both.** That is
  **wrong for the X5**:
  - Gyro rad/s comes out **6.1 % low** (×0.9387).
  - Accel comes out 2.4 % high.
  - Checked on three clips: the mean-gravity magnitude is 1.022 / 1.031 /
    1.026 g with `/1000`, and **0.998 / 1.007 / 1.002 g with `/1024`**.
  - A uniform 6 % gyro under-scale
    doesn't explain the ~50° axis disagreement in §6.3 on its own, but it biases
    every integrated angle.
- Size: 20 B × ~1006/s ≈ 72 MB/hour. `ReadInsvTelemetry(path, max)` can cap
  it. ExifTool stops at 20 000 samples.

### 6.2 Timing

- **Clock** ✅: the device's monotonic clock in **µs**, shared by types 3, 4,
  29 and metadata fields 24/98. It isn't zero-based and doesn't reset per file.
- **Rate** ✅: ~**1006 Hz**, mean step 994 µs, range 877–1000 µs. The steps
  are irregular, so integrate with the real `dt`.
- **Latency offset** 📖: metadata field 28 `gyro_timestamp` = **1.6 ms**
  (with field 29 true). Gyroflow computes, for raw-gyro files,
  `t_imu_video = (ts − first_frame_timestamp)/1e6 − gyro_timestamp/1000` in
  seconds. That is, IMU samples are shifted 1.6 ms **earlier** relative to
  video. Exposure samples are not shifted.
- **Pre-roll** ✅: the IMU track starts **157–778 ms before the first recorded
  frame** (§7.1).

### 6.3 Axes and IMU→camera orientation

- The raw axes are the IMU chip's own.
- **Gyroflow's X5 mapping** 📖 (telemetry-parser `process_map` +
  `insert_lens_profile`), applied to both gyro and accel:
  1. Rotate the raw vectors by the lens-0 `yaw, pitch, roll` from `offset_v3`
     (field 54: 0.326°, −0.017°, 89.997°). The matrix is `Rz(roll)·Ry(pitch)·
     Rx(yaw)`, which is essentially a **90° rotation about raw z**. So the
     "~90° sensor clock" angle in the calibration **is the lens↔IMU
     rotation**. It isn't just an image-plane quirk.
  2. Apply the IMU orientation string **`"yzX"`**. Each letter picks the
     source axis for output X, Y, Z; lowercase means negated. So
     `out = (−v.y, −v.z, +v.x)` in Gyroflow's camera frame. The X4 uses the
     same string.
  3. Net effect: `out ≈ (−raw_x, −raw_z, −raw_y)`, up to the sub-degree
     yaw/pitch terms.
- **insv-stitch** takes Gyroflow's normalised output and still needs a
  hand-solved `IMU_TO_CAM` (a Wahba fit against Studio renders on one unit,
  "~120° from identity", for camera X right / Y down / Z forward). It calls
  that matrix unit-specific. So even Gyroflow's mapping isn't the whole
  answer for 360 reprojection.
- **Ours** ✅ (measured 2026-09-25): **`gyroAxes = "z,-x,-y"`**, i.e.
  sphere X ← raw z, Y ← −raw x, Z ← −raw y, in the stitched sphere's frame
  (+X right, +Y up, −Z forward; `reframe_reproject.comp`).
  - Method: render raw pinhole views, estimate per-frame camera rotation with
    an essential matrix (RANSAC; separates rotation from forward-motion
    parallax), rotate into the sphere frame, and least-squares fit a 3×3 map
    against the §7.1-synced, §6.1-scaled gyro. Three view directions on
    `193105` agree to a few degrees; the combined fit is within ~6° of the
    signed permutation (scale ≈ 1). The camera rotation left after correcting
    with it: 0.37 °/frame vs 0.74 raw (0.96 with the old map).
  - The old default `"y,z,x"` (an early `|corr|` pick, sign-blind and made with
    the ~0.8 s sync error) is ~120° off. `tools/horizon_spike`'s gravity fit
    found `"z,x,y"` on the handlebar clip: the same permutation, with two
    axes' signs flipped (and run without the §7.1 sync).
  - **Verified on four clips** ✅ (2026-09-25): 8 segments (20 s, the
    highest-rotation windows, 4 horizon-band views each, lag-corrected per
    §7.1) on `193105`, `192900`, `201257` and `144221_020-trim`.
    `z,-x,-y` ranks 1st of the 24 signed permutations on every one, 2.4–4.8°
    from that segment's free fit, the runner-up far behind. The handlebar
    clip (`144221_019-trim`) stays unmeasurable: handlebars fill every view
    (inlier ratio ~0.1). The mapping is a property of the camera body, not the
    mount, and its sibling `020-trim` (same camera) confirms it.
  - Exact rotation + per-axis scale, pooled (7 segments, 5 397 frames): **1.9°** from
    the permutation (about sphere Y), gyro scale 1.00 / 0.95 / 1.00 (raw
    x/y/z). Using them instead of the plain permutation improves the residual
    by < 1 % on every segment, held-out included — not worth adopting.
- **Residual gyro bias** (2026-10-09): on the five still `Modes` clips (accel
  spread ~0.001 g) the raw gyro means (−0.0051, +0.0066, +0.0018) rad/s
  (~0.35 °/s), drifting slowly between clips (−0.0042, +0.0057, +0.0015 three
  minutes later). Not field 31's values. `GyroStabilizer::build()` fits it per
  clip for horizon leveling.
- Observed on stick/handlebar clips: mean accel ≈ (−0.80, 0.60, −0.23) g
  (`/1000`), which splits across raw axes 0 and 1. The stick resonance puts
  ~0.75 g RMS of ~9–11 Hz on the accelerometer, so low-pass it (~1.5 Hz)
  before using it as gravity.

## 7. Type 4 — Exposure ✅

- 16-byte samples: `u64` device µs timestamp, then `f64` **exposure time in
  seconds** (0.00066–0.020 s here; ExifTool shows `ShutterSpeed` 1/335).
- **One per sensor frame**, 33 368 µs apart (29.97 fps, ±80 µs).
- **More samples than video frames**: the sensor streams before recording
  starts. `193105` has 1308 exposure samples for 1282 frames: 24 before and 2
  after.
- telemetry-parser treats type 12 (`ExposureSecondary`) the same way; it
  isn't present on the X5.

### 7.1 Video ↔ IMU time sync ✅ 📖

`first_frame_timestamp` (field 24; also 98.1) is the device timestamp of the
first recorded frame. On every clip it **equals one exposure timestamp
exactly**:

| clip | exposure idx of frame 0 | first frame − IMU[0] | exposure[0] − IMU[0] |
|---|---|---|---|
| `193105` (`.insv` and `.lrv`) | 24 | **+773.5 ms** | −27.4 ms |
| `201257` | 24 | **+778.4 ms** | −22.4 ms |
| `192900` | 5 | **+157.3 ms** | −9.5 ms |
| `144221_019-trim` | 23 | **+768.6 ms** | +1.1 ms |

This is **exactly how Gyroflow aligns Insta360 files** 📖. telemetry-parser
subtracts `first_frame_timestamp` from every gyro, accel and exposure time,
then subtracts `gyro_timestamp` from the IMU. Field 64 (`pts_type =
VideoPtsEexposureFile`) says the video PTS come from the exposure record.

```
video time of an IMU sample   t = (imu.ts − field24)·1e-6 − field28·1e-3        [s]
video frame n                 ↔ exposure[k + n],   exposure[k].ts == field24
```

Our code used to assume `exposure[0]` is frame 0 (a "−26.5 ms"
offset; the real offset is **+0.16 to +0.78 s**). Fixed 2026-09-25:
`GyroStabilizer::build(InsvTelemetry, …)` anchors on field 24 + field 28,
verified against image motion on two clips.

**Trimmed files** ✅ (2026-09-25): a file re-written by Insta360 software after
a trim keeps field 24 = the *original* first frame, but its video starts later.
The TimeMap record (type 128, §4.3) gives the original time of video t = 0:
2002 ms on `144221_019-trim` (its video has 53 880 frames against 53 981
exposure samples after field 24's frame). Without adding it the gyro lands 2 s
off, which made horizon lock tilt ~45° in corners and was
`tools/horizon_spike`'s "corners" blocker. `InsvTelemetry::time_map_offset_ms`
carries it (median source − video offset; speed-ramped maps aren't modelled).

**Frame-clock drift + readout** ✅ (measured and fixed 2026-09-25). The MP4 PTS are a nominal constant 1001/30000 s
(frame 14220 of `192900` sits at exactly 474.474 s), but the camera's frame
clock isn't exactly 29.97 fps. `exposure[k+n].ts − field24 − n/fps` grows
linearly: **+40 ppm** on `192900`, `193105` and `144221_020-trim` (+10 ms at
255 s, +19 ms at 475 s), **−15 ppm** on `201257` (−7 ms at 475 s). On a 30-min
clip that's ~70 ms. Mapping video time through `n/fps` (as the code does)
lets the sync drift by that much. On top of that, the image content lags the
frame timestamp by about **half the readout** (field 25 / 2: 10.6 ms at
21.244 ms, 5.9 ms on `201257`), i.e. the timestamp marks the start of
readout. Image-measured best lag on 8 segments / 4 clips vs
`drift + readout/2`: mean +2 ms, sd 9 ms (each segment's own lag repeats to
~3 ms across its two halves; the spread is content-dependent — which sensor
rows the tracked features sit in). Rotational shake removed (residual vs
none, 8-segment mean): 35 % now → 40 % with drift → 43 % with drift +
readout/2 (per-segment best lag: 48 %).
`GyroStabilizer::build(InsvTelemetry, …)` now fits the clock rate from the
exposure record (least-squares slope vs `n/fps`) and maps video time as
`t·(1 + rate) + offset`, the derived offset including `field25/2`:

```
gyro time of video time t = t·(1 + rate) + (field24 − imu[0].ts)·1e-6
                            + field28·1e-3 + timeMap·1e-3·(1 + rate) + field25·0.5e-3
```

Stabilized Lean-follow renders (16 views): rotation removed vs raw 20 % →
27 %.

**Rolling shutter, both lenses** ✅ (measured 2026-09-26;
**supersedes the per-lens model below**). Per-cell lags (8×6
flow cells per view, ~570 cells mapped to lens + sensor row) show both lenses
read out top to bottom over field 25, in step: row gradient ≈ field 25 on
all clips (18–22 ms at 21.2 ms, 10.6–11.8 ms at 11.8 ms). The centre row sits
at `field24 + field28 − 2.3 ms` on `192900`, with a few ms of per-clip spread
(fine-tuned per clip in the app). Row v of either lens is captured at
`centre + (v/H − 0.5)·field25`.

~~Per-lens capture time** ✅ (measured 2026-09-26;
supersedes the `+ field25/2` term above). The `readout/2`
term, and the "content-dependent" ±9 ms spread, came from two things. The
two lenses aren't exposed at the same instant. And the renders used for the
fit had their stab matrices a frame out of step: `SeekTo` lands on the first
frame at or after trim-in, but the tools baked `trimIn + i/fps`. Measured
with correctly paired renders: 40° views around the horizon, each view's
best gyro lag from the essential-matrix inter-frame rotation, mapped to lens
and sensor pixel. Lags are relative to `field24 + field28`:

| clip | readout (field 25) | lens 0 | lens 1 | within a lens |
|---|---|---|---|---|
| `192900` | 21.24 ms | −7.2 ms (7 views) | −0.2 ms (6) | ≤ 3 ms over the sensor |
| `201257` | 11.84 ms | −7.4 ms (7) | −4.2 ms (9) | ≤ 5 ms |
| `193105` | 21.24 ms | (1 usable) | −3.2 ms (6) | ~14 ms (noisy) |

Model now used: lens 0 at `field24 + field28 − 7.3 ms` (the stabilizer's
reference time), lens 1 **a third of the readout later**. The stitch
de-rotates lens 1 to lens 0's time (`stitch::CaptureTiming`). There is little
per-row rolling shutter within a lens, so the per-row correction stays
opt-in.

```
gyro time of video t (lens 0) = t·(1 + rate) + (field24 − imu[0].ts)·1e-6
                                + field28·1e-3 + timeMap·1e-3·(1 + rate) − 7.3e-3
lens 1 captured at            + field25 / 3
```

## 8. Type 7 — GPS ✅ 📖

53-byte records at **10 Hz**, written only when a GPS source is connected (a
GPS remote, per Insta360). Layout, identical in ExifTool, telemetry-parser and
our decode:

| offset | type | field |
|---|---|---|
| 0 | u64 | Unix time, seconds (UTC). ExifTool reads it as two u32 |
| 8 | u16 | milliseconds |
| 10 | char | `A` acquired / `V` void (void fixes carry no N/S/E/W) |
| 11 | f64 | latitude, degrees |
| 19 | char | `N` / `S` |
| 20 | f64 | longitude, degrees |
| 28 | char | `E` / `W` (ExifTool has also seen `O`, which it treats as west) |
| 29 | f64 | speed, **m/s** |
| 37 | f64 | track / heading, degrees |
| 45 | f64 | altitude, m (telemetry-parser comment: "geoid undulation") |

**Records are 10 Hz, fixes 1 Hz.** The X5 writes one record per 100 ms of
video (430 records on the 42.8 s `193105` clip), but the fix behind them
updates once a second: each fix -- timestamp and position alike -- repeats
over ~10 consecutive records (9 or 11 at times). When the signal is lost the
last fix keeps repeating: on `201257` the fix updates for 29 s, then one
timestamp repeats over 211 records (21 s of video), one last record follows,
and the record ends ~48 s into a 10:41 recording. So neither the record count
nor the first-to-last time span measures the track; count distinct
timestamps.

GPS is timed in Unix time. Align it to video with field 98.3 (Unix ms of the
first frame) or field 36. `ReadInsvTelemetry` only counts the record --
records, `A` records, the fix rate (median step between distinct timestamps)
and the track length (distinct timestamps × that step) -- for the Media
information panel; positions aren't parsed. They could feed a speed-aware
lean estimate.

## 9. `MISC/Camera01/<file>.insv.pb` sidecar 📖

Per insv-stitch (`_find_pb_path`), the camera writes
`MISC/Camera01/VID_…_00_NNN.insv.pb` next to `DCIM/Camera01/`. It's a protobuf
containing a base64 blob, and the decoded text holds the same 56-token MEI
string. (Earlier notes said `*.lrv.pb`; the insv-stitch code
says `.insv.pb`.) We don't need it, because the trailer's field 111 holds the
same string. It doesn't contain the IMU→camera rotation. It isn't in our
sample set.

## 10. Geometry cheat-sheet (X5)

- Sensor window 5376² per lens, focal reference 5312 (field 27). The video
  stream is 3840² (scale 5/7), with fisheye radius ≈ 1920 stream px.
- **Field of view** ≈ 200° (bare lens), which puts `radius` at θ ≈ 100°.
  Protectors reduce it to 195°, dive cases to 190° (field 145).
- Two lenses back to back: 180° apart in yaw, ~32 mm apart. Studio's "front"
  needs a 180° output-yaw relative to our lens 0.
- The "mirror" the removed short-string path needed is simply the MEI
  calibration's image axes (§5.3).
- **Rolling-shutter readout = field 25**, per clip: 21.24 ms in the 8K 30 fps
  sample mode and 11.84 ms on `201257`. Use the file's value rather than a
  constant.

## 11. Tools

| tool | use |
|---|---|
| `build/insv_trailer_dump <file>` | serial, model, firmware, Parameters, IMU/exposure summary; `--imu` / `--exposure` dumps |
| `exiftool -ee -G3 <file>` | independent decode of types 1 (partly), 2, 3 (first 20 000, `/1000` scaling), 4, 7 |
| Gyroflow `telemetry-parser` (`gyro2bb`, or the Python `telemetry_parser`) | full metadata JSON (`Extra metadata`), correctly scaled and synced IMU, Insta360 lens profile |
| `ffprobe -show_streams` | container and streams (§3) |
| `tools/residual_stab --diagnose` | IMU spectra, image-vs-gyro sweep, sync (block [5]) |

## 12. Open questions (after the 2026-09-24 research)

Resolved by the research: the record-type names, the type-9 layout, the
meaning of most metadata fields, the IMU scale/range, the IMU part, the sync
anchor, the rolling-shutter source, the sidecar path, and the file-split rule.

Still open:
1. **IMU→camera rotation for 360 reprojection.** Gyroflow's `yzX` + lens
   rotation is a starting point, but insv-stitch still needed a per-unit
   solved matrix. Re-measure the ~50° axis discrepancy (§6.3) now that the
   sync anchor is fixed, and after fixing the gyro scale (§6.1).
2. **Is `gyro_calib` (field 31) already applied** to the type-3 data, and
   what are its 6 values (gyro bias + accel bias?).
3. **Short-string field order** (§5.2): the `radius cx cy` reading is
   likely but unconfirmed. Unused by the app since 2026-09-26.
4. Unknown record types 11 (layout), 22, 28, 29, 57, and metadata fields
   98+ other than those listed.
5. What sets field 25 per mode (resolution / frame rate / sensor mode).
   8K (HDR on/off), 5.7K+, 5.7K 60 and 4K 120 are now covered (§3.3); log,
   timelapse and `.insp` are still untested.
6. X5 I-Log's transfer curve (§3.3; bit depth and tagging are known now).
   Not needed while the maker's LUT does the conversion.

## 13. External sources

- Gyroflow **telemetry-parser**, `src/insta360/{mod,record,extra_info}.rs`
  (record framing, record types, `ExtraMetadata` protobuf schema, raw-gyro
  scaling, timestamp alignment, X5 IMU orientation `yzX`, lens-profile
  construction): <https://github.com/AdrianEddy/telemetry-parser>
- **ExifTool** `lib/Image/ExifTool/QuickTimeStream.pl`, `ProcessInsta360` +
  `%insvDataLen`: <https://github.com/exiftool/exiftool>; the "Parameters"
  field: <https://exiftool.org/forum/index.php?msg=78942>
- **insv-stitch** (`x5_pipeline.py`: 56-token MEI layout, sensor→video
  scaling, `.insv.pb` sidecar, `IMU_TO_CAM`):
  <https://github.com/BenjaminHenriksson/insv-stitch>
- Insta360 X5 manual, "File Formats" (naming digits):
  <https://onlinemanual.insta360.com/x5/en-us/operating-tutorials/storage/fileformat>
- Insta360 support: segmented recording every 29 min 59 s on X5/X4/X3
  (search result, Insta360 support pages)
- TDK ICM-40609-D datasheet (full-scale ranges):
  <https://invensense.tdk.com/products/motion-tracking/6-axis/icm-40609-d/>
- Insta360 manual, I-Log mode (X5 and X6):
  <https://onlinemanual.insta360.com/x6/en-us/operating-tutorials/capture-preview/features/ilog>
- Insta360 I-Log LUT downloads (X5 8-bit, 10-bit):
  <https://www.insta360.com/download/i-log>
- Gyroflow docs, Insta360 support (360 models unsupported in Gyroflow itself):
  <https://docs.gyroflow.xyz/app/getting-started/supported-cameras/insta360>
