#pragma once

#include <QHash>
#include <QKeySequence>
#include <QList>
#include <QMainWindow>
#include <QString>

#include <functional>
#include <array>
#include <memory>
#include <optional>
#include <vector>

#include <GyroStabilizer.h> // insv::GyroStabilizer / StabConfig
#include <InsvMediaInfo.h>   // insv::MediaInfo

#include "PrivacyBlur.h"              // PrivacyBlur
#ifdef ORBIS_REFRAME_HAVE_PLATE_SCAN
#include "PlateScanJob.h"             // platescan::Request, Result, FollowResult
#endif
#ifdef ORBIS_REFRAME_HAVE_TRACKING
#include "SubjectTrackJob.h"          // subjecttrack::Result
#endif
#include "ClipBackend.h"              // ClipBackend
#include "ShortcutHelp.h"             // KeyHelp
#include "StabilizerCache.h"          // StabilizerCache
#include "FrameViewCache.h"           // FrameViewCache
#include "ReframeExportController.h" // ReframeExportController::ClipJob
#include "../core/ClipItem.h"         // ColorGrade (the copied grade)
#include "../core/InsetGeometry.h"    // inset::Look (overlays)

class QAction;
class QTimer;
class QCloseEvent;
class QShortcut;
class QSplitter;
class QStackedWidget;
class QThread;
class QVulkanInstance;
class Project;
class ProjectLibrary;
class ProjectFiles;
class TrackFiles;
class ProjectLibraryPage;
class Timeline;
struct ClipItem;
class EquirectViewportWindow;
class PlaybackController;
class PlateJobController;
class ClipMeasurements;
class ExportRunner;
class MediaImporter;
class SubjectTrackController;
class AudioPlayer;
class AudioPanel;
class ColourPanel;
class PrivacyPanel;
class MediaInfoPanel;
class OverlayPanel;
class ReframePanel;
class MediaPoolPanel;
class TitlesPanel;
class TitleTextPanel;
class TransitionsPanel;
class TransitionPanel;
class SettingsSidebar;
class TabSidebar;
class QActionGroup;
namespace stitch {
struct PreviewStitchConfig;
}
class StitchingPanel;
class StabilizationPanel;
class TimelineWidget;
class WaveformCache;
class ThumbnailCache;
class Autosave;
class PlayerBar;
class EditToolbar;
struct Track;
struct TrackClip;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    // No Vulkan instance (tests): the viewport is created but never shown,
    // so it keeps the view's state (pose, marker, frames handed to it)
    // without rendering; nothing else needs a GPU until a clip is stitched
    // through `clipBackend`.
    explicit MainWindow(QVulkanInstance *vulkanInstance,
                        ClipBackend clipBackend = ClipBackend::real(), QWidget *parent = nullptr);
    ~MainWindow() override;

    EquirectViewportWindow *viewport() const { return m_viewport; }

    // Open a path given on the command line: a *.orp project file loads the
    // project, anything else is treated as a loose source clip.
    void openPath(const QString &path);
    // Probe a bare source clip, make it a fresh one-clip project, and play it.
    void openClip(const QString &clipPath);
    // Load a project file and start playing its timeline.
    void openProject(const QString &projectPath);
    // The start screen (no project open): the project library.
    void showLibrary();

protected:
    void closeEvent(QCloseEvent *event) override;
    // Installed on the application: a text/number field (the panels' spin
    // boxes) claims Space, [ ], K, Delete... while it has focus, so the
    // editor's shortcuts go dead. A click anywhere outside the field --
    // viewport, timeline, empty panel space -- or Enter / Escape in it hands
    // focus back (the field commits its value on focus-out).
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    // The constructor's parts, in the order it runs them: the viewport (and
    // the widget holding its place), the edit model, the window's layout and
    // pages, the playback thread, then each area's wiring and the menus.
    // Where two parts connect to the same signal, that order is the order
    // their slots run in.
    QWidget *createViewport(QVulkanInstance *vulkanInstance);
    void setupModel();
    void setupSourcesSidebar();
    void setupLayout(QWidget *viewportContainer);
    void setupPages();
    void setupPlayback();
    void connectStitchAndStabilization();
    void connectTimeline();
    void connectOverlaysAndAudio();
    void connectEdits();
    void connectReframe();
    void connectColour();
    void connectMeasurements();
    void connectPrivacy();
    void connectTransitions();
    void setupMenus();
    void connectController();
    void connectPlayerBar();
    void setupMediaCheck();
    void connectFullScreen();

    // File -> "Export flat video…": checks (NVENC, the privacy blur), the
    // options dialog and where to save, then m_exports renders the whole
    // timeline to one flat .mp4.
    void exportFlatVideo();
    // One timeline clip as the export renders it (renderjobs::clipJob).
    ReframeExportController::ClipJob exportJobFor(const ClipItem &c) const;
    // Player bar camera button / Ctrl+Shift+P: render the frame at the
    // playhead at the project output size, through the export path (same
    // stitch, stabilization, grade; the viewport's current view), and save
    // it as a PNG where a save dialog says.
    void takeScreenshot();
    void updateExportEnabled();
    void updateScreenshotEnabled();
    // Track subject (Reframe panel; ORBIS_REFRAME_HAVE_TRACKING): arm / disarm
    // drawing a box in the viewport; a drawn box starts tracking from the
    // playhead, and the path lands in the clip's ReframeTrack as keys.
    void setTrackSelecting(bool on);
    void startSubjectTracking(const QRectF &box);
