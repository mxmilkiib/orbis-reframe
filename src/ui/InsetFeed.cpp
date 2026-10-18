#include "InsetFeed.h"

#include <FlatVideoReader.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

// Frames decoded ahead of the playhead per clip while playing.
constexpr size_t kAhead = 4;
// Decode pictures no bigger than this on the long side: an inset rarely
// covers more of the view, and it keeps the per-frame upload small.
constexpr int kMaxSide = 1280;
// A flat main-track clip fills the whole view: as large as a full-HD view.
constexpr int kFullFrameMaxSide = 1920;
// Start decoding a clip this long before it appears (the first frame then
// shows on time), and let go of its decoder this long after.
constexpr qint64 kPrerollMs = 1500;
constexpr qint64 kLingerMs = 500;
// While playing, how far the clock may run past the newest decoded frame
// before the decode is re-anchored (a seek) rather than waited for.
constexpr qint64 kBehindMs = 500;
// How far past its span a transition's layer still shows over its main
// clip (Clip::overMainClipId): the main picture lagging the clock.
constexpr qint64 kOverMainSlackMs = 250;

} // namespace

struct InsetFeed::Stream {
    Clip clip; // guarded by m_mx
    // --- guarded by m_mx ---
    std::deque<Frame> queue; // front = showing (or next to)
    bool eof = false;
    bool needAnchor = true;
    uint64_t stillKey = 0; // a title's picture's frame key
    uint64_t gen = 0; // bumped on every re-anchor request
    bool failed = false;
    bool removed = false;
    Frame shown;
    bool haveShown = false;
    double frameMs = 0.0; // set when the reader opens
    // --- decoder thread only ---
    std::unique_ptr<exportpipe::FlatVideoReader> reader;
    std::unique_ptr<exportpipe::PictureSource> picture; // instead, for a 360 view
    QString openedPath; // or the picture's key
    bool open() const { return reader || picture; }
    void close()
    {
        reader.reset();
        picture.reset();
    }
};

InsetFeed::InsetFeed()
{
    m_thread = std::thread([this] { decoderLoop(); });
}

InsetFeed::~InsetFeed()
{
    {
        std::lock_guard<std::mutex> lk(m_mx);
        m_stop = true;
    }
    m_cv.notify_all();
    if (m_thread.joinable())
        m_thread.join();
}

void InsetFeed::setClips(std::vector<Clip> clips)
{
    {
        std::lock_guard<std::mutex> lk(m_mx);
        std::vector<std::shared_ptr<Stream>> next;
        for (Clip &c : clips) {
            std::shared_ptr<Stream> s;
            for (const auto &o : m_streams)
                if (o->clip.id == c.id)
                    s = o;
            if (!s) {
                s = std::make_shared<Stream>();
            } else if (!s->clip.sameFrames(c)) {
                // Trimmed / moved / re-sourced: the queued frames are wrong.
                s->queue.clear();
                s->eof = false;
                s->needAnchor = true;
                ++s->gen;
                if (s->clip.path != c.path || s->clip.pictureKey != c.pictureKey)
                    s->failed = false;
            }
            if (c.still && (s->stillKey == 0 || s->clip.still != c.still))
                s->stillKey = m_nextKey++; // a new picture: uploaded again
            s->clip = std::move(c);
            next.push_back(std::move(s));
        }
        for (const auto &o : m_streams)
            if (std::find(next.begin(), next.end(), o) == next.end())
                o->removed = true;
        m_streams = std::move(next);
    }
    m_cv.notify_all();
}

bool InsetFeed::active(const Clip &c, qint64 ms) const
{
    // (A title has nothing to decode.)
    return !c.still && ms >= c.startMs - kPrerollMs && ms < c.endMs() + kLingerMs;
}

