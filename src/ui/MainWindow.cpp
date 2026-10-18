#include "MainWindow.h"
#include "Theme.h"

#include "AboutDialog.h"
#include "NewProjectDialog.h"
#include "ProjectLibraryPage.h"
#include "SystemCheck.h"

#include "../core/Log.h"

#include <QAbstractSpinBox>
#include <QFontDatabase>
#include <QPlainTextEdit>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QDialog>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFile>
#include <QLocale>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlatformSurfaceEvent>
#include <QPointer>
#include <QProgressDialog>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSettings>
#include <QSplitter>
#include <QShortcut>
#include <QTimer>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QThread>
#include <QThreadPool>
#include <QVBoxLayout>
#include <QWidget>
#include <QWindow>

#include <array>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <cstdio>

#include <InsvAudioDemux.h> // insv::InsvAudioDemux (audio files)
#include <InsvTrailer.h>   // insv::ReadInsvTelemetry
#include <IStitchSource.h> // stitch::PreviewStitchConfig
#include <FlatVideoReader.h> // exportpipe::FlatVideoReader::probe (overlays)

#ifdef ORBIS_REFRAME_HAVE_AUDIO
#include "AudioPlayer.h"
#endif
#ifdef ORBIS_REFRAME_HAVE_TRACKING
#include "SubjectTrackController.h" // + track::seedFromViewBox, reduceToKeys
#endif
#ifdef ORBIS_REFRAME_HAVE_PLATE_SCAN
#include "PlateJobController.h"
#endif
#include "ClipMeasurements.h"
#include "ExportOptionsDialog.h"
#include "ExportRunner.h"
#include "RenderJobs.h"
#include "MediaImporter.h"
#include "MediaPoolPanel.h"
#include "TitlesPanel.h"
#include "TransitionsPanel.h"
#include "MediaProbe.h"
#include "PlaybackController.h"
#include "ReframeExportController.h"
#include "ProjectFiles.h"
#include "ProjectSettingsDialog.h"
#include "SettingsPanel.h"
#include "SettingsSidebar.h"
#include "TabSidebar.h"
#include "TimelineWidget.h"
#include "TrackFiles.h"
#include "ThumbnailCache.h"
#include "../core/Autosave.h"
#include "WaveformCache.h"
#include "PlayerBar.h"
#include "EditToolbar.h"
#include "UserManual.h"
#include "panels/AudioPanel.h"
#include "panels/ColourPanel.h"
#include "panels/MediaInfoPanel.h"
#include "panels/OverlayPanel.h"
#include "panels/TitleTextPanel.h"
#include "panels/PrivacyPanel.h"
#include "panels/ReframePanel.h"
#include "panels/StabilizationPanel.h"
#include "panels/StitchingPanel.h"
#include "../core/ClipItem.h"
#include "../core/MediaPool.h"
#include "../core/Project.h"
#include "../core/ProjectFolder.h"
#include "../core/ProjectLibrary.h"
#include "../core/Timeline.h"
#include "../core/Version.h"
#include "../render/EquirectViewportWindow.h"

MainWindow::MainWindow(QVulkanInstance *vulkanInstance, ClipBackend clipBackend, QWidget *parent)
    : QMainWindow(parent), m_clipBackend(std::move(clipBackend))
{
    setWindowTitle(QStringLiteral("orbis-reframe"));

    QWidget *viewportContainer = createViewport(vulkanInstance);
    qApp->installEventFilter(this); // see eventFilter(): release text fields

    m_timelineWidget = new TimelineWidget(this);
    m_waveforms = new WaveformCache(this); // the timeline's waveforms (scanned in the background)
    m_editToolbar = new EditToolbar(this);
    m_playerBar = new PlayerBar(this);

    setupModel();
    setupSourcesSidebar();
    setupLayout(viewportContainer);
    setupPages();
    setupPlayback();
    connectStitchAndStabilization();
    connectTimeline();
    connectOverlaysAndAudio();
    connectEdits();
    connectReframe();
    connectColour();
    connectMeasurements();
    connectPrivacy();
    connectTransitions();
    setupMenus();
    setupViewMenu();
    setupKeyboardShortcuts();
    connectController();
    connectPlayerBar();
    setupMediaCheck();
    connectFullScreen();

    m_playbackThread->start();
    setEditorActive(false); // the library is shown until a project opens
}

QWidget *MainWindow::createViewport(QVulkanInstance *vulkanInstance)
{
    m_viewport = new EquirectViewportWindow;
    m_viewport->setVulkanInstance(vulkanInstance);
    // Import decoded lens frames from the decoder's Vulkan device
    // (enabled only if supported; the viewport checks before using it).
    m_viewport->setDeviceExtensions({QByteArrayLiteral("VK_KHR_external_memory_fd")});
    // Once Vulkan is up: say if this GPU can't decode video or export.
    // Only the first time -- vulkanReady() repeats after a device loss.
    connect(m_viewport, &EquirectViewportWindow::vulkanReady, this,
            [this] { syscheck::warnAtStartup(this, m_viewport); }, Qt::SingleShotConnection);
    // The shaders write display-ready (gamma-encoded) colour, so the swapchain
    // must not encode it again: an *_SRGB format -- the first one a headless
    // Sway offers -- lifted every dark tone.
    m_viewport->setPreferredColorFormats({VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM});
    const auto viewportColors = [this] {
        const theme::Colors &tc = theme::colors();
        m_viewport->SetSurroundColor(tc.surround);
        m_viewport->SetMarkerColors(tc.accent, tc.success, tc.error);
    };
    viewportColors();
    connect(theme::notifier(), &theme::Notifier::changed, this, viewportColors);

    // Without Vulkan (tests) the viewport isn't shown -- it would only warn
    // that it can't start -- and a blank widget holds its place.
    QWidget *viewportContainer = nullptr;
    if (vulkanInstance) {
        viewportContainer = QWidget::createWindowContainer(m_viewport, this);
    } else {
        m_unshownViewport.reset(m_viewport);
        viewportContainer = new QWidget(this);
    }
    viewportContainer->setObjectName(QStringLiteral("viewport"));
    viewportContainer->setMinimumSize(320, 160);
    viewportContainer->setFocusPolicy(Qt::StrongFocus);
    return viewportContainer;
}

void MainWindow::setupModel()
{
    // The edit model, owned by the GUI thread: a Project holding a Timeline.
    // The whole timeline is handed to PlaybackController as a segment list
    // (pushTimelineToController); it plays across the clips in timeline time.
    m_project = new Project(this);
    m_timeline = m_project->timeline();
    m_autosave = new Autosave(m_project, Autosave::kDefaultIntervalMs, this);
    m_measurements = new ClipMeasurements(m_project, m_clipBackend.measure, this);
    m_exports = new ExportRunner(this);
    connect(m_exports, &ExportRunner::busyChanged, this, [this] {
        updateExportEnabled();
        updateScreenshotEnabled();
    });
    connect(m_exports, &ExportRunner::stillFinished, this,
            [this](const QString &path, const QString &error) {
                if (error.isEmpty()) {
                    m_editToolbar->showNotice(tr("Saved %1").arg(QDir::toNativeSeparators(path)));
                } else {
                    m_editToolbar->showNotice(QString());
                    QMessageBox::warning(this, tr("Screenshot failed"), error);
                }
            });
    connect(m_measurements, &ClipMeasurements::statusChanged, this, [this] {
        updateSyncStatus();
        updateSeamStatus();
    });
    // An older project's clips have their sound put on the stream tracks.
    m_project->setAudioStreamCounter([this](const QString &path) { return audioStreamCount(path); });
    connect(m_project, &Project::modifiedChanged, this, [this](bool) { updateWindowTitle(); });
    connect(m_project, &Project::filePathChanged, this, [this](const QString &) {
        updateWindowTitle();
        updateCacheDirs(m_project->filePath());
    });
    updateWindowTitle();
}

void MainWindow::setupSourcesSidebar()
{
    // The left sidebar (sources), beside the viewport: the Media tab.
    m_sourcesSidebar = new TabSidebar(TabSidebar::Side::Left, QStringLiteral("sourcesSidebar"), this);
    // Resizable (file names can be long); the width is remembered per user.
    const int sourcesWidth = std::max(280, fontMetrics().height() * 17);
    m_sourcesSidebar->setPageWidthRange(200, sourcesWidth, 900);
    m_sourcesSidebar->setPageWidth(
        QSettings(QStringLiteral("orbis-reframe"), QStringLiteral("orbis-reframe"))
            .value(QStringLiteral("sidebars/sourcesWidth"), sourcesWidth)
            .toInt());
    connect(m_sourcesSidebar, &TabSidebar::pageWidthChanged, this, [](int width) {
        QSettings(QStringLiteral("orbis-reframe"), QStringLiteral("orbis-reframe"))
            .setValue(QStringLiteral("sidebars/sourcesWidth"), width);
    });
    m_mediaPanel = new MediaPoolPanel(m_project->mediaPool());
    m_thumbnailCache = std::make_shared<ThumbnailCache>();
    m_mediaPanel->setThumbnailProvider(
        [cache = m_thumbnailCache](const MediaItem &item, const QSize &size) {
            return cache->get(item, size, &mediaThumbnail);
        });
    m_sourcesSidebar->addTab(m_mediaPanel, QStringLiteral("film"), tr("Media"));
    m_titlesPanel = new TitlesPanel;
    m_sourcesSidebar->addTab(m_titlesPanel, QStringLiteral("type"), tr("Titles"));
    connect(m_titlesPanel, &TitlesPanel::addTitleRequested, this,
            [this] { addTitle(0, m_lastTimelineMs); });
    m_transitionsPanel = new TransitionsPanel;
    m_sourcesSidebar->addTab(m_transitionsPanel, QStringLiteral("blend"), tr("Transitions"));
    m_sourcesSidebar->setToggleKeys(
        QKeySequence(Qt::CTRL | Qt::Key_B).toString(QKeySequence::NativeText));
    m_sourcesSidebar->setOpen(
        QSettings(QStringLiteral("orbis-reframe"), QStringLiteral("orbis-reframe"))
            .value(QStringLiteral("sidebars/sourcesOpen"), true)
            .toBool());
    connect(m_sourcesSidebar, &TabSidebar::openChanged, this, [](bool open) {
        QSettings(QStringLiteral("orbis-reframe"), QStringLiteral("orbis-reframe"))
            .setValue(QStringLiteral("sidebars/sourcesOpen"), open);
    });
    m_mediaImport = new MediaImporter(m_project, m_mediaPanel, m_sourcesSidebar,
                                      m_clipBackend.probe, this);
    connect(m_mediaPanel, &MediaPoolPanel::importFilesRequested, m_mediaImport,
            &MediaImporter::importFiles);
    connect(m_mediaPanel, &MediaPoolPanel::importFolderRequested, m_mediaImport,
            &MediaImporter::importFolder);
    connect(m_mediaPanel, &MediaPoolPanel::filesDropped, m_mediaImport, &MediaImporter::import);
    connect(m_mediaPanel, &MediaPoolPanel::addToTimelineRequested, this,
            &MainWindow::addMediaToTimeline);
    connect(m_mediaPanel, &MediaPoolPanel::locateRequested, this, &MainWindow::locateMedia);
}

void MainWindow::setupLayout(QWidget *viewportContainer)
{
    // Top: the sources sidebar, the viewport over its player bar, and the
    // settings sidebar (below). Bottom, across the full width: the timeline
    // under its edit toolbar.
    auto *viewColumn = new QVBoxLayout;
    viewColumn->setContentsMargins(0, 0, 0, 0);
    viewColumn->setSpacing(0);
    viewColumn->addWidget(viewportContainer, 1);
    viewColumn->addWidget(m_playerBar, 0);
    auto *top = new QHBoxLayout;
    top->setContentsMargins(0, 0, 0, 0);
    top->setSpacing(0);
    top->addWidget(m_sourcesSidebar, 0);
    top->addLayout(viewColumn, 1);
    m_timelinePane = new QWidget(this);
    m_timelinePane->setObjectName(QStringLiteral("timelinePane"));
    auto *timelineLayout = new QVBoxLayout(m_timelinePane);
    timelineLayout->setContentsMargins(0, 0, 0, 0);
    timelineLayout->setSpacing(0);
    timelineLayout->addWidget(m_editToolbar, 0);
    timelineLayout->addWidget(m_timelineWidget, 1);

    // Right: the settings sidebar. Each section is wired to Project /
    // PlaybackController / the clip under the playhead below.
    m_sidebar = new SettingsSidebar(this);
    m_stitchingPanel = m_sidebar->stitching();
    m_reframePanel = m_sidebar->reframe();
    m_stabilizationPanel = m_sidebar->stabilization();
    m_stabilizationPanel->setNoClip(); // nothing open yet
    m_stitchingPanel->setNoClip();
    m_audioPanel = m_sidebar->audio();
    m_audioPanel->setNoClip();
    m_colourPanel = m_sidebar->colour();
    m_colourPanel->setNoClip();
    m_privacyPanel = m_sidebar->privacy();         // starts in its no-clip state
    m_overlayPanel = m_sidebar->overlay();         // starts with no overlay clip
    m_titleTextPanel = m_sidebar->titleText();     // ... and no title
    m_titlePlacementPanel = m_sidebar->titlePlacement();
    m_transitionPanel = m_sidebar->transition();   // ... and no transition
    m_mediaInfoPanel = m_sidebar->mediaInfo();     // starts in its no-clip state
    // Open / closed, its width, its tab and which sections are open are
    // remembered per user, like the left sidebar's.
    {
        QSettings settings(QStringLiteral("orbis-reframe"), QStringLiteral("orbis-reframe"));
        m_sidebar->setPageWidth(
            settings.value(QStringLiteral("sidebars/settingsWidth"), m_sidebar->pageWidth())
                .toInt());
        m_sidebar->setOpen(settings.value(QStringLiteral("sidebars/settingsOpen"), true).toBool());
        m_sidebar->setCurrentIndex(settings.value(QStringLiteral("sidebars/settingsTab"), 0).toInt());
        for (SettingsPanel *section : m_sidebar->sections()) {
            const QString key = QStringLiteral("sidebars/sections/") + section->objectName();
            section->setExpanded(settings.value(key, section->isExpanded()).toBool());
            connect(section, &SettingsPanel::expandedToggled, this, [key](bool expanded) {
                QSettings(QStringLiteral("orbis-reframe"), QStringLiteral("orbis-reframe"))
                    .setValue(key, expanded);
            });
        }
    }
    m_sidebar->setToggleKeys(
        QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_B).toString(QKeySequence::NativeText));
    connect(m_sidebar, &TabSidebar::pageWidthChanged, this, [](int width) {
        QSettings(QStringLiteral("orbis-reframe"), QStringLiteral("orbis-reframe"))
            .setValue(QStringLiteral("sidebars/settingsWidth"), width);
    });
    connect(m_sidebar, &TabSidebar::openChanged, this, [](bool open) {
        QSettings(QStringLiteral("orbis-reframe"), QStringLiteral("orbis-reframe"))
            .setValue(QStringLiteral("sidebars/settingsOpen"), open);
    });
    connect(m_sidebar, &TabSidebar::currentChanged, this, [](int index) {
        QSettings(QStringLiteral("orbis-reframe"), QStringLiteral("orbis-reframe"))
            .setValue(QStringLiteral("sidebars/settingsTab"), index);
    });

    top->addWidget(m_sidebar, 0);
    auto *topPane = new QWidget(this);
    topPane->setObjectName(QStringLiteral("viewerPane"));
    topPane->setLayout(top);

    // The split between them is dragged by the user and remembered per user;
    // the first time, the timeline gets the height its tracks want.
    m_editorPage = new QSplitter(Qt::Vertical, this);
    m_editorPage->setObjectName(QStringLiteral("editorSplitter"));
    m_editorPage->setChildrenCollapsible(false);
    m_editorPage->addWidget(topPane);
    m_editorPage->addWidget(m_timelinePane);
    m_editorPage->setStretchFactor(0, 1);
    m_editorPage->setStretchFactor(1, 0);
    {
        const QByteArray state =
            QSettings(QStringLiteral("orbis-reframe"), QStringLiteral("orbis-reframe"))
                .value(QStringLiteral("layout/editorSplitter"))
                .toByteArray();
        m_timelineHeightSet = !state.isEmpty() && m_editorPage->restoreState(state);
        if (!m_timelineHeightSet)
            m_editorPage->setSizes({1000, m_timelinePane->sizeHint().height()});
    }
    connect(m_editorPage, &QSplitter::splitterMoved, this, [this] {
        m_timelineHeightSet = true;
        QSettings(QStringLiteral("orbis-reframe"), QStringLiteral("orbis-reframe"))
            .setValue(QStringLiteral("layout/editorSplitter"), m_editorPage->saveState());
    });
}

void MainWindow::setupPages()
{
    // The start screen (no project open) and the editor share the window.
    m_library = std::make_unique<ProjectLibrary>();
    m_files = std::make_unique<ProjectFiles>(m_project, m_library.get(), m_autosave, this);
    m_trackFiles = std::make_unique<TrackFiles>(
        m_project, [this](int clipId) { addOverlayAudio(clipId); }, this);
    m_libraryPage = new ProjectLibraryPage(m_library.get(), this);
    m_libraryPage->setFlatThumbnail([](const QString &path, const QSize &size) {
        return mediaThumbnail({path, MediaItem::Kind::Video}, size);
    });
    connect(m_libraryPage, &ProjectLibraryPage::openRequested, this, &MainWindow::openProject);
    connect(m_libraryPage, &ProjectLibraryPage::newProjectRequested, this, &MainWindow::newProject);
    connect(m_libraryPage, &ProjectLibraryPage::openOtherRequested, this,
            &MainWindow::openProjectFile);
    m_pages = new QStackedWidget(this);
    m_pages->addWidget(m_libraryPage);
    m_pages->addWidget(m_editorPage);
    m_pages->setCurrentWidget(m_libraryPage); // until a project opens (main.cpp)
    setCentralWidget(m_pages);
}

void MainWindow::setupPlayback()
{
    // The controller runs on its own thread: opening a clip and its first
    // stitch block, which must not stall the GUI. No parent
    // -- ownership is handed to the thread via the finished/deleteLater wiring
    // below.
    m_controller = new PlaybackController;
    m_controller->setViewport(m_viewport);
    m_controller->setSourceFactory(m_clipBackend.makeSource);
    m_controller->setStabilizationProvider(m_stabilizers.provider());
    m_controller->setViewProvider(m_frameViews.provider());
    // The copy of every clip's reframe path and grade that presented frames
    // take their view from: refreshed with any change to them or the clips.
    const auto views = [this] { m_frameViews.update(*m_timeline); };
    connect(m_timeline, &Timeline::clipReframeChanged, this, views);
    connect(m_timeline, &Timeline::clipColorChanged, this, views);
    connect(m_timeline, &Timeline::clipInserted, this, views);
    connect(m_timeline, &Timeline::clipRemoved, this, views);
    connect(m_timeline, &Timeline::clipsReordered, this, views);
    views();
    m_playbackThread = new QThread(this);
    m_controller->moveToThread(m_playbackThread);
    connect(m_playbackThread, &QThread::finished, m_controller, &QObject::deleteLater);

    // Audio: a slave of m_controller's clock, entirely on the GUI thread --
    // see AudioPlayer's own comment for why. A normal child, unlike
    // m_controller.
#ifdef ORBIS_REFRAME_HAVE_AUDIO
    m_audioPlayer = new AudioPlayer(this);
#endif
}

