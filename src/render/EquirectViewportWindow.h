#pragma once

#include <QColor>

// Interactive rectilinear ("flat") viewport onto an equirectangular 360
// frame (or a decoded fisheye pair, SetLensFrame). See
// src/render/shaders/equirect.frag for the reprojection math.
//
// Frames are pushed in from outside rather than pulled from an IStitchSource
// here -- keeps rendering independent of the stitch layer.
// The playback clock (src/ui/PlaybackController) pushes them on a timer.

#include <QMutex>
#include <QRectF>
#include <QVulkanWindow>

#include "ColorLut.h" // color::Grade
#include "LensFrame.h"
#include "../core/InsetGeometry.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

// A presented frame's own view and colour grade (its clip's reframe path at
// its source time, and its clip's grade), applied with the picture in the
// same render -- set from the clock instead, a cut would show one frame of a
// clip through the next one's view. (EquirectViewportWindow::FrameView.)
struct ViewportFrameView {
    bool hasPose = false; // else the view stays (free look, a drag)
    float pose[4] = {0.0f, 0.0f, 0.0f, 1.309f}; // yaw, pitch, roll, fovY
    bool hasGrade = false;
    color::Grade grade; // ColorGrade::render()
};

class EquirectViewportWindow : public QVulkanWindow {
    Q_OBJECT

public:
    QVulkanWindowRenderer *createRenderer() override;

    // Outlines drawn over the picture, on the sphere: the Privacy panel's
    // "Show regions" (privacy blur regions of the frame shown). Corners TL,
    // TR, BR, BL as unit directions in the equirect (sphere) frame -- the
    // frame insv::BlurQuad uses -- so they follow look-around and the
    // stabilization like the picture does. Edges are great-circle arcs,
    // straight in the rectilinear view.
    struct RegionOutline {
        float corner[4][3] = {};
        float rgb[3] = {1, 1, 1};
    };

    // Thread-safe: may be called from a decode thread. Takes effect on the
    // next rendered frame. rgba must be width*height*4 bytes. `stab`
    // (row-major 3x3, optional) is that frame's own gyro de-rotation: it
    // replaces the SetStabilization() matrix in the same rendered frame the
    // new picture first appears in, so picture and correction never drift
    // apart by a frame (at the stick mount's ~11 Hz shake that would undo
    // most of the stabilization). `outlines` are that frame's region
    // outlines; they replace the shown ones with the picture.
    // A presented frame's own view and colour grade (ViewportFrameView).
    using FrameView = ViewportFrameView;
    void SetFrame(int width, int height, std::vector<uint8_t> rgba, const float stab[9] = nullptr,
                  std::vector<RegionOutline> outlines = {}, const FrameView &view = {});

    // Show a decoded fisheye pair instead of an equirect -- the
    // viewport then renders the view straight from the lenses every frame
    // (full detail; look-around re-renders the resident pair). Thread-safe,
    // like SetFrame(); whichever of the two was called last wins. `stab` as
    // for SetFrame(). Only valid while LensFramesSupported().
    void SetLensFrame(std::shared_ptr<const render::LensFrame> frame,
                      const float stab[9] = nullptr, std::vector<RegionOutline> outlines = {},
                      const FrameView &view = {});
    // Whether this viewport can import lens frames (its Vulkan device has
    // external-memory FD import on the same GPU). False until the renderer
    // has initialised, and after a failed import. Thread-safe.
    bool LensFramesSupported() const { return m_lensSupported.load(); }
    void setLensFramesSupported(bool on) { m_lensSupported.store(on); } // renderer

    // Nothing to show (an empty timeline): back to the blank picture a new
    // window starts with, without overlays, outlines, marker, stabilization
    // or colour grade. Thread-safe, like SetFrame(); not counted in
    // FramesHanded() / FramesTaken().
    void ClearPicture();