#ifdef ORBIS_REFRAME_HAVE_TRACKING
    // The tracked path -> the clip's reframe keys; the playhead to where it
    // ended.
    void finishSubjectTracking(const subjecttrack::Result &r);
#endif
    // A Track subject run is going (always false without tracking).
    bool trackingRunning() const;
    // Point the viewport along the authored reframe path. `sourceMs` is in the
    // current clip's (m_clipId) source time. No-op when the track is empty or a
    // viewport drag is in progress.
    void applySampledPose(qint64 sourceMs);
    // The clip under the playhead and the one after it: build their
    // stabilizers (so a cut finds the next one ready), publish, and refresh
    // the paused picture if the current one was new.
    void ensureStabilizers();
    // The current clip's sync / seam status lines from m_measurements.
    void updateSyncStatus();
    void updateSeamStatus();
    // The seam alignment fine-tune for a source (ClipItem::seamAdjustDeg), if
    // it was measured under the current lens-guard setting (else zero).
    // pushSeamAdjust() hands every timeline source's value to the playback
    // controller.
    bool seamAdjustFor(const QString &clipPath, std::array<double, 3> &deg) const;
    void pushSeamAdjust();
    // Push the current clip's (m_clipId) correction for `sourceMs` (its
    // source time) into the viewport. Identity when its stabilization is Off
    // or its stabilizer isn't built.
    void applyStabilization(qint64 sourceMs);
    // Copy the viewport's current pose into the Reframe panel's numeric fields
    // (whatever's on screen -- sampled path, free-look, or a drag result).
    void syncReframePanel();
    // Push a clip's colour grade into the viewport. Small helper since
    // every call site has a ColorGrade struct but SetColorGrade takes a flat
    // float[4].
    void applyColorGrade(const ColorGrade &grade);
    // Bind authoring (m_clipId / m_lastPositionMs / stabilizer / viewport pose
    // / Audio+Colour panels) to whichever clip covers `timelineMs`. Shared by
    // the playback position tick and undo()/redo() (which replace the whole
    // Timeline, so m_clipId can't just be reused -- rebind by position).
    void bindActiveClipAt(qint64 timelineMs);
    // Moves the player bar and the timeline's playhead together.
    void showPlayhead(qint64 timelineMs);
    // Undo/redo: Project::undo()/redo() rebuild the whole Timeline (new
    // clip ids throughout), so this resyncs everything downstream of the
    // model by timeline position rather than trying to preserve m_clipId.
    void undo();
    void redo();
    void afterUndoRedo();

    // Rebuild the controller's segment list from the timeline (also updates
    // m_totalTimelineMs). Queued to the worker thread.
    void pushTimelineToController();
    // Audio streams in `path`, cached per source.
    int audioStreamCount(const QString &path);
    // A stream's waveform for the timeline (null until scanned -- the scan
    // starts here and refreshTimelineWidget runs when it lands).
    std::shared_ptr<const std::vector<float>> waveformFor(const QString &path, int stream);
    // Extra tracks: push them to the timeline widget; select an audio clip
    // (-1 = none); react to any change to them.
    void refreshTrackRows();
    void selectTrackClip(int clipId);
    // Transitions (MainWindowTransitions.cpp). Select the one at the cut
    // after clip `leftClipId` (0 = none): the Transition tab shows it.
    void selectTransition(int leftClipId);
    // Put a transition of `type` (Transition::typeName) at the cut after
    // clip `leftClipId`, or at the cut nearest the playhead; it's selected.
    void addTransition(int leftClipId, const QString &type);
    void addTransitionAtPlayhead(const QString &type);
    // The timeline's transition boxes and the Transition panel, from the
    // model (after a transition, trim or speed changed).
    void refreshTransitions();
    // The video clip selected by a click on the timeline (0 = none: the
    // panels then follow the playhead). The panels (Audio, Colour, Media
    // info) and the trim buttons edit editClipId(): the selected clip, else
    // the one under the playhead. Reframe keys and the viewport stay with the
    // clip under the playhead (m_clipId).
    void selectVideoClip(int clipId);
    const ClipItem *selectedVideoClip() const;
    int editClipId() const;
    void refreshClipPanels();
    // Reflect the panel's clip's (editClipId()) stabilization in the
    // Stabilization panel; greyed out without one.
    void syncStabilizationPanel();
    // The Stitching panel shows the clip it edits (editClipId), or nothing.
    void syncStitchingPanel();
    // Privacy blur (Privacy panel): the panel for the clip being edited;
    // every source's blur (and "Show regions" outlines) to playback.
    void syncPrivacyPanel();
    void pushBlur();
    // Right click on the picture: remove / restore the blur track under it
    // (manual correction).
    void showPictureMenu(const QPoint &globalPos, const std::array<float, 3> &dir);
    // Before an export / screenshot: false when a clip's blur is stale and
    // the user cancels.
    bool confirmStalePrivacy(const QString &title, const QStringList &warnings);
    // Before an export: false, after saying which, when any of `missing`
    // (source files) is missing -- the user sorts them out first.
    bool checkSourcesPresent(const QStringList &missing);
    void startPlateScan();
    // "Blur a plate…": arm / disarm the box select for it; follow the plate
    // in a drawn box (output-rect fractions) from the playhead on.
    void setPlateSelecting(bool on);
    // The "Blur a plate…" button / B: arm it, cancel the box, or stop a
    // running follow.
    void togglePlateBlur();
    void startPlateFollow(const QRectF &box);