void MainWindow::connectStitchAndStabilization()
{
    // Stitching settings: a panel edit goes onto the file of the clip it
    // shows (every clip cut from it), "Apply to all clips" onto every 360
    // clip's; clipStitchChanged below re-opens the stitch with them.
    connect(m_stitchingPanel, &StitchingPanel::stitchEdited, this,
            [this](StitchingPanel::LensGuard guard, bool depthSeam) {
                const ClipItem *c = m_timeline->clipById(editClipId());
                if (c && !c->flat)
                    m_timeline->setSourceStitch(c->sourcePath, guard, depthSeam);
            });
    connect(m_stitchingPanel, &StitchingPanel::applyToAllRequested, this, [this] {
        const ClipItem *c = m_timeline->clipById(editClipId());
        if (!c || c->flat)
            return;
        const StitchingPanel::LensGuard guard = c->lensGuard;
        const bool depthSeam = c->depthSeam;
        for (int i = 0; i < m_timeline->count(); ++i)
            if (!m_timeline->clipAt(i).flat)
                m_timeline->setSourceStitch(m_timeline->clipAt(i).sourcePath, guard, depthSeam);
    });
    // Project settings (File ▸ Project settings…): output aspect / size /
    // fps. The viewport is pinned to the output aspect; export reads the size
    // and rate from the Project when it starts.
    connect(m_project, &Project::outputChanged, this, [this](const OutputSettings &out) {
        m_viewport->SetOutputAspect(static_cast<float>(out.aspectRatio()));
        pushInsetsToController(); // flat main clips are fitted to the output's shape
    });
    m_viewport->SetOutputAspect(static_cast<float>(m_project->output().aspectRatio()));
    // Stabilization panel -> the clip it shows (the selected one, else the
    // one under the playhead), like the Colour panel. All modes drive the
    // native reprojection-time path; the panel hands back the right
    // insv::StabConfig for the mode (LeanFollow window from its Strength
    // slider, Custom from the knobs, Off otherwise).
    connect(m_stabilizationPanel, &StabilizationPanel::modeChanged, this,
            [this](StabilizationPanel::Mode, const insv::StabConfig &nativeCfg) {
                if (const int id = editClipId())
                    m_timeline->setClipStabilization(id, nativeCfg);
            });
    // Copy / paste a clip's stabilization, as the colour grade.
    const auto copyStab = [this](int clipId) {
        const ClipItem *c = m_timeline->clipById(clipId);
        if (!c)
            return;
        m_copiedStab = c->stabilization;
        m_stabilizationPanel->setCanPaste(true);
        m_timelineWidget->setCanPasteStabilization(true);
    };
    const auto pasteStab = [this](int clipId) {
        if (m_copiedStab && clipId)
            m_timeline->setClipStabilization(clipId, *m_copiedStab);
    };
    connect(m_stabilizationPanel, &StabilizationPanel::copyRequested, this,
            [this, copyStab] { copyStab(editClipId()); });
    connect(m_stabilizationPanel, &StabilizationPanel::pasteRequested, this,
            [this, pasteStab] { pasteStab(editClipId()); });
    connect(m_timelineWidget, &TimelineWidget::stabilizationCopyRequested, this, copyStab);
    connect(m_timelineWidget, &TimelineWidget::stabilizationPasteRequested, this, pasteStab);
    connect(m_timelineWidget, &TimelineWidget::stabilizationPasteAllRequested, this, [this] {
        if (!m_copiedStab)
            return;
        for (int i = 0; i < m_timeline->count(); ++i)
            m_timeline->setClipStabilization(m_timeline->clipAt(i).id, *m_copiedStab);
    });
    // The preview's quality and size (View menu): the controller re-opens
    // its sources with them.
    connect(m_project, &Project::previewStitchChanged, this,
            [this](const stitch::PreviewStitchConfig &cfg) {
                updatePreviewMenu(cfg);
                QMetaObject::invokeMethod(m_controller, [ctrl = m_controller, cfg] {
                    ctrl->setPreviewConfig(cfg);
                });
            });
    // A file's stitch settings: its segments carry them to the controller,
    // which re-opens its stitch. A seam fine-tune is only valid for the guards
    // it was measured with, so the file may need measuring again.
    connect(m_timeline, &Timeline::clipStitchChanged, this, [this](int id) {
        if (m_rebindingModel)
            return; // a load / undo: the rebind after it covers this
        const ClipItem *c = m_timeline->clipById(id);
        if (!c)
            return;
        pushTimelineToController();
        pushSeamAdjust();
        m_measurements->forgetSeamFailure(c->sourcePath);
        m_measurements->schedule();
        updateSeamStatus();
        if (id == editClipId())
            syncStitchingPanel();
    });
    connect(m_timeline, &Timeline::clipStabilizationChanged, this,
            [this](int id, const insv::StabConfig &) {
                if (m_rebindingModel)
                    return; // a load / undo: the rebind after it covers this
                if (id == editClipId())
                    syncStabilizationPanel();
                // The stab matrix is applied at present time, but the per-lens
                // capture-time correction is baked into stitched frames, so
                // re-decode in place when it toggles with stabilization.
                const ClipItem *c = m_timeline->clipById(id);
                const bool rsOn = c && StabilizerCache::captureTimingOn(*c);
                const bool rsToggled = m_captureTimingOn.value(id, false) != rsOn;
                m_captureTimingOn[id] = rsOn;
                ensureStabilizers();
                if (id == m_clipId) {
                    applyStabilization(m_lastPositionMs);
                    updateSyncStatus();
                }
                if (rsToggled) {
                    QMetaObject::invokeMethod(m_controller, "seek", Qt::QueuedConnection,
                                              Q_ARG(qint64, m_lastTimelineMs));
                    // The privacy blur was found in one of the two stitches.
                    syncPrivacyPanel();
                    qint64 from = 0, to = 0;
                    if (c && m_timeline->shownSourceRange(c->id, from, to) &&
                        !m_privacy.staleness(*c, from, to, false).isEmpty())
                        m_editToolbar->showNotice(
                            tr("The privacy blur was found with the stabilization %1; it may sit "
                               "off the plates now. See the Privacy panel.")
                                .arg(rsOn ? tr("off") : tr("on")),
                            true);
                }
            });
}

void MainWindow::connectTimeline()
{
    // View -> controller. These connections cross the thread boundary, so Qt
    // queues them onto the worker's event loop automatically.
    connect(m_playerBar, &PlayerBar::playPauseClicked,
            m_controller, &PlaybackController::togglePlay);

    // Timeline widget -> controller / model. It works entirely in timeline
    // time.
    const auto seekTo = [this](qint64 timelineMs) {
        QMetaObject::invokeMethod(m_controller, "seek", Qt::QueuedConnection,
                                  Q_ARG(qint64, timelineMs));
#ifdef ORBIS_REFRAME_HAVE_AUDIO
        m_audioPlayer->seek(timelineMs);
#endif
    };
    const auto scrubTo = [this](qint64 timelineMs) {
        QMetaObject::invokeMethod(m_controller, "scrub", Qt::QueuedConnection,
                                  Q_ARG(qint64, timelineMs));
#ifdef ORBIS_REFRAME_HAVE_AUDIO
        m_audioPlayer->seek(timelineMs);
#endif
    };
    connect(m_timelineWidget, &TimelineWidget::seekRequested, this, seekTo);
    connect(m_timelineWidget, &TimelineWidget::scrubRequested, this, scrubTo);
    // The player bar's progress strip scrubs like the ruler.
    connect(m_playerBar, &PlayerBar::seekRequested, this, seekTo);
    connect(m_playerBar, &PlayerBar::scrubRequested, this, scrubTo);
    connect(m_timelineWidget, &TimelineWidget::clipTrimInRequested, m_timeline,
            &Timeline::setClipTrimIn);
    connect(m_timelineWidget, &TimelineWidget::clipTrimOutRequested, m_timeline,
            &Timeline::setClipTrimOut);
    connect(m_timelineWidget, &TimelineWidget::clipMoved, m_timeline, &Timeline::moveClip);
    connect(m_timelineWidget, &TimelineWidget::clipRemoveRequested, m_timeline,
            &Timeline::removeClip);
    connect(m_timelineWidget, &TimelineWidget::clipSplitRequested, this,
            [this](int id, qint64 atSourceMs) { m_timeline->splitClip(id, atSourceMs); });
    connect(m_timelineWidget, &TimelineWidget::clipDuplicateRequested, this, [this](int id) {
        if (const int copy = m_timeline->duplicateClip(id))
            selectVideoClip(copy);
    });
    connect(m_timelineWidget, &TimelineWidget::clipSpeedRequested, m_timeline,
            &Timeline::setClipSpeed);

    // Extra tracks. The widget edits them through Timeline; any change
    // comes back as tracksChanged, which redraws and re-feeds the mixer.
    connect(m_timelineWidget, &TimelineWidget::trackClipSelected, this,
            &MainWindow::selectTrackClip);
    // Clicking a video clip selects it for the panels and the trim buttons,
    // without moving the playhead; 0 = back to following the playhead.
    connect(m_timelineWidget, &TimelineWidget::clipSelected, this, &MainWindow::selectVideoClip);
    connect(m_timeline, &Timeline::clipRemoved, this, [this](int id) {
        if (id == m_selectedClipId)
            selectVideoClip(0);
    });
    connect(m_timelineWidget, &TimelineWidget::trackClipMoved, m_timeline,
            &Timeline::moveTrackClip);
    connect(m_timelineWidget, &TimelineWidget::trackClipTrimInRequested, m_timeline,
            &Timeline::setTrackClipTrimIn);
    connect(m_timelineWidget, &TimelineWidget::trackClipTrimOutRequested, m_timeline,
            &Timeline::setTrackClipTrimOut);
    connect(m_timelineWidget, &TimelineWidget::trackClipRemoveRequested, m_timeline,
            &Timeline::removeTrackClip);
    connect(m_timelineWidget, &TimelineWidget::audioClipLinkRequested, this,
            [this](int id, bool link) {
                const TrackClip *a = m_timeline->trackClipById(id);
                if (a)
                    m_timeline->linkAudioClip(id, link ? m_timeline->linkTargetFor(*a) : 0);
            });
    connect(m_timelineWidget, &TimelineWidget::trackClipSplitRequested, this,
            [this](int id, qint64 atMs) {
                if (const int right = m_timeline->splitTrackClip(id, atMs))
                    selectTrackClip(right);
            });
    connect(m_timelineWidget, &TimelineWidget::trackMuteToggled, this, [this](int id, bool muted) {
        if (const Track *t = m_timeline->trackById(id))
            m_timeline->setTrackAudio(id, t->gainDb, muted);
    });
    connect(m_timelineWidget, &TimelineWidget::trackRemoveRequested, m_timeline,
            &Timeline::removeTrack);
    connect(m_timelineWidget, &TimelineWidget::trackRenameRequested, m_timeline,
            &Timeline::setTrackName);
    connect(m_timelineWidget, &TimelineWidget::trackMoved, m_timeline, &Timeline::moveTrack);
    // Folded sound tracks are a view setting, not part of the project (no
    // undo, no unsaved change): the main track's are remembered per user,
    // an overlay track's for this session.
    m_timelineWidget->setSoundCollapsed(
        0, QSettings(QStringLiteral("orbis-reframe"), QStringLiteral("orbis-reframe"))
               .value(QStringLiteral("timeline/videoSoundFolded"), false)
               .toBool());
    connect(m_timelineWidget, &TimelineWidget::soundCollapsedToggled, this,
            [this](int videoTrackId, bool collapsed) {
                if (videoTrackId == 0)
                    QSettings(QStringLiteral("orbis-reframe"), QStringLiteral("orbis-reframe"))
                        .setValue(QStringLiteral("timeline/videoSoundFolded"), collapsed);
                fitTimelineHeight();
            });
    connect(m_timelineWidget, &TimelineWidget::addClipRequested, this,
            &MainWindow::addClipToTimeline);
    connect(m_timelineWidget, &TimelineWidget::clipFilesDropped, this,
            &MainWindow::addClipFiles);
    connect(m_timelineWidget, &TimelineWidget::addAudioTrackRequested, this,
            [this] { m_timeline->addTrack(Track::Kind::Audio); });
    connect(m_timelineWidget, &TimelineWidget::addAudioFileRequested, this,
            [this](int trackId, qint64 atMs) { selectAdded(m_trackFiles->pickAudio(trackId, atMs)); });
    connect(m_timelineWidget, &TimelineWidget::audioFilesDropped, this,
            [this](const QStringList &paths, int trackId, qint64 atMs) {
                selectAdded(m_trackFiles->addAudio(paths, trackId, atMs));
            });
    // Overlay (video) tracks.
    connect(m_timelineWidget, &TimelineWidget::addVideoTrackRequested, this,
            [this] { m_timeline->addTrack(Track::Kind::Video); });
    connect(m_timelineWidget, &TimelineWidget::addVideoFileRequested, this,
            [this](int trackId, qint64 atMs) { selectAdded(m_trackFiles->pickVideo(trackId, atMs)); });
    connect(m_timelineWidget, &TimelineWidget::videoFilesDropped, this,
            [this](const QStringList &paths, int trackId, qint64 atMs) {
                selectAdded(m_trackFiles->addVideo(paths, trackId, atMs));
            });
    connect(m_timelineWidget, &TimelineWidget::titleDropped, this, &MainWindow::addTitle);
}

void MainWindow::connectOverlaysAndAudio()
{
    // Picking an inset in the view selects its clip; dragging it moves it.
    connect(m_viewport, &EquirectViewportWindow::insetPressed, this,
            [this](int clipId) { selectTrackClip(clipId); });
    connect(m_viewport, &EquirectViewportWindow::insetMoved, this,
            [this](int clipId, float cx, float cy) {
                const TrackClip *c = m_timeline->trackClipById(clipId);
                if (!c)
                    return;
                const qint64 src = c->sourceAtLocalMs(
                    std::clamp<qint64>(m_lastTimelineMs - c->startMs, 0, c->timelineDurationMs()));
                inset::Look l = c->lookAt(src);
                l.centerX = cx;
                l.centerY = cy;
                applyInsetLook(clipId, l);
            });
    // Overlay tab (overlay videos) and the Title tab's Position and timing
    // (titles) -> the selected overlay clip. Each only ever shows its kind.
    for (OverlayPanel *panel : {m_overlayPanel, m_titlePlacementPanel}) {
        connect(panel, &OverlayPanel::lookEdited, this, [this](const inset::Look &l) {
            if (const TrackClip *c = selectedOverlayClip())
                applyInsetLook(c->id, l);
        });
        connect(panel, &OverlayPanel::frameEdited, this, [this](const inset::Frame &f) {
            if (const TrackClip *c = selectedOverlayClip())
                m_timeline->setTrackClipInset(c->id, c->look, f);
        });
        connect(panel, &OverlayPanel::timingEdited, this,
                [this](qint64 fadeInMs, qint64 fadeOutMs, qint64 startMs, qint64 spanMs) {
                    int trackId = 0;
                    const TrackClip *c = selectedOverlayClip(&trackId);
                    if (!c)
                        return;
                    const int id = c->id;
                    // The length first: the fades are clamped to it.
                    if (spanMs != c->timelineDurationMs())
                        m_timeline->setTrackClipTrimOut(id, c->trimInMs + std::llround(spanMs * c->speed));
                    m_timeline->setTrackClipFades(id, fadeInMs, fadeOutMs);
                    m_timeline->moveTrackClip(id, trackId, startMs);
                });
        connect(panel, &OverlayPanel::bringForwardRequested, this,
                [this] { moveSelectedOverlayLayer(-1); });
        connect(panel, &OverlayPanel::sendBackwardRequested, this,
                [this] { moveSelectedOverlayLayer(1); });
        connect(panel, &OverlayPanel::addKeyRequested, this,
                &MainWindow::setOverlayKeyAtPlayhead);
        connect(panel, &OverlayPanel::removeKeyRequested, this,
                &MainWindow::deleteOverlayKeyAtPlayhead);
        connect(panel, &OverlayPanel::clearKeysRequested, this,
                &MainWindow::clearOverlayKeys);
        connect(panel, &OverlayPanel::easeEdited, this, [this](keytrack::Ease e) {
            if (const TrackClip *c = selectedOverlayClip())
                m_timeline->setTrackClipInsetEase(
                    c->id, c->sourceAtLocalMs(m_lastTimelineMs - c->startMs), e);
        });
    }
    // Title tab's Text -> the selected title.
    connect(m_titleTextPanel, &TitleTextPanel::titleEdited, this, [this](const title::Style &t) {
        if (const TrackClip *c = selectedOverlayClip())
            m_timeline->setTrackClipTitle(c->id, t);
    });
    // Waveforms arrive from background scans: redraw.
    connect(m_waveforms, &WaveformCache::ready, this, &MainWindow::refreshTimelineWidget);
    connect(m_timelineWidget, &TimelineWidget::overlayAudioRequested, this,
            &MainWindow::putOverlayAudioOnTrack);
    connect(m_timeline, &Timeline::tracksChanged, this, &MainWindow::onTracksChanged);
    // Audio panel -> the audio clip a section shows, and its track.
    connect(m_audioPanel, &AudioPanel::audioClipEdited, this,
            [this](int id, double gainDb, bool muted, qint64 fadeInMs, qint64 fadeOutMs,
                   qint64 startMs) {
                int trackId = 0;
                if (!m_timeline->trackClipById(id, &trackId))
                    return;
                m_timeline->setAudioClipLevel(id, gainDb, muted);
                m_timeline->setTrackClipFades(id, fadeInMs, fadeOutMs);
                m_timeline->moveTrackClip(id, trackId, startMs);
            });
    connect(m_audioPanel, &AudioPanel::trackLevelEdited, m_timeline, &Timeline::setTrackAudio);
    connect(m_audioPanel, &AudioPanel::noiseEdited, m_timeline, &Timeline::setAudioClipNoise);
    connect(m_project, &Project::projectReset, this, [this] {
        selectTrackClip(-1);
        m_selectedClipId = 0; // ids don't survive a load / undo
    });
}

void MainWindow::connectEdits()
{
    // Trim from the transport buttons / [ ] shortcuts acts on the clip under
    // the playhead, at m_lastPositionMs (that clip's *source* time -- the
    // transport bar only knows timeline time). Timeline owns the clamp /
    // min-gap validation.
    connect(m_editToolbar, &EditToolbar::splitRequested, this, [this] { splitAtPlayhead(); });
    connect(m_editToolbar, &EditToolbar::trimInRequested, this,
            [this] { trimAtPlayhead(true); });
    connect(m_editToolbar, &EditToolbar::trimOutRequested, this,
            [this] { trimAtPlayhead(false); });
    connect(m_editToolbar, &EditToolbar::trimCleared, this,
            [this] { clearTrim(); });

    // Any structural or trim change to the timeline -> rebuild the controller's
    // segments and repaint the widget, then keep the playhead on the same
    // source frame it was showing: its timeline position moves when its
    // own trim-in shifts, or when an earlier clip's length changes.
    connect(m_timeline, &Timeline::clipTrimChanged, this, [this](int, qint64, qint64) {
        remapPlayheadAfterEdit();
    });
    connect(m_timeline, &Timeline::clipInserted, this, [this](int) { remapPlayheadAfterEdit(); });
    connect(m_timeline, &Timeline::clipRemoved, this, [this](int) { remapPlayheadAfterEdit(); });
    connect(m_timeline, &Timeline::clipsReordered, this, [this] { remapPlayheadAfterEdit(); });
    connect(m_timeline, &Timeline::clipSpeedChanged, this,
            [this](int, double) { remapPlayheadAfterEdit(); });
    connect(m_timeline, &Timeline::clipFitChanged, this, [this](int id, ClipItem::Fit fit) {
        pushInsetsToController();
        if (id == editClipId())
            m_reframePanel->setFlatClip(true, fit == ClipItem::Fit::Fill);
    });
}