    // Called by the renderer once per frame to pick up a pending SetFrame() /
    // SetLensFrame() call, if any. Returns false if nothing new is pending;
    // else exactly one of `rgba` (with width/height) or `lens` is filled.
    bool TakePendingFrame(int &width, int &height, std::vector<uint8_t> &rgba,
                          std::shared_ptr<const render::LensFrame> &lens);
    // How many frames have been handed in (SetFrame / SetLensFrame), and how
    // many of them the renderer has taken (one replaced before a render
    // isn't). Thread-safe; the smoke test reads them.
    int FramesHanded() const { return m_framesHanded.load(); }
    int FramesTaken() const { return m_framesTaken.load(); }
    // The shown frame's region outlines. Renderer thread (set by
    // TakePendingFrame).
    const std::vector<RegionOutline> &Outlines() const { return m_outlines; }

    // The video tracks' picture-in-picture insets to draw over the view,
    // bottom-most first (at most kMaxInsets). Thread-safe; replaces the
    // previous list. A picture is uploaded only when its frameKey changes.
    // Four overlays plus a flat main-track clip under them.
    static constexpr int kMaxInsets = 5;
    struct InsetDraw {
        int clipId = 0;
        uint64_t frameKey = 0; // new for every decoded picture
        std::shared_ptr<const std::vector<uint8_t>> rgba; // width * height * 4
        int width = 0;
        int height = 0;
        inset::Look look; // fades applied to the opacity
        inset::Frame frame;
        double aspect = 16.0 / 9.0; // the video's display aspect (sizes the rect)
        color::Grade colorGrade; // ColorGrade::render(); a flat main clip's
        bool locked = false; // a flat main clip: fills its place, can't be picked or dragged
    };
    void SetInsets(std::vector<InsetDraw> insets);
    // SetInsets for the insets that go with the picture last set (SetFrame /
    // SetLensFrame): while that picture waits for the renderer, the renderer
    // keeps drawing the insets it had, and takes these with the picture --
    // both change in the same render. A transition's layer must change on
    // the same frame as the main picture under it, or that frame shows one
    // clip blended with itself. (Insets() has them at once.)
    void SetFrameInsets(std::vector<InsetDraw> insets);
    // The list to draw, with an in-progress drag applied.
    std::vector<InsetDraw> Insets() const;
    // Insets() as the renderer draws them: held back while SetFrameInsets'
    // picture hasn't been taken (TakePendingFrame). Renderer thread.
    std::vector<InsetDraw> RenderInsets();

    float Yaw() const { return m_yaw; }
    float Pitch() const { return m_pitch; }
    float Roll() const { return m_roll; }
    float FovY() const { return m_fovY; }

    // Set the camera pose directly. Used by MainWindow to drive the viewport
    // along the authored reframe path during playback / scrubbing, and by the
    // Reframe panel's numeric fields. Plain float writes, matching how the
    // mouse handlers already mutate these while the render thread reads them.
    void SetPose(float yaw, float pitch, float roll, float fovY);

    // A 3x3 (row-major) gyro de-rotation in the sphere frame, which the
    // shader applies to the world ray *after* the yaw/pitch/roll pose -- so it
    // stabilizes the picture without touching Yaw()/Pitch()/Roll() (drag +
    // keyframe capture stay on the un-stabilized pose). Identity = no
    // stabilization. Plain float writes.
    void SetStabilization(const float rowMajor3x3[9]);
    void StabRowMajor(float out[9]) const;

    // Colour panel: the LUT and the four adjustments; nullptr = neutral.
    // Under a lock (the LUT is shared-owned): any thread sets, the renderer
    // reads.
    void SetColorGrade(const color::Grade *grade);
    color::Grade CurrentColorGrade() const;
    // For the renderer: SetSurroundColor's value.
    const float *SurroundColor() const { return m_surround; }