std::vector<EquirectViewportWindow::InsetDraw> InsetFeed::frameFor(qint64 clockMs, bool playing,
                                                                   int mainClipId)
{
    std::vector<EquirectViewportWindow::InsetDraw> out;
    bool wake = false;
    {
        std::lock_guard<std::mutex> lk(m_mx);
        wake = clockMs != m_targetMs || playing != m_playing;
        m_targetMs = clockMs;
        m_playing = playing;
        for (const auto &sp : m_streams) {
            Stream &s = *sp;
            const Clip &c = s.clip;
            qint64 ms = clockMs;
            if (c.overMainClipId) {
                if (c.overMainClipId != mainClipId || ms < c.startMs - kOverMainSlackMs ||
                    ms >= c.endMs() + kOverMainSlackMs) {
                    s.haveShown = false;
                    continue;
                }
                ms = std::clamp(ms, c.startMs, std::max(c.startMs, c.endMs() - 1));
            }
            if (ms < c.startMs || ms >= c.endMs()) {
                s.haveShown = false;
                continue;
            }
            if (c.still) {
                EquirectViewportWindow::InsetDraw d;
                d.clipId = c.id;
                d.frameKey = s.stillKey;
                d.rgba = c.still;
                d.width = c.stillW;
                d.height = c.stillH;
                d.look = c.keys.isEmpty() ? c.look : c.keys.sample(c.sourceAt(ms));
                d.look.width = float(d.look.width * c.stillWidthPerSize);
                d.look.opacity *= inset::fadeGain(ms - c.startMs, c.spanMs(), c.fadeInMs, c.fadeOutMs) *
                                  inset::rampGain(ms - c.startMs, c.spanMs(), c.rampFrom, c.rampTo);
                d.aspect = c.fullFrame ? c.aspect : double(c.stillW) / std::max(1, c.stillH);
                d.locked = c.fullFrame;
                out.push_back(std::move(d));
                continue;
            }
            const size_t before = s.queue.size();
            while (s.queue.size() >= 2 && s.queue[1].timelineMs <= ms)
                s.queue.pop_front();
            wake |= s.queue.size() != before;
            // Does the queue hold the frame for `ms`? The first frame of a
            // clip may start a hair after the clip on the timeline.
            const double tol = s.frameMs / 2.0;
            bool covered = false;
            if (!s.queue.empty() && s.queue.front().timelineMs <= ms + tol) {
                const qint64 slack = playing ? kBehindMs : std::llround(s.frameMs * 1.5);
                covered = s.queue.size() >= 2 || s.eof || ms < s.queue.front().timelineMs + slack;
            }
            if (!covered && !s.needAnchor && !s.failed) {
                s.queue.clear();
                s.eof = false;
                s.needAnchor = true;
                ++s.gen;
                wake = true;
            }
            if (covered) {
                s.shown = s.queue.front();
                s.haveShown = true;
            }
            if (!s.haveShown)
                continue; // nothing decoded for it yet
            const qint64 local = ms - c.startMs;
            EquirectViewportWindow::InsetDraw d;
            d.clipId = c.id;
            d.frameKey = s.shown.key;
            d.rgba = s.shown.rgba;
            d.width = s.shown.width;
            d.height = s.shown.height;
            d.look = c.keys.isEmpty() ? c.look : c.keys.sample(c.sourceAt(ms));
            d.look.opacity *= inset::fadeGain(local, c.spanMs(), c.fadeInMs, c.fadeOutMs) *
                              inset::rampGain(local, c.spanMs(), c.rampFrom, c.rampTo);
            d.frame = c.frame;
            d.aspect = c.aspect;
            d.colorGrade = c.colorGrade;
            d.locked = c.fullFrame;
            out.push_back(std::move(d));
        }
    }
    if (wake)
        m_cv.notify_all();
    // Too many at once: the topmost win.
    if (out.size() > size_t(EquirectViewportWindow::kMaxInsets))
        out.erase(out.begin(), out.end() - EquirectViewportWindow::kMaxInsets);
    return out;
}