void MainWindow::connectReframe()
{
    // Reframe authoring. The viewport is a dumb pose display + input device; this
    // window is the only thing that knows both the playhead and the Timeline.
    //  * arm toggle gates whether a look gesture writes a keyframe;
    //  * a finished drag / a zoom notch (lookCommitted) writes one at the
    //    playhead when armed;
    //  * during a drag we stop pushing the sampled pose so the path doesn't
    //    fight the user's hand.
    connect(m_editToolbar, &EditToolbar::reframeArmedChanged, this,
            [this](bool armed) { m_reframeArmed = armed; });
    connect(m_viewport, &EquirectViewportWindow::draggingChanged, this,
            [this](bool active) {
                m_viewportDragging = active;
                m_frameViews.setDragging(active);
            });
    connect(m_viewport, &EquirectViewportWindow::lookCommitted, this, [this] {
        if (m_reframeArmed && m_clipId)
            m_timeline->upsertClipReframeKey(
                m_clipId, m_lastPositionMs,
                {m_viewport->Yaw(), m_viewport->Pitch(), m_viewport->Roll(), m_viewport->FovY()});
        syncReframePanel(); // the drag / zoom moved the pose
    });

    // Reframe panel: a numeric edit sets the live viewport pose and, when
    // authoring is armed, keyframes it at the playhead -- same as a drag.
    connect(m_reframePanel, &ReframePanel::fitEdited, this, [this](bool fill) {
        m_timeline->setClipFit(editClipId(), fill ? ClipItem::Fit::Fill : ClipItem::Fit::Bars);
    });
    connect(m_reframePanel, &ReframePanel::poseEdited, this, [this](const ReframePose &p) {
        m_viewport->SetPose(p.yaw, p.pitch, p.roll, p.fovY);
        if (m_reframeArmed && m_clipId)
            m_timeline->upsertClipReframeKey(m_clipId, m_lastPositionMs, p);
    });
    // Easing needs no arming: it changes keys already there, not the path's
    // points.
    connect(m_reframePanel, &ReframePanel::easeEdited, this, [this](keytrack::Ease e) {
        if (m_clipId)
            m_timeline->setClipReframeEase(m_clipId, m_lastPositionMs, e);
    });
    // Track subject: the panel arms a box select in the viewport; a drawn box
    // starts tracking from the playhead.
    m_reframePanel->setTrackUnavailable(syscheck::trackUnavailableReason());
#ifdef ORBIS_REFRAME_HAVE_TRACKING
    connect(m_reframePanel, &ReframePanel::trackSubjectToggled, this,
            &MainWindow::setTrackSelecting);
    m_tracker = new SubjectTrackController(m_viewport, m_reframePanel->trackStopButton(), this);
    connect(m_reframePanel, &ReframePanel::trackStopRequested, m_tracker,
            &SubjectTrackController::stop);
    connect(m_tracker, &SubjectTrackController::progress, this, [this](qint64 ms, qint64 lostFor) {
        const SubjectTrackController::Run &run = m_tracker->run();
        const int idx = m_timeline->indexOfId(run.clipId);
        if (idx >= 0)
            showPlayhead(
                m_timeline->clipToTimeline(idx, m_timeline->clipAt(idx).localAtSourceMs(ms)));
        const QString t = QString::number((ms - run.req.fromMs) / 1000.0, 'f', 1);
        m_reframePanel->setTrackStatus(
            lostFor < 0 ? tr("%1 s tracked. Following the subject.").arg(t)
                        : tr("%1 s. Lost the subject %2 s ago; stopping keeps the path up to "
                             "where it was last seen.")
                              .arg(t)
                              .arg(lostFor / 1000.0, 0, 'f', 1));
    });
    connect(m_tracker, &SubjectTrackController::finished, this, &MainWindow::finishSubjectTracking);
#endif
    // The viewport's box select serves Track subject and "Blur a plate…".
    connect(m_viewport, &EquirectViewportWindow::boxSelected, this, [this](const QRectF &box) {
        if (m_plateSelecting)
            startPlateFollow(box);
        else
            startSubjectTracking(box);
    });
    connect(m_viewport, &EquirectViewportWindow::boxSelectCanceled, this, [this] {
        if (m_plateSelecting)
            setPlateSelecting(false);
        else
            setTrackSelecting(false);
    });
    connect(m_editToolbar, &EditToolbar::zoomOutRequested, m_timelineWidget,
            &TimelineWidget::zoomOut);
    connect(m_editToolbar, &EditToolbar::zoomToFitRequested, m_timelineWidget,
            &TimelineWidget::zoomToFit);
    connect(m_editToolbar, &EditToolbar::zoomInRequested, m_timelineWidget,
            &TimelineWidget::zoomIn);
    connect(m_editToolbar, &EditToolbar::deleteKeyRequested, this, [this] {
        if (m_clipId)
            m_timeline->removeClipReframeKeyNear(m_clipId, m_lastPositionMs);
    });
    connect(m_editToolbar, &EditToolbar::clearReframeRequested, this,
            [this] { if (m_clipId) m_timeline->clearClipReframe(m_clipId); });
    connect(m_timeline, &Timeline::clipReframeChanged, this, [this](int id) {
        refreshTimelineWidget(); // re-draw the keyframe diamonds
        if (id == m_clipId) {
            const ClipItem *c = m_timeline->clipById(id);
            if (c && !c->reframe.isEmpty() && !m_viewportDragging)
                applySampledPose(m_lastPositionMs); // reflect an edit/delete at once
            syncReframePanel();
        }
    });
}

void MainWindow::connectColour()
{
    // Colour panel: an edit writes the grade onto the clip under the playhead;
    // clipColorChanged pushes it straight into the viewport if it's the
    // clip currently showing -- no re-stitch, applied at reprojection time.
    connect(m_colourPanel, &ColourPanel::gradeEdited, this, [this](const ColorGrade &g) {
        if (const int id = editClipId())
            m_timeline->setClipColor(id, g);
    });
    // Copy / paste a grade between clips: the panel's buttons act on its clip,
    // the timeline's right-click menu on the clicked one (or every clip --
    // one undo step, the edits land in one burst).
    const auto copyGrade = [this](int clipId) {
        const ClipItem *c = m_timeline->clipById(clipId);
        if (!c)
            return;
        m_copiedGrade = c->color;
        m_colourPanel->setCanPaste(true);
        m_timelineWidget->setCanPasteGrade(true);
    };
    const auto pasteGrade = [this](int clipId) {
        if (m_copiedGrade && clipId)
            m_timeline->setClipColor(clipId, *m_copiedGrade);
    };
    connect(m_colourPanel, &ColourPanel::copyRequested, this,
            [this, copyGrade] { copyGrade(editClipId()); });
    connect(m_colourPanel, &ColourPanel::pasteRequested, this,
            [this, pasteGrade] { pasteGrade(editClipId()); });
    connect(m_timelineWidget, &TimelineWidget::gradeCopyRequested, this, copyGrade);
    connect(m_timelineWidget, &TimelineWidget::gradePasteRequested, this, pasteGrade);
    connect(m_timelineWidget, &TimelineWidget::gradePasteAllRequested, this, [this] {
        if (!m_copiedGrade)
            return;
        for (int i = 0; i < m_timeline->count(); ++i)
            m_timeline->setClipColor(m_timeline->clipAt(i).id, *m_copiedGrade);
    });
    connect(m_timeline, &Timeline::clipColorChanged, this, [this](int id, ColorGrade grade) {
        if (const ClipItem *c = m_timeline->clipById(id); c && c->flat)
            pushInsetsToController(); // its picture is a layer, graded there
        if (id == m_clipId) // on screen
            applyColorGrade(grade);
        if (id == editClipId()) // in the panel
            m_colourPanel->setState(grade);
    });
}

void MainWindow::connectMeasurements()
{
    // A measured sync fine-tune for the clip under the playhead: rebuild the
    // stabilizer, and re-stitch in place since the capture-time correction is
    // baked into stitched frames. While playing, both wait for the pause: the
    // re-stitch is a seek, which would stall playback for a few hundred ms.
    connect(m_timeline, &Timeline::clipSyncChanged, this, [this](int id, double) {
        if (m_playing) {
            m_measuredWhilePlaying = true;
            return;
        }
        const ClipItem *c = m_timeline->clipById(id);
        const ClipItem *cur = m_timeline->clipById(m_clipId);
        if (!c || !cur || c->sourcePath != cur->sourcePath)
            return;
        ensureStabilizers();
        applyStabilization(m_lastPositionMs);
        updateSyncStatus();
        QMetaObject::invokeMethod(m_controller, "seek", Qt::QueuedConnection,
                                  Q_ARG(qint64, m_lastTimelineMs));
    });
    // A measured seam fine-tune: hand it to the stitch and re-stitch the frame
    // on screen if it's this clip's source.
    connect(m_timeline, &Timeline::clipSeamChanged, this, [this](int id) {
        updateSeamStatus();
        if (m_playing) {
            m_measuredWhilePlaying = true;
            return;
        }
        pushSeamAdjust();
        const ClipItem *c = m_timeline->clipById(id);
        const ClipItem *cur = m_timeline->clipById(m_clipId);
        if (c && cur && c->sourcePath == cur->sourcePath)
            QMetaObject::invokeMethod(m_controller, "seek", Qt::QueuedConnection,
                                      Q_ARG(qint64, m_lastTimelineMs));
    });
}

void MainWindow::connectPrivacy()
{
    // Privacy panel: the flag goes onto the clip being edited; a change
    // re-sends playback's blur regions and re-stitches the paused frame.
    connect(m_privacyPanel, &PrivacyPanel::blurToggled, this, [this](bool on) {
        if (const int id = editClipId())
            m_timeline->setClipBlurPlates(id, on);
    });
    connect(m_privacyPanel, &PrivacyPanel::scanRequested, this, &MainWindow::startPlateScan);
    connect(m_privacyPanel, &PrivacyPanel::plateButtonClicked, this,
            &MainWindow::togglePlateBlur);
    connect(m_privacyPanel, &PrivacyPanel::restoreRemovedRequested, this, [this] {
        if (const int id = editClipId())
            m_timeline->setClipBlurRemovals(id, {});
    });
    connect(m_viewport, &EquirectViewportWindow::pictureContextMenu, this,
            &MainWindow::showPictureMenu);
    connect(m_privacyPanel, &PrivacyPanel::scanStopRequested, this, &MainWindow::stopPlateJob);
#ifdef ORBIS_REFRAME_HAVE_PLATE_SCAN
    m_plateJobs = new PlateJobController(this);
    connect(m_plateJobs, &PlateJobController::scanProgress, this, [this](qint64 tMs, double fps) {
        const qint64 from = m_plateJobFromMs, to = m_plateJobToMs;
        const int pct = static_cast<int>(100 * (tMs - from) / std::max<qint64>(1, to - from));
        m_privacyPanel->setScanStatus(tr("Scanning… %1% (%2 fps). Stop keeps the part scanned.")
                                          .arg(std::clamp(pct, 0, 100))
                                          .arg(fps, 0, 'f', 1));
    });
    connect(m_plateJobs, &PlateJobController::scanFinished, this,
            [this](std::shared_ptr<const platescan::Result> r) { finishPlateScan(*r); });
    connect(m_plateJobs, &PlateJobController::followProgress, this, [this](qint64 tMs, bool backward) {
        const int idx = m_timeline->indexOfId(m_plateJobClipId);
        if (idx >= 0)
            showPlayhead(
                m_timeline->clipToTimeline(idx, m_timeline->clipAt(idx).localAtSourceMs(tMs)));
        const qint64 from = m_plateJobFromMs;
        m_privacyPanel->setScanStatus(
            backward ? tr("Following the plate back… %1 s before").arg((from - tMs) / 1000.0, 0, 'f', 1)
                     : tr("Following the plate… %1 s").arg((tMs - from) / 1000.0, 0, 'f', 1));
    });
    connect(m_plateJobs, &PlateJobController::followFinished, this,
            [this](std::shared_ptr<const platescan::FollowResult> r) { finishPlateFollow(*r); });
#endif
    connect(m_privacyPanel, &PrivacyPanel::showRegionsToggled, this, [this](bool on) {
        m_showPlateRegions = on;
        pushBlur();
        if (!m_playing)
            QMetaObject::invokeMethod(m_controller, "seek", Qt::QueuedConnection,
                                      Q_ARG(qint64, m_lastTimelineMs));
    });
    connect(m_timeline, &Timeline::clipPrivacyChanged, this, [this](int, bool) {
        pushBlur();
        syncPrivacyPanel();
        if (!m_playing)
            QMetaObject::invokeMethod(m_controller, "seek", Qt::QueuedConnection,
                                      Q_ARG(qint64, m_lastTimelineMs));
    });
    connect(m_timeline, &Timeline::clipTrimChanged, this, [this] { syncPrivacyPanel(); });
}

void MainWindow::setupMenus()
{
    QMenu *fileMenu = menuBar()->addMenu(tr("&File"));
    QAction *newProjectAction =
        fileMenu->addAction(tr("&New project…"), this, &MainWindow::newProject);
    newProjectAction->setObjectName(QStringLiteral("newProjectAction"));
    newProjectAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_N));
    newProjectAction->setIcon(theme::icon(QStringLiteral("file-plus-2")));
    QAction *openProjectAction =
        fileMenu->addAction(tr("&Open project…"), this, &MainWindow::openProjectFile);
    openProjectAction->setObjectName(QStringLiteral("openProjectAction"));
    openProjectAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_O));
    openProjectAction->setIcon(theme::icon(QStringLiteral("folder-open")));
    QAction *libraryAction =
        fileMenu->addAction(tr("Project &library…"), this, &MainWindow::closeToLibrary);
    libraryAction->setObjectName(QStringLiteral("projectLibraryAction"));
    libraryAction->setIcon(theme::icon(QStringLiteral("layers")));
    fileMenu->addSeparator();
    QAction *saveProjectAction =
        fileMenu->addAction(tr("&Save project"), this, [this] { m_files->save(); });
    saveProjectAction->setObjectName(QStringLiteral("saveProjectAction"));
    saveProjectAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_S));
    saveProjectAction->setIcon(theme::icon(QStringLiteral("save")));
    QAction *saveProjectAsAction =
        fileMenu->addAction(tr("Save project &as…"), this, [this] { m_files->saveAs(m_clipPath); });
    saveProjectAsAction->setObjectName(QStringLiteral("saveProjectAsAction"));
    saveProjectAsAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S));
    // Their own enabled state (a clip open, undo history) is already off
    // while no project is open; these would act on the hidden editor.
    m_editorActions = {libraryAction, saveProjectAction, saveProjectAsAction};
    fileMenu->addSeparator();
    QAction *projectSettingsAction = fileMenu->addAction(tr("Project se&ttings…"), this,
                                                         &MainWindow::editProjectSettings);
    projectSettingsAction->setObjectName(QStringLiteral("projectSettingsAction"));
    projectSettingsAction->setIcon(theme::icon(QStringLiteral("clapperboard")));
    m_editorActions += projectSettingsAction;
    fileMenu->addSeparator();

    // File -> Export flat video… (disabled until a clip is open).
    m_exportAction = fileMenu->addAction(tr("&Export flat video…"), this,
                                        &MainWindow::exportFlatVideo);
    m_exportAction->setObjectName(QStringLiteral("exportAction"));
    m_exportAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_E));
    m_exportAction->setIcon(theme::icon(QStringLiteral("download")));
    m_exportAction->setEnabled(false);

    // Save screenshot: the player bar's button and Ctrl+Shift+P, in no menu
    // -- a window-level action (listed under Playback in the shortcut help).
    m_screenshotAction = new QAction(tr("Save &screenshot"), this);
    m_screenshotAction->setObjectName(QStringLiteral("screenshotAction"));
    m_screenshotAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_P));
    m_screenshotAction->setShortcutContext(Qt::ApplicationShortcut);
    m_screenshotAction->setEnabled(false);
    connect(m_screenshotAction, &QAction::triggered, this, &MainWindow::takeScreenshot);
    addAction(m_screenshotAction);
    connect(m_playerBar, &PlayerBar::screenshotRequested, this, &MainWindow::takeScreenshot);

    fileMenu->addSeparator();
    // close() runs closeEvent() -> m_files->confirmDiscard(), so an unsaved
    // project still prompts before the app exits.
    QAction *quitAction = fileMenu->addAction(tr("&Quit"), this, &QWidget::close);
    quitAction->setObjectName(QStringLiteral("quitAction"));
    quitAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Q));
    quitAction->setMenuRole(QAction::QuitRole);

    // Edit -> Undo/Redo. Project::undo()/redo() operate on whole-project
    // snapshots coalesced by a settle timer (see Project.cpp) -- there's no
    // per-action label, so these are just "Undo"/"Redo".
    QMenu *editMenu = menuBar()->addMenu(tr("&Edit"));
    m_undoAction = editMenu->addAction(tr("&Undo"), this, &MainWindow::undo);
    m_undoAction->setObjectName(QStringLiteral("undoAction"));
    m_undoAction->setShortcut(QKeySequence::Undo);
    m_undoAction->setEnabled(m_project->canUndo());
    m_redoAction = editMenu->addAction(tr("&Redo"), this, &MainWindow::redo);
    m_redoAction->setObjectName(QStringLiteral("redoAction"));
    m_redoAction->setShortcut(QKeySequence::Redo);
    m_redoAction->setEnabled(m_project->canRedo());
    connect(m_project, &Project::canUndoChanged, m_undoAction, &QAction::setEnabled);
    connect(m_project, &Project::canRedoChanged, m_redoAction, &QAction::setEnabled);

    // Media: into the project's media (the Media tab), or straight onto the
    // timeline.
    QMenu *mediaMenu = menuBar()->addMenu(tr("&Media"));
    QAction *importFilesAction =
        mediaMenu->addAction(tr("&Import files…"), m_mediaImport, &MediaImporter::importFiles);
    importFilesAction->setObjectName(QStringLiteral("importFilesAction"));
    importFilesAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_I));
    importFilesAction->setIcon(theme::icon(QStringLiteral("file-plus-2")));
    QAction *importFolderAction =
        mediaMenu->addAction(tr("Import &folder…"), m_mediaImport, &MediaImporter::importFolder);
    importFolderAction->setObjectName(QStringLiteral("importFolderAction"));
    importFolderAction->setIcon(theme::icon(QStringLiteral("folder-plus")));
    mediaMenu->addSeparator();
    QAction *addClipAction =
        mediaMenu->addAction(tr("&Add clip to timeline…"), this, [this] { addClipToTimeline(); });
    addClipAction->setObjectName(QStringLiteral("addClipAction"));
    addClipAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_A));
    QAction *addAudioAction = mediaMenu->addAction(
        tr("Add a&udio file…"), this, [this] { selectAdded(m_trackFiles->pickAudio(0, m_lastTimelineMs)); });
    addAudioAction->setObjectName(QStringLiteral("addAudioAction"));
    addAudioAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_M));
    QAction *addVideoAction = mediaMenu->addAction(
        tr("Add &video overlay…"), this, [this] { selectAdded(m_trackFiles->pickVideo(0, m_lastTimelineMs)); });
    addVideoAction->setObjectName(QStringLiteral("addVideoAction"));
    addVideoAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_V));
    addVideoAction->setIcon(theme::icon(QStringLiteral("picture-in-picture-2")));
    mediaMenu->addSeparator();
    QAction *addAudioTrackAction = mediaMenu->addAction(
        tr("Add audio &track"), this, [this] { m_timeline->addTrack(Track::Kind::Audio); });
    addAudioTrackAction->setObjectName(QStringLiteral("addAudioTrackAction"));
    QAction *addOverlayTrackAction = mediaMenu->addAction(
        tr("Add &overlay track"), this, [this] { m_timeline->addTrack(Track::Kind::Video); });
    addOverlayTrackAction->setObjectName(QStringLiteral("addOverlayTrackAction"));
    m_editorActions += {importFilesAction, importFolderAction, addClipAction, addAudioAction,
                        addVideoAction, addAudioTrackAction, addOverlayTrackAction};
}

void MainWindow::connectController()
{
    // Controller -> view.
    connect(m_controller, &PlaybackController::durationChanged, m_playerBar,
            &PlayerBar::setDuration);
    connect(m_controller, &PlaybackController::openFailed, this,
            [this](const QString &error) {
                QMessageBox::warning(this, tr("Playback error"), error);
            });
    connect(m_controller, &PlaybackController::positionChanged, this,
            [this](qint64 timelineMs) {
                m_lastTimelineMs = timelineMs;
                showPlayhead(timelineMs);
#ifdef ORBIS_REFRAME_HAVE_AUDIO
                m_audioPlayer->notePlaybackPosition(timelineMs);
#endif
                bindActiveClipAt(timelineMs);
                if (selectedOverlayClip())
                    refreshOverlayPanel(); // keyed values / key buttons follow the playhead
            });
    connect(m_controller, &PlaybackController::playingChanged,
            m_playerBar, &PlayerBar::setPlaying);
    connect(m_controller, &PlaybackController::playingChanged, this, [this](bool playing) {
        m_playing = playing;
        m_measurements->setPlaying(playing);
        if (playing)
            return;
        if (m_measuredWhilePlaying) { // results held back while playing
            m_measuredWhilePlaying = false;
            pushSeamAdjust();
            ensureStabilizers();
            applyStabilization(m_lastPositionMs);
            updateSyncStatus();
            QMetaObject::invokeMethod(m_controller, "seek", Qt::QueuedConnection,
                                      Q_ARG(qint64, m_lastTimelineMs)); // re-stitch
        }
    });
    connect(m_timeline, &Timeline::clipInserted, m_measurements, &ClipMeasurements::schedule);
    connect(m_controller, &PlaybackController::bufferingChanged,
            m_playerBar, &PlayerBar::setBuffering);
#ifdef ORBIS_REFRAME_HAVE_AUDIO
    // Every play/pause entry point (transport button, Space, export pausing
    // playback, reaching end-of-timeline) funnels through this one signal.
    // Off 1x the preview is silent (the player bar's speed).
    connect(m_controller, &PlaybackController::playingChanged, this, [this](bool playing) {
        if (playing && m_previewRate == 1.0)
            m_audioPlayer->play();
        else
            m_audioPlayer->pause();
    });
    // Hold the audio exactly while the video clock is frozen (a seek / stall)
    // so the two resume in step. Not bufferingChanged: that also fires
    // on ticks that merely find the queue empty while the clock runs on, and
    // holding the audio then chopped it for a second after every seek.
    connect(m_controller, &PlaybackController::clockHeldChanged, m_audioPlayer,
            &AudioPlayer::setHold);
#endif
}

