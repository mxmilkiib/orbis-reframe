# orbis-reframe

A native Linux video editor for **Insta360 X5** 360° footage: stitching,
gyro stabilization and, above all, **reframing**. Reframing means steering a
virtual camera through the sphere over time to get a normal flat video out of a
360° recording.

[![The orbis-reframe editor: a reframed ride on a country road with a picture-in-picture inset, the Media sidebar on the left, the Overlay tab on the right, a player bar under the video and a timeline with video, camera mic, music and overlay tracks below](site/screenshot-1400.jpg)](site/screenshot-full.jpg)

▶ **Demo video:** [plate blur and stabilization on a motorcycle ride](https://youtu.be/BER8GgtOpjQ)
(YouTube, 3:41)

> **AI disclaimer:** most of the code in this project was written with AI,
> using [Claude Code](https://claude.com/claude-code) with Claude Opus 5.5.
> The design, direction and testing on real footage are human.

## Status: "Works on my computer"

> Status: work in progress, built and tested on one machine (Debian Testing,
> NVIDIA RTX, Wayland, Sway). AMD and Intel GPUs are supported best-effort:
> a contributor has run it on an AMD Radeon; Intel hasn't been tried. Other
> setups that meet the requirements should work but haven't been tried.

**Download:** a `.deb` and a tarball for Debian Testing, Debian 13 and
Ubuntu 26.04 LTS on the
[releases page](https://github.com/Schabo/orbis-reframe/releases). The
[user manual](docs/manual/README.md) starts with the
[getting-started guide](docs/manual/getting-started.md): what your computer
needs and how to install. **Found a problem?**
[Open an issue](https://github.com/Schabo/orbis-reframe/issues).

**Have another 360 camera?** Support for other Insta360, DJI and GoPro
cameras is planned, and needs real footage to build on:
[sample clips wanted](docs/formats/README.md#sample-clips-wanted).


## Why

Insta360's own editor, **Insta360 Studio**, runs on Windows and macOS only.
Linux has no way to edit X5 footage properly:

- The X5 records **dual-fisheye `.insv` files**. They need lens-calibrated
  stitching into a sphere before you can do anything with them. General
  players and editors can't read the calibration or the telemetry.
- The camera's **gyro/accelerometer data** sits in a proprietary trailer at
  the end of each file. Without it there is no stabilization or horizon
  leveling, and 360° footage from a moving vehicle is hard to watch without
  that.
- The most useful part, **interactive reframing**, doesn't exist in Linux
  editors. Kdenlive and Shotcut are built on MLT, which has no concept of
  spherical video or of keyframing a viewing direction. A plugin wouldn't be
  enough, so this is a standalone application.

My use case is **motorcycle footage** from an X5 on a selfie stick. That
footage needs lean-preserving stabilization that removes the shake but keeps
the bike's roll in corners, a level horizon when you want one, and a quick way
to pick where the camera looks at each moment.

Orbis Reframe doesn't use Insta360's Media SDK. An early prototype was built
on it, but its Linux support (an Ubuntu 22.04 / CUDA build and a file-to-file
stitch API) didn't fit an interactive editor on current Linux. So the
container and trailer parser, lens calibration, fisheye→equirect stitch and
gyro stabilization were **reimplemented independently** (`src/insv/`), from
public documentation and from studying camera-produced files. The editor has
no closed-source dependencies.

## Features so far

**Import and playback**
- Opens X5 `.insv` files directly, including every capture mode tested so far
  (8K, 5.7K/5.7K+, 5.7K 60, 4K 120).
- Real-time playback: Vulkan video decode, then a Vulkan compute
  fisheye→view pass on the GPU, with CPU fallbacks.
- Interactive 360° viewport: drag to look around during playback and while
  scrubbing.
- The viewport renders straight from the fisheyes at full source detail.

**Reframing**
- Keyframed virtual camera: yaw, pitch, roll and field of view, interpolated
  between keyframes. Set keyframes by dragging the viewport or by typing
  values into the sidebar.
- Easing per keyframe (Linear, Fast to slow, Slow to fast), and moves that
  start and stop gently instead of abruptly.
- **Track subject**: draw a box around a rider, car or anything else in the
  view, and the camera follows it through the sphere. Tracking plays back live
  in the viewport, with the box green while the subject is found and red when
  it's lost, and you stop it when you've seen enough. The result is ordinary
  keyframes you can edit. It works in a gyro-stabilized flat view around the
  subject, so the camera shake and the 360° seam don't get in the way.

**Timeline**
- Multiple clips with trim in/out, split, duplicate and reorder, from the
  keyboard or the edit toolbar above the timeline.
- Flat videos (from a phone or an action camera) on the video track too,
  mixed with 360° clips, shown whole with bars or filling the frame.
- Transitions at cuts: crossfade or dip to black.
- Per-clip playback speed (0.1–4×) with pitch-preserving audio time-stretch.
- Multiple tracks: any number of audio and video (overlay) tracks, which can
  be renamed, reordered, muted, zoomed and scrolled. Tracks keep to fixed
  groups, and a video track folds its sound tracks away under it.
- Music clips with trim, level and fades. Add them from the menu or drag and
  drop them onto the timeline, where their waveforms are drawn.
- Every audio stream in a clip gets its own track, linked to the video: the
  camera's mic and an external mic such as the Insta360 Mic Air.
- Keyboard shortcuts for frame stepping, jumping between cuts, split and
  delete (Help ▸ Keyboard shortcuts, F1).

**Picture-in-picture and titles**
- Flat video files on overlay tracks, drawn as insets over the reframed view:
  position, size, opacity, rounded corners and a border, set in the Overlay
  tab or by dragging the inset in the viewport.
- Keyframe an inset's position, size and opacity, and bring an overlay forward
  or send it back. An overlay's own sound comes along on its own track, linked
  like a video clip's.
- Text titles on overlay tracks: font, colour, position and size, keyframed
  and faded like an inset.

**Stabilization** (native, from the camera's own IMU data)
- Modes: **Off / Lean follow / Horizon lock / Custom** (per-axis strength),
  à la Gyroflow.
- Accelerometer-fused horizon reference, gyro↔video time sync and optional
  rolling-shutter correction.

**Stitching**
- Stitch based on the MEI fisheye model, using the per-camera calibration
  stored in each file.
- Lens-guard profiles and a seam fine-tune per clip, so clips filmed with and
  without lens guards mix in one project.
- Depth-aware seam: the stitch distance is measured along the seam every
  frame, so near objects (the bike, handlebars, hands) aren't doubled.

**Look and sound**
- Per-clip colour grading: exposure, contrast, saturation, temperature. The
  same formula runs in the viewport and in export.
- Audio playback: level, mute and fades for every audio clip, a level and
  mute per track, and a mixer used in both playback and export.
- Per-clip noise reduction: wind (a generic mode, and a motorcycle mode that
  tracks the engine's harmonics and turns down the wind between them), hiss
  and mains hum for quiet scenes, and voice isolation ("Voice only", or
  "Voice + ambience" that turns the rest down instead) with DeepFilterNet 3.

**Privacy blur**
- Blur number plates per clip: "Scan clip" in the Privacy tab finds them
  faster than real time on the GPU, and the blur shows in the viewport, export
  and screenshots. "Show regions" outlines what the scan found.
- Right-click a blur to remove a false hit, or use "Blur a plate…" to box a
  plate the scan missed; it's followed forwards and backwards from the box.

**Output**
- Flat reframed export of the whole timeline to H.264/H.265 on the GPU's
  hardware encoder (NVENC on NVIDIA, VA-API or Vulkan video on AMD and
  Intel), with the overlays and titles composited in and the audio mixed.
- Project settings: aspect ratio (16:9, 9:16, 1:1, 4:3), resolution and frame
  rate. The viewport is letterboxed to the chosen aspect.
- Screenshot of the current view at project resolution (PNG), from the
  player bar.

**Editor**
- A project library to start from: one folder per project, with its exports
  and caches; rename and duplicate projects there.
- Save/load (`.orp`, JSON), undo/redo, and autosave to a recovery file.
- A Media sidebar with the project's files, which notices files going missing
  and helps find moved ones.
- Settings in tabs (Video, Audio, Overlay, Privacy) with collapsible sections,
  a player bar under the video, and the timeline across the full window.
- Three themes: Ember, Light and Neutral dark.

Planned next: other 360° cameras (other Insta360 models, DJI, GoPro) and an
effects library. Further out: a "tiny planet" projection and face blur.

## Building

Linux only, Wayland only, a GPU with Vulkan: NVIDIA (tested), AMD or Intel
on Mesa (best-effort). Playback uses Vulkan video decode where the GPU has it
and falls back to (slower) CPU decoding where it doesn't; export needs a
hardware encoder: NVENC on NVIDIA, VA-API or Vulkan video on AMD and Intel.
The app tells you at startup what's missing instead of failing. Dependencies are the Debian Testing (or Debian 13, Ubuntu 26.04) packages for Qt 6,
Vulkan, glslang, nlohmann-json and FFmpeg. Qt Multimedia (audio), OpenCV
(analysis tools; with the contrib modules also subject tracking), ONNX Runtime
(voice noise modes) and ncnn (the plate scan; `scripts/fetch_ncnn.sh`) are
optional. [`docs/dev-environment.md`](docs/dev-environment.md) has the full
list and the pinned versions.

```sh
./scripts/build.sh          # configure + build
./scripts/build.sh -r       # build and run
./scripts/package.sh        # release tarball + .deb into dist/debian-testing/ (all features)
./scripts/package.sh --target debian-13      # the same for Debian 13 (or ubuntu-26.04), in a container
./scripts/fetch_release.sh vX.Y.Z               # or fetch a draft release's packages into dist/
./scripts/release_test.sh [--target debian-13]  # test them in GPU containers (podman, docs/RELEASE.md)
```

## Documentation

| Doc | Contents |
|---|---|
| [`docs/manual/`](docs/manual/README.md) | User manual |
| [`CONTRIBUTING.md`](CONTRIBUTING.md) | Reporting bugs, building, commit messages, licensing of contributions |
| [`docs/dev-environment.md`](docs/dev-environment.md) | Build dependencies, toolchain, environment switches for diagnosing problems |
| [`docs/formats/`](docs/formats/README.md) | Camera file formats: everything known about the X5 `.insv`/`.lrv` format, and what's known about other cameras |
| [`docs/RELEASE.md`](docs/RELEASE.md) | How to make a release, step by step |
| [`tools/README.md`](tools/README.md) | The developer tools: what each is for, when to use it |

## Sources and thanks

Everything about the X5 file format was learned from **interoperability
documentation, published research and camera-produced files**. The Insta360
SDK was never disassembled. [`docs/formats/`](docs/formats/README.md) marks
every fact with where it came from and how well it's verified. We're grateful
to these projects and people:

- **[Gyroflow](https://gyroflow.xyz/) and
  [telemetry-parser](https://github.com/AdrianEddy/telemetry-parser)** by
  Adrian Eddy and contributors. telemetry-parser's Insta360 reader
  (`src/insta360/`) gave us the trailer record framing and type names, the
  protobuf schema of the metadata record, the raw-gyro scaling, the timestamp
  alignment, the X5 IMU orientation and the lens-profile construction.
  Gyroflow's approach to gyro stabilization (smoothing, per-axis strength,
  horizon lock, rolling-shutter readout time) is the model for our
  stabilization modes.
- **[ExifTool](https://exiftool.org/)** by Phil Harvey. `QuickTimeStream.pl`
  (`ProcessInsta360`) gave an independent decode of the `inst` trailer, which
  we checked our parser against byte for byte. The
  [ExifTool forum thread on the "Parameters" field](https://exiftool.org/forum/index.php?msg=78942)
  covers the short calibration string.
- **[insv-stitch](https://github.com/BenjaminHenriksson/insv-stitch)** by
  Benjamin Henriksson: the 56-token MEI calibration layout, sensor→video
  scaling, the `.insv.pb` sidecar and the IMU→camera matrix for the X5.
- **Insta360**:
  the [X5 online manual](https://onlinemanual.insta360.com/x5/en-us/operating-tutorials/storage/fileformat)
  (file naming, segmented recording) and Insta360 Studio, which we used as a
  visual reference for stitching and horizon leveling.
- **TDK InvenSense**: the
  [ICM-40609-D datasheet](https://invensense.tdk.com/products/motion-tracking/6-axis/icm-40609-d/)
  for the IMU's full-scale ranges.
- **Published methods**: the unified omnidirectional (MEI) camera model
  (C. Mei & P. Rives, "Single View Point Omnidirectional Camera Calibration
  from Planar Grids", ICRA 2007) for the fisheye lens model, and the Mahony
  complementary filter (R. Mahony, T. Hamel & J.-M. Pflimlin, "Nonlinear
  Complementary Filters on the Special Orthogonal Group", IEEE TAC 2008) for
  fusing the accelerometer into the horizon reference.
- **[DeepFilterNet](https://github.com/Rikorose/DeepFilterNet)** by Hendrik
  Schröter and contributors (MIT / Apache-2.0; H. Schröter et al.,
  "DeepFilterNet: Perceptually Motivated Real-Time Speech Enhancement",
  Interspeech 2023): its DeepFilterNet 3 model runs the voice noise modes
  (`resources/models/deepfilternet3`, with ONNX Runtime). FFmpeg's `afftdn`
  filter was a comparison reference in `tools/audio_filter_bench`, not built in.
- **Subject tracking** combines OpenCV's CSRT tracker (A. Lukežič, T. Vojíř,
  L. Čehovin Zajc, J. Matas & M. Kristan, "Discriminative Correlation Filter
  with Channel and Spatial Reliability", CVPR 2017), which follows the subject,
  with the **VitTrack** model from [OpenCV Zoo](https://github.com/opencv/opencv_zoo)
  (Apache-2.0, `resources/models/vittrack`), which checks each frame that it's
  still the same subject.
- **Number-plate detection** uses the YOLOv9-s plate model from
  [open-image-models](https://github.com/ankandrew/open-image-models) by
  ankandrew (MIT, `resources/models/plates`; C.-Y. Wang, I.-H. Yeh &
  H.-Y. M. Liao, "YOLOv9: Learning What You Want to Learn Using Programmable
  Gradient Information", ECCV 2024), run on the GPU by ncnn.

Built with [Qt 6](https://www.qt.io/), [Vulkan](https://www.vulkan.org/),
[FFmpeg](https://ffmpeg.org/) (demux, decode, hardware encode, audio, time-stretch),
[glslang](https://github.com/KhronosGroup/glslang) (shader compiler),
[nlohmann/json](https://github.com/nlohmann/json), optionally
[OpenCV](https://opencv.org/) (analysis tools, subject tracking) and
[ONNX Runtime](https://onnxruntime.ai/) (the voice noise modes) and
[ncnn](https://github.com/Tencent/ncnn) (Tencent, BSD-3-Clause; the plate
scan), and [Lucide](https://lucide.dev/) icons (ISC
license).

Insta360, Insta360 Studio and X5 are trademarks of Arashi Vision Inc. This
project is not affiliated with or endorsed by Insta360.

## License

Copyright (C) 2026 Markku Kokko

The editor is licensed under the **GNU General Public License v3.0 or later**
([`LICENSE`](LICENSE)).

The `.insv` library in [`src/insv/`](src/insv/) (container and trailer parsing,
lens calibration, stitching, gyro stabilization, audio processing) is licensed
more permissively, under **MIT OR Apache-2.0** at your option
([`src/insv/LICENSE-MIT`](src/insv/LICENSE-MIT),
[`src/insv/LICENSE-APACHE`](src/insv/LICENSE-APACHE)). Every file there carries
an SPDX header saying so. That way other projects can use it, and it can move to
its own repository later. Contributions to `src/insv/` are accepted under the
same dual license.

Third-party components, their licenses and where the license texts are (in the
source tree and in the packages): [`THIRD_PARTY.md`](THIRD_PARTY.md).
