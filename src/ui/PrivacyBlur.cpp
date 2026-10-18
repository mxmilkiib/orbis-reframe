#include "PrivacyBlur.h"

#include <QFileInfo>
#include <QHash>

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <memory>

#include "PlaybackController.h"
#include "StabilizerCache.h" // captureTimingOn
#include "Theme.h"
#include "../core/ClipItem.h"
#include "../core/Timeline.h"
#include "../render/EquirectViewportWindow.h" // RegionOutline

namespace {

// The clip's removed blur tracks as they fall in `scan` (null when none).
std::shared_ptr<const privacy::RemovedSpans> resolveRemovals(const ClipItem &c,
                                                              const privacy::PlateScan &scan)
{
    if (c.blurRemovals.empty())
        return {};
    auto r = std::make_shared<privacy::RemovedSpans>(scan.resolveRemovals(c.blurRemovals));
    return r->empty() ? nullptr : r;
}

} // namespace

PlateScanCache::Scan PrivacyBlur::scanOf(const ClipItem &c) const
{
    return m_scans.get(c.sourcePath, c.plateScan);
}

stitch::BlurQuadsFn PrivacyBlur::blurFor(const ClipItem &c) const
{
    PlateScanCache::Scan scan = c.blurPlates ? scanOf(c) : nullptr;
    if (!scan && c.blurAdditions.empty())
        return {};
    std::shared_ptr<const privacy::RemovedSpans> removed;
    if (scan)
        removed = resolveRemovals(c, *scan);
    const double fps = c.fps;
    return [scan, removed, added = c.blurAdditions, fps](int64_t ptsMs, std::vector<insv::BlurQuad> &out) {
        if (scan)
            scan->quadsAt(ptsMs, out, removed.get());
        privacy::addedQuadsAt(added, ptsMs, fps, out);
    };
}

void PrivacyBlur::pushTo(PlaybackController &controller, const Timeline &timeline,
                         bool showRegions) const
{
    // Per clip, as two clips of one file -- a split, a duplicate -- may
    // differ; the export takes the same blurFor.
    std::map<int, stitch::BlurQuadsFn> blurs;
    // "Show regions": the scan's regions of every clip with a scan, blur on or
    // not, in the privacy spike's debug colours (by RegionKind; the Privacy
    // panel's legend matches); the ones removed by hand in grey.
    struct Outlined {
        PlateScanCache::Scan scan;
        std::shared_ptr<const privacy::RemovedSpans> removed;
        std::shared_ptr<const std::vector<privacy::BlurAddition>> added; // null = none
        double fps = 30.0;
    };
    QHash<int, Outlined> outlined;
    for (int i = 0; i < timeline.count(); ++i) {
        const ClipItem &c = timeline.clipAt(i);
        if (stitch::BlurQuadsFn fn = blurFor(c))
            blurs.emplace(c.id, std::move(fn));
        if (!showRegions)
            continue;
        Outlined o;
        o.scan = scanOf(c);
        if (o.scan)
            o.removed = resolveRemovals(c, *o.scan);
        if (!c.blurAdditions.empty())
            o.added = std::make_shared<const std::vector<privacy::BlurAddition>>(c.blurAdditions);
        o.fps = c.fps;
        if (o.scan || o.added)
            outlined.insert(c.id, std::move(o));
    }
    controller.setBlurQuads(std::move(blurs));

    if (outlined.isEmpty()) {
        controller.setOutlineProvider({});
        return;
    }
    controller.setOutlineProvider(
        [outlined](int clipId, qint64 sourceMs,
                   std::vector<EquirectViewportWindow::RegionOutline> &out) {
            const auto r = outlined.find(clipId);
            if (r == outlined.end())
                return;
            // Detected, Filled, Tracked, Unverified; removed; added by hand.
            const auto rgb = [](const QColor &c) {
                return std::array<float, 3>{float(c.redF()), float(c.greenF()), float(c.blueF())};
            };
            const theme::Colors &tc = theme::colors();
            const std::array<float, 3> kColour[4] = {rgb(tc.plateDetected), rgb(tc.plateFilled),
                                                     rgb(tc.plateTracked), rgb(tc.plateUnverified)};
            const std::array<float, 3> kRemovedRgb = rgb(tc.plateRemoved);
            const std::array<float, 3> kAddedRgb = rgb(tc.plateAdded);
            const float *kRemoved = kRemovedRgb.data();
            const float *kAdded = kAddedRgb.data();
            if (r->added)
                for (const privacy::BlurAddition &a : *r->added)
                    if (const privacy::BlurAddition::Frame *af = privacy::addedFrameAt(a, sourceMs, r->fps)) {
                        EquirectViewportWindow::RegionOutline o;
                        std::memcpy(o.corner, af->corner, sizeof(o.corner));
                        std::copy_n(kAdded, 3, o.rgb);
                        out.push_back(o);
                    }
            const privacy::ScanFrame *f = r->scan ? r->scan->frameAt(sourceMs) : nullptr;
            if (!f)
                return;
            for (const privacy::BlurRegion &reg : f->regions) {
                EquirectViewportWindow::RegionOutline o;
                std::memcpy(o.corner, reg.quad.corner, sizeof(o.corner));
                const bool removed = privacy::PlateScan::isRemoved(reg, f->tMs, r->removed.get());
                std::copy_n(removed ? kRemoved : kColour[std::min<int>(static_cast<int>(reg.kind), 3)].data(), 3,
                            o.rgb);
                out.push_back(o);
            }
        });
}