void MainWindow::connectPlayerBar()
{
    // The player bar's speed and volume.
    connect(m_playerBar, &PlayerBar::speedChanged, this, [this](double speed) {
        m_previewRate = speed;
        QMetaObject::invokeMethod(m_controller, [ctrl = m_controller, speed] { ctrl->setRate(speed); });
#ifdef ORBIS_REFRAME_HAVE_AUDIO
        if (m_playing) {
            if (speed == 1.0) {
                m_audioPlayer->seek(m_lastTimelineMs);
                m_audioPlayer->play();
            } else {
                m_audioPlayer->pause();
            }
        }
#endif
    });
    {
        QSettings settings(QStringLiteral("orbis-reframe"), QStringLiteral("orbis-reframe"));
        m_playerBar->setVolume(settings.value(QStringLiteral("player/volume"), 100).toInt());
        m_playerBar->setMuted(settings.value(QStringLiteral("player/muted"), false).toBool());
    }
    applyPreviewVolume();
    connect(m_playerBar, &PlayerBar::volumeChanged, this, [this](int percent) {
        QSettings(QStringLiteral("orbis-reframe"), QStringLiteral("orbis-reframe"))
            .setValue(QStringLiteral("player/volume"), percent);
        applyPreviewVolume();
    });
    connect(m_playerBar, &PlayerBar::mutedChanged, this, [this](bool muted) {
        QSettings(QStringLiteral("orbis-reframe"), QStringLiteral("orbis-reframe"))
            .setValue(QStringLiteral("player/muted"), muted);
        applyPreviewVolume();
    });
}

void MainWindow::setupMediaCheck()
{
    // Files going missing or coming back while the app runs: looked for every
    // 2 s while it's in front, and at once when it comes back to the front.
    m_mediaCheck = new QTimer(this);
    m_mediaCheck->setInterval(2000);
    connect(m_mediaCheck, &QTimer::timeout, this, &MainWindow::recheckMissingMedia);
    connect(qApp, &QGuiApplication::applicationStateChanged, this,
            [this](Qt::ApplicationState state) {
                if (state == Qt::ApplicationActive) {
                    recheckMissingMedia();
                    m_mediaCheck->start();
                } else {
                    m_mediaCheck->stop();
                }
            });
    if (QGuiApplication::applicationState() == Qt::ApplicationActive)
        m_mediaCheck->start();
}

void MainWindow::connectFullScreen()
{
    // Full screen (the player bar's button, a double-click on the picture):
    // the player bar hides while the mouse rests.
    m_playerBarHide = new QTimer(this);
    m_playerBarHide->setSingleShot(true);
    m_playerBarHide->setInterval(2500);
    connect(m_playerBarHide, &QTimer::timeout, this, [this] {
        // Not while the mouse is on it (or a time is being typed).
        if (!m_fullScreen)
            return;
        if (m_playerBar->underMouse() || m_playerBar->isAncestorOf(QApplication::focusWidget()))
            m_playerBarHide->start();
        else
            m_playerBar->setVisible(false);
    });
    connect(m_playerBar, &PlayerBar::fullScreenClicked, this,
            [this] { setFullScreenView(!m_fullScreen); });
    connect(m_viewport, &EquirectViewportWindow::doubleClicked, this,
            [this] { setFullScreenView(!m_fullScreen); });
    connect(m_project, &Project::outputChanged, this, [this] { updatePlayerBar(); });
}

MainWindow::~MainWindow()
{
    delete m_measurements; // waits for a running one
    m_measurements = nullptr;
    delete m_exports; // waits for a screenshot being saved
    m_exports = nullptr;
    // Background jobs that stitch and draw into the viewport: stopped and
    // waited for while the viewport is still there.
#ifdef ORBIS_REFRAME_HAVE_PLATE_SCAN
    delete m_plateJobs;
    m_plateJobs = nullptr;
#endif
#ifdef ORBIS_REFRAME_HAVE_TRACKING
    delete m_tracker;
    m_tracker = nullptr;
#endif
    m_playbackThread->quit();
    m_playbackThread->wait();
    m_unshownViewport.reset(); // its users are gone
}

namespace {

// A widget that takes typed text, so it swallows the editor's single-key
// shortcuts while focused. Spin boxes report themselves as the focus widget,
// not their inner QLineEdit.
bool isTextEntry(const QWidget *w)
{
    return qobject_cast<const QAbstractSpinBox *>(w) || qobject_cast<const QLineEdit *>(w) ||
           qobject_cast<const QPlainTextEdit *>(w); // a title's text
}

// A title as the timeline and the Overlay tab name it: its first line.
QString titleLabel(const title::Style &t)
{
    const QString first = t.text.trimmed().section(QLatin1Char('\n'), 0, 0).trimmed();
    return first.isEmpty() ? MainWindow::tr("(empty title)") : first;
}

} // namespace

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (m_fullScreen && event->type() == QEvent::MouseMove)
        showPlayerBarForAWhile();

    // Only the editor's own fields: a dialog's keep Enter (its default
    // button) and their focus.
    QWidget *fw = QApplication::focusWidget();
    if (!isTextEntry(fw) || fw->window() != this)
        return QMainWindow::eventFilter(watched, event);

    switch (event->type()) {
    case QEvent::MouseButtonPress: {
        // Presses also arrive at the top-level QWindow before its widgets --
        // ignore those; the only bare QWindow that matters is the viewport.
        bool outside = false;
        if (watched == m_viewport) {
            outside = true;
        } else if (auto *w = qobject_cast<QWidget *>(watched)) {
            // Clicking the field itself (text, arrows) keeps it; clicking
            // another text field moves focus there on its own.
            bool inEntry = false;
            for (QWidget *p = w; p && !inEntry; p = p->parentWidget())
                inEntry = p == fw || isTextEntry(p);
            outside = !inEntry;
        }
        if (outside)
            fw->clearFocus();
        break;
    }
    case QEvent::ShortcutOverride:
        // A field keeps its own editing keys -- arrows, Home / End, Delete,
        // Backspace, Escape -- over the editor's unmodified shortcuts (a
        // spin box doesn't claim Up / Down itself). Ctrl / Alt ones still
        // reach the shortcuts.
        if (watched == fw) {
            const auto *ke = static_cast<QKeyEvent *>(event);
            if (!(ke->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
                event->accept();
                return true;
            }
        }
        break;
    case QEvent::KeyPress:
        // A field that handles Enter / Escape itself (the timeline's rename box).
        if (watched == fw && !fw->property("ownsReturnEscape").toBool()) {
            const int key = static_cast<QKeyEvent *>(event)->key();
            if (key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Escape) {
                fw->clearFocus();
                return true;
            }
        }
        break;
    default:
        break;
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_files->confirmDiscard()) {
        // Let the viewport release its swapchain while it's still shown.
        // Closing destroys the window, which hides it first and only then
        // tells QVulkanWindow (SurfaceAboutToBeDestroyed); with Qt 6.8 on
        // Wayland (Debian 13) the NVIDIA driver crashes destroying a swapchain
        // whose window is already hidden. Releasing twice is harmless.
        QPlatformSurfaceEvent aboutToGo(QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed);
        QCoreApplication::sendEvent(m_viewport, &aboutToGo);
        event->accept();
    } else
        event->ignore();
}

void MainWindow::applySampledPose(qint64 sourceMs)
{
    const ClipItem *c = m_timeline->clipById(m_clipId);
    if (!c)
        return;
    const ReframePose p = c->reframe.sample(sourceMs);
    m_viewport->SetPose(p.yaw, p.pitch, p.roll, p.fovY);
}

void MainWindow::ensureStabilizers()
{
    // The current clip's first: used last, so the next one's can't evict it.
    const int idx = m_timeline->indexOfId(m_clipId);
    auto ensure = [this](const ClipItem &c) {
        return m_stabilizers.ensure(c, m_timeline->sourceSyncAdjust(c.sourcePath));
    };
    bool currentBuilt = false;
    if (idx >= 0)
        currentBuilt = ensure(m_timeline->clipAt(idx));
    if (idx >= 0 && idx + 1 < m_timeline->count())
        ensure(m_timeline->clipAt(idx + 1)); // ready for the cut
    publishStabilizer();
    if (currentBuilt) {
        applyStabilization(m_lastPositionMs); // the frame on screen, if paused
        updateSyncStatus();
    }
}

bool MainWindow::seamAdjustFor(const QString &clipPath, std::array<double, 3> &deg) const
{
    return m_timeline->sourceSeamAdjust(clipPath, deg);
}

void MainWindow::pushSeamAdjust()
{
    for (int i = 0; i < m_timeline->count(); ++i) {
        const QString &path = m_timeline->clipAt(i).sourcePath;
        std::array<double, 3> deg;
        seamAdjustFor(path, deg);
        m_controller->setSeamAdjust(path, deg);
    }
}

void MainWindow::updateSeamStatus()
{
    m_stitchingPanel->setSeamStatus(m_measurements->seamStatus(m_timeline->clipById(m_clipId)));
}

void MainWindow::updateSyncStatus()
{
    m_stabilizationPanel->setSyncStatus(
        m_measurements->syncStatus(m_timeline->clipById(m_clipId)));
}

void MainWindow::publishStabilizer()
{
    m_controller->setCaptureTimings(m_stabilizers.publish(*m_timeline));
}

void MainWindow::applyStabilization(qint64 sourceMs)
{
    float m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    if (const ClipItem *c = m_timeline->clipById(m_clipId))
        m_stabilizers.correctionFor(*c, m_timeline->sourceSyncAdjust(c->sourcePath), sourceMs, m);
    m_viewport->SetStabilization(&m[0][0]);
}

void MainWindow::syncReframePanel()
{
    m_reframePanel->setPose({m_viewport->Yaw(), m_viewport->Pitch(), m_viewport->Roll(),
                             m_viewport->FovY()});
    const ClipItem *c = m_timeline->clipById(m_clipId);
    const int seg = c ? keytrack::segmentAt(c->reframe.keys(), m_lastPositionMs,
                                            ClipItem::kReframeDeleteSnapMs)
                      : -1;
    m_reframePanel->setEase(seg >= 0, seg >= 0 ? c->reframe.keys()[seg].ease
                                               : keytrack::Ease::Linear);
}

void MainWindow::applyColorGrade(const ColorGrade &grade)
{
    m_viewport->SetColorGrade(grade.packed().data());
}

void MainWindow::bindActiveClipAt(qint64 timelineMs)
{
    const Timeline::Point pt = m_timeline->timelineToClip(timelineMs);
    if (pt.clipIndex < 0) {
        m_clipId = 0;
        m_lastPositionMs = 0;
        return;
    }
    const ClipItem &c = m_timeline->clipAt(pt.clipIndex);
    const bool clipChanged = c.id != m_clipId;
    m_clipId = c.id;
    if (clipChanged) {
        pushSeamAdjust();
        updateSeamStatus();
    }
    m_timelineWidget->setActiveClip(editClipId());
    m_lastPositionMs = c.sourceAtLocalMs(pt.clipLocalMs); // this clip's source time
    // The stab matrix itself rides with each presented frame (the playback
    // controller's stabilization provider) -- setting it here from the clock
    // position would put it up to a frame out of step with the picture.
    if (clipChanged) {
        ensureStabilizers();
        updateSyncStatus();
    }
    // While playing, each presented frame brings its own view and grade
    // (FrameViewCache): set from the clock here, they would change a frame
    // apart from the picture at a cut.
    if (!c.reframe.isEmpty() && !m_viewportDragging && !m_playing)
        applySampledPose(m_lastPositionMs);
    if (!m_viewportDragging)
        syncReframePanel();
    if (clipChanged) {
        if (!m_playing)
            applyColorGrade(c.color); // the viewport shows this clip
        if (!selectedVideoClip()) // the panels follow the playhead unless a clip is selected
            refreshClipPanels();
    }
}

const ClipItem *MainWindow::selectedVideoClip() const
{
    return m_selectedClipId > 0 ? m_timeline->clipById(m_selectedClipId) : nullptr;
}

int MainWindow::editClipId() const
{
    if (const ClipItem *c = selectedVideoClip())
        return c->id;
    return m_clipId;
}

void MainWindow::selectVideoClip(int clipId)
{
    if (clipId > 0 && !m_timeline->clipById(clipId))
        clipId = 0;
    m_selectedClipId = clipId;
    if (clipId > 0 && m_selectedTrackClipId > 0)
        selectTrackClip(-1); // one selection at a time
    if (clipId > 0 && m_selectedTransition)
        selectTransition(0);
    refreshClipPanels();
}

void MainWindow::refreshClipPanels()
{
    m_timelineWidget->setActiveClip(editClipId());
    const ClipItem *c = m_timeline->clipById(editClipId());
    if (!c) {
        m_privacyPanel->setNotApplicable(QString());
        m_privacyPanel->setNoClip();
        m_stabilizationPanel->setNotApplicable(QString());
        m_stabilizationPanel->setNoClip();
        syncStitchingPanel();
        m_reframePanel->setFlatClip(false);
        refreshAudioPanel();
        return;
    }
    syncMediaInfoPanel(*c);
    m_colourPanel->setState(c->color);
    m_reframePanel->setFlatClip(c->flat, c->fit == ClipItem::Fit::Fill);
    syncStabilizationPanel();
    syncStitchingPanel();
    syncPrivacyPanel();
    refreshAudioPanel(); // the selected audio clip, if any, else this clip
    const ClipItem *playing = m_timeline->clipById(m_clipId);
    m_editToolbar->setHasReframeKeys(playing && !playing->reframe.isEmpty());
    m_reframePanel->setClipUnkeyed(playing && playing->reframe.isEmpty());
}

void MainWindow::syncStabilizationPanel()
{
    const ClipItem *c = m_timeline->clipById(editClipId());
    m_stabilizationPanel->setNotApplicable(
        c && c->flat ? tr("A flat video has no motion data to stabilize with.") : QString());
    if (!c || c->flat) {
        m_stabilizationPanel->setNoClip();
        return;
    }
    using M = StabilizationPanel::Mode;
    const insv::StabConfig &s = c->stabilization;
    M mode = M::Off;
    if (s.mode == insv::StabMode::LeanFollow)
        mode = M::LeanFollow;
    else if (s.mode == insv::StabMode::HorizonLock)
        mode = M::HorizonLock;
    else if (s.mode == insv::StabMode::Custom)
        mode = M::Custom;
    m_stabilizationPanel->setState(
        mode, mode != M::Off ? s : insv::StabConfig::preset(insv::StabMode::LeanFollow));
}

void MainWindow::syncStitchingPanel()
{
    const ClipItem *c = m_timeline->clipById(editClipId());
    m_stitchingPanel->setNotApplicable(
        c && c->flat ? tr("A flat video isn't stitched: it has one picture, not two.") : QString());
    if (!c || c->flat) {
        m_stitchingPanel->setNoClip();
        return;
    }
    m_stitchingPanel->setState(c->lensGuard, c->depthSeam);
}

TimelineWidget::Peaks MainWindow::waveformFor(const QString &path, int stream)
{
    const WaveformCache::Peaks p = m_waveforms->peaks(path, stream);
    return p ? TimelineWidget::Peaks(p, &p->peaks) : nullptr; // shares ownership with p
}

int MainWindow::audioStreamCount(const QString &path)
{
    auto it = m_audioTrackCounts.constFind(path);
    if (it == m_audioTrackCounts.constEnd())
        it = m_audioTrackCounts.insert(
            path, m_clipBackend.audioStreamCount(path.toStdString()));
    return *it;
}

void MainWindow::refreshTrackRows()
{
    QVector<TimelineWidget::TrackView> tracks;
    const auto moveTargets = [this](const Track &from, int clipId) {
        QVector<int> ids;
        for (int k = 0; k < m_timeline->trackCount(); ++k)
            if (const int id = m_timeline->trackAt(k).id;
                id != from.id && m_timeline->canHoldClip(id, clipId))
                ids.push_back(id);
        return ids;
    };
    for (int i = 0; i < m_timeline->trackCount(); ++i) {
        const Track &t = m_timeline->trackAt(i);
        TimelineWidget::TrackView v;
        v.id = t.id;
        v.name = t.name;
        v.muted = t.muted;
        v.video = t.kind == Track::Kind::Video;
        v.group = t.group() == Track::Group::Main    ? TimelineWidget::TrackView::Group::Main
                  : t.group() == Track::Group::Audio ? TimelineWidget::TrackView::Group::Audio
                                                     : TimelineWidget::TrackView::Group::Overlay;
        v.overlayAudio = t.role == Track::Role::OverlayAudio;
        for (const TrackClip &c : t.clips) {
            TimelineWidget::TrackClipView a;
            a.id = c.id;
            a.label = c.isTitle() ? titleLabel(*c.title) : QFileInfo(c.sourcePath).fileName();
            if (v.video) { // an overlay: no audio to draw, nothing to link
                a.startMs = c.startMs;
                a.trimInMs = c.trimInMs;
                a.trimOutMs = c.trimOutMs;
                a.sourceDurationMs = c.sourceDurationMs;
                a.speed = c.speed;
                a.fadeInMs = c.fadeInMs;
                a.fadeOutMs = c.fadeOutMs;
                a.missing = c.mediaMissing;
                for (const InsetKey &k : c.insetKeys.keys())
                    a.keySourceMs.push_back(k.timeMs);
                a.hasSound =
                    !c.isTitle() && !c.mediaMissing && audioStreamCount(c.sourcePath) > 0;
                a.moveTargets = moveTargets(t, c.id);
                v.clips.push_back(a);
                continue;
            }
            a.startMs = c.startMs;
            a.trimInMs = c.trimInMs;
            a.trimOutMs = c.trimOutMs;
            a.sourceDurationMs = c.sourceDurationMs;
            a.speed = c.speed;
            a.fadeInMs = c.fadeInMs;
            a.fadeOutMs = c.fadeOutMs;
            a.muted = c.muted;
            a.missing = c.mediaMissing;
            a.linked = c.linkedClipId != 0;
            if (!c.mediaMissing)
                a.peaks = waveformFor(c.sourcePath, c.stream);
            a.gain = std::pow(10.0, c.gainDb / 20.0);
            a.linkable = !a.linked && m_timeline->linkTargetFor(c) != 0;
            a.moveTargets = moveTargets(t, c.id);
            v.clips.push_back(a);
        }
        tracks.push_back(std::move(v));
    }
    m_timelineWidget->setTracks(tracks);
    m_timelineWidget->setSelectedTrackClip(m_selectedTrackClipId);
    fitTimelineHeight();
}

void MainWindow::fitTimelineHeight()
{
    // Until the user drags the split, the timeline is as tall as its tracks
    // need, but never more than 40 % of the editor; once dragged, their
    // height stays and extra tracks scroll.
    if (m_timelineHeightSet)
        return;
    const int total = m_editorPage->height();
    if (total <= 0)
        return;
    const int want = std::max(m_timelinePane->minimumSizeHint().height(),
                              std::min(m_timelinePane->sizeHint().height(), total * 2 / 5));
    m_editorPage->setSizes({total - want - m_editorPage->handleWidth(), want});
}

void MainWindow::selectTrackClip(int clipId)
{
    if (clipId > 0 && !m_timeline->trackClipById(clipId))
        clipId = -1;
    const bool changed = clipId != m_selectedTrackClipId;
    m_selectedTrackClipId = clipId;
    m_timelineWidget->setSelectedTrackClip(clipId);
    // Between the Overlay and Title tabs, the one for what's selected.
    if (const TrackClip *tc = changed && clipId > 0 ? m_timeline->trackClipById(clipId) : nullptr) {
        const int tab = m_sidebar->currentIndex();
        if (tc->isTitle() && tab == SettingsSidebar::OverlayTab)
            m_sidebar->setCurrentIndex(SettingsSidebar::TitleTab);
        else if (!tc->isTitle() && tab == SettingsSidebar::TitleTab && selectedOverlayClip())
            m_sidebar->setCurrentIndex(SettingsSidebar::OverlayTab);
    }
    if (clipId > 0 && m_selectedTransition)
        selectTransition(0); // one selection at a time
    if (clipId > 0 && m_selectedClipId > 0) {
        m_selectedClipId = 0;
        refreshClipPanels();
    } else if (changed) {
        refreshAudioPanel();
    }
    refreshOverlayPanel();
}

