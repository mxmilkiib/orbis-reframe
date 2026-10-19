## v0.4.0 (2026-10-18)

### Feat

- **export**: skip Vulkan encoders when the device is NVIDIA's
- **export**: try VAAPI before Vulkan video
- **export**: set VBR and quality hints on Vulkan and VA-API encoders
- **export**: ORBIS_ENCODER env var pins the encoder backend
- **export**: probe Vulkan and VA-API encoders when NVENC is absent
- **export**: refuse to export when a source file is missing
- **timeline**: crossfade and dip-to-black transitions at cuts
- **timeline**: keyframe moves start and stop gently
- **ui**: text titles on overlay tracks
- **timeline**: keyframes ease fast to slow or slow to fast
- **timeline**: duplicate a clip from its menu
- **stitch**: lens guards and depth-aware seam per clip
- **timeline**: flat videos on the main track, mixed with 360 clips

### Fix

- **export**: probe each encoder once, off the GUI thread, and say why none works
- **export**: fall back to the next encoder when open fails
- **render**: check the viewport's descriptor set layout, pools and sets
- **build**: link OpenCV 5's features and geometry modules
- **render**: size the stitch descriptor pool from the ycbcr sampler's count
- **render**: build on FFmpeg before 9, name the right queue flag
- **render**: adopt FFmpeg's Vulkan queue with its creation flags
- **insv**: include features2d.hpp for goodFeaturesToTrack
- **export**: missing overlay or sound files block the export too
- **playback**: a cut changes the view and grade on the same frame as the picture
- **ui**: tracking status shows under the Track subject button
- **ui**: a new project starts from a blank view and default settings
- **privacy**: saving removes plate scans no clip uses
- **privacy**: each clip has its own plate scan
- **privacy**: the plate blur shows only on the clip it's on
- **playback**: a flat main-track clip can't be dragged out of place
- **playback**: a flat clip's colour change shows while paused
- **export**: overlay videos decoded at their own size no longer overrun the frame
- **timeline**: draw linked clips' link mark as an icon, not an emoji

### Refactor

- **privacy**: the plate scanner's geometry and quad tracker apart
- **playback**: the decoder thread apart from the controller
- **timeline**: split the timeline widget into layout, paint and input
- **render**: the viewport's renderer apart from its window
- remove dead code found by a whole-build link check
- **render**: stitcher setup apart from its per-frame work
- **ui**: split MainWindow's constructor into its parts
- **timeline**: adding track files out of MainWindow
- **ui**: project saving and its dialogs out of MainWindow
- **ui**: media import and Locate file out of MainWindow
- **export**: render jobs and the export runner out of MainWindow
- **privacy**: the blur's data side out of MainWindow
- **ui**: keyboard shortcut help dialog out of MainWindow
- **ui**: stabilizer cache and clip measurements out of MainWindow

### Perf

- **export**: flat clips export many times faster

## v0.3.0 (2026-10-14)

### Feat

- **timeline**: split the clip at the playhead from the edit toolbar
- **ui**: pick a theme under View ▸ Theme: Ember, Light or Neutral dark
- **core**: autosave unsaved changes to a recovery file
- **ui**: rename and duplicate projects from the project library
- **ui**: notice media files going missing or coming back while the app runs
- **ui**: show each overlay track's layer and move it forward or backward
- **timeline**: fold a video track's sound tracks away under it
- **timeline**: keep unlinked audio in place and own-sound tracks to their source
- **timeline**: tracks in fixed groups, each overlay with its own sound track
- **audio**: an overlay's sound works like a video clip's
- **audio**: Audio tab lists every audio clip of the video clip
- **audio**: Audio tab in three sections
- **audio**: every .insv audio stream on its own track
- **timeline**: edit toolbar above the timeline, centred
- **timeline**: timeline across the full window width
- **ui**: Help ▸ User manual opens the manual on GitHub
- **ui**: player bar under the viewport
- **ui**: settings sidebar in tabs, with collapsible sections
- **ui**: locate a moved or renamed media file
- **ui**: the left sidebar can be resized
- **ui**: left sidebar with the Media tab, the project's media pool
- **ui**: the Ember theme, built from colour tokens
- **ui**: project library start screen and one folder per project
- **insv**: read the camera's stitched thumbnail from the trailer

### Fix

- **ui**: show "1 key" / "3 keys" instead of "key(s)" style plurals
- **ui**: keep the player bar's time moving while Track subject or Blur a plate runs
- **timeline**: keep a folded overlay track folded through undo and redo
- **core**: write a finished sync or seam measurement into the project file
- **ui**: keep a video overlay's frame rate in the media pool
- **timeline**: scroll the tracks while a drag is held at their top or bottom
- **timeline**: name an overlay's track "audio" and indent the mic tracks
- **audio**: move hand-placed mics onto their stream tracks on open
- **core**: opening a project no longer empties the project library
- **ui**: Enter presses a dialog's default button again

### Refactor

- **core**: drop the "micAir" track role alias
- **timeline**: drop the "overlaySound" role alias
- **timeline**: call an overlay's track its audio track in the code too

### Perf

- **ui**: keep the Media tab's thumbnails on disk

## v0.2.0 (2026-10-11)

### Feat

- **timeline**: drop .insv clips from the file manager
- **stab**: set stabilization per clip

### Fix

- **build**: .deb file names without '~', as GitHub serves them

## v0.1.0 (2026-10-10)

### Feat

- orbis-reframe, a 360 video editor for the Insta360 X5
