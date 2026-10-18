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
  `bt709` - **including the I-Log ones**. The stream-level colour
  metadata does not mark I-Log; record 0x101 field 22 `gamma_mode` does:
  `standard` vs `I_Log` (confirmed on 8K/6K/4K clips, fw v1.2.36).
  `ApplyLensProfile` reads it into `StitchParams::logInput`; on `I_Log`
  the lens sampling (GPU shader and CPU stitcher alike) decodes the
  2026.05.20 white-paper curve to linear reflectance on BT.2020, then
  gamut-maps to BT.709 and re-encodes with the BT.709 OETF, so the seam
  measure, blending and grading see ordinary display-encoded RGB.
- The app's GPU stitch samples the decoder's pictures as 8-bit NV12
  (`VK_FORMAT_G8_B8R8_2PLANE_420_UNORM`) through a BT.709 full-range
  conversion. A 10-bit stream needs a second sampler format and shader
  variant; Dolby Vision presumably BT.2020 with a PQ or HLG transfer
  (❓ unconfirmed) and a tone map.
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
- Gyroflow telemetry-parser, `src/insta360/mod.rs`:
  <https://github.com/AdrianEddy/telemetry-parser>