QList<int> MainWindow::audioPanelClipIds(QString *scope) const
{
    // Every audio clip linked to `clipId`, in track order: a video clip's
    // camera mic and external mic, an overlay's sound.
    const auto linkedTo = [this](int clipId) {
        QList<int> ids;
        for (int i = 0; i < m_timeline->trackCount(); ++i)
            for (const TrackClip &a : m_timeline->trackAt(i).clips)
                if (a.linkedClipId == clipId)
                    ids.append(a.id);
        return ids;
    };
    int trackId = 0;
    if (m_selectedTrackClipId > 0 && m_timeline->trackClipById(m_selectedTrackClipId, &trackId)) {
        if (m_timeline->trackById(trackId)->kind == Track::Kind::Video) {
            if (scope)
                *scope = tr("Selected overlay clip");
            return linkedTo(m_selectedTrackClipId);
        }
        if (scope)
            *scope = tr("Selected audio clip");
        return {m_selectedTrackClipId};
    }
    // Else the video clip selected or under the playhead.
    if (scope)
        *scope = tr("This clip");
    const int clipId = editClipId();
    return clipId ? linkedTo(clipId) : QList<int>{};
}

void MainWindow::refreshAudioPanel()
{
    QString scope;
    QList<AudioClipState> states;
    for (int id : audioPanelClipIds(&scope)) {
        int trackId = 0;
        const TrackClip *a = m_timeline->trackClipById(id, &trackId);
        const Track *t = a ? m_timeline->trackById(trackId) : nullptr;
        if (!t)
            continue;
        AudioClipState st;
        st.clipId = id;
        st.trackId = trackId;
        st.kind = t->role == Track::Role::CameraMic     ? QStringLiteral("cameraMic")
                  : t->role == Track::Role::ExternalMic ? QStringLiteral("externalMic")
                                                        : QStringLiteral("clip");
        st.label = QFileInfo(a->sourcePath).fileName();
        st.gainDb = a->gainDb;
        st.muted = a->muted;
        st.fadeInMs = a->fadeInMs;
        st.fadeOutMs = a->fadeOutMs;
        st.spanMs = a->timelineDurationMs();
        st.startMs = a->startMs;
        st.linked = a->linkedClipId != 0;
        st.trackName = t->name;
        st.trackGainDb = t->gainDb;
        st.trackMuted = t->muted;
        st.noise = a->noise;
        states.append(st);
    }
    m_audioPanel->setClips(states, scope);
}

void MainWindow::onTracksChanged()
{
    if (m_selectedTrackClipId > 0 && !m_timeline->trackClipById(m_selectedTrackClipId))
        m_selectedTrackClipId = -1;
    refreshTrackRows();
    refreshAudioPanel();
    refreshOverlayPanel();
    pushInsetsToController();
#ifdef ORBIS_REFRAME_HAVE_AUDIO
    m_audioPlayer->setMix(renderjobs::mixClips(*m_timeline));
#endif
}

void MainWindow::selectAdded(std::optional<int> lastClipId)
{
    if (lastClipId)
        selectTrackClip(*lastClipId);
}

void MainWindow::addTitle(int trackId, qint64 atMs)
{
    if (m_timeline->count() == 0)
        return; // the main track sets the length
    atMs = std::clamp<qint64>(atMs, 0, m_timeline->totalDurationMs());
    // The track given if it has room there, else the first that has, else a
    // new one.
    const qint64 endMs = atMs + title::kDefaultSpanMs;
    const int given = m_timeline->freeTrackFor(atMs, endMs, true, [trackId](const Track &t) {
        return t.id == trackId;
    });
    trackId = given ? given
                    : m_timeline->freeTrackFor(atMs, endMs, true, [](const Track &) { return true; });
    if (!trackId)
        trackId = m_timeline->addTrack(Track::Kind::Video);
    title::Style style;
    style.text = tr("Title");
    style.font = QFontDatabase::systemFont(QFontDatabase::GeneralFont).family();
    const int id = m_timeline->addTitleClip(trackId, atMs, style);
    if (!id)
        return;
    selectTrackClip(id);
    m_sidebar->setCurrentIndex(SettingsSidebar::TitleTab);
    m_sidebar->setOpen(true);
}

void MainWindow::pushInsetsToController()
{
    const OutputSettings &out = m_project->output();
    m_controller->setInsets(renderjobs::insetFeed(*m_timeline, out.width, out.height, m_privacy)); // thread-safe
}

const TrackClip *MainWindow::selectedOverlayClip(int *trackId) const
{
    int tid = 0;
    const TrackClip *c =
        m_selectedTrackClipId > 0 ? m_timeline->trackClipById(m_selectedTrackClipId, &tid) : nullptr;
    if (!c || m_timeline->trackById(tid)->kind != Track::Kind::Video)
        return nullptr;
    if (trackId)
        *trackId = tid;
    return c;
}

void MainWindow::refreshOverlayPanel()
{
    int trackId = 0;
    const TrackClip *c = selectedOverlayClip(&trackId);
    if (!c) {
        m_overlayPanel->setNoClip();
        m_titleTextPanel->setNoTitle(QString());
        m_titlePlacementPanel->setNoClip();
        return;
    }
    // A title shows on the Title tab, an overlay video on the Overlay tab;
    // the other says where to look.
    OverlayPanel *panel = c->isTitle() ? m_titlePlacementPanel : m_overlayPanel;
    if (c->isTitle()) {
        m_titleTextPanel->setTitle(*c->title);
        m_overlayPanel->setNoClip(tr("A title is selected: its settings are in the Title tab."));
    } else {
        const QString hint = tr("An overlay video is selected: its settings are in the Overlay "
                                "tab.");
        m_titleTextPanel->setNoTitle(hint);
        m_titlePlacementPanel->setNoClip(hint);
    }
    OverlayPanel::State st;
    st.label = c->isTitle() ? titleLabel(*c->title) : QFileInfo(c->sourcePath).fileName();
    st.trackName = m_timeline->trackById(trackId)->name;
    // The layer: the overlay tracks in order, the first in front.
    QStringList overlays;
    for (int i = 0; i < m_timeline->trackCount(); ++i) {
        const Track &t = m_timeline->trackAt(i);
        if (t.kind != Track::Kind::Video)
            continue;
        overlays << t.name;
        if (t.id == trackId)
            st.layer = int(overlays.size());
    }
    st.layerCount = int(overlays.size());
    if (st.layer > 1)
        st.behind = overlays[st.layer - 2];
    if (st.layer > 0 && st.layer < st.layerCount)
        st.inFrontOf = overlays[st.layer];
    st.playheadInClip = m_lastTimelineMs >= c->startMs && m_lastTimelineMs < c->endMs();
    const qint64 src = c->sourceAtLocalMs(
        std::clamp<qint64>(m_lastTimelineMs - c->startMs, 0, c->timelineDurationMs()));
    st.look = c->lookAt(src);
    st.frame = c->frame;
    st.fadeInMs = c->fadeInMs;
    st.fadeOutMs = c->fadeOutMs;
    st.spanMs = c->timelineDurationMs();
    st.maxSpanMs = std::llround((c->sourceDurationMs - c->trimInMs) / c->speed);
    st.startMs = c->startMs;
    st.keyCount = c->insetKeys.count();
    for (const InsetKey &k : c->insetKeys.keys())
        st.keyAtPlayhead |= st.playheadInClip &&
                            std::llabs(k.timeMs - src) <= Timeline::kInsetKeyDeleteSnapMs;
    const int seg = st.playheadInClip ? keytrack::segmentAt(c->insetKeys.keys(), src,
                                                            Timeline::kInsetKeyDeleteSnapMs)
                                      : -1;
    st.easeAvailable = seg >= 0;
    if (seg >= 0)
        st.ease = c->insetKeys.keys()[seg].ease;
    panel->setState(st);
}

void MainWindow::moveSelectedOverlayLayer(int step)
{
    // Past the neighbouring overlay track (with its audio track): moveTrack
    // puts it before that track going up, after it going down.
    int trackId = 0;
    if (!selectedOverlayClip(&trackId))
        return;
    int prev = -1, next = -1, at = -1;
    for (int i = 0; i < m_timeline->trackCount(); ++i) {
        if (m_timeline->trackAt(i).kind != Track::Kind::Video)
            continue;
        if (m_timeline->trackAt(i).id == trackId)
            at = i;
        else if (at < 0)
            prev = i;
        else if (next < 0)
            next = i;
    }
    const int to = step < 0 ? prev : next;
    if (to >= 0)
        m_timeline->moveTrack(trackId, to);
}

void MainWindow::setOverlayKeyAtPlayhead()
{
    const TrackClip *c = selectedOverlayClip();
    if (!c)
        return;
    if (m_lastTimelineMs < c->startMs || m_lastTimelineMs >= c->endMs()) {
        m_editToolbar->showNotice(tr("Move the playhead into the overlay clip to set a key"), false);
        return;
    }
    const qint64 src = c->sourceAtLocalMs(m_lastTimelineMs - c->startMs);
    m_timeline->upsertTrackClipInsetKey(c->id, src, c->lookAt(src));
}

void MainWindow::deleteOverlayKeyAtPlayhead()
{
    if (const TrackClip *c = selectedOverlayClip())
        m_timeline->removeTrackClipInsetKeyNear(
            c->id, c->sourceAtLocalMs(m_lastTimelineMs - c->startMs));
}

void MainWindow::clearOverlayKeys()
{
    if (const TrackClip *c = selectedOverlayClip())
        m_timeline->clearTrackClipInsetKeys(
            c->id, c->sourceAtLocalMs(std::clamp<qint64>(m_lastTimelineMs - c->startMs, 0,
                                                         c->timelineDurationMs())));
}

void MainWindow::applyInsetLook(int clipId, const inset::Look &look)
{
    const TrackClip *c = m_timeline->trackClipById(clipId);
    if (!c)
        return;
    if (c->insetKeys.isEmpty()) {
        m_timeline->setTrackClipInset(clipId, look, c->frame);
        return;
    }
    // Keyed: the edit becomes a key at the playhead, which has to be over it.
    if (m_lastTimelineMs < c->startMs || m_lastTimelineMs >= c->endMs()) {
        m_editToolbar->showNotice(tr("Move the playhead into the overlay clip to set a key"), false);
        refreshOverlayPanel(); // put the fields back
        pushInsetsToController();
        return;
    }
    m_timeline->upsertTrackClipInsetKey(clipId, c->sourceAtLocalMs(m_lastTimelineMs - c->startMs),
                                        look);
}

void MainWindow::trimAtPlayhead(bool in)
{
    // A selected audio clip takes the trim, cut at the playhead's point
    // in it; the playhead has to be over the clip.
    int trackId = 0;
    if (const TrackClip *a = m_selectedTrackClipId > 0
                                 ? m_timeline->trackClipById(m_selectedTrackClipId, &trackId)
                                 : nullptr) {
        if (m_lastTimelineMs <= a->startMs || m_lastTimelineMs >= a->endMs()) {
            m_editToolbar->showNotice(tr("Move the playhead into the selected clip to trim it"),
                                    false);
            return;
        }
        const qint64 src = a->sourceAtLocalMs(m_lastTimelineMs - a->startMs);
        if (in)
            m_timeline->setTrackClipTrimIn(a->id, src);
        else
            m_timeline->setTrackClipTrimOut(a->id, src);
        return;
    }
    // A selected video clip only if the playhead is over it (the playhead's
    // source time, m_lastPositionMs, is m_clipId's).
    if (!m_clipId)
        return;
    if (editClipId() != m_clipId) {
        m_editToolbar->showNotice(tr("Move the playhead into the selected clip to trim it"), false);
        return;
    }
    if (in)
        m_timeline->setClipTrimIn(m_clipId, m_lastPositionMs);
    else
        m_timeline->setClipTrimOut(m_clipId, m_lastPositionMs);
}

void MainWindow::addKey(const QString &group, const QString &what,
                        const QList<QKeySequence> &keys, const std::function<void()> &fn,
                        const QString &keysLabel)
{
    for (const QKeySequence &seq : keys) {
        auto *sc = new QShortcut(seq, this);
        sc->setContext(Qt::ApplicationShortcut);
        sc->setAutoRepeat(true);
        connect(sc, &QShortcut::activated, this, fn);
        m_editorShortcuts.push_back(sc);
    }
    m_keyHelp.append({group, what, keys, keysLabel});
}

void MainWindow::setupViewMenu()
{
    // View: what's seen while editing. The preview's stitch quality and size
    // are part of the project's stitch settings (Project::previewStitch) but
    // change only the preview, never the export.
    QMenu *viewMenu = menuBar()->addMenu(tr("&View"));
    viewMenu->setToolTipsVisible(true);
    const auto setPreview = [this](const std::function<void(stitch::PreviewStitchConfig &)> &edit) {
        stitch::PreviewStitchConfig cfg = m_project->previewStitch();
        edit(cfg);
        m_project->setPreviewStitch(cfg);
    };

    QMenu *qualityMenu = viewMenu->addMenu(tr("Preview &quality"));
    qualityMenu->setObjectName(QStringLiteral("previewQualityMenu"));
    qualityMenu->setToolTipsVisible(true);
    m_previewQuality = new QActionGroup(this);
    QAction *standard = qualityMenu->addAction(tr("&Standard"));
    standard->setObjectName(QStringLiteral("previewQualityStandardAction"));
    standard->setData(int(stitch::PreviewStitchConfig::Quality::Template));
    QAction *flow = qualityMenu->addAction(tr("&Optical flow (not yet available)"));
    flow->setObjectName(QStringLiteral("previewQualityFlowAction"));
    flow->setData(int(stitch::PreviewStitchConfig::Quality::OpticalFlow));
    flow->setToolTip(tr("Only Standard for now. Optical flow, a smoother seam for close "
                        "objects, isn't available yet."));
    flow->setEnabled(false); // no native optical-flow seam yet: shown, not pickable
    for (QAction *a : {standard, flow}) {
        a->setCheckable(true);
        m_previewQuality->addAction(a);
    }
    connect(m_previewQuality, &QActionGroup::triggered, this, [setPreview](QAction *a) {
        setPreview([a](stitch::PreviewStitchConfig &c) {
            c.quality = static_cast<stitch::PreviewStitchConfig::Quality>(a->data().toInt());
        });
    });

    // 2:1 presets only -- equirect is 360x180 and the reprojection shader
    // assumes it. Width is the source of truth; height is always width / 2.
    // Above 3840, check real time for the clip's mode (tools/playback_bench).
    QMenu *sizeMenu = viewMenu->addMenu(tr("Preview &size"));
    sizeMenu->setObjectName(QStringLiteral("previewSizeMenu"));
    sizeMenu->setToolTipsVisible(true);
    const QString sizeTip =
        tr("Almost never matters: the viewport normally draws straight from the two lens "
           "pictures at full detail. Only when the graphics card or driver can't do that is a "
           "flat 360° preview stitched first, and this sets its size. Auto uses the clip's "
           "full detail, as much as plays smoothly at its frame rate.");
    m_previewSize = new QActionGroup(this);
    for (const int w : {0, 1920, 2560, 3072, 3840, 5120, 5760, 7680}) {
        QAction *a = sizeMenu->addAction(w == 0 ? tr("&Auto (match source)")
                                                : QStringLiteral("%1 x %2").arg(w).arg(w / 2));
        a->setObjectName(QStringLiteral("previewSize%1Action").arg(w));
        a->setData(w);
        a->setCheckable(true);
        a->setToolTip(sizeTip);
        m_previewSize->addAction(a);
        if (w == 0)
            sizeMenu->addSeparator();
    }
    connect(m_previewSize, &QActionGroup::triggered, this, [setPreview](QAction *a) {
        setPreview([a](stitch::PreviewStitchConfig &c) {
            c.width = a->data().toInt(); // 0 = Auto
            c.height = c.width / 2;
        });
    });
    m_editorActions += {qualityMenu->menuAction(), sizeMenu->menuAction()};
    updatePreviewMenu(m_project->previewStitch());

    // The app's colours, per user (QSettings ui/theme, read in main()).
    // Also on the start screen, unlike the preview's settings.
    viewMenu->addSeparator();
    QMenu *themeMenu = viewMenu->addMenu(tr("&Theme"));
    themeMenu->setObjectName(QStringLiteral("themeMenu"));
    auto *themeGroup = new QActionGroup(this);
    for (const theme::Theme &t : theme::themes()) {
        QAction *a = themeMenu->addAction(t.name);
        a->setObjectName(QStringLiteral("theme_%1_Action").arg(t.id));
        a->setData(t.id);
        a->setCheckable(true);
        a->setChecked(t.id == theme::current().id);
        themeGroup->addAction(a);
    }
    connect(themeGroup, &QActionGroup::triggered, this, [](QAction *a) {
        const QString id = a->data().toString();
        if (id == theme::current().id)
            return;
        theme::apply(*qApp, theme::byId(id));
        QSettings(QStringLiteral("orbis-reframe"), QStringLiteral("orbis-reframe"))
            .setValue(QStringLiteral("ui/theme"), id);
    });
}

void MainWindow::updatePreviewMenu(const stitch::PreviewStitchConfig &cfg)
{
    if (!m_previewQuality)
        return; // a project change before the menu is built
    for (QAction *a : m_previewQuality->actions())
        if (a->data().toInt() == int(cfg.quality))
            a->setChecked(true);
    // A width that isn't a preset (a hand-edited .orp): the nearest one.
    QAction *best = nullptr;
    for (QAction *a : m_previewSize->actions())
        if (!best || std::abs(a->data().toInt() - cfg.width) <
                         std::abs(best->data().toInt() - cfg.width))
            best = a;
    if (best)
        best->setChecked(true);
}

void MainWindow::editProjectSettings()
{
    ProjectSettingsDialog dialog(m_project->output(),
                                 m_timeline->count() > 0 ? m_timeline->clipAt(0).fps : 0.0, this);
    if (dialog.exec() == QDialog::Accepted)
        m_project->setOutput(dialog.output()); // a no-op when nothing changed
}