#ifdef ORBIS_REFRAME_HAVE_PLATE_SCAN
    platescan::Request plateRequestFor(const ClipItem &c);
    // A finished scan -> the scan cache; a finished follow -> the clip's
    // blur additions.
    void finishPlateScan(const platescan::Result &r);
    void finishPlateFollow(const platescan::FollowResult &r);
#endif
    // A plate scan or follow is running / is a follow; stop it (all no-ops
    // without the plate scan).
    bool plateJobRunning() const;
    bool plateFollowing() const;
    void stopPlateJob();

    void onTracksChanged();
    void fitTimelineHeight(); // the split, until the user drags it
    // The Audio panel: the selected audio clip if there is one, else the
    // clip under the playhead.
    void refreshAudioPanel();
    // The audio clips the Audio tab shows, and what they are to it
    // (`scope`): the selected audio clip; else every clip linked to the
    // selected overlay clip, or to the video clip selected or under the
    // playhead, in track order.
    QList<int> audioPanelClipIds(QString *scope = nullptr) const;
    // Select the last clip TrackFiles added, if it added any.
    void selectAdded(std::optional<int> lastClipId);
    // A new title at `atMs` (clamped to the timeline) on overlay track
    // `trackId` if it has room there (else the first that has, else a new
    // one), selected, with the Overlay tab showing to edit it.
    void addTitle(int trackId, qint64 atMs);
    // The overlay clips to playback's InsetFeed.
    void pushInsetsToController();
    // The selected track clip if it is on an overlay track (else null).
    const TrackClip *selectedOverlayClip(int *trackId = nullptr) const;
    // The Overlay panel: the selected overlay clip, at the playhead.
    void refreshOverlayPanel();
    // The selected overlay clip's track a layer to the front (-1) / back (1).
    void moveSelectedOverlayLayer(int step);
    // The selected overlay clip's keys at the playhead (the Overlay panel's
    // Set key / Delete, and K / Shift+K with an overlay selected): set one
    // with its look there (the playhead must be over it), or delete the one
    // within Timeline::kInsetKeyDeleteSnapMs. Clear (Ctrl+Shift+K): delete
    // them all, leaving the look at the playhead.
    void setOverlayKeyAtPlayhead();
    void deleteOverlayKeyAtPlayhead();
    void clearOverlayKeys();
    // Set an overlay clip's look (position / size / opacity): a key at the
    // playhead if it has keys (the playhead must be over it), else its one
    // look.
    void applyInsetLook(int clipId, const inset::Look &look);
    // Put an overlay clip's sound on an audio track, linked to it so it
    // follows its moves, trims, splits and removal, and select it.
    void putOverlayAudioOnTrack(int clipId);
    // The same without selecting (adding an overlay does it): the audio
    // clip's id -- the one already there if its sound is on a track -- or 0
    // if the file has no sound.
    int addOverlayAudio(int clipId);
    // The trim buttons / [ ] keys: the selected audio clip if there is one
    // (at the playhead's point in it), else the video clip under the playhead.
    void trimAtPlayhead(bool in);
    void clearTrim();

    // Keyboard shortcuts. Every single-key / navigation shortcut goes
    // through addKey(), which also lists it in Help -> Keyboard shortcuts
    // (menu actions are listed from the menus themselves).
    void setupKeyboardShortcuts();
    // View menu: the preview's stitch quality and size.
    void setupViewMenu();
    void updatePreviewMenu(const stitch::PreviewStitchConfig &cfg);
    // File ▸ Project settings…
    void editProjectSettings();
    // `keysLabel` replaces the keys' own names in the list ("Alt+1 … Alt+9").
    void addKey(const QString &group, const QString &what, const QList<QKeySequence> &keys,
                const std::function<void()> &fn, const QString &keysLabel = QString());
    // Seek the playhead from the keyboard. Updates the cached position at
    // once, so a held arrow key steps from where the last press went rather
    // than from a position the worker hasn't reported yet.
    void navigateTo(qint64 timelineMs, bool pause);
    // Full screen (F11, a double-click on the picture): only the viewport and
    // the player bar, which hides when the mouse rests and comes back when it
    // moves. Off again with Esc / F11, or when the project closes.
    void setFullScreenView(bool on);
    void showPlayerBarForAWhile();
    // The player bar's time display (the project's output rate) and its cut
    // ticks, from the timeline.
    void updatePlayerBar();
    // The player bar's volume / mute -> the audio output, remembered per user.
    void applyPreviewVolume();
    // S: split the selected audio / overlay clip at the playhead if there is
    // one, else the video clip under the playhead.
    void splitAtPlayhead();
    // Delete: the selected audio / overlay clip, else the selected video
    // clip, else the reframe keyframe at the playhead.
    void deleteAtPlayhead();
    // Reflect clip `c`'s source file in the Media information panel (probed
    // once per path, cached in m_mediaInfo; a failed probe isn't cached).
    void syncMediaInfoPanel(const ClipItem &c);
    // Repaint the timeline widget from the model + refresh the edit
    // toolbar's reframe-button state + update the export action.
    void refreshTimelineWidget();
    // pushTimelineToController + refreshTimelineWidget.
    void syncTimeline();
    // syncTimeline + re-seek so the playhead stays on the same source frame
    // after a trim / structural edit. Connected to every structural / trim
    // Timeline signal.
    void remapPlayheadAfterEdit();
    // File dialog -> probe -> Timeline::insertClip at `index` (-1 = append).
    void addClipToTimeline(int index = -1);
    // Probe -> insert `paths` in order from `index` (-1 = append). Files that
    // aren't clips are reported and skipped. Used by the picker and by drops.
    void addClipFiles(const QStringList &paths, int index);

    // Media pool (the left sidebar's Media tab; importing is m_mediaImport's).
    // Add pool files to the timeline by kind: an .insv at the end of the
    // video track, flat video onto an overlay track and audio onto an audio
    // track at the playhead (the first track with room, or a new one).
    void addMediaToTimeline(const QStringList &paths);
    // Mark the files the timeline uses in the Media tab.
    void updateMediaInUse();
    // Locate file… on a missing file (MediaImporter::locate): the file found
    // (and others found with it) replaces the old path in the pool and on
    // the timeline.
    void locateMedia(const QString &path);
    // Files that went missing or came back since the last look (a drive
    // unplugged or plugged in again, a file moved away or back): the Media
    // tab, the timeline, playback, the overlays and the sound follow, and the
    // notice line says which.
    void recheckMissingMedia();

    // File menu project actions.
    // New project…: name, location and output format, then a project folder
    // with the (empty) project saved in it.
    void newProject();
    // Open project…: any project file, also one outside the library.
    void openProjectFile();
    // Project library…: close the project (asking to save) and show the
    // start screen.
    void closeToLibrary();
    // The editor in the window instead of the library; the editor's actions
    // and shortcuts work only while it's shown.
    void showEditor();
    void setEditorActive(bool active);
    // No clip: the panels' no-clip state, the controller idle.
    void bindEmptyTimeline();
    // Plate scans and waveforms in the project folder's cache/ (the user
    // cache for an untitled or loose project) of `projectFile`; on every
    // file path change, and before a project loads.
    void updateCacheDirs(const QString &projectFile);
    void updateWindowTitle();
    // QFileDialog::getSaveFileName with a "&Save" button (Alt+S / S in the
    // file list): Qt's own label has no mnemonic. `path` may name the file.
    QString askSaveFileName(const QString &caption, const QString &path, const QString &filter);

    // The window's two pages: the library (start screen) and the editor.
    QStackedWidget *m_pages = nullptr;
    ProjectLibraryPage *m_libraryPage = nullptr;
    QSplitter *m_editorPage = nullptr; // viewer pane over the timeline pane
    QWidget *m_timelinePane = nullptr; // the edit toolbar over the timeline
    bool m_timelineHeightSet = false;  // the user dragged the split (remembered)
    std::unique_ptr<ProjectLibrary> m_library;
    // Save / Save as, the unsaved-changes and recovery questions, the open
    // and new-project dialogs.
    std::unique_ptr<ProjectFiles> m_files;
    // Off while the library is shown (they'd act on the hidden editor).
    QList<QAction *> m_editorActions;
    // The left sidebar (sources) and its Media tab.
    TabSidebar *m_sourcesSidebar = nullptr;
    MediaPoolPanel *m_mediaPanel = nullptr;
    TitlesPanel *m_titlesPanel = nullptr;
    TransitionsPanel *m_transitionsPanel = nullptr;
    MediaImporter *m_mediaImport = nullptr; // Import files… / folder…, drops, Locate file…
    QList<QShortcut *> m_editorShortcuts;
    QActionGroup *m_previewQuality = nullptr; // View menu
    QActionGroup *m_previewSize = nullptr;

    ClipBackend m_clipBackend;
    EquirectViewportWindow *m_viewport = nullptr;
    // Without Vulkan: m_viewport, not in a window container, so owned here.
    std::unique_ptr<EquirectViewportWindow> m_unshownViewport;
    TimelineWidget *m_timelineWidget = nullptr;
    PlayerBar *m_playerBar = nullptr; // under the viewport
    EditToolbar *m_editToolbar = nullptr;
    // The right sidebar (settings) and its sections, which it owns.
    SettingsSidebar *m_sidebar = nullptr;
    StitchingPanel *m_stitchingPanel = nullptr;
    StabilizationPanel *m_stabilizationPanel = nullptr;
    ReframePanel *m_reframePanel = nullptr;
    AudioPanel *m_audioPanel = nullptr;
    QHash<QString, int> m_audioTrackCounts; // source path -> audio stream count
    ColourPanel *m_colourPanel = nullptr;
    OverlayPanel *m_overlayPanel = nullptr;        // overlay videos
    TitleTextPanel *m_titleTextPanel = nullptr;     // the Title tab: a title's text
    OverlayPanel *m_titlePlacementPanel = nullptr;  // ... and its position and timing
    TransitionPanel *m_transitionPanel = nullptr;   // the selected transition
    MediaInfoPanel *m_mediaInfoPanel = nullptr;
    QHash<QString, insv::MediaInfo> m_mediaInfo; // source path -> probe
    // The edit model: a Project owning a Timeline. Authoring (trim via [ ],
    // reframe via K / viewport drag) targets m_clipId, the clip currently
    // under the playhead (0 == nothing open); the timeline widget's own
    // edge-drag / context menu act on whichever clip was grabbed. m_timeline
    // is a cached m_project->timeline().
    Project *m_project = nullptr;
    Autosave *m_autosave = nullptr; // the recovery file of unsaved changes
    Timeline *m_timeline = nullptr;
    int m_clipId = 0;
    int m_selectedTrackClipId = -1; // an audio clip selected on the timeline
    int m_selectedClipId = 0;       // a video clip selected on the timeline
    int m_selectedTransition = 0;   // a transition selected: the clip before its cut
    std::unique_ptr<TrackFiles> m_trackFiles; // audio files and overlay videos onto tracks
    WaveformCache *m_waveforms = nullptr;
    // The Media tab's thumbnails on disk; shared with the panel's worker
    // threads, which may outlive this window's members.
    std::shared_ptr<ThumbnailCache> m_thumbnailCache;

    // Native gyro stabilization, per clip; the clip under the playhead and
    // the next one are built ahead (ensureStabilizers()).
    StabilizerCache m_stabilizers;
    FrameViewCache m_frameViews; // each clip's reframe path and grade, for presented frames
    // Hand every clip's built stabilizer (and its capture-time correction)
    // to playback. Call after the cache or the clips change.
    void publishStabilizer();
    // Per clip id: whether its capture-time correction was on (its
    // stabilization on) when last seen, to tell when a change toggles it.
    // Rebuilt with the controller's segments.
    QHash<int, bool> m_captureTimingOn;
    // The background sync / seam measurements. A result that lands while
    // playing is applied at the next pause (the re-stitch is a seek).
    ClipMeasurements *m_measurements = nullptr;
    bool m_playing = false;    // the controller's playingChanged
    double m_previewRate = 1.0; // the player bar's speed; the sound plays at 1x only
    bool m_fullScreen = false;  // setFullScreenView()
    bool m_wasMaximized = false; // before full screen
    QTimer *m_playerBarHide = nullptr; // full screen: hides the player bar
    QTimer *m_mediaCheck = nullptr; // recheckMissingMedia() while the app is in front
    // A sync / seam result arrived while playing; applied on the next pause.
    bool m_measuredWhilePlaying = false;
    // Total trimmed timeline length, cached from the last pushTimelineToController.
    qint64 m_totalTimelineMs = 0;

    PlaybackController *m_controller = nullptr;
    QThread *m_playbackThread = nullptr;
    // Audio playback, only when the app was built with Qt6::Multimedia (see
    // CMakeLists.txt) -- a slave of m_controller's clock, not a separate
    // playback path; see AudioPlayer's own comment.