QString PrivacyBlur::blocker(const ClipItem &c, qint64 fromMs, qint64 toMs) const
{
    if (!c.blurPlates)
        return {};
    const PlateScanCache::Scan scan = scanOf(c);
    if (scan && scan->covers(fromMs, toMs))
        return {};
    const QString name = QFileInfo(c.sourcePath).fileName();
    return scan ? tr("“%1” has the privacy blur on, but only part of it has been scanned for "
                     "plates. Scan the clip in the Privacy panel first.")
                      .arg(name)
                : tr("“%1” has the privacy blur on, but it hasn't been scanned for plates yet. "
                     "Scan the clip in the Privacy panel first.")
                      .arg(name);
}

QString PrivacyBlur::staleness(const ClipItem &c, qint64 fromMs, qint64 toMs, bool name) const
{
    const bool on = StabilizerCache::captureTimingOn(c);
    bool scanStale = false;
    if (c.blurPlates)
        if (const PlateScanCache::Scan scan = scanOf(c))
            scanStale = scan->staleOver(fromMs, toMs, on);
    int handStale = 0;
    for (const privacy::BlurAddition &a : c.blurAdditions)
        if (!a.frames.empty() && a.frames.back().tMs >= fromMs && a.frames.front().tMs <= toMs &&
            privacy::timingStale(a.timing, on))
            ++handStale;
    if (!scanStale && !handStale)
        return {};
    // Found with the correction = with the stabilization on.
    const QString then = on ? tr("off") : tr("on");
    QString what;
    if (scanStale && handStale)
        what = (handStale == 1
                    ? tr("The plate scan and 1 plate blurred by hand were found with the "
                         "stabilization %1")
                    : tr("The plate scan and %1 plates blurred by hand were found with the "
                         "stabilization %2")
                          .arg(handStale))
                   .arg(then);
    else if (scanStale)
        what = tr("The plate scan was made with the stabilization %1").arg(then);
    else
        what = (handStale == 1
                    ? tr("1 plate blurred by hand was followed with the stabilization %1")
                    : tr("%1 plates blurred by hand were followed with the stabilization %2")
                          .arg(handStale))
                   .arg(then);
    QString text = tr("%1, which stitches the picture slightly differently: the blurs may sit a "
                      "little off the plates in fast swings. Check them with \"Show regions\", "
                      "or redo them (scan again, blur the plate again) -- or set the "
                      "stabilization back %2.")
                       .arg(what, then);
    if (name)
        text = QStringLiteral("“%1”: ").arg(QFileInfo(c.sourcePath).fileName()) + text;
    return text;
}

QString PrivacyBlur::status(const ClipItem &c) const
{
    return status(c, c.trimInMs, c.trimOutMs);
}