void MainWindow::setupKeyboardShortcuts()
{
    // The player bar's buttons do what these keys do.
    const auto playPause = [this] {
        QMetaObject::invokeMethod(m_controller, "togglePlay", Qt::QueuedConnection);
    };
    const auto frameBack = [this] {
        navigateTo(m_timeline->stepFrames(m_lastTimelineMs, -1), true);
    };
    const auto frameForward = [this] {
        navigateTo(m_timeline->stepFrames(m_lastTimelineMs, 1), true);
    };
    const auto previousCut = [this] {
        navigateTo(m_timeline->adjacentEditPoint(m_lastTimelineMs, -1), false);
    };
    const auto nextCut = [this] {
        navigateTo(m_timeline->adjacentEditPoint(m_lastTimelineMs, 1), false);
    };
    const auto toStart = [this] { navigateTo(0, false); };
    const auto toEnd = [this] { navigateTo(m_timeline->totalDurationMs(), false); };
    connect(m_playerBar, &PlayerBar::frameBackClicked, this, frameBack);
    connect(m_playerBar, &PlayerBar::frameForwardClicked, this, frameForward);
    connect(m_playerBar, &PlayerBar::previousCutClicked, this, previousCut);
    connect(m_playerBar, &PlayerBar::nextCutClicked, this, nextCut);
    connect(m_playerBar, &PlayerBar::toStartClicked, this, toStart);
    connect(m_playerBar, &PlayerBar::toEndClicked, this, toEnd);

    const QString play = tr("Playback");
    addKey(play, tr("Play / pause"), {QKeySequence(Qt::Key_Space)}, playPause);
    addKey(play, tr("Previous / next frame (pauses)"), {QKeySequence(Qt::Key_Left)}, frameBack);
    addKey(play, QString(), {QKeySequence(Qt::Key_Right)}, frameForward);
    addKey(play, tr("Back / forward 1 second"), {QKeySequence(Qt::SHIFT | Qt::Key_Left)},
           [this] { navigateTo(m_lastTimelineMs - 1000, false); });
    addKey(play, QString(), {QKeySequence(Qt::SHIFT | Qt::Key_Right)},
           [this] { navigateTo(m_lastTimelineMs + 1000, false); });
    addKey(play, tr("Previous / next cut or clip edge"), {QKeySequence(Qt::Key_Up)}, previousCut);
    addKey(play, QString(), {QKeySequence(Qt::Key_Down)}, nextCut);
    // With an overlay clip selected, its keys (as K / Shift+K).
    const auto toKey = [this](int direction) {
        const TrackClip *o = selectedOverlayClip();
        navigateTo(m_timeline->adjacentKey(m_lastTimelineMs, direction, o ? o->id : 0), false);
    };
    addKey(play, tr("Previous / next keyframe: the selected overlay's, else the reframe ones"),
           {QKeySequence(Qt::SHIFT | Qt::Key_Up)}, [toKey] { toKey(-1); });
    addKey(play, QString(), {QKeySequence(Qt::SHIFT | Qt::Key_Down)}, [toKey] { toKey(1); });
    addKey(play, tr("Start / end of the timeline"), {QKeySequence(Qt::Key_Home)}, toStart);
    addKey(play, QString(), {QKeySequence(Qt::Key_End)}, toEnd);
    // J / L: the player bar's speed (K is "set keyframe", so Space stops).
    addKey(play, tr("Slower / faster playback (L also starts it); the sound plays at 1× only"),
           {QKeySequence(Qt::Key_J)}, [this] { m_playerBar->stepSpeed(-1); });
    addKey(play, QString(), {QKeySequence(Qt::Key_L)}, [this, playPause] {
        if (m_playing)
            m_playerBar->stepSpeed(1);
        else
            playPause();
    });
    addKey(play, tr("Type a time to jump to"), {QKeySequence(Qt::CTRL | Qt::Key_G)},
           [this] {
               if (m_fullScreen)
                   showPlayerBarForAWhile();
               m_playerBar->editTime();
           });
    addKey(play, tr("Full screen on / off (also a double-click on the picture; Esc leaves)"),
           {QKeySequence(Qt::Key_F11)}, [this] { setFullScreenView(!m_fullScreen); });
    // The window-level screenshot action (no menu holds it) in the list.
    m_keyHelp.append({play, tr("Save a screenshot of the frame at the playhead"),
                      m_screenshotAction->shortcuts(), QString()});

    const QString edit = tr("Editing");
    addKey(edit, tr("Set the trim in / out point at the playhead"),
           {QKeySequence(Qt::Key_BracketLeft)}, [this] { trimAtPlayhead(true); });
    addKey(edit, QString(), {QKeySequence(Qt::Key_BracketRight)},
           [this] { trimAtPlayhead(false); });
    addKey(edit, tr("Restore: give the selected audio / overlay clip, else the video clip, its "
                    "full length"),
           {QKeySequence(Qt::Key_R)}, [this] { clearTrim(); });
    addKey(edit, tr("Split the selected audio / overlay clip, else the video clip, at the playhead"),
           {QKeySequence(Qt::Key_S)}, [this] { splitAtPlayhead(); });
    addKey(edit,
           tr("Delete the selected clip or transition, else the reframe keyframe at the "
              "playhead"),
           {QKeySequence(Qt::Key_Delete), QKeySequence(Qt::Key_Backspace)},
           [this] { deleteAtPlayhead(); });
    addKey(edit,
           tr("Clear the selection (or cancel drawing a Track subject / Blur a plate box, or "
              "leave full screen)"),
           {QKeySequence(Qt::Key_Escape)}, [this] {
               if (m_trackSelecting) {
                   setTrackSelecting(false);
                   return;
               }
               if (m_plateSelecting) {
                   setPlateSelecting(false);
                   return;
               }
               if (m_fullScreen) {
                   setFullScreenView(false);
                   return;
               }
               selectTrackClip(-1);
               selectVideoClip(0);
               selectTransition(0);
           });

    const QString reframe = tr("Reframe");
    addKey(reframe, tr("Arm / disarm reframe authoring (drags and zooms set keyframes)"),
           {QKeySequence(Qt::Key_A)}, [this] { m_editToolbar->toggleReframeArmed(); });
#ifdef ORBIS_REFRAME_HAVE_TRACKING
    addKey(reframe, tr("Track subject: draw a box around it (again to cancel)"),
           {QKeySequence(Qt::Key_T)}, [this] { setTrackSelecting(!m_trackSelecting); });
#endif
    // With an overlay clip selected, K / Shift+K act on its keys instead (the
    // Overlay panel's Set key / Delete).
    addKey(reframe,
           tr("Set a keyframe at the playhead: the selected overlay's look, else the "
              "current view"),
           {QKeySequence(Qt::Key_K)}, [this] {
               if (selectedOverlayClip())
                   setOverlayKeyAtPlayhead();
               else if (m_clipId)
                   m_timeline->upsertClipReframeKey(m_clipId, m_lastPositionMs,
                                                    {m_viewport->Yaw(), m_viewport->Pitch(),
                                                     m_viewport->Roll(), m_viewport->FovY()});
           });
    addKey(reframe,
           tr("Delete the keyframe at the playhead: the selected overlay's, else the "
              "reframe one"),
           {QKeySequence(Qt::SHIFT | Qt::Key_K)}, [this] {
               if (selectedOverlayClip())
                   deleteOverlayKeyAtPlayhead();
               else if (m_clipId)
                   m_timeline->removeClipReframeKeyNear(m_clipId, m_lastPositionMs);
           });
    addKey(reframe,
           tr("Delete all keyframes: the selected overlay's, else the video clip's reframe ones"),
           {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_K)}, [this] {
               if (selectedOverlayClip())
                   clearOverlayKeys();
               else if (m_clipId)
                   m_timeline->clearClipReframe(m_clipId);
           });

#ifdef ORBIS_REFRAME_HAVE_PLATE_SCAN
    addKey(tr("Privacy"), tr("Blur a plate: draw a box around it (again to cancel or stop)"),
           {QKeySequence(Qt::Key_B)}, [this] { togglePlateBlur(); });
#endif

    // Timeline zoom (also Ctrl+wheel over the timeline).
    const QString view = tr("Timeline view");
    addKey(view, tr("Zoom in"),
           {QKeySequence(Qt::CTRL | Qt::Key_Equal), QKeySequence(Qt::CTRL | Qt::Key_Plus)},
           [this] { m_timelineWidget->zoomIn(); });
    addKey(view, tr("Zoom out"), {QKeySequence(Qt::CTRL | Qt::Key_Minus)},
           [this] { m_timelineWidget->zoomOut(); });
    addKey(view, tr("Zoom to fit"), {QKeySequence(Qt::CTRL | Qt::Key_0)},
           [this] { m_timelineWidget->zoomToFit(); });

    const QString sidebars = tr("Sidebars");
    addKey(sidebars, tr("Open / close the left sidebar (Media)"),
           {QKeySequence(Qt::CTRL | Qt::Key_B)}, [this] { m_sourcesSidebar->toggle(); });
    addKey(sidebars, tr("Open / close the right sidebar (settings)"),
           {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_B)}, [this] { m_sidebar->toggle(); });
    // The right sidebar's tabs, left to right; the one showing closes it.
    constexpr Qt::Key kDigits[] = {Qt::Key_1, Qt::Key_2, Qt::Key_3, Qt::Key_4, Qt::Key_5,
                                   Qt::Key_6, Qt::Key_7, Qt::Key_8, Qt::Key_9};
    const int tabs = std::min(m_sidebar->count(), 9);
    for (int i = 0; i < tabs; ++i)
        addKey(sidebars,
               i == 0 ? tr("Show a settings tab (Video, Audio, Overlay, Title, Transition, "
                           "Privacy); again to close the sidebar")
                      : QString(),
               {QKeySequence(Qt::ALT | kDigits[i])}, [this, i] { m_sidebar->toggleTab(i); },
               i == 0 ? QKeySequence(Qt::ALT | Qt::Key_1).toString(QKeySequence::NativeText) +
                            QStringLiteral(" … ") +
                            QKeySequence(Qt::ALT | kDigits[tabs - 1])
                                .toString(QKeySequence::NativeText)
                      : QString());

    QMenu *helpMenu = menuBar()->addMenu(tr("&Help"));
    QAction *manualAction = helpMenu->addAction(tr("&User manual"), this, [] {
        QDesktopServices::openUrl(userManualUrl(QString::fromUtf8(core::version::string())));
    });
    manualAction->setObjectName(QStringLiteral("userManualAction"));
    QAction *keysAction =
        helpMenu->addAction(tr("&Keyboard shortcuts"), this,
                           [this] { showShortcutHelp(this, m_keyHelp, menuBar()); });
    keysAction->setObjectName(QStringLiteral("shortcutHelpAction"));
    keysAction->setShortcut(QKeySequence(Qt::Key_F1));
    helpMenu->addSeparator();
    QAction *aboutAction = helpMenu->addAction(tr("&About orbis-reframe"), this,
                                               [this] { showAboutDialog(this, m_viewport); });
    aboutAction->setObjectName(QStringLiteral("aboutAction"));
}

void MainWindow::navigateTo(qint64 timelineMs, bool pause)
{
    if (m_timeline->isEmpty())
        return;
    timelineMs = std::clamp<qint64>(timelineMs, 0, m_timeline->totalDurationMs());
    if (pause && m_playing)
        QMetaObject::invokeMethod(m_controller, "pause", Qt::QueuedConnection);
    QMetaObject::invokeMethod(m_controller, "seek", Qt::QueuedConnection,
                              Q_ARG(qint64, timelineMs));
#ifdef ORBIS_REFRAME_HAVE_AUDIO
    m_audioPlayer->seek(timelineMs);
#endif
    m_lastTimelineMs = timelineMs;
    showPlayhead(timelineMs);
    bindActiveClipAt(timelineMs);
}

void MainWindow::setFullScreenView(bool on)
{
    if (on == m_fullScreen || (on && m_pages->currentWidget() != m_editorPage))
        return;
    m_fullScreen = on;
    if (on)
        m_wasMaximized = isMaximized();
    // Everything but the viewport and the player bar.
    for (QWidget *w : {static_cast<QWidget *>(menuBar()), static_cast<QWidget *>(m_sourcesSidebar),
                       static_cast<QWidget *>(m_sidebar), static_cast<QWidget *>(m_timelinePane)})
        w->setVisible(!on);
    m_playerBar->setFullScreen(on);
    m_playerBar->setVisible(true);
    if (on) {
        showFullScreen();
        m_playerBarHide->start();
    } else {
        m_playerBarHide->stop();
        if (m_wasMaximized)
            showMaximized();
        else
            showNormal();
    }
}

void MainWindow::showPlayerBarForAWhile()
{
    if (!m_fullScreen)
        return;
    m_playerBar->setVisible(true);
    m_playerBarHide->start();
}

void MainWindow::updatePlayerBar()
{
    const double firstFps = m_timeline->count() > 0 ? m_timeline->clipAt(0).fps : 0.0;
    m_playerBar->setFrameRate(m_project->output().effectiveFps(firstFps));
    QList<qint64> cuts;
    qint64 t = 0;
    for (int i = 0; i + 1 < m_timeline->count(); ++i) {
        t += m_timeline->clipAt(i).timelineDurationMs();
        cuts.push_back(t);
    }
    m_playerBar->setCuts(cuts);
}

void MainWindow::applyPreviewVolume()
{
#ifdef ORBIS_REFRAME_HAVE_AUDIO
    // Squared: the slider's middle sounds about half as loud, not barely quieter.
    const double v = m_playerBar->volume() / 100.0;
    m_audioPlayer->setVolume(m_playerBar->isMuted() ? 0.0f : static_cast<float>(v * v));
#endif
}

void MainWindow::splitAtPlayhead()
{
    if (const TrackClip *a = m_selectedTrackClipId > 0
                                 ? m_timeline->trackClipById(m_selectedTrackClipId)
                                 : nullptr) {
        if (m_lastTimelineMs <= a->startMs || m_lastTimelineMs >= a->endMs()) {
            m_editToolbar->showNotice(tr("Move the playhead into the selected clip to split it"),
                                    false);
            return;
        }
        if (const int right = m_timeline->splitTrackClip(a->id, m_lastTimelineMs))
            selectTrackClip(right);
        else
            m_editToolbar->showNotice(tr("Too close to the clip's edge to split"), false);
        return;
    }
    if (!m_clipId)
        return;
    if (editClipId() != m_clipId) {
        m_editToolbar->showNotice(tr("Move the playhead into the selected clip to split it"), false);
        return;
    }
    if (!m_timeline->splitClip(m_clipId, m_lastPositionMs))
        m_editToolbar->showNotice(tr("Too close to the clip's edge to split"), false);
}

void MainWindow::deleteAtPlayhead()
{
    if (m_selectedTransition) {
        m_timeline->removeTransition(m_selectedTransition);
        return;
    }
    if (m_selectedTrackClipId > 0) {
        m_timeline->removeTrackClip(m_selectedTrackClipId);
        return;
    }
    if (m_selectedClipId > 0) {
        m_timeline->removeClip(m_selectedClipId);
        return;
    }
    if (m_clipId)
        m_timeline->removeClipReframeKeyNear(m_clipId, m_lastPositionMs);
}

void MainWindow::clearTrim()
{
    if (const TrackClip *a = m_selectedTrackClipId > 0
                                 ? m_timeline->trackClipById(m_selectedTrackClipId)
                                 : nullptr) {
        const int id = a->id;
        const qint64 full = a->sourceDurationMs;
        m_timeline->setTrackClipTrimIn(id, 0); // the start moves back with it
        m_timeline->setTrackClipTrimOut(id, full);
        return;
    }
    if (const int id = editClipId())
        m_timeline->clearClipTrim(id);
}

void MainWindow::putOverlayAudioOnTrack(int clipId)
{
    if (const int id = addOverlayAudio(clipId))
        selectTrackClip(id);
}

int MainWindow::addOverlayAudio(int clipId)
{
    int trackId = 0;
    const TrackClip *o = m_timeline->trackClipById(clipId, &trackId);
    if (!o || m_timeline->trackById(trackId)->kind != Track::Kind::Video || o->isTitle() ||
        audioStreamCount(o->sourcePath) < 1)
        return 0;
    return m_timeline->addOverlaySound(clipId);
}

void MainWindow::syncMediaInfoPanel(const ClipItem &c)
{
    auto it = m_mediaInfo.constFind(c.sourcePath);
    if (it == m_mediaInfo.constEnd()) {
        insv::MediaInfo info;
        std::string err;
        if (!insv::ProbeMediaInfo(c.sourcePath.toStdString(), info, err)) {
            m_mediaInfoPanel->setError(c.sourcePath, QString::fromStdString(err));
            return;
        }
        it = m_mediaInfo.insert(c.sourcePath, info);
    }
    m_mediaInfoPanel->setInfo(*it);
}

void MainWindow::pushTimelineToController()
{
    pushBlur(); // the clips' trims / sources may have changed
    QList<PlaybackController::Segment> segments;
    segments.reserve(m_timeline->count());
    qint64 total = 0;
    m_captureTimingOn.clear(); // ids don't survive a load / undo
    for (int i = 0; i < m_timeline->count(); ++i) {
        const ClipItem &c = m_timeline->clipAt(i);
        segments.push_back({c.sourcePath, c.trimInMs, c.trimOutMs, c.speed, c.id, c.flat, c.fps,
                            c.sourceDurationMs, c.lensGuard, c.depthSeam});
        total += c.timelineDurationMs();
        m_captureTimingOn[c.id] = StabilizerCache::captureTimingOn(c);
    }
    m_totalTimelineMs = total;
    pushInsetsToController(); // flat main clips are drawn as layers
    // Queued lambda (not a slot) so QList<Segment> needs no metatype.
    QMetaObject::invokeMethod(m_controller,
                              [ctrl = m_controller, segments] { ctrl->setTimeline(segments); });
#ifdef ORBIS_REFRAME_HAVE_AUDIO
    m_audioPlayer->setTimeline(segments); // GUI-thread resident -- no invokeMethod needed
#endif
    ensureStabilizers(); // the stabilizers by the clips' (new) ids
}

void MainWindow::refreshTimelineWidget()
{
    updateMediaInUse();
    QVector<TimelineWidget::Clip> clips;
    clips.reserve(m_timeline->count());
    bool activeHasKeys = false;
    for (int i = 0; i < m_timeline->count(); ++i) {
        const ClipItem &c = m_timeline->clipAt(i);
        TimelineWidget::Clip w;
        w.id = c.id;
        w.label = QFileInfo(c.sourcePath).fileName();
        w.trimInMs = c.trimInMs;
        w.trimOutMs = c.trimOutMs;
        w.sourceDurationMs = c.sourceDurationMs;
        w.missing = c.mediaMissing;
        w.speed = c.speed;
        for (qint64 t : c.reframe.keyTimes())
            w.keySourceMs.push_back(t);
        clips.push_back(std::move(w));
        if (c.id == m_clipId && !c.reframe.isEmpty())
            activeHasKeys = true;
    }
    m_timelineWidget->setClips(clips);
    m_timelineWidget->setActiveClip(editClipId());
    refreshTransitions(); // a trim or speed change moves what they play
    refreshTrackRows();
    m_editToolbar->setHasReframeKeys(activeHasKeys);
    updatePlayerBar();
    m_reframePanel->setClipUnkeyed(m_timeline->clipById(m_clipId) && !activeHasKeys);
    updateExportEnabled();
    updateScreenshotEnabled();
}

void MainWindow::syncTimeline()
{
    pushTimelineToController();
    refreshTimelineWidget();
}

void MainWindow::remapPlayheadAfterEdit()
{
    // A trim / insert / remove / reorder changes the timeline<->source mapping.
    // Rebuild the controller's segments, then re-seek so the frame the user was
    // looking at (this clip's source time m_lastPositionMs) stays under the
    // playhead -- clamped into the clip's new [in, out] if the edit cut past it.
    if (m_rebindingModel || !m_clipId) {
        syncTimeline();
        return;
    }
    const int idx = m_timeline->indexOfId(m_clipId);
    if (idx < 0) { // the clip under the playhead is gone -- let the clamp settle it
        syncTimeline();
        return;
    }
    const ClipItem &c = m_timeline->clipAt(idx);
    const qint64 srcClamped = std::clamp(m_lastPositionMs, c.trimInMs, c.trimOutMs);
    const qint64 newTimelineMs = m_timeline->clipToTimeline(idx, c.localAtSourceMs(srcClamped));

    syncTimeline();

    m_lastPositionMs = srcClamped;
    QMetaObject::invokeMethod(m_controller, "seek", Qt::QueuedConnection,
                              Q_ARG(qint64, newTimelineMs));
#ifdef ORBIS_REFRAME_HAVE_AUDIO
    m_audioPlayer->seek(newTimelineMs);
#endif
}

void MainWindow::undo()
{
    if (!m_project->canUndo())
        return;
    m_rebindingModel = true; // suppress the storm of remapPlayheadAfterEdit calls this triggers
    m_project->undo();
    m_rebindingModel = false;
    afterUndoRedo();
}

void MainWindow::redo()
{
    if (!m_project->canRedo())
        return;
    m_rebindingModel = true;
    m_project->redo();
    m_rebindingModel = false;
    afterUndoRedo();
}

void MainWindow::showPlayhead(qint64 timelineMs)
{
    m_playerBar->setPosition(timelineMs);
    m_timelineWidget->setPlayhead(timelineMs);
}

void MainWindow::afterUndoRedo()
{
    // undo()/redo() replace the whole Timeline (Project::applyDocument), so
    // every ClipItem has a fresh id -- rebind by the timeline position the
    // playhead was already at rather than trying to find the "same" clip id.
    syncTimeline();
    bindActiveClipAt(m_lastTimelineMs);
    showPlayhead(m_lastTimelineMs);
    QMetaObject::invokeMethod(m_controller, "seek", Qt::QueuedConnection,
                              Q_ARG(qint64, m_lastTimelineMs));
#ifdef ORBIS_REFRAME_HAVE_AUDIO
    m_audioPlayer->seek(m_lastTimelineMs);
#endif
}

// "*.insv *.mp4 ...": the files the main track takes, 360 clips and flat
// videos, for a file dialog's filter.
static QString clipFileGlobs()
{
    QStringList globs;
    for (const auto kind : {MediaItem::Kind::Clip360, MediaItem::Kind::Video})
        for (const QString &ext : media::extensions(kind))
            globs << QStringLiteral("*.") + ext;
    return globs.join(QLatin1Char(' '));
}

void MainWindow::addClipToTimeline(int index)
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Add clip to timeline"),
        m_clipPath.isEmpty() ? QDir::homePath() : QFileInfo(m_clipPath).absolutePath(),
        tr("Videos (%1);;All files (*)").arg(clipFileGlobs()));
    if (!path.isEmpty())
        addClipFiles({path}, index);
}

