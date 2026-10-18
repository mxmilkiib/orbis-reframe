#pragma once

#include <QCoreApplication>
#include <QString>

#include <cstdint>
#include <optional>
#include <vector>

#include <IStitchSource.h> // stitch::BlurQuadsFn

#include "PlateScanCache.h"

struct ClipItem;
class PlaybackController;
class Timeline;

// The privacy blur's data side: the plate scans (each clip's, on disk)
// and each clip's blur -- "Blur number plates" with its removed tracks, and
// the plates blurred by hand -- resolved against them, for playback, the
// export, the Privacy panel and the picture's right-click menu. No widgets:
// MainWindow runs the scans and follows, and shows what this says.
class PrivacyBlur {
    Q_DECLARE_TR_FUNCTIONS(PrivacyBlur)

public:
    PlateScanCache &scans() { return m_scans; }

    // Clip `c`'s blur for playback and the export: the scan's plates (with
    // "Blur number plates" on) and the ones blurred by hand either way; empty
    // when none.
    stitch::BlurQuadsFn blurFor(const ClipItem &c) const;
    // Hand playback every timeline clip's blur (blurFor, per clip id, as two
    // clips of one file may differ) and, with `showRegions`, the outlines
    // "Show regions" draws.
    void pushTo(PlaybackController &controller, const Timeline &timeline, bool showRegions) const;

    // Why `c` can't be exported blurred over [fromMs, toMs] (its blur is on
    // but the scan doesn't cover it); empty = it can.
    QString blocker(const ClipItem &c, qint64 fromMs, qint64 toMs) const;
    // A warning when `c`'s blurs over [fromMs, toMs] were found in a stitch
    // other than today's, so they may sit off the plates; empty when they
    // match. `name` = start with the clip's file name.
    QString staleness(const ClipItem &c, qint64 fromMs, qint64 toMs, bool name) const;
    // The Privacy panel's status line for `c`: how much of its source range
    // [fromMs, toMs] (its trim, else) is scanned, and the plates blurred by
    // hand.
    QString status(const ClipItem &c, qint64 fromMs, qint64 toMs) const;
    QString status(const ClipItem &c) const;

    // What the picture shows at `dir` (sphere frame) in `c`'s frame at
    // `tMs`: a plate blurred by hand (its index in c.blurAdditions), else a
    // scan region -- one removed by hand only while `showRegions` shows it.
    struct Hit {
        int added = -1;      // >= 0: a hand-blurred plate
        bool region = false; // a scan region: its track, and whether it's removed
        int32_t track = 0;
        bool removed = false;
    };
    Hit hitAt(const ClipItem &c, qint64 tMs, const float dir[3], bool showRegions) const;
    // c.blurAdditions with addition `index` cut from `fromMs` on (dropped
    // when nothing is left of it).
    static std::vector<privacy::BlurAddition> cutAddition(const ClipItem &c, int index,
                                                          qint64 fromMs);
    // c.blurRemovals with `track` removed from `fromMs` to its end, or
    // without every removal that takes `track` away at `tMs`; nullopt when
    // the source has no scan (or the track no region there).
    std::optional<std::vector<privacy::BlurRemoval>> withRemoved(const ClipItem &c, int32_t track,
                                                                 qint64 fromMs) const;
    std::optional<std::vector<privacy::BlurRemoval>> withRestored(const ClipItem &c,
                                                                  int32_t track,
                                                                  qint64 tMs) const;

    // Clip `c`'s plate scan (ClipItem::plateScan); null when it has none.
    PlateScanCache::Scan scanOf(const ClipItem &c) const;

private:
    mutable PlateScanCache m_scans; // reads lazily
};