QString PrivacyBlur::status(const ClipItem &c, qint64 fromMs, qint64 toMs) const
{
    const PlateScanCache::Scan scan = scanOf(c);
    const bool covered = scan && scan->covers(fromMs, toMs);
    QString status;
    if (covered) {
        int frames = 0, withPlates = 0;
        for (const privacy::ScanFrame &f : scan->frames)
            if (f.tMs >= fromMs && f.tMs < toMs) {
                ++frames;
                withPlates += f.regions.empty() ? 0 : 1;
            }
        status = withPlates ? tr("Scanned: plates on %1 of %2 frames.").arg(withPlates).arg(frames)
                            : tr("Scanned: no plates found.");
    } else if (scan && std::any_of(scan->spans.begin(), scan->spans.end(), [&](const auto &s) {
                   return s.toMs >= fromMs && s.fromMs <= toMs;
               })) {
        status = tr("Partly scanned. Scan the clip to cover all of it.");
    } else {
        status = tr("Not scanned yet.");
    }
    if (c.blurPlates && !covered)
        status += QLatin1Char(' ') + tr("Exports with the blur on are refused until it is.");
    if (!c.blurAdditions.empty())
        status += QLatin1Char(' ') + (c.blurAdditions.size() == 1
                                          ? tr("1 plate blurred by hand.")
                                          : tr("%1 plates blurred by hand.")
                                                .arg(c.blurAdditions.size()));
    return status;
}

PrivacyBlur::Hit PrivacyBlur::hitAt(const ClipItem &c, qint64 tMs, const float dir[3],
                                    bool showRegions) const
{
    Hit hit;
    // A plate blurred by hand (always shown; on top of the scan's).
    hit.added = privacy::addedAt(c.blurAdditions, tMs, c.fps, dir);
    if (hit.added >= 0)
        return hit;
    if (!c.blurPlates && !showRegions)
        return hit; // no blur and no outlines: nothing to point at
    const PlateScanCache::Scan scan = scanOf(c);
    const privacy::ScanFrame *f = scan ? scan->frameAt(tMs) : nullptr;
    if (!f)
        return hit;
    const auto removed = resolveRemovals(c, *scan);
    // The topmost region under the pointer; a removed one only while the
    // outlines show it.
    for (auto it = f->regions.rbegin(); it != f->regions.rend(); ++it) {
        const bool isRemoved = privacy::PlateScan::isRemoved(*it, f->tMs, removed.get());
        if (privacy::quadContains(it->quad, dir) && (showRegions || !isRemoved)) {
            hit.region = true;
            hit.track = it->track;
            hit.removed = isRemoved;
            break;
        }
    }
    return hit;
}

std::vector<privacy::BlurAddition> PrivacyBlur::cutAddition(const ClipItem &c, int index,
                                                            qint64 fromMs)
{
    std::vector<privacy::BlurAddition> all = c.blurAdditions;
    if (index < 0 || index >= int(all.size()))
        return all;
    auto &frames = all[index].frames;
    frames.erase(std::lower_bound(frames.begin(), frames.end(), fromMs,
                                  [](const privacy::BlurAddition::Frame &f, qint64 t) {
                                      return f.tMs < t;
                                  }),
                 frames.end());
    if (frames.empty())
        all.erase(all.begin() + index);
    return all;
}

std::optional<std::vector<privacy::BlurRemoval>>
PrivacyBlur::withRemoved(const ClipItem &c, int32_t track, qint64 fromMs) const
{
    const PlateScanCache::Scan scan = scanOf(c);
    if (!scan)
        return std::nullopt;
    privacy::BlurRemoval rm = scan->describeRemoval(track, fromMs);
    if (rm.path.empty())
        return std::nullopt;
    std::vector<privacy::BlurRemoval> all = c.blurRemovals;
    all.push_back(std::move(rm));
    return all;
}

std::optional<std::vector<privacy::BlurRemoval>>
PrivacyBlur::withRestored(const ClipItem &c, int32_t track, qint64 tMs) const
{
    const PlateScanCache::Scan scan = scanOf(c);
    if (!scan)
        return std::nullopt;
    // Drop every removal that takes this track away here.
    std::vector<privacy::BlurRemoval> keep;
    for (const privacy::BlurRemoval &rm : c.blurRemovals) {
        const privacy::RemovedSpans spans = scan->resolveRemovals({rm});
        if (std::none_of(spans.begin(), spans.end(), [track, tMs](const privacy::RemovedSpan &s) {
                return s.track == track && tMs >= s.fromMs && tMs <= s.toMs;
            }))
            keep.push_back(rm);
    }
    return keep;
}