void MainWindow::addClipFiles(const QStringList &paths, int index)
{
    if (index < 0 || index > m_timeline->count())
        index = m_timeline->count();
    QStringList failed;
    for (const QString &path : paths) {
        // A 360 clip through the clip probe, a flat video through FFmpeg.
        QString why;
        const std::optional<MediaItem> m = probeMedia(path, &why, m_clipBackend.probe);
        if (!m || m->kind == MediaItem::Kind::Audio) {
            failed << tr("%1: %2").arg(QFileInfo(path).fileName(),
                                        m ? tr("not a video") : why);
            continue;
        }
        const bool flat = m->kind == MediaItem::Kind::Video;
        const bool wasEmpty = m_timeline->count() == 0;
        m_timeline->insertClip(index++, path, m->durationMs, m->fps, m->width, m->height,
                               audioStreamCount(path), flat);
        if (wasEmpty) {
            m_clipPath = path;
            m_fps = m->fps;
        }
    }
    if (!failed.isEmpty())
        QMessageBox::warning(this, tr("Could not add clip"), failed.join(QLatin1Char('\n')));
}

void MainWindow::addMediaToTimeline(const QStringList &paths)
{
    QStringList clips, videos, audio;
    for (const QString &path : paths) {
        const MediaItem *m = m_project->mediaPool()->item(path);
        if (!m || m->missing)
            continue;
        switch (m->kind) {
        case MediaItem::Kind::Clip360:
            clips << path;
            break;
        case MediaItem::Kind::Video:
            videos << path;
            break;
        case MediaItem::Kind::Audio:
            audio << path;
            break;
        }
    }
    // Clips first: overlays and audio need a video track to lie over. Flat
    // videos are overlays over a main clip; with none, they become the
    // main clips.
    if (m_timeline->count() == 0 && clips.isEmpty())
        std::swap(clips, videos);
    if (!clips.isEmpty())
        addClipFiles(clips, -1);
    if (!audio.isEmpty() && m_timeline->count() == 0) {
        QMessageBox::information(this, tr("Add to timeline"),
                                 tr("Audio goes under a video: add a clip to the timeline "
                                    "first."));
        return;
    }
    if (!videos.isEmpty())
        selectAdded(m_trackFiles->addVideo(videos, 0, m_lastTimelineMs));
    if (!audio.isEmpty())
        selectAdded(m_trackFiles->addAudio(audio, 0, m_lastTimelineMs));
}

void MainWindow::locateMedia(const QString &path)
{
    const MediaImporter::Relinks relinks = m_mediaImport->locate(path);
    if (relinks.isEmpty())
        return;
    for (const auto &[from, to] : relinks) {
        QSet<QString> keys;
        for (int i = 0; i < m_timeline->count(); ++i)
            if (m_timeline->clipAt(i).sourcePath == from)
                keys.insert(m_timeline->clipAt(i).plateScan);
        for (const QString &key : keys)
            m_privacy.scans().carryOver(from, to.path, key);
        m_project->relinkMedia(from, to);
        if (m_clipPath == from)
            m_clipPath = to.path;
    }
    refreshClipPanels();
    m_measurements->schedule(); // sync / seam for a clip that had no file until now
    m_mediaImport->showRelinked(relinks);
}

void MainWindow::recheckMissingMedia()
{
    const QHash<QString, bool> changed = m_project->recheckMissingMedia();
    if (changed.isEmpty())
        return;
    QStringList gone, back;
    for (auto it = changed.cbegin(); it != changed.cend(); ++it)
        (*it ? gone : back) << it.key();
    bool mainTrack = false;
    for (int i = 0; i < m_timeline->count() && !mainTrack; ++i)
        mainTrack = changed.contains(m_timeline->clipAt(i).sourcePath);

    // A file that's back is opened afresh: the source cached for it may have
    // lost its file (a drive unplugged). One that's gone keeps playing from
    // what's open as long as it can.
    m_controller->reopenSources(back);
    if (mainTrack)
        syncTimeline(); // the ⚠ marks; a clip that's back plays again
    onTracksChanged(); // the track rows, overlays and sound skip missing clips
    refreshClipPanels();
    if (!back.isEmpty())
        m_measurements->schedule(); // sync / seam for a clip that had no file until now

    const auto names = [](QStringList paths) {
        paths.sort();
        for (QString &p : paths)
            p = QFileInfo(p).fileName();
        return paths.join(QStringLiteral(", "));
    };
    QStringList notice;
    if (!gone.isEmpty())
        notice << tr("File not found: %1").arg(names(gone));
    if (!back.isEmpty())
        notice << tr("Found again: %1").arg(names(back));
    m_editToolbar->showNotice(notice.join(QStringLiteral(" · ")), false);
}

void MainWindow::updateMediaInUse()
{
    QSet<QString> used;
    for (int i = 0; i < m_timeline->count(); ++i)
        used.insert(m_timeline->clipAt(i).sourcePath);
    for (int i = 0; i < m_timeline->trackCount(); ++i)
        for (const TrackClip &c : m_timeline->trackAt(i).clips)
            used.insert(c.sourcePath);
    m_mediaPanel->setInUse(used);
}

void MainWindow::openPath(const QString &path)
{
    if (QFileInfo(path).suffix().compare(QLatin1String(Project::kFileExtension),
                                         Qt::CaseInsensitive) == 0)
        openProject(path);
    else
        openClip(path);
}

void MainWindow::openClip(const QString &clipPath)
{
    stitch::ClipProbe probe;
    std::string probeErr;
    if (!m_clipBackend.probe(clipPath.toStdString(), probe, probeErr)) {
        QMessageBox::warning(this, tr("Could not open clip"),
                             probeErr.empty() ? tr("Unrecognised file: %1").arg(clipPath)
                                              : QString::fromStdString(probeErr));
        return;
    }
    m_clipPath = clipPath;
    m_fps = probe.fps;
    m_rebindingModel = true;
    m_clipId = m_project->resetWithClip(clipPath, probe.duration_ms, probe.fps, probe.width,
                                        probe.height, audioStreamCount(clipPath));
    m_rebindingModel = false;
    if (const ClipItem *c = m_timeline->clipById(m_clipId)) {
        refreshAudioPanel();
        syncMediaInfoPanel(*c);
        applyColorGrade(c->color);
        m_colourPanel->setState(c->color);
        syncStabilizationPanel();
        syncStitchingPanel();
        syncPrivacyPanel();
    }
    showEditor();
    syncTimeline(); // opens paused on the first frame
}

void MainWindow::bindEmptyTimeline()
{
    m_clipId = 0;
    m_clipPath.clear();
    m_fps = 0.0;
    m_audioPanel->setNoClip();
    m_colourPanel->setNoClip();
    m_stabilizationPanel->setNoClip();
    m_stitchingPanel->setNoClip();
    m_privacyPanel->setNoClip();
    m_mediaInfoPanel->setNoClip();
    // Nothing of the last project carries over: the view looks straight
    // ahead again, and reframe authoring starts disarmed.
    const ReframePose ahead;
    m_viewport->SetPose(ahead.yaw, ahead.pitch, ahead.roll, ahead.fovY);
    m_reframePanel->setClipUnkeyed(false);
    m_editToolbar->setReframeArmed(false);
    m_editToolbar->setHasReframeKeys(false);
    syncTimeline(); // empty -> controller goes idle, widget clears
    syncReframePanel();
    updateSeamStatus();
    updateSyncStatus();
}

void MainWindow::openProject(const QString &projectPath)
{
    QString error;
    const bool recover = m_files->askRecover(projectPath);
    const QString recovery = Autosave::pathFor(projectPath);
    // Before the load: rebuilding the timeline already asks for waveforms.
    updateCacheDirs(projectPath);
    m_rebindingModel = true;
    const bool loaded = recover ? m_project->recover(recovery, projectPath, &error)
                                : m_project->load(projectPath, &error);
    if (loaded && recover)
        m_autosave->tick(); // the recovery file is this session's now
    m_rebindingModel = false;
    if (!loaded) {
        updateCacheDirs(m_project->filePath());
        QMessageBox::warning(this, tr("Could not open project"), error);
        return;
    }
    m_files->noteOpened();
    showEditor();
    if (m_timeline->count() == 0) {
        bindEmptyTimeline();
        return;
    }

    const ClipItem &first = m_timeline->clipAt(0);
    m_clipId = first.id;
    m_clipPath = first.sourcePath;
    m_fps = first.fps;
    refreshAudioPanel();
    syncMediaInfoPanel(first);
    applyColorGrade(first.color);
    m_colourPanel->setState(first.color);
    syncStabilizationPanel();
    syncStitchingPanel();
    syncPrivacyPanel();

    if (first.mediaMissing) {
        // Keep the loaded model on screen but don't try to play a missing file.
        QMessageBox::warning(this, tr("Missing media"),
                             tr("A clip's source file was not found:\n%1\n\nIf it was moved or "
                                "renamed, find it with Locate file… on it in the Media tab.")
                                 .arg(first.sourcePath));
        syncTimeline();
        return;
    }
    syncTimeline(); // opens paused on the first frame
}

void MainWindow::openProjectFile()
{
    if (!m_files->confirmDiscard())
        return;
    if (const QString path = m_files->pickProjectFile(); !path.isEmpty())
        openProject(path);
}

void MainWindow::newProject()
{
    if (!m_files->confirmDiscard())
        return;
    const std::optional<ProjectFiles::NewProject> made = m_files->askNewProject();
    if (!made)
        return;
    m_rebindingModel = true;
    m_project->newProject();
    m_project->setOutput(made->output);
    m_rebindingModel = false;
    // Saved straight away, so the project is on disk and in the library
    // from the start.
    QString error;
    if (!m_project->save(made->file, &error))
        QMessageBox::warning(this, tr("Could not save project"), error);
    else
        m_files->noteOpened();
    showEditor();
    bindEmptyTimeline();
}

void MainWindow::closeToLibrary()
{
    if (!m_files->confirmDiscard())
        return;
    stopPlateJob(); // its scan is about clips that are going away
    m_rebindingModel = true;
    m_project->newProject();
    m_rebindingModel = false;
    bindEmptyTimeline();
    showLibrary();
}

void MainWindow::showLibrary()
{
    m_mediaImport->newSession();
    setEditorActive(false);
    if (m_pages->currentWidget() != m_libraryPage) {
        // Let the viewport release its swapchain while it's still shown (as
        // closeEvent does): hiding it first crashes some NVIDIA drivers. It
        // starts again on its own when the editor is shown.
        QPlatformSurfaceEvent aboutToGo(QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed);
        QCoreApplication::sendEvent(m_viewport, &aboutToGo);
        m_pages->setCurrentWidget(m_libraryPage);
    }
    m_libraryPage->refresh();
    m_libraryPage->setFocus();
    updateWindowTitle();
}

void MainWindow::showEditor()
{
    m_mediaImport->newSession();
    m_pages->setCurrentWidget(m_editorPage);
    setEditorActive(true);
    updateWindowTitle();
    // Once the page has its height (the window may still be laying out).
    QTimer::singleShot(0, this, &MainWindow::fitTimelineHeight);
}

void MainWindow::setEditorActive(bool active)
{
    if (!active)
        setFullScreenView(false);
    for (QAction *a : std::as_const(m_editorActions))
        a->setEnabled(active);
    for (QShortcut *sc : std::as_const(m_editorShortcuts))
        sc->setEnabled(active);
}

void MainWindow::updateCacheDirs(const QString &projectFile)
{
    // A project folder's own cache/; the user cache for an untitled or loose
    // project (and if the folder can't be made).
    const QString dir = projectfolder::ensure(projectfolder::cacheDir(projectFile));
    m_privacy.scans().setDirectory(dir);
    m_waveforms->setDirectory(dir);
    if (m_thumbnailCache)
        m_thumbnailCache->setDirectory(dir);
}

QString MainWindow::askSaveFileName(const QString &caption, const QString &path,
                                    const QString &filter)
{
    QFileDialog dlg(this, caption, path, filter);
    dlg.setAcceptMode(QFileDialog::AcceptSave);
    dlg.setLabelText(QFileDialog::Accept, tr("&Save"));
    if (dlg.exec() != QDialog::Accepted || dlg.selectedFiles().isEmpty())
        return {};
    return dlg.selectedFiles().constFirst();
}

void MainWindow::updateWindowTitle()
{
    if (m_pages && m_pages->currentWidget() == m_libraryPage) {
        setWindowTitle(QStringLiteral("orbis-reframe"));
        return;
    }
    QString name = m_project && !m_project->filePath().isEmpty()
                       ? QFileInfo(m_project->filePath()).fileName()
                       : tr("untitled");
    const QString star =
        (m_project && m_project->modified()) ? QStringLiteral(" *") : QString();
    setWindowTitle(QStringLiteral("orbis-reframe — %1%2").arg(name, star));
}

void MainWindow::exportFlatVideo()
{
    if (m_exports->exporting())
        return;
    const int nclips = m_timeline->count();
    if (nclips == 0)
        return;
    // Checked first: without a hardware encoder the export would only fail
    // once FFmpeg opens the encoder, with its own error text.
    if (!syscheck::hwEncoderAvailable()) {
        QMessageBox::warning(this, tr("Export flat video"), syscheck::hwEncoderMissingText());
        return;
    }
    // Every source looked for on the disk now (the marks follow), not as last
    // checked: a missing main clip would only fail partway through, a
    // missing overlay or sound clip would be left out without a word. Either
    // way the user sorts it out first.
    recheckMissingMedia();
    if (!checkSourcesPresent(renderjobs::missingSources(*m_timeline)))
        return;

    // One output frame rate for the whole timeline: the project's (Project
    // Settings), or clip 0's when it's "match first clip". Each clip is
    // conformed to it -- frames dropped or repeated by source time, at the
    // clip's speed (ReframeExportController).
    const double fps = m_project->output().effectiveFps(m_timeline->clipAt(0).fps);

    // A clip with the privacy blur on is only exported once all of it that
    // shows has been scanned for plates: its trimmed range, and the footage
    // past it that a transition plays.
    for (int i = 0; i < nclips; ++i) {
        const ClipItem &c = m_timeline->clipAt(i);
        qint64 from = c.trimInMs, to = c.trimOutMs;
        m_timeline->shownSourceRange(c.id, from, to);
        if (const QString why = m_privacy.blocker(c, from, to); !why.isEmpty()) {
            QMessageBox::warning(this, tr("Export flat video"), why);
            return;
        }
    }
    {
        QStringList stale;
        for (int i = 0; i < nclips; ++i) {
            const ClipItem &c = m_timeline->clipAt(i);
            qint64 from = c.trimInMs, to = c.trimOutMs;
            m_timeline->shownSourceRange(c.id, from, to);
            if (const QString w = m_privacy.staleness(c, from, to, true); !w.isEmpty())
                stale << w;
        }
        if (!confirmStalePrivacy(tr("Export flat video"), stale))
            return;
    }

    qint64 totalRangeMs = 0;
    int totalKeys = 0;
    QStringList keyless; // "2 · name (at 00:12)", in timeline order
    for (int i = 0; i < nclips; ++i) {
        const ClipItem &c = m_timeline->clipAt(i);
        if (c.reframe.isEmpty()) {
            const qint64 s = totalRangeMs / 1000;
            keyless << tr("%1 · %2 (at %3:%4)")
                           .arg(i + 1)
                           .arg(QFileInfo(c.sourcePath).fileName().toHtmlEscaped())
                           .arg(s / 60, 2, 10, QLatin1Char('0'))
                           .arg(s % 60, 2, 10, QLatin1Char('0'));
        }
        totalRangeMs += c.timelineDurationMs();
        totalKeys += c.reframe.count();
    }
    if (totalRangeMs < 100) {
        QMessageBox::warning(this, tr("Export flat video"),
                             tr("The timeline is too short to export."));
        return;
    }

    ExportOptionsDialog opts(nclips, totalRangeMs, totalKeys, fps, keyless, m_project->output(),
                             this);
    if (opts.exec() != QDialog::Accepted)
        return;
    opts.rememberQuality();

    QString path = askSaveFileName(tr("Export flat video"),
                                   renderjobs::suggestedExportPath(m_project->filePath()),
                                   tr("MP4 video (*.mp4)"));
    if (path.isEmpty())
        return;
    if (QFileInfo(path).suffix().isEmpty())
        path += QStringLiteral(".mp4");

    ReframeExportController::Request req;
    req.outPath = path;
    req.fps = fps;
    req.outWidth = opts.outWidth();
    req.outHeight = opts.outHeight();
    req.h265 = opts.h265();
    req.bitrateKbps = opts.bitrateKbps();

    // Per clip: the reframe track and the built stabilizer; the export samples
    // both per decoded frame at its own source time.
    for (int i = 0; i < nclips; ++i)
        req.clips.push_back(exportJobFor(m_timeline->clipAt(i)));
    req.mix = renderjobs::mixClips(*m_timeline); // the audio tracks
    // The overlay tracks, over the flat main clips.
    req.insets = renderjobs::insetJobs(*m_timeline, req.outWidth, req.outHeight, m_privacy);

    // The export opens its own NativeStitchSource (on the shared decode
    // device) -- pause playback so the two stitch pipelines don't contend
    // for the GPU.
    QMetaObject::invokeMethod(m_controller, &PlaybackController::pause);

    m_exports->startExport(req);
}

ReframeExportController::ClipJob MainWindow::exportJobFor(const ClipItem &c) const
{
    return renderjobs::clipJob(c, *m_timeline, m_privacy);
}

bool MainWindow::checkSourcesPresent(const QStringList &missing)
{
    if (missing.isEmpty())
        return true;
    QMessageBox box(QMessageBox::Warning, tr("Export flat video"),
                    tr("The timeline can't be exported: these source files are missing."),
                    QMessageBox::Ok, this);
    box.setInformativeText(missing.join(QLatin1Char('\n')) + QStringLiteral("\n\n") +
                           tr("If a file was moved or renamed, find it with Locate file… on it "
                              "in the Media tab. Or remove its clips from the timeline."));
    box.exec();
    return false;
}

bool MainWindow::confirmStalePrivacy(const QString &title, const QStringList &warnings)
{
    if (warnings.isEmpty())
        return true;
    QMessageBox box(QMessageBox::Warning, title,
                    tr("The privacy blur may be out of date and needs checking or redoing."),
                    QMessageBox::Cancel, this);
    box.setInformativeText(warnings.join(QStringLiteral("\n\n")));
    QPushButton *go = box.addButton(tr("Continue anyway"), QMessageBox::AcceptRole);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    return box.clickedButton() == go;
}

void MainWindow::pushBlur()
{
    m_privacy.pushTo(*m_controller, *m_timeline, m_showPlateRegions);
}

void MainWindow::showPictureMenu(const QPoint &globalPos, const std::array<float, 3> &dir)
{
    // The clip on screen and the frame shown (paused: the playhead's).
    const ClipItem *c = m_timeline->clipById(m_clipId);
    if (!c)
        return;
    const qint64 t = m_lastPositionMs;
    const int clipId = c->id;
    const PrivacyBlur::Hit hit = m_privacy.hitAt(*c, t, dir.data(), m_showPlateRegions);
    if (hit.added >= 0) {
        QMenu menu;
        auto edit = [this, clipId, ai = hit.added](qint64 fromMs) {
            const ClipItem *c = m_timeline->clipById(clipId);
            if (c && ai < int(c->blurAdditions.size()))
                m_timeline->setClipBlurAdditions(clipId, PrivacyBlur::cutAddition(*c, ai, fromMs));
        };
        menu.addAction(tr("Remove this blur"), this, [edit] { edit(INT64_MIN); })
            ->setToolTip(tr("A plate blurred by hand: drop it in every frame."));
        menu.addAction(tr("Remove this blur from here on"), this, [edit, t] { edit(t - 1); });
        menu.exec(globalPos);
        return;
    }
    if (!hit.region)
        return;
    const int32_t track = hit.track;
    QMenu menu;
    if (hit.removed) {
        menu.addAction(tr("Restore this blur"), this, [this, clipId, track, t] {
            const ClipItem *c = m_timeline->clipById(clipId);
            if (const auto keep = c ? m_privacy.withRestored(*c, track, t) : std::nullopt)
                m_timeline->setClipBlurRemovals(clipId, *keep);
        });
    } else {
        auto remove = [this, clipId, track](qint64 fromMs) {
            const ClipItem *c = m_timeline->clipById(clipId);
            const auto all = c ? m_privacy.withRemoved(*c, track, fromMs) : std::nullopt;
            if (!all)
                return;
            m_timeline->setClipBlurRemovals(clipId, *all);
            m_editToolbar->showNotice(tr("Blur removed. Right-click it with Show regions on to "
                                       "restore it, or undo."));
        };
        menu.addAction(tr("Remove this blur"), this, [remove] { remove(INT64_MIN); })
            ->setToolTip(tr("A false hit: drop this region in every frame of its track."));
        menu.addAction(tr("Remove this blur from here on"), this, [remove, t] { remove(t); });
    }
    menu.exec(globalPos);
}

