# Developer tools

Command-line tools for checking, measuring and debugging the pieces of
orbis-reframe on real `.insv` clips. They use the same code as the app, but
without the user interface. They're for development only: `scripts/build.sh`
builds them into `build/`, and the release packages don't include them.

Run a tool with `--help` to see all of its options. This page says what
each tool is for and when you'd reach for it. The examples use clips from
`sample_footage/` (not in the repository; any X5 `.insv` works).

| Tool | Use it to… | Needs |
|---|---|---|
| [`insv_trailer_dump`](#insv_trailer_dump) | see what's inside a clip's metadata trailer | — |
| [`insv_stitch`](#insv_stitch) | stitch a frame on the CPU, or measure the lens seam | FFmpeg |
| [`stitch_source_smoke`](#stitch_source_smoke) | check that the app's stitch, seek and decode work | GPU |
| [`playback_bench`](#playback_bench) | time each step of playback and export | GPU |
| [`view_compare`](#view_compare) | compare the direct view render with the equirect path | GPU |
| [`reframe_export`](#reframe_export) | export a reframed flat video without the app | GPU, hw encoder |
| [`residual_stab`](#residual_stab) | measure the shake left after gyro stabilization | GPU, OpenCV |
| [`track_subject`](#track_subject) | try subject tracking and make a follow-cam | GPU, OpenCV contrib |
| [`plate_scan`](#plate_scan) | scan for number plates, check the blur by eye | GPU, OpenCV, ncnn |
| [`audio_filter_bench`](#audio_filter_bench) | try and score the audio noise filters | FFmpeg |

"GPU" means the app's own requirements: Vulkan with video decode, plus
FFmpeg. "OpenCV contrib" is `libopencv-contrib-dev`
(the `tracking` and `dnn` modules). ncnn comes from `scripts/fetch_ncnn.sh`.
`docs/dev-environment.md` lists the packages. CMake skips a tool whose
dependencies are missing and says so.

Angles are in degrees unless an option says otherwise. Times are
milliseconds of clip (source) time unless they're marked `S` (seconds).

---

## insv_trailer_dump

Prints what the X5 writes into a clip's `inst` trailer: camera model,
firmware, lens calibration, capture settings, and a summary of the IMU and
exposure data. `--imu` / `--exposure` print every sample, one per line, in
a layout that lines up with `exiftool -ee` for comparison.

**When:** a clip behaves oddly (stitch, sync, stabilization) and you want
to see what the camera recorded; or when working on `src/insv/InsvTrailer`.
`docs/formats/insta360-x5.md` explains the fields.

```sh
build/insv_trailer_dump clip.insv
build/insv_trailer_dump clip.insv --imu > imu.txt
```

## insv_stitch

Stitches one frame into an equirect with the CPU stitcher in `src/insv/`
alone. No Qt, no Vulkan. This is the reference the GPU stitch is checked
against, and the app's fallback. It can also measure the seam between the
two lenses:
- `--measure-seam IN OUT` fits the extra lens-1 rotation over a range.
- `--depth-seam` / `--depth-dump` show the depth-aware seam.
- `--baseline` changes the lens spacing, and its sign tests which lens is
  in front.

**When:** working on the stitch geometry, the calibration or the seam;
checking that the `.insv` library works on its own.

```sh
build/insv_stitch clip.insv --time-ms 5000 --eq-width 3840 --out eq.ppm
build/insv_stitch clip.insv --measure-seam 0 10000
```

## stitch_source_smoke

Opens a clip through `stitch::NativeStitchSource` (the app's own stitch:
Vulkan video decode and the GPU stitch) and writes the frames at the given
times as PPM images. Times far apart exercise seeking. `--range START END
STEP` reads a run of frames instead and writes one every STEP ms.

**When:** a quick check that decode, seek and stitch still work after a
change, before starting the app. The release test runs it on a build from
the source archive (`tests/release/dev_test.sh`).

```sh
build/stitch_source_smoke clip.insv 0 5000 60000    # stitch_source_smoke_<ms>.ppm
build/stitch_source_smoke --range 10000 12000 500 clip.insv
```

## playback_bench

Measure-only benchmark of where a frame's time goes, per clip (capture mode)
and per equirect size: decode alone, decode + stitch at each size, the
export paths, the viewport's lens-frame path, and seeks. Writes no files.

**When:** after a change to decoding, stitching or the frame path, to see
whether a mode still plays in real time. The preview-size presets and the
"Auto" preview size are based on its numbers.

```sh
build/playback_bench clip-8k30.insv clip-5k60.insv
build/playback_bench --frames 300 --eq 3840,5760 clip.insv
```

## view_compare

Renders a flat 1920x1080 view of one frame from a few directions in two
ways: straight from the fisheyes (the "fused" pass the viewport and export
use) and through an equirect of several sizes. It compares each with an 8K
equirect reference: PSNR, and a sharpness figure. `--out DIR` saves the
renders as raw NV12 images to look at with `ffmpeg`.

**When:** changing the view or stitch shaders, to check that the picture
still matches and hasn't lost detail.

```sh
build/view_compare --at 5000 clip.insv
```

## reframe_export

Exports a trimmed range of a clip as a flat H.264 / H.265 video through
the app's export path: GPU stitch, reprojection, hardware encode. The
encoder is picked from NVENC, VAAPI and Vulkan video in that order (the
first one a throwaway open succeeds with; Vulkan video is skipped on
NVIDIA); `ORBIS_ENCODER=nvenc|vaapi|vulkan` pins one backend, any other
value is reported and ignored. With `ORBIS_VERBOSE=1` it prints which
encoder was picked and why the ones before it can't be used; the "no usable
hardware encoder" error says the same. The camera path
comes from a `--keys` file (one line per keyframe:
`time_ms yaw pitch fovY [roll]`, in **radians**), or a fixed `--yaw/--pitch/--fov`.
It also takes the colour grade, a 3D LUT (`--lut FILE.cube`,
`--lut-strength PCT`) and a picture-in-picture inset.

**When:** testing export without the app, e.g. to script many renders or
to replay keys from `track_subject`. With `--lut`, to check the GPU's LUT
against FFmpeg's own `lut3d` filter on an export without it (both GPU
paths: set `INSV_FLAT_VIA_EQUIRECT=1` for the equirect + reproject one).

```sh
build/reframe_export clip.insv out.mp4 --trim-in 10000 --trim-out 20000 --yaw 30 --fov 80
build/reframe_export clip.insv out.mp4 --keys keys.txt --codec h264
build/reframe_export clip.insv lut.mp4 --trim-in 1000 --trim-out 2000 --lut X5_I-Log_To_Rec.709_V1.0.cube
```

## residual_stab

Measures the camera movement that's left after gyro stabilization, from
the image itself (feature tracking in the forward view). It can render the
result with that rest removed too. `--diagnose` is a measure-only report:
- the IMU's spectra
- what the gyro stabilization removes, sweeping axis map, scale and time
  offset
- the image's shake spectrum against the gyro
- whether what's left is rotation or parallax

**When:** footage still looks shaky after stabilization and you need to
know why. Typical cases: a camera on a stick (the shake is a swing),
a sync or axis problem, or parallax that no rotation can fix. Run
`--diagnose` before changing the stabilizer.

What each `--diagnose` block tells you:

| Block | Shows | Read it as |
|---|---|---|
| [0] | shutter time × gyro rate | the motion blur inside each frame; no stabilization removes it |
| [1] | gyro / accel spectra | a mount resonance, and up to what frequency the IMU still carries real signal |
| [2] | rotation the gyro records vs. the correction applied | ratio well below 1: the stabilizer leaves rotation in |
| [3] | image pan vs. the gyro, sweeping axis map, scale and time offset | which gyro setup matches the picture, and whether the IMU carries the shake at all |
| [4] | forward residual, narrow vs. wide view | ≈ 1: rotation (correctable); ≫ 1: parallax from the camera moving |
| [5] | video↔IMU time sync | the offset that lines the gyro up with the picture |
| [6] | Horizon lock's "up" vs. gravity, roll / pitch per 10 s | ≈ 0: level; a steady offset: gyro bias or a bad gravity start |

If the shake is rotation, the IMU has it, and it still shows after
stabilization, suspect the pipeline before the physics: a sync error of a
millisecond or two, a wrong gyro axis map, or the lens model. Only rule
those out before calling a clip's shake parallax, motion blur or vibration
faster than the frame rate can capture.

```sh
build/residual_stab clip.insv --diagnose --trim-in 0 --trim-out 30000
build/residual_stab clip.insv out.mp4 --render derotate2d --stab-mode horizonLock
```

## track_subject

Tracks a subject through the 360 sphere with the same tracker as the app's
"Track subject" button, from a box you give it. It writes reframe keys
(`reframe_export --keys` format), or renders a follow-cam video directly.
`--dump-equirect` writes the stabilized sphere with a yaw/pitch grid, to
read the box off. `--tracker` compares the app's hybrid tracker with plain
CSRT, VitTrack and NanoTrack, and `--occlude` tests how it recovers after
the subject is hidden.

**When:** tuning or debugging subject tracking on a clip where the app's
tracking loses the subject.

```sh
build/track_subject clip.insv --t0 60000 --dump-equirect grid.jpg
build/track_subject clip.insv --t0 60000 --dur 15 --box 40,-5,8,10 --render follow.mp4
```

## plate_scan

The privacy blur from the command line: scans a range for number plates the
way the app's "Scan clip" does, and writes the scan in the app's cache
format. It can also render stills of the blurred frame through the GPU
equirect, the fused view and the CPU stitch, to check by eye that no plate
pixels show. `--follow` runs "Blur a plate…" from a box instead of a scan,
`--back` runs it backwards.

**When:** a plate wasn't blurred (or something else was), or after changing
the plate model, the scanner or the blur.

```sh
build/plate_scan --t0 430000 --dur 20 --scan scan.bin clip.insv
build/plate_scan --scan-in scan.bin --still 439300 --stills-dir stills clip.insv
```

## audio_filter_bench

Test bench for the audio noise filters. `extract` decodes one audio stream
of a clip to a WAV, and also runs it through the app's "MC: Wind reduction".
`general` / `hiss` / `voice` / `voiceamb` run the app's other filters on a
WAV. `analyze` scores a filtered WAV against the raw one, per frequency band
and on / between the engine's harmonics. `run_all.sh` runs every filter
(optionally the DeepFilterNet CLI as a reference) on one stream and writes
the WAVs plus a report to listen and compare.

**When:** tuning a noise filter, or checking one on a new kind of audio
(another bike, wind without an engine, speech). Your ears decide; the scores
show what changed.

```sh
build/audio_filter_bench extract clip.insv out --stream 1 --start 60 --dur 20
build/audio_filter_bench analyze out/raw_s1.wav out/wind_s1.wav
tools/audio_filter_bench/run_all.sh clip.insv 1 60 20 out
```
