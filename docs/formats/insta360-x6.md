# Insta360 X6 file format

**Desk research (2026-10-15), no X6 file examined.** From public specs,
Insta360's manuals and Gyroflow's telemetry-parser, set against what our code
assumes. Written as differences from the X5 ([`insta360-x5.md`](insta360-x5.md),
"X5 §n" below). Markers as in [`README.md`](README.md).

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
  no name yet.
- 8K 50 is ~1.5 Gpx/s across both lenses, ~1.7× the X5's 8K 30 (X5 §3.3).
  Real-time decode at that rate isn't measured.

## Colour: 10-bit, log and HDR

- 📖 Colour modes Standard, **Dolby Vision** and **I-Log 10-bit**.
- The app's GPU stitch samples the decoder's pictures as 8-bit NV12
  (`VK_FORMAT_G8_B8R8_2PLANE_420_UNORM`) through a BT.709 full-range
  conversion. A 10-bit stream needs a second sampler format and shader
  variant; I-Log needs a log-to-linear conversion; Dolby Vision presumably
  BT.2020 with a PQ or HLG transfer (❓ unconfirmed) and a tone map.
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
  runs as a bare lens at the default `fovDeg` 200. That produced correct
  output on the sample clip; the X6's usable FOV may differ slightly.
- ❓ telemetry-parser has no X6-specific code; an unknown model gets its
  default IMU orientation, `Xyz`. The IMU axis convention is unconfirmed.

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