void MainWindow::syncPrivacyPanel()
{
    const ClipItem *c = m_timeline->clipById(editClipId());
    m_privacyPanel->setNotApplicable(
        c && c->flat ? tr("The number-plate blur works on 360° clips only for now.") : QString());
    if (!c || c->flat) {
        m_privacyPanel->setNoClip();
        return;
    }
    const QString cantScan = syscheck::plateScanUnavailableReason();
#ifdef ORBIS_REFRAME_HAVE_PLATE_SCAN
    const bool canScan = cantScan.isEmpty() && !c->mediaMissing && !plateJobRunning();
#else
    const bool canScan = false;
#endif
    // What shows of it: the trim, and past it where a transition plays on.
    qint64 from = c->trimInMs, to = c->trimOutMs;
    m_timeline->shownSourceRange(c->id, from, to);
    QString status = m_privacy.status(*c, from, to);
    if (!cantScan.isEmpty())
        status += QLatin1Char(' ') + cantScan;
    m_privacyPanel->setState(c->blurPlates, status, canScan, static_cast<int>(c->blurRemovals.size()));
    m_privacyPanel->setWarning(m_privacy.staleness(*c, from, to, false));
}

#ifdef ORBIS_REFRAME_HAVE_PLATE_SCAN
platescan::Request MainWindow::plateRequestFor(const ClipItem &c)
{
    // The stitch as playback / export set it up for this clip.
    platescan::Request req;
    req.clipPath = c.sourcePath.toStdString();
    qint64 from = c.trimInMs, to = c.trimOutMs;
    m_timeline->shownSourceRange(c.id, from, to); // with a transition's footage past the trims
    req.fromMs = from;
    req.toMs = to;
    req.fps = c.fps;
    req.syncAdjustMs = std::isnan(c.syncAdjustMs) ? 0.0 : c.syncAdjustMs;
    seamAdjustFor(c.sourcePath, req.seamAdjustDeg);
    req.lensGuard = c.lensGuard;
    req.depthSeam = c.depthSeam;
    req.captureTiming = StabilizerCache::captureTimingOn(c);
    return req;
}
#endif

void MainWindow::setPlateSelecting(bool on)
{
#ifdef ORBIS_REFRAME_HAVE_PLATE_SCAN
    const ClipItem *c = m_timeline->clipById(m_clipId);
    if (on && (!c || c->flat || c->mediaMissing || plateJobRunning() || trackingRunning() ||
               m_exports->exporting()))
        on = false;
    if (on && m_trackSelecting)
        setTrackSelecting(false);
    m_plateSelecting = on;
    m_privacyPanel->setPlateState(on ? PrivacyPanel::PlateState::Selecting : PrivacyPanel::PlateState::Idle);
    m_viewport->SetBoxSelectMode(on);
    if (on) {
        // A still frame to draw on.
        QMetaObject::invokeMethod(m_controller, &PlaybackController::pause);
        m_viewport->ClearMarker();
        m_viewport->requestActivate(); // so Esc reaches it
        m_editToolbar->showNotice(tr("Drag a box around the plate to blur (Esc cancels). It is "
                                   "followed from this frame on, then back from it."),
                                true);
    } else {
        m_viewport->ClearMarker();
        m_editToolbar->showNotice(QString());
    }
#else
    Q_UNUSED(on);
#endif
}

void MainWindow::togglePlateBlur()
{
    if (plateFollowing())
        stopPlateJob();
    else
        setPlateSelecting(!m_plateSelecting);
}

void MainWindow::startPlateFollow(const QRectF &box)
{
#ifdef ORBIS_REFRAME_HAVE_PLATE_SCAN
    m_plateSelecting = false;
    m_privacyPanel->setPlateState(PrivacyPanel::PlateState::Idle);
    const ClipItem *c = m_timeline->clipById(m_clipId);
    if (!c || c->mediaMissing || plateJobRunning() || trackingRunning() || m_exports->exporting()) {
        m_viewport->ClearMarker();
        return;
    }
    const qint64 from = m_lastPositionMs, to = c->trimOutMs, back = c->trimInMs;
    if (to - from < 100 && from - back < 100) {
        m_viewport->ClearMarker();
        m_editToolbar->showNotice(tr("The clip is too short."));
        return;
    }
    // The box's corners on the sphere, through the frame on screen.
    insv::BlurQuad seed;
    const double xs[4] = {box.left(), box.right(), box.right(), box.left()};
    const double ys[4] = {box.top(), box.top(), box.bottom(), box.bottom()};
    for (int k = 0; k < 4; ++k)
        m_viewport->SphereDirAtFraction(xs[k], ys[k], seed.corner[k]);
    platescan::Request req = plateRequestFor(*c);
    req.fromMs = from;

    QMetaObject::invokeMethod(m_controller, &PlaybackController::pause);
    m_plateJobClipId = c->id;
    m_plateJobFromMs = from;
    m_plateJobs->startFollow(req, seed, back);
    m_privacyPanel->setPlateState(PrivacyPanel::PlateState::Running);
    m_privacyPanel->setScanStatus(tr("Following the plate…"));
    m_editToolbar->showNotice(tr("Following the plate. Stop following in the Privacy panel ends it."), true);
    syncPrivacyPanel();
#else
    Q_UNUSED(box);
#endif
}

#ifdef ORBIS_REFRAME_HAVE_PLATE_SCAN
void MainWindow::finishPlateFollow(const platescan::FollowResult &r)
{
    m_privacyPanel->setPlateState(PrivacyPanel::PlateState::Idle);
    m_viewport->ClearMarker();
    const auto &frames = r.follow.frames;
    core::log::Info("[plates] follow: %zu frames, %d detected%s%s%s\n", frames.size(),
                    r.follow.detections, r.follow.lost ? ", lost" : "",
                    r.lostBackward ? ", lost backwards" : "", r.stopped ? ", stopped" : "");
    syncPrivacyPanel();
    const int clipId = m_plateJobClipId;
    const qint64 from = m_plateJobFromMs;
    const int idx = m_timeline->indexOfId(clipId);
    auto settle = [this, idx](qint64 sourceMs) {
        if (idx >= 0)
            navigateTo(m_timeline->clipToTimeline(idx, m_timeline->clipAt(idx).localAtSourceMs(sourceMs)),
                       true);
    };
    // A failed backward pass keeps the forward one (and says so below).
    if (frames.empty() || idx < 0) {
        m_editToolbar->showNotice(QString());
        settle(from);
        if (!r.error.empty())
            QMessageBox::warning(this, tr("Blur a plate"),
                                 tr("Following the plate failed: %1").arg(QString::fromStdString(r.error)));
        return;
    }
    // The playhead first: the change re-shows the paused frame there --
    // where it was lost going forwards, else going backwards, else the
    // end.
    const qint64 first = frames.front().tMs, until = frames.back().tMs;
    const bool lostBackOnly = r.lostBackward && !r.follow.lost;
    settle(lostBackOnly ? first : until);
    std::vector<privacy::BlurAddition> all = m_timeline->clipAt(idx).blurAdditions;
    all.push_back(platescan::toBlurAddition(r));
    m_timeline->setClipBlurAdditions(clipId, all); // one undo step
    const QString secs = QString::number((until - first) / 1000.0, 'f', 1);
    const QString before = QString::number((from - first) / 1000.0, 'f', 1);
    if (r.follow.lost || r.lostBackward)
        m_editToolbar->showNotice(
            tr("Blurred the plate for %1 s (%2 s of it before the box), up to where it was lost "
               "-- the playhead is there. Blur a plate again to pick it up.")
                .arg(secs, before),
            true);
    else
        m_editToolbar->showNotice(tr("Blurred the plate for %1 s (%2 s of it before the box).")
                                    .arg(secs, before));
    if (!r.error.empty())
        QMessageBox::warning(this, tr("Blur a plate"),
                             tr("Following the plate failed part of the way: %1")
                                 .arg(QString::fromStdString(r.error)));
}
#endif

void MainWindow::startPlateScan()
{
#ifdef ORBIS_REFRAME_HAVE_PLATE_SCAN
    const ClipItem *c = m_timeline->clipById(editClipId());
    if (plateJobRunning() || !c || c->mediaMissing)
        return;
    if (m_plateSelecting)
        setPlateSelecting(false);
    m_plateJobSource = c->sourcePath;
    m_plateJobClipId = c->id;
    const platescan::Request req = plateRequestFor(*c);
    m_plateJobFromMs = req.fromMs;
    m_plateJobToMs = req.toMs;
    m_plateJobs->startScan(req);
    m_privacyPanel->setScanRunning(true);
    m_privacyPanel->setScanStatus(tr("Starting…"));
    syncPrivacyPanel();
#endif
}

#ifdef ORBIS_REFRAME_HAVE_PLATE_SCAN
void MainWindow::finishPlateScan(const platescan::Result &r)
{
    const QString source = m_plateJobSource;
    m_privacyPanel->setScanRunning(false);
    core::log::Info("[plates] %s: %zu frames, %d plates, %.1f fps on %s%s\n",
                    source.toUtf8().constData(), r.scan.frames.size(), r.scan.plates,
                    r.fpsAchieved, r.device.c_str(), r.stopped ? ", stopped" : "");
    if (!r.error.empty()) {
        syncPrivacyPanel();
        QMessageBox::warning(this, tr("Scan for plates"),
                             tr("The scan failed: %1").arg(QString::fromStdString(r.error)));
        return;
    }
    const ClipItem *c = m_timeline->clipById(m_plateJobClipId);
    if (!r.scan.empty() && (!c || c->sourcePath != source)) {
        core::log::Info("[plates] the scanned clip is gone; the scan is dropped\n");
    } else if (!r.scan.empty()) {
        // A new scan of the clip's own, from the one it had: a copy or a
        // split half that shared it keeps that one. Undo goes back to it.
        privacy::PlateScan scan;
        if (const PlateScanCache::Scan had = m_privacy.scanOf(*c))
            scan = *had;
        scan.merge(r.scan);
        const QString key = PlateScanCache::newKey();
        QString err;
        if (!m_privacy.scans().store(source, key, scan, &err))
            QMessageBox::warning(this, tr("Scan for plates"),
                                 tr("The scan couldn't be saved (%1); it holds until the "
                                    "app closes.")
                                     .arg(err));
        m_timeline->setClipPlateScan(c->id, key);
    }
    pushBlur();
    syncPrivacyPanel();
    if (!m_playing)
        QMetaObject::invokeMethod(m_controller, "seek", Qt::QueuedConnection,
                                  Q_ARG(qint64, m_lastTimelineMs));
    m_editToolbar->showNotice(r.stopped ? tr("Plate scan stopped; the part scanned is kept.")
                                      : tr("Plate scan done."));
}
#endif

bool MainWindow::plateJobRunning() const
{
#ifdef ORBIS_REFRAME_HAVE_PLATE_SCAN
    return m_plateJobs && m_plateJobs->running();
#else
    return false;
#endif
}

bool MainWindow::plateFollowing() const
{
#ifdef ORBIS_REFRAME_HAVE_PLATE_SCAN
    return m_plateJobs && m_plateJobs->following();
#else
    return false;
#endif
}

void MainWindow::stopPlateJob()
{
#ifdef ORBIS_REFRAME_HAVE_PLATE_SCAN
    if (m_plateJobs)
        m_plateJobs->stop();
#endif
}

void MainWindow::updateExportEnabled()
{
    m_exportAction->setEnabled(!m_exports->exporting() && m_timeline->count() > 0 && m_fps > 0.0);
}

void MainWindow::updateScreenshotEnabled()
{
    const bool on = !m_exports->savingStill() && !m_exports->exporting() &&
                    m_timeline->count() > 0 && m_fps > 0.0;
    m_screenshotAction->setEnabled(on);
    m_playerBar->setScreenshotEnabled(on);
    // While tracking, the button is the Stop control.
    m_reframePanel->setTrackEnabled(trackingRunning() ||
                                    (!m_exports->exporting() && m_timeline->count() > 0 && m_fps > 0.0));
}

bool MainWindow::trackingRunning() const
{
#ifdef ORBIS_REFRAME_HAVE_TRACKING
    return m_tracker && m_tracker->running();
#else
    return false;
#endif
}

void MainWindow::setTrackSelecting(bool on)
{
#ifdef ORBIS_REFRAME_HAVE_TRACKING
    if (on && m_plateSelecting)
        setPlateSelecting(false);
    const ClipItem *playing = m_timeline->clipById(m_clipId);
    if (on && (!playing || playing->flat || trackingRunning() || m_exports->exporting()))
        on = false; // a flat clip isn't reframed: nothing to track with
    m_trackSelecting = on;
    m_reframePanel->setTrackSelecting(on);
    m_viewport->SetBoxSelectMode(on);
    if (on) {
        // A still frame to draw on.
        QMetaObject::invokeMethod(m_controller, &PlaybackController::pause);
        m_viewport->ClearMarker();
        m_viewport->requestActivate(); // so Esc reaches it
        m_editToolbar->showNotice(tr("Drag a box around the subject to track (Esc cancels)."), true);
    } else {
        if (!trackingRunning())
            m_viewport->ClearMarker();
        m_editToolbar->showNotice(QString());
    }
#else
    Q_UNUSED(on);
#endif
}

void MainWindow::startSubjectTracking(const QRectF &box)
{
#ifdef ORBIS_REFRAME_HAVE_TRACKING
    m_trackSelecting = false;
    m_reframePanel->setTrackSelecting(false);
    const ClipItem *c = m_timeline->clipById(m_clipId);
    if (!c || trackingRunning() || m_exports->exporting()) {
        m_viewport->ClearMarker();
        return;
    }
    const qint64 from = m_lastPositionMs, to = c->trimOutMs;
    if (to - from < 500) {
        m_viewport->ClearMarker();
        m_editToolbar->showNotice(tr("Too close to the end of the clip to track."));
        return;
    }

    // The box -> the subject's direction and angular size, in the stabilized
    // frame the reframe keys live in (the viewport's pose is applied before
    // the stab matrix, so the pose alone maps the box).
    const float aspect = m_viewport->OutputAspect() > 0.0f
                             ? m_viewport->OutputAspect()
                             : float(m_viewport->width()) / std::max(1, m_viewport->height());
    const ReframePose look{m_viewport->Yaw(), m_viewport->Pitch(), m_viewport->Roll(),
                           m_viewport->FovY()};
    SubjectTrackController::Run run;
    subjecttrack::Request &req = run.req;
    req.seed = track::seedFromViewBox(box.x(), box.y(), box.width(), box.height(), look.yaw,
                                      look.pitch, look.roll, look.fovY, aspect);
    const ReframeExportController::ClipJob job = exportJobFor(*c); // the stab, as exported
    req.clipPath = c->sourcePath.toStdString();
    req.fromMs = from;
    req.toMs = to;
    req.fps = c->fps;
    req.stab = job.stab;
    req.seamAdjustDeg = job.seamAdjustDeg;
    req.blur = job.blur;
    req.lensGuard = job.lensGuard;
    run.clipId = c->id;
    run.look = look;
    run.aspect = aspect;

    // It plays what it tracks in the viewport instead: pause playback, which
    // would push frames too (and its own NativeStitchSource would contend).
    QMetaObject::invokeMethod(m_controller, &PlaybackController::pause);
    m_tracker->start(run);
    m_reframePanel->setTrackRunning(true);
    m_reframePanel->setTrackStatus(tr("Starting…"));
    m_editToolbar->showNotice(tr("Tracking. Space, Esc or Stop tracking ends it."), true);
    updateScreenshotEnabled();
#else
    Q_UNUSED(box);
#endif
}

#ifdef ORBIS_REFRAME_HAVE_TRACKING
void MainWindow::finishSubjectTracking(const subjecttrack::Result &r)
{
    const SubjectTrackController::Run &run = m_tracker->run();
    const int clipId = run.clipId;
    const qint64 from = run.req.fromMs;
    const ReframePose look = run.look;
    m_reframePanel->setTrackRunning(false);
    updateScreenshotEnabled();
    core::log::Info("[track] %zu frames, tracker %.1f fps%s%s\n", r.samples.size(),
                    r.fpsAchieved, r.endedLost ? ", ended lost" : "",
                    r.stopped ? ", stopped" : "");

    // Back to normal playback at the last frame tracked -- where to
    // track on from.
    const int idx = m_timeline->indexOfId(clipId);
    auto settle = [this, idx](qint64 sourceMs) {
        if (idx < 0)
            return;
        const ClipItem &clip = m_timeline->clipAt(idx);
        navigateTo(m_timeline->clipToTimeline(idx, clip.localAtSourceMs(sourceMs)), true);
    };
    if (!r.error.empty()) {
        m_editToolbar->showNotice(QString());
        settle(from);
        QMessageBox::warning(this, tr("Track subject"),
                             tr("Tracking failed: %1").arg(QString::fromStdString(r.error)));
        return;
    }
    if (r.samples.size() < 2) {
        settle(from);
        m_editToolbar->showNotice(
            tr("Lost the subject straight away. Try again with a box that fits it closely."));
        return;
    }
    const std::vector<track::PathKey> keys = track::reduceToKeys(r.samples, run.req.fps);
    QList<ReframeKey> list;
    for (const track::PathKey &k : keys)
        list.append(ReframeKey{k.tMs, {float(k.yaw), float(k.pitch), look.roll, look.fovY}});
    const qint64 until = r.samples.back().tMs;
    m_timeline->replaceClipReframeSpan(clipId, from, until, list); // one undo step
    settle(until);
    const QString secs = QString::number((until - from) / 1000.0, 'f', 1);
    if (r.endedLost)
        m_editToolbar->showNotice(
            tr("Tracked %1 s (%2 keyframes), up to where the subject was last seen -- "
               "the playhead is there to track on from.")
                .arg(secs)
                .arg(list.size()),
            true);
    else
        m_editToolbar->showNotice(
            tr("Tracked %1 s (%2 keyframes).").arg(secs).arg(list.size()));
}
#endif

void MainWindow::takeScreenshot()
{
    if (m_exports->savingStill() || m_exports->exporting())
        return;
    const ClipItem *c = m_timeline->clipById(m_clipId);
    if (!c)
        return;
    if (const QString why = m_privacy.blocker(*c, m_lastPositionMs, m_lastPositionMs); !why.isEmpty()) {
        QMessageBox::warning(this, tr("Screenshot"), why);
        return;
    }
    // In a transition, the other clip shows too: past its trim.
    for (const renderjobs::TransitionLayer &l : renderjobs::transitionLayers(*m_timeline)) {
        if (l.clipIndex < 0)
            continue; // black
        const ClipItem &o = m_timeline->clipAt(l.clipIndex);
        const qint64 end = l.startMs + std::llround((l.trimOutMs - l.trimInMs) / o.speed);
        if (m_lastTimelineMs < l.startMs || m_lastTimelineMs >= end)
            continue;
        const qint64 at = l.trimInMs + std::llround((m_lastTimelineMs - l.startMs) * o.speed);
        if (const QString why = m_privacy.blocker(o, at, at); !why.isEmpty()) {
            QMessageBox::warning(this, tr("Screenshot"), why);
            return;
        }
    }
    // A frame either side: the playhead's time is not the shown frame's.
    const qint64 frameMs = std::llround(1000.0 / c->fps);
    if (const QString w = m_privacy.staleness(*c, m_lastPositionMs - frameMs, m_lastPositionMs + frameMs, true);
        !confirmStalePrivacy(tr("Screenshot"), w.isEmpty() ? QStringList() : QStringList{w}))
        return;

    // Captured before the save dialog, so a playing timeline doesn't move the
    // frame on: the export's view of the clip, looked at through whatever the
    // viewport shows now -- the sampled reframe path, or a free look / drag.
    ExportRunner::Still still;
    still.job = exportJobFor(*c);
    still.pose = {m_viewport->Yaw(), m_viewport->Pitch(), m_viewport->Roll(), m_viewport->FovY()};
    still.sourceMs = m_lastPositionMs;
    still.timelineMs = std::max<qint64>(0, m_lastTimelineMs);
    still.width = m_project->output().width;
    still.height = m_project->output().height;

    QString path = askSaveFileName(
        tr("Save screenshot"),
        renderjobs::suggestedScreenshotPath(m_project->filePath(), c->sourcePath, still.timelineMs),
        tr("PNG image (*.png)"));
    if (path.isEmpty() || m_exports->savingStill() || m_exports->exporting())
        return;
    if (QFileInfo(path).suffix().isEmpty())
        path += QStringLiteral(".png");
    still.path = path;

    m_editToolbar->showNotice(tr("Saving screenshot…"), true);
    // The overlays showing at the playhead, composited like the export does.
    still.insets = renderjobs::insetJobs(*m_timeline, still.width, still.height, m_privacy);
    m_exports->startStill(std::move(still));
}
