#include "ClipViewSource.h"

#include <NativeStitchSource.h>
#include <ReframeRenderer.h> // exportpipe::RenderRectilinear

#include <algorithm>
#include <utility>

ClipViewSource::ClipViewSource(Spec spec) : m_spec(std::move(spec)) {}

ClipViewSource::~ClipViewSource() = default;

bool ClipViewSource::open(std::string &err)
{
    const ReframeExportController::ClipJob &job = m_spec.job;
    if (!job.stab && m_spec.buildStab.mode != insv::StabMode::Off)
        m_spec.job.stab = ReframeExportController::buildStabilizer(job.clipPath, m_spec.fps,
                                                                   m_spec.buildStab, m_spec.syncMs);
    stitch::PreviewStitchConfig sc;
    sc.width = m_spec.stitchWidth & ~1;
    sc.height = sc.width / 2 & ~1;
    sc.lensGuard = job.lensGuard;
    sc.depthSeam = job.depthSeam;
    m_src = std::make_unique<stitch::NativeStitchSource>();
    if (!m_src->Open(job.clipPath.toStdString(), sc)) {
        err = m_src->GetLastError();
        m_src.reset();
        return false;
    }
    m_src->SetCaptureTiming(ReframeExportController::captureTimingFor(job.stab));
    m_src->SetLens1Adjust(job.seamAdjustDeg.data());
    if (job.blur && !m_src->SetBlurQuads(job.blur)) {
        err = "this stitch can't apply the privacy blur";
        m_src.reset();
        return false;
    }
    if (m_src->GetInfo().fps > 0.0)
        m_fps = m_src->GetInfo().fps;
    return true;
}

void ClipViewSource::setOutputSize(int width, int height)
{
    m_w = std::max(4, width & ~3); // the fused pass's needs
    m_h = std::max(2, height & ~1);
}

bool ClipViewSource::read(exportpipe::VideoFrame &out)
{
    if (!m_src)
        return false;
    const ReframeExportController::ClipJob &job = m_spec.job;
    // The view and stabilization at the frame's own time, as the export.
    auto poseAt = [&job](int64_t ptsMs, float pose[4], float stab[9]) {
        if (job.poseAt)
            job.poseAt(ptsMs, pose);
        if (job.stab) {
            float m[3][3];
            job.stab->correctionMatrix(ptsMs / 1000.0, m);
            std::copy(&m[0][0], &m[0][0] + 9, stab);
        }
    };
    stitch::Frame f;
    if (m_src->GpuViewAvailable()) {
        f = m_src->ReadViewFrame(m_w, m_h, poseAt, &job.colorGrade);
    } else {
        const stitch::Frame eq = m_src->ReadFrame();
        if (!eq.rgba.empty()) {
            exportpipe::ReframeView view;
            float pose[4] = {view.yaw, view.pitch, view.roll, view.fovY};
            poseAt(eq.timestamp_ms, pose, view.stab);
            view.yaw = pose[0];
            view.pitch = pose[1];
            view.roll = pose[2];
            view.fovY = pose[3];
            view.aspect = float(m_w) / m_h;
            view.colorGrade = job.colorGrade;
            f.rgba.resize(size_t(m_w) * m_h * 4);
            exportpipe::RenderRectilinear(eq.rgba.data(), eq.width, eq.height, f.rgba.data(), m_w,
                                          m_h, view);
            f.width = m_w;
            f.height = m_h;
            f.timestamp_ms = eq.timestamp_ms;
        }
    }
    if (f.rgba.empty()) {
        m_error = m_src->GetLastError(); // empty at the end of the clip
        return false;
    }
    out.ptsMs = f.timestamp_ms;
    out.width = f.width;
    out.height = f.height;
    out.rgba = std::move(f.rgba);
    return true;
}

const exportpipe::VideoFrame *ClipViewSource::frameAt(int64_t ms)
{
    if (!m_src)
        return nullptr;
    const double halfFrame = 500.0 / m_fps;
    // Behind the frame shown, or well ahead of it: jump there.
    if (!m_haveCur || ms + halfFrame < m_cur.ptsMs || ms > m_cur.ptsMs + 1000) {
        m_haveCur = m_haveNext = false;
        if (!m_src->SeekTo(ms)) {
            m_error = m_src->GetLastError();
            return nullptr;
        }
        if (!read(m_cur))
            return nullptr;
        m_haveCur = true;
        return &m_cur;
    }
    // Else on: the frame whose time window [pts - half, pts + half) holds
    // `ms`, as the export conforms the main clip's.
    for (;;) {
        if (!m_haveNext) {
            if (!read(m_next))
                break; // the end: the last one stays
            m_haveNext = true;
        }
        if (m_next.ptsMs - halfFrame > ms)
            break;
        std::swap(m_cur, m_next);
        m_haveNext = false;
    }
    return &m_cur;
}

bool ClipViewSource::readNext(exportpipe::VideoFrame &out)
{
    if (m_haveNext) {
        out = std::move(m_next);
        m_haveNext = false;
        return true;
    }
    return read(out);
}