#ifdef ORBIS_REFRAME_HAVE_AUDIO
    AudioPlayer *m_audioPlayer = nullptr;
#endif
    QAction *m_exportAction = nullptr;
    // The clip under the playhead, remembered so the export can re-open it
    // independently of playback; empty until a clip opens. Plus its frame rate.
    QString m_clipPath;
    double m_fps = 0.0;
    QAction *m_screenshotAction = nullptr;
    ExportRunner *m_exports = nullptr; // the export and screenshots, one of each at a time
    SubjectTrackController *m_tracker = nullptr; // Track subject (null without tracking)
    PrivacyPanel *m_privacyPanel = nullptr;
    PrivacyBlur m_privacy; // the plate scans and each clip's blur resolved against them
    // Plate scan or "Blur a plate…" follow, one at a time (null without the
    // plate scan). The source / start of the one running, for its handlers.
    PlateJobController *m_plateJobs = nullptr;
    QString m_plateJobSource;
    int m_plateJobClipId = 0;
    qint64 m_plateJobFromMs = 0, m_plateJobToMs = 0;
    bool m_plateSelecting = false;       // "Blur a plate…" is waiting for its box
    bool m_showPlateRegions = false; // Privacy panel "Show regions" (a view setting)
    bool m_trackSelecting = false;
    // Last playhead position, in the current clip's source time, so the
    // [ / ] / K shortcuts can act "at the playhead" without querying the
    // worker thread.
    qint64 m_lastPositionMs = 0;
    // Last playhead position in *timeline* ms (unlike m_lastPositionMs, valid
    // independently of m_clipId) -- undo()/redo() rebuild every ClipItem with
    // a fresh id, so afterUndoRedo() rebinds from this instead.
    qint64 m_lastTimelineMs = 0;
    QAction *m_undoAction = nullptr;
    // The grade from Copy grade (Colour panel / a clip's right-click menu);
    // kept for the session, not in the project.
    std::optional<ColorGrade> m_copiedGrade;
    std::optional<insv::StabConfig> m_copiedStab; // Copy / Paste stabilization
    QList<KeyHelp> m_keyHelp; // shortcuts made by addKey(), in order
    QAction *m_redoAction = nullptr;
    // Reframe authoring armed (drag/zoom writes a keyframe); viewport drag in
    // progress (suspend pushing the sampled pose).
    bool m_reframeArmed = false;
    bool m_viewportDragging = false;
    // True while openClip()/openProject() are rebuilding the model, so the
    // clipTrimChanged handler doesn't try to remap the playhead against a
    // half-built timeline / a stale m_clipId.
    bool m_rebindingModel = false;
};