    // The output frame's width / height. The picture is letterboxed /
    // pillarboxed to this aspect inside the window, and the reprojection uses
    // it instead of the window's, so resizing the window never changes what's
    // framed. <= 0 = fill the window. Plain float write.
    void SetOutputAspect(float aspect) { m_outputAspect = aspect; }
    // The theme's colours this window paints with: around the picture, and
    // the marker box while drawing it / tracking / lost (MarkerStyle order).
    void SetSurroundColor(const QColor &c);
    void SetMarkerColors(const QColor &select, const QColor &tracking, const QColor &lost);
    float OutputAspect() const { return m_outputAspect; }

    // The unit direction in the sphere frame (RegionOutline's) that the
    // picture shows at output-rect fractions (fx, fy) -- the shown frame's
    // pose and stabilization. "Blur a plate…" maps its box with it.
    void SphereDirAtFraction(double fx, double fy, float out[3]) const;

    // Track subject / "Blur a plate…": while on, a left drag draws a box (instead of looking
    // around) and releasing emits boxSelected with it -- fractions of the
    // output rect, {x, y, w, h}, top left 0,0 -- and leaves the mode; Esc or
    // a click without a drag emits boxSelectCanceled. The box stays drawn
    // (the marker) until ClearMarker().
    void SetBoxSelectMode(bool on);
    bool BoxSelectMode() const { return m_boxSelect; }
    // How the marker is drawn: the box being drawn, a subject being tracked,
    // a subject lost.
    enum class MarkerStyle { Select, Tracking, Lost };
    void SetMarker(const QRectF &fractions, MarkerStyle style = MarkerStyle::Select);
    void ClearMarker();
    // The marker as output-rect fractions {x, y, w, h}; false when none.
    // Called by the renderer.
    bool Marker(float out[4], MarkerStyle *style = nullptr) const;
    // For the renderer: SetMarkerColors' colour for `style`.
    const float *MarkerColor(MarkerStyle style) const
    {
        return m_markerColors[static_cast<int>(style)];
    }

    // An equirect frame plus the view to show it through and (optionally) the
    // marker, all applied together when the renderer takes the frame -- Track
    // subject's live playback moves the camera with the subject frame by
    // frame, so pose and box must change with the picture, not a render early
    // or late. Thread-safe like SetFrame.
    struct FrameOverlay {
        float pose[4] = {0.0f, 0.0f, 0.0f, 1.309f}; // yaw, pitch, roll, fovY
        bool hasMarker = false;
        QRectF marker; // output-rect fractions
        MarkerStyle markerStyle = MarkerStyle::Tracking;
    };
    void SetFrame(int width, int height, std::vector<uint8_t> rgba, const float stab[9],
                  const FrameOverlay &overlay);
    // Done with overlaid frames: a still-pending one keeps its picture but no
    // longer moves the view or draws its marker, and the marker goes.
    void EndFrameOverlays();

signals:
    // The viewport has started Vulkan (its device and pipelines exist) --
    // physicalDeviceProperties() and supportedDeviceExtensions() are valid.
    // Again after a device loss.
    void vulkanReady();
    // The user is (true) / is no longer (false) dragging the view. MainWindow
    // suppresses reframe-path pose updates while a drag is in progress so the
    // authored path doesn't fight the user's hand.
    void draggingChanged(bool active);
    // A single look gesture finished with the pose actually changed: a
    // mouse-drag release, or a wheel-zoom notch. MainWindow turns this into a
    // reframe keyframe at the playhead when reframe authoring is armed.
    void lookCommitted();
    // A press on an inset (it is being picked / dragged).
    void insetPressed(int clipId);
    // An inset was dragged to a new centre (fractions of the output frame).
    void insetMoved(int clipId, float centerX, float centerY);
    void boxSelected(const QRectF &fractions);
    void boxSelectCanceled();
    // A right click (no drag) on the picture: `globalPos` for a context menu,
    // `dir` the unit direction under it in the sphere frame -- the frame
    // RegionOutline / insv::BlurQuad use (the Privacy panel's "Remove this
    // blur").
    void pictureContextMenu(const QPoint &globalPos, const std::array<float, 3> &dir);
    // A left double-click that isn't drawing a box or on an inset: full
    // screen on / off.
    void doubleClicked();

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    // The output rect inside the window, in window (logical) coordinates --
    // the same letterbox the renderer draws.
    QRectF outputRect() const;
    // The unit direction in the sphere frame that the picture shows at `pos`
    // (window coordinates) -- equirect.frag's ray. False outside the output
    // rect.
    bool sphereDirAt(QPointF pos, float out[3]) const;
    // The topmost inset under `pos` (window coordinates), or -1.
    int insetAt(QPointF pos) const;