bool InsetFeed::work(Stream &s, qint64 target, bool playing)
{
    Clip clip;
    uint64_t gen = 0;
    bool anchor = false;
    {
        std::lock_guard<std::mutex> lk(m_mx);
        clip = s.clip;
        gen = s.gen;
        anchor = s.needAnchor;
        if (s.removed || s.failed || !active(clip, target))
            return false;
        if (!anchor && (!playing || s.eof || s.queue.size() >= kAhead))
            return false;
    }

    // A 360 clip's view: open (or re-open, if what it renders changed) its
    // picture source, at the size of a full-frame picture of its shape.
    if (clip.picture && (!s.picture || s.openedPath != clip.pictureKey)) {
        s.close();
        s.picture = clip.picture();
        s.openedPath = clip.pictureKey;
        std::string err;
        if (!s.picture || !s.picture->open(err)) {
            std::fprintf(stderr, "[inset] %s: %s\n", qPrintable(clip.path), err.c_str());
            s.picture.reset();
            std::lock_guard<std::mutex> lk(m_mx);
            s.failed = true;
            return true;
        }
        const double aspect = std::max(0.1, clip.aspect);
        const int w = aspect >= 1.0 ? kFullFrameMaxSide : int(std::lround(kFullFrameMaxSide * aspect));
        s.picture->setOutputSize(std::max(4, w & ~3),
                                 std::max(2, int(std::lround(w / aspect)) & ~1));
        std::lock_guard<std::mutex> lk(m_mx);
        s.frameMs = 1000.0 / std::max(1.0, s.picture->fps());
    }
    // Open (or re-open, if the clip was re-sourced) the reader.
    if (!clip.picture && (!s.reader || s.openedPath != clip.path)) {
        s.close();
        s.reader = std::make_unique<exportpipe::FlatVideoReader>();
        s.openedPath = clip.path;
        std::string err;
        if (!s.reader->open(clip.path.toStdString(), err)) {
            std::fprintf(stderr, "[inset] %s\n", err.c_str());
            s.reader.reset();
            std::lock_guard<std::mutex> lk(m_mx);
            s.failed = true;
            return true;
        }
        const auto &info = s.reader->info();
        const double scale = std::min(1.0, double(clip.fullFrame ? kFullFrameMaxSide : kMaxSide) /
                                               std::max(info.width, info.height));
        s.reader->setOutputSize(std::max(2, int(std::lround(info.width * scale)) & ~1),
                                std::max(2, int(std::lround(info.height * scale)) & ~1));
        std::lock_guard<std::mutex> lk(m_mx);
        s.frameMs = 1000.0 / info.fps;
    }

    Frame f;
    bool got = false;
    bool ended = false;
    if (anchor) {
        const qint64 at = std::clamp(target, clip.startMs, std::max(clip.startMs, clip.endMs() - 1));
        const qint64 srcMs = clip.sourceAt(at);
        if (const exportpipe::VideoFrame *v = s.picture ? s.picture->frameAt(srcMs)
                                                        : s.reader->frameAt(srcMs)) {
            f.timelineMs = clip.timelineAt(v->ptsMs);
            f.width = v->width;
            f.height = v->height;
            f.rgba = std::make_shared<const std::vector<uint8_t>>(v->rgba);
            got = true;
        } else {
            ended = true;
        }
    } else {
        exportpipe::VideoFrame v;
        const bool read = s.picture ? s.picture->readNext(v) : s.reader->readNext(v);
        if (read && clip.timelineAt(v.ptsMs) < clip.endMs()) {
            f.timelineMs = clip.timelineAt(v.ptsMs);
            f.width = v.width;
            f.height = v.height;
            f.rgba = std::make_shared<const std::vector<uint8_t>>(std::move(v.rgba));
            got = true;
        } else {
            ended = true;
        }
    }

    std::lock_guard<std::mutex> lk(m_mx);
    if (s.gen != gen || s.removed)
        return true; // re-anchored or removed meanwhile: drop it
    if (anchor) {
        s.needAnchor = false;
        s.queue.clear();
        s.eof = ended;
    } else if (ended) {
        s.eof = true;
    }
    if (got) {
        f.key = m_nextKey++;
        s.queue.push_back(std::move(f));
    }
    return true;
}

void InsetFeed::decoderLoop()
{
    for (;;) {
        std::vector<std::shared_ptr<Stream>> streams;
        qint64 target = 0;
        bool playing = false;
        {
            std::unique_lock<std::mutex> lk(m_mx);
            m_cv.wait(lk, [this] {
                if (m_stop)
                    return true;
                for (const auto &s : m_streams) {
                    const bool live = !s->removed && !s->failed && active(s->clip, m_targetMs);
                    if (!live && s->open())
                        return true; // a decoder to let go of
                    if (live && (s->needAnchor ||
                                 (m_playing && !s->eof && s->queue.size() < kAhead)))
                        return true;
                }
                return false;
            });
            if (m_stop)
                return;
            streams = m_streams;
            target = m_targetMs;
            playing = m_playing;
        }
        // Re-anchors first (a seek wants its frame now), then top up the
        // queues. One step each per pass, so no clip starves another.
        bool did = false;
        for (const auto &s : streams) {
            bool live = false;
            {
                std::lock_guard<std::mutex> lk(m_mx);
                live = !s->removed && !s->failed && active(s->clip, target);
                if (!live && s->open()) {
                    s->queue.clear();
                    s->needAnchor = true;
                    ++s->gen;
                }
            }
            if (!live) {
                s->close(); // free the decoder
                continue;
            }
            did |= work(*s, target, playing);
        }
        if (!did)
            std::this_thread::yield();
    }
}
