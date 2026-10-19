# Insta360 X6 file format

**Desk research (2026-10-15) plus empirical findings (2026-10-18) from a
test set of 24 clips (8K/6K/4K, standard and I-Log, fw v1.2.36).** From
public specs, Insta360's manuals and Gyroflow's telemetry-parser, set against
what our code assumes. Written as differences from the X5
([`insta360-x5.md`](insta360-x5.md), "X5 §n" below). Markers as in
[`README.md`](README.md).

## Files and streams

- 📖 One `.insv` per take holding both lenses, the X5's naming (X5 §1).
- 📖 Two 1/1.1" square sensors (a third more area than the X5's). H.265
  only, up to 180 Mb/s.
- 📖 360 modes:

  | mode | frame | fps | per lens (⚠️) |
  |---|---|---|---|
  | 8K | 7680×3840 | 50 / 48 / 30 / 25 / 24 | 3840² |
  | **6K** (new) | 6016×3008 | 60 / 50 / 48 / 30 / 25 / 24 | **3008²** |
  | 4K | 3840×1920 | 100 / 60 / 50 / 48 / 30 / 25 / 24 | 1920² |

- The app names a clip's mode by its lens size (8K / 5.7K / 4K); 3008² has
  no name yet. Clips at all three sizes (3840², 3008², 1920²) open and
  stitch correctly ✅.
- ✅ Vulkan decode+stitch on RADV does ~84 fps decode, ~35 fps full-res
  7680-wide stitch, ~60 fps fused view on 8K30 (Strix Halo iGPU). 8K 50
  (~1.5 Gpx/s) remains unmeasured.

## Colour: 10-bit, log and HDR

- 📖 Colour modes Standard, **Dolby Vision** and **I-Log 10-bit**.
- ✅ Every sample clip is H.265 Main 10 (`yuv420p10le`), VUI-tagged
  `bt709` for Standard and I-Log clips; **Dolby Vision clips tag
  `bt2020nc` + `arib-std-b67` (HLG)** with no RPU side data observed -
  the stream is plain HLG/BT.2020 limited-range (confirmed on
  8K/6K/4K clips, fw v1.2.36). The mode marker is record 0x101 field
  22 `gamma_mode`: `standard` / `I_Log` / `Dolby_Vision`.
  `ApplyLensProfile` reads it into `StitchParams::gammaMode` (0/1/2);
  for the non-standard modes the lens sampling (GPU shader and CPU
  stitcher alike) decodes to linear light on BT.2020 - the 2026.05.20
  white-paper curve for I_Log, the HLG inverse OETF plus the BT.2100
  OOTF evaluated at the SDR system gamma (~0.78 for 100 nits) for
  Dolby_Vision - then gamut-maps to BT.709 and re-encodes with the
  BT.709 OETF, so the seam measure, blending and grading see ordinary
  display-encoded RGB.
- The app's GPU stitch samples the decoder's pictures through a BT.709
  full-range ycbcr conversion. Since 2026-10-18 it takes 10-bit HEVC (Main
  10) too: FFmpeg's Vulkan decoder gives P010 images
  (`VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16`), the stitcher's
  conversion is made for that format (by the stream's bit depth), and the
  viewport's shared lens planes are 16-bit (R16 / R16G16). ✅ Confirmed
  on real X6 clips as well as the earlier stand-in. Exports of a 10-bit
  source keep the precision end to end: they request HEVC Main 10, the
  fused view packs P010 straight into the encoder's Vulkan frame pool
  (`hevc_vulkan`), CPU paths widen NV12 staging to P010LE (`hevc_vaapi`),
  and the open retries at 8-bit when Main 10 can't.
- The transfer decode above runs inside the stitch, so an I_Log or
  Dolby_Vision clip reaches grading already in BT.709: the official
  I-Log-to-Rec.709 LUT (below) is meant for the still-log samples --
  applying it on top of a decoded clip double-corrects. The Colour
  section's missing-LUT prompt is therefore suppressed for these
  profiles; it still fires for a log profile the app doesn't decode.
- 📖 Insta360's 10-bit I-Log LUTs (`X6_I-Log_LUTs.zip`, version 2.0, made
  2026-05-20): 3D `.cube` files, each at 33³ and 65³, from I-Log to Rec.709
  / BT.1886 (`X6_I-Log_to_Rec709_BT1886_s65_v2.cube`), to Rec.2020 PQ and to
  Rec.2020 HLG (1000 nits, 203 nits diffuse white). The app reads all of
  them (Colour ▸ LUT); the Rec.709 one is the one for its SDR output. The
  10-bit pictures are graded in float before the 8-bit output, so they
  shouldn't band.
- The X5 has I-Log too (X5 §3.3), so the same work covers it.

## Trailer

- ✅ Examined (8K30 clip, fw v1.2.36): same magic/footer/record-directory
  framing as the X5. The maker-notes record carries serial, model
  ("Insta360 X6"), firmware, and the 0x400/0x300 exposure/IMU records parse
  with the X5 layouts (~1 kHz raw IMU).
- ✅ The extended MEI calibration string is present in fields 111/112 of
  record 0x101 (56 tokens, same layout as the X5) - e.g. xi 2.455,
  fx/fy 7232, cx/cy ~3850/3880, roll ~90, combined frame **15488×7744**,
  type 193. `ParseExtendedCalibrationString`'s sanity ranges hold.
- ✅ Field 27 `window_crop_info` is present: src 7744², **dst 7680²** (vs
  5376²→5312² on the X5). The stitch's focal-reference width is no longer
  hard-coded to 5312; it comes from this field (`InsvMakerNotes::crop_dst_w`
  → `StitchParams::cropRefW`). With the wrong reference the projected circle
  lands too small and the equirect shows two inscribed discs with gaps.
- ❌ **No field 145 lens-profile table** on the X6: `lens_profiles` is empty,
  so accessory names ("ProtectorA"/...) never resolve and the stitch always
  runs as a bare lens at the default `fovDeg` 200. Stitched output at all
  three resolutions shows continuous coverage, so 200° bare appears right;
  accessory-guard correction is impossible without a profile source.
- ❓ telemetry-parser has no X6-specific code; an unknown model gets its
  default IMU orientation, `Xyz`. The accel/gyro ranges and ~1 kHz rate
  parse fine at rest; the axis convention still needs a clip with
  deliberate rotations to confirm.

## Sources

- Insta360 X6 manual, shooting specs:
  <https://onlinemanual.insta360.com/x6/en-us/specs/shooting-specs>
- Insta360 X6 manual, file formats:
  <https://onlinemanual.insta360.com/x6/en-us/operating-tutorials/storage/fileformat>
- Insta360 manual, I-Log mode (X5 and X6):
  <https://onlinemanual.insta360.com/x6/en-us/operating-tutorials/capture-preview/features/ilog>
- Insta360 X6 specs page: <https://www.insta360.com/specs/x6>
- Insta360 I-Log LUT downloads: <https://www.insta360.com/download/i-log>
- Gyroflow telemetry-parser, `src/insta360/mod.rs`:
  <https://github.com/AdrianEddy/telemetry-parser>