    QMutex m_frameMutex;
    std::vector<uint8_t> m_pendingRgba;
    std::shared_ptr<const render::LensFrame> m_pendingLens; // set => a lens frame is pending
    std::atomic<bool> m_lensSupported{false};
    std::atomic<int> m_framesHanded{0};
    std::atomic<int> m_framesTaken{0};
    int m_pendingWidth = 0;
    int m_pendingHeight = 0;
    bool m_framePending = false;
    bool m_pendingIsClear = false; // ClearPicture()'s blank, not a decoded frame

    bool m_dragging = false;
    QPoint m_lastMousePos;
    // Pose at the start of the current drag, to tell a real look change from a
    // bare click on release.
    float m_pressYaw = 0.0f;
    float m_pressPitch = 0.0f;
    Qt::MouseButton m_dragButton = Qt::NoButton;

    float m_yaw = 0.0f;
    float m_pitch = 0.0f;
    float m_roll = 0.0f;   // set only via SetPose (numeric panel / reframe path)
    float m_fovY = 1.309f; // ~75 degrees, a reasonable default rectilinear FOV
    // Row-major 3x3, identity by default (no stabilization).
    float m_stab[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    float m_pendingStab[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}; // with the pending frame, guarded by m_frameMutex
    bool m_pendingHasStab = false;
    std::vector<RegionOutline> m_pendingOutlines; // with the pending frame, guarded by m_frameMutex
    std::vector<RegionOutline> m_outlines;        // the shown frame's; renderer thread
    mutable QMutex m_gradeMutex;
    color::Grade m_colorGrade; // guarded by m_gradeMutex
    float m_surround[3] = {0, 0, 0};
    float m_markerColors[3][3] = {{1.0f, 0.8f, 0.1f}, {0.2f, 0.9f, 0.3f}, {1.0f, 0.25f, 0.2f}};
    float m_outputAspect = 16.0f / 9.0f;   // project output width / height

    // Insets, guarded by m_insetMutex.
    mutable QMutex m_insetMutex;
    std::vector<InsetDraw> m_insets;
    // A drag in progress (or just released, until the model's new position
    // comes back through SetInsets): that clip's centre, overridden.
    int m_overrideClipId = 0;
    float m_overrideCx = 0.0f, m_overrideCy = 0.0f;
    bool m_clearOverride = false; // released: drop it once SetInsets brings the new position
    int m_clearOverrideTicks = 0;
    // GUI thread only.
    int m_insetDragClipId = 0;
    QPointF m_insetDragStart;
    float m_insetDragCx0 = 0.0f, m_insetDragCy0 = 0.0f;
    bool m_insetDragMoved = false;

    // Track subject box select (GUI thread) and the marker (read by the renderer).
    bool m_boxSelect = false;
    bool m_boxDragging = false;
    QPointF m_boxStart;
    bool m_hasMarker = false;
    QRectF m_marker; // output-rect fractions, guarded by m_insetMutex
    MarkerStyle m_markerStyle = MarkerStyle::Select;
    bool m_pendingHasOverlay = false; // with the pending frame, guarded by m_frameMutex
    FrameOverlay m_pendingOverlay;
    FrameView m_pendingView; // with the pending frame, guarded by m_frameMutex
    bool m_insetsHeld = false;          // RenderInsets, guarded by m_insetMutex
    std::vector<InsetDraw> m_renderInsets; // ... and what it drew last
};
