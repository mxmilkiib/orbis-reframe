#include "PlateScanner.h"

#include "DataPaths.h"
#include "PlateTracking.h"

#include <gpu.h> // ncnn (its target adds include/ncnn)
#include <net.h>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/video/tracking.hpp>

#include <algorithm>
#include <deque>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <map>
#include <numbers>
#include <tuple>

namespace privacy {

using namespace plates;

namespace {

constexpr const char* kModelFile = "yolo_v9_s_608_license_plates_heads.ncnn.param";

using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// --- the plate detector -----------------------------------------------------------
// The tiles go through the model as one mosaic: a kCols x kRows grid of
// kCell-pixel cells, a tile in the top-left of each and grey in the gap, so
// the network sees padding-like grey between unrelated tiles. One big
// inference keeps the GPU far busier than twelve small ones.
constexpr int kCols = 4, kRows = 3;

struct TileDet {
    cv::Rect2f box; // tile pixels
    float score;
};

// Greedy NMS over one tile's boxes.
std::vector<TileDet> nms(std::vector<TileDet> d, float iouThr)
{
    std::sort(d.begin(), d.end(), [](const TileDet& a, const TileDet& b) { return a.score > b.score; });
    std::vector<TileDet> out;
    for (const TileDet& x : d) {
        bool keep = true;
        for (const TileDet& o : out) {
            const float inter = (x.box & o.box).area();
            if (inter / (x.box.area() + o.box.area() - inter) > iouThr) {
                keep = false;
                break;
            }
        }
        if (keep)
            out.push_back(x);
    }
    return out;
}

// Tile box -> sphere box (levelled frame).
SphereBox toSphere(const View& v, const cv::Rect2f& b, float score)
{
    const double cxp = b.x + 0.5 * b.width, cyp = b.y + 0.5 * b.height;
    SphereBox s;
    lonLatOf(v.ray(cxp, cyp), s.lon, s.lat);
    s.w = std::acos(std::clamp(v.ray(b.x, cyp).dot(v.ray(b.x + b.width, cyp)), -1.0, 1.0));
    s.h = std::acos(std::clamp(v.ray(cxp, b.y).dot(v.ray(cxp, b.y + b.height)), -1.0, 1.0));
    s.score = score;
    return s;
}

// Overlapping tiles see the same plate twice: keep one box -- a whole one
// over one cut off by its tile's edge (that box sits off the plate's centre
// and can score higher: on the cars clip a cut box won by 0.003 and the
// region missed the plate), else the higher score.
std::vector<SphereBox> mergeDuplicates(std::vector<SphereBox> in)
{
    std::sort(in.begin(), in.end(), [](const SphereBox& a, const SphereBox& b) {
        return a.cut != b.cut ? !a.cut : a.score > b.score;
    });
    std::vector<SphereBox> out;
    for (const SphereBox& b : in) {
        bool dup = false;
        for (const SphereBox& o : out)
            if (angleBetween(b, o) < 0.5 * std::max(std::max(b.w, b.h), std::max(o.w, o.h))) {
                dup = true;
                break;
            }
        if (!dup)
            out.push_back(b);
    }
    return out;
}

// One YOLOv9 head (stride s): channels 0..63 = 4 sides x 16 DFL bins, 64 =
// the class logit; cells over the logit threshold decode to boxes in input
// pixels (the model's own decoding, cut off for any input size).
void decodeHead(const ncnn::Mat& m, int stride, float logitThr, std::vector<TileDet>& out)
{
    if (m.c < 65)
        return;
    const int W = m.w, H = m.h;
    const float* cls = m.channel(64);
    for (int gy = 0; gy < H; ++gy)
        for (int gx = 0; gx < W; ++gx) {
            const int i = gy * W + gx;
            if (cls[i] < logitThr)
                continue;
            float dist[4];
            for (int side = 0; side < 4; ++side) {
                float mx = -1e30f, v[16];
                for (int k = 0; k < 16; ++k) {
                    v[k] = static_cast<const float*>(m.channel(side * 16 + k))[i];
                    mx = std::max(mx, v[k]);
                }
                float sum = 0, acc = 0;
                for (int k = 0; k < 16; ++k) {
                    const float e = std::exp(v[k] - mx);
                    sum += e;
                    acc += e * k;
                }
                dist[side] = acc / sum;
            }
            const float ax = gx + 0.5f, ay = gy + 0.5f;
            const float x1 = (ax - dist[0]) * stride, y1 = (ay - dist[1]) * stride;
            const float x2 = (ax + dist[2]) * stride, y2 = (ay + dist[3]) * stride;
            out.push_back({cv::Rect2f(x1, y1, x2 - x1, y2 - y1), 1.0f / (1.0f + std::exp(-cls[i]))});
        }
}

struct Track {
    std::vector<std::pair<int, SphereBox>> obs; // frame index, box
};

// A quad region recorded for a track on a frame (levelled frame).
struct QuadRec {
    int track;
    Quad q;
    RegionKind kind;
};

struct FrameRec {
    int64_t tMs = 0;
    float M[9];
    std::vector<QuadRec> quads;
};

// A frame in flight: its tiles (the detector reads them on its own thread,
// hence shared) and the quad views rendered for it.
struct Pending {
    int64_t tMs = 0;
    float M[9];
    std::shared_ptr<std::vector<uint8_t>> tiles;
    std::map<int, std::pair<View, cv::Mat>> views; // track -> view, gray
};

} // namespace

std::string defaultModelParam()
{
    if (const char* env = std::getenv("ORBIS_PLATE_MODEL"); env && *env)
        return env;
    return core::paths::modelDir("plates") + "/" + kModelFile;
}

bool modelAvailable(const std::string& param)
{
    if (param.size() < 6 || param.compare(param.size() - 6, 6, ".param") != 0)
        return false;
    const std::string bin = param.substr(0, param.size() - 6) + ".bin";
    std::error_code ec;
    return std::filesystem::exists(param, ec) && std::filesystem::exists(bin, ec);
}

struct PlateScanner::Impl {
    ScanParams p;
    ncnn::Net net;
    int px = 608;   // tile size
    int cell = 640; // mosaic cell (tile + grey gap), a multiple of 32
    std::string device;
    std::vector<View> tiles;

    std::vector<FrameRec> frames;
    std::vector<Track> tracks;
    std::vector<QuadTracker> quads;
    std::map<int, SphereBox> lastDetected; // track -> box, on the last settled frame
    Stats stats;

    int canvasW() const { return kCols * cell; }
    int canvasH() const { return kRows * cell; }

    // The model on a W x H picture (RGBA8 rows `stride` pixels apart): the
    // boxes over `conf`, in its pixels, not NMS'd.
    std::vector<TileDet> infer(const uint8_t* rgba, int stride, int W, int H, float conf)
    {
        // RGBA8 -> the model's planar RGB in 0..1, in one pass over many
        // threads (ncnn's from_pixels + normalize is single-threaded: ~10 ms
        // for the mosaic).
        ncnn::Mat in(W, H, 3);
        cv::parallel_for_(cv::Range(0, H), [&](const cv::Range& r) {
            constexpr float k = 1.0f / 255.0f;
            for (int y = r.start; y < r.end; ++y) {
                const uint8_t* src = rgba + size_t(y) * stride * 4;
                float* pr = in.channel(0).row(y);
                float* pg = in.channel(1).row(y);
                float* pb = in.channel(2).row(y);
                for (int x = 0; x < W; ++x) {
                    pr[x] = src[4 * x] * k;
                    pg[x] = src[4 * x + 1] * k;
                    pb[x] = src[4 * x + 2] * k;
                }
            }
        });
        ncnn::Extractor ex = net.create_extractor();
        ex.input("in0", in);
        const float logitThr = std::log(conf / (1.0f - conf));
        std::vector<TileDet> all;
        const char* names[3] = {"out0", "out1", "out2"};
        for (int k = 0; k < 3; ++k) {
            ncnn::Mat out;
            ex.extract(names[k], out);
            decodeHead(out, 8 << k, logitThr, all);
        }
        return all;
    }

    std::vector<SphereBox> detect(const std::vector<uint8_t>& canvas)
    {
        const std::vector<TileDet> all = infer(canvas.data(), canvasW(), canvasW(), canvasH(), p.conf);
        // Back to the tiles: a box belongs to the tile its centre is in;
        // one that reaches past the tile is clipped to it.
        std::vector<std::vector<TileDet>> perTile(tiles.size());
        for (const TileDet& d : all) {
            const float cx = d.box.x + 0.5f * d.box.width, cy = d.box.y + 0.5f * d.box.height;
            const int col = static_cast<int>(cx) / cell, row = static_cast<int>(cy) / cell;
            if (cx < 0 || cy < 0 || col >= kCols || row >= kRows)
                continue;
            const cv::Rect2f tile(float(col * cell), float(row * cell), float(px), float(px));
            if (!tile.contains(cv::Point2f(cx, cy)))
                continue; // centre in the grey gap
            const int ti = row * kCols + col;
            if (ti >= static_cast<int>(tiles.size()))
                continue;
            cv::Rect2f b = d.box & tile;
            b.x -= tile.x;
            b.y -= tile.y;
            perTile[ti].push_back({b, d.score});
        }
        std::vector<SphereBox> boxes;
        for (size_t i = 0; i < perTile.size(); ++i)
            for (const TileDet& td : nms(std::move(perTile[i]), 0.45f)) {
                boxes.push_back(toSphere(tiles[i], td.box, td.score));
                // Cut off by the tile's edge: the neighbouring tile (they
                // overlap by 10 deg) sees the whole plate.
                const float e = 2.0f;
                boxes.back().cut = td.box.x < e || td.box.y < e || td.box.x + td.box.width > px - e ||
                                   td.box.y + td.box.height > px - e;
            }
        boxes = mergeDuplicates(std::move(boxes));
        if (p.maxBoxDeg > 0)
            boxes.erase(std::remove_if(boxes.begin(), boxes.end(),
                                       [&](const SphereBox& b) { return b.w * kRadToDeg > p.maxBoxDeg; }),
                        boxes.end());
        return boxes;
    }

    std::vector<ScanView> tileViews() const
    {
        std::vector<ScanView> v;
        for (size_t i = 0; i < tiles.size(); ++i) {
            ScanView s;
            s.lonDeg = tiles[i].lonDeg;
            s.latDeg = tiles[i].latDeg;
            s.hfovDeg = tiles[i].hfovDeg;
            s.x = static_cast<int>(i % kCols) * cell;
            s.y = static_cast<int>(i / kCols) * cell;
            s.w = s.h = px;
            v.push_back(s);
        }
        return v;
    }

    // The quad views for the next frame: every live quad's, and one around
    // each plate detected on the last settled frame that has none yet.
    bool renderQuadViews(FrameSource& src, Pending& pf, std::string& err)
    {
        pf.views.clear();
        std::vector<std::pair<int, View>> want;
        for (const QuadTracker& t : quads)
            if (t.holdLeft < 0)
                want.push_back({t.track, viewForQuad(t.q)});
        for (const auto& [tid, b] : lastDetected) {
            const bool has = std::any_of(want.begin(), want.end(), [tid = tid](const auto& w) { return w.first == tid; });
            if (!has)
                want.push_back({tid, viewForQuad(quadFromBox(b))});
        }
        if (want.empty())
            return true;
        std::vector<ScanView> sv;
        for (size_t i = 0; i < want.size(); ++i) {
            const View& v = want[i].second;
            ScanView s;
            s.lonDeg = v.lonDeg;
            s.latDeg = v.latDeg;
            s.hfovDeg = v.hfovDeg;
            s.x = 0;
            s.y = static_cast<int>(i) * kQuadPx;
            s.w = s.h = kQuadPx;
            sv.push_back(s);
        }
        std::vector<uint8_t> rgba;
        if (!src.render(kQuadPx, kQuadPx * static_cast<int>(want.size()), sv, rgba, err))
            return false;
        const cv::Mat all(kQuadPx * static_cast<int>(want.size()), kQuadPx, CV_8UC4, rgba.data());
        for (size_t i = 0; i < want.size(); ++i) {
            cv::Mat g;
            cv::cvtColor(all(cv::Rect(0, static_cast<int>(i) * kQuadPx, kQuadPx, kQuadPx)), g, cv::COLOR_RGBA2GRAY);
            pf.views[want[i].first] = {want[i].second, g};
        }
        return true;
    }

    // Link this frame's detections to the tracks (greedy by cost, constant
    // angular velocity prediction); returns each detection's track.
    std::vector<int> link(int f, const std::vector<SphereBox>& dets)
    {
        struct Cand {
            double cost;
            int d, t;
        };
        std::vector<Cand> cands;
        for (int t = 0; t < static_cast<int>(tracks.size()); ++t) {
            const auto& last = tracks[t].obs.back();
            const int gap = f - last.first;
            if (gap > p.maxGap + 1)
                continue;
            SphereBox pred = last.second;
            const bool hasVel = tracks[t].obs.size() >= 2;
            if (hasVel) {
                const auto& prev = tracks[t].obs[tracks[t].obs.size() - 2];
                const double dl = std::remainder(last.second.lon - prev.second.lon, 2 * kPi);
                const double k = double(gap) / (last.first - prev.first);
                pred.lon = last.second.lon + dl * k;
                pred.lat = last.second.lat + (last.second.lat - prev.second.lat) * k;
            }
            for (int d = 0; d < static_cast<int>(dets.size()); ++d) {
                const SphereBox& b = dets[d];
                // Allowed miss: one box size, plus what the plate can move in
                // `gap` frames -- up to ~1.5 deg/frame (a vehicle passing
                // close) before a velocity is known, ~0.3 deg/frame of
                // acceleration after.
                const double size = std::max({b.w, b.h, last.second.w, last.second.h});
                const double allowed = size + gap * (hasVel ? 0.3 : 1.5) * kDegToRad;
                const double cost = angleBetween(pred, b) / allowed;
                if (cost < 1.0)
                    cands.push_back({cost, d, t});
            }
        }
        std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.cost < b.cost; });
        std::vector<int> trackOf(dets.size(), -1);
        std::vector<char> tUsed(tracks.size(), 0);
        for (const Cand& c : cands) {
            if (trackOf[c.d] >= 0 || tUsed[c.t])
                continue;
            trackOf[c.d] = c.t;
            tUsed[c.t] = 1;
            tracks[c.t].obs.push_back({f, dets[c.d]});
        }
        for (int d = 0; d < static_cast<int>(dets.size()); ++d)
            if (trackOf[d] < 0) {
                trackOf[d] = static_cast<int>(tracks.size());
                tracks.push_back(Track{{{f, dets[d]}}});
            }
        return trackOf;
    }

    // While the detector works on `pf`: each live quad's provisional step.
    void stepQuads(const Pending& pf)
    {
        for (QuadTracker& t : quads) {
            t.stepOk = false;
            if (t.holdLeft >= 0)
                continue;
            if (const auto it = pf.views.find(t.track); it != pf.views.end())
                stepQuad(t, it->second.first, it->second.second);
        }
    }

    // The frame's detections are in: link them, anchor / commit / hold the
    // quads, record the frame.
    void settle(const Pending& pf, const std::vector<SphereBox>& boxes)
    {
        const int f = static_cast<int>(frames.size());
        frames.emplace_back();
        FrameRec& fr = frames.back();
        fr.tMs = pf.tMs;
        std::copy(pf.M, pf.M + 9, fr.M);
        stats.detections += static_cast<int>(boxes.size());

        const std::vector<int> trackOf = link(f, boxes);
        std::map<int, SphereBox> detOf;
        for (size_t i = 0; i < boxes.size(); ++i)
            detOf[trackOf[i]] = boxes[i];

        for (QuadTracker& t : quads) {
            const auto v = pf.views.find(t.track);
            if (const auto it = detOf.find(t.track); it != detOf.end()) {
                // Re-anchor in this frame's view; a box outside it drops the
                // quad (a new one starts from the next detection).
                if (v == pf.views.end() || !anchorQuad(t, it->second, v->second.first, v->second.second))
                    t.holdLeft = 0;
                continue;
            }
            // Lost -- the step failed, it stopped matching the plate, or it
            // ran too long without a detection: held where it was last seen,
            // growing, from this very frame.
            if (t.holdLeft < 0) {
                const int unverified = t.stepOk && t.stepNcc < p.quadNcc ? t.unverified + 1 : 0;
                if (!t.stepOk || v == pf.views.end() || unverified > p.quadMaxUnverified ||
                    t.sinceAnchor + 1 > p.quadMax) {
                    t.holdLeft = p.quadHold;
                } else {
                    t.q = t.stepQ;
                    t.view = v->second.first;
                    t.gray = v->second.second;
                    t.unverified = unverified;
                    ++t.sinceAnchor;
                    // Tight while verified; grows with time since the
                    // anchor and fast while unverified.
                    const double grow = std::min(1.15 + 0.01 * t.sinceAnchor, 1.4) + 0.15 * t.unverified;
                    fr.quads.push_back({t.track, growQuad(t.q, grow),
                                        t.unverified ? RegionKind::Unverified : RegionKind::Tracked});
                    continue;
                }
            }
            if (t.holdLeft > 0) {
                const int k = p.quadHold - t.holdLeft + 1;
                fr.quads.push_back({t.track, growQuad(t.q, 1.4 + 0.15 * k), RegionKind::Filled});
                --t.holdLeft;
            }
        }
        quads.erase(std::remove_if(quads.begin(), quads.end(), [](const QuadTracker& t) { return t.holdLeft == 0; }),
                    quads.end());
        // New quads: detected tracks without one, where a view was rendered
        // for them (seen on the frame before).
        for (const auto& [tid, b] : detOf) {
            if (std::any_of(quads.begin(), quads.end(), [tid = tid](const QuadTracker& q) { return q.track == tid; }))
                continue;
            const auto v = pf.views.find(tid);
            if (v == pf.views.end())
                continue;
            QuadTracker t;
            t.track = tid;
            if (anchorQuad(t, b, v->second.first, v->second.second))
                quads.push_back(std::move(t));
        }
        lastDetected = std::move(detOf);
        ++stats.frames;
        stats.tracks = static_cast<int>(tracks.size());
    }
};

PlateScanner::PlateScanner() : d(std::make_unique<Impl>()) {}
PlateScanner::~PlateScanner() = default;

const PlateScanner::Stats& PlateScanner::stats() const
{
    return d->stats;
}

std::string PlateScanner::device() const
{
    return d->device;
}

bool PlateScanner::init(const ScanParams& params, std::string& err)
{
    d->p = params;
    if (d->p.modelParam.empty())
        d->p.modelParam = defaultModelParam();
    const std::string& param = d->p.modelParam;
    if (!modelAvailable(param)) {
        err = "plate model not found: " + param;
        return false;
    }
    const bool gpu = d->p.gpu && ncnn::get_gpu_count() > 0;
    d->net.opt.use_vulkan_compute = gpu;
    d->net.opt.num_threads = 4;
    if (gpu)
        d->net.set_vulkan_device(0);
    const std::string bin = param.substr(0, param.size() - 6) + ".bin";
    if (d->net.load_param(param.c_str()) || d->net.load_model(bin.c_str())) {
        err = "ncnn could not load " + param;
        return false;
    }
    d->device = gpu ? std::string("Vulkan: ") + ncnn::get_gpu_info(0).device_name() : std::string("CPU");
    d->px = std::max(32, d->p.tilePx);
    // Grey gaps between the tiles: without them a plate near a tile's edge
    // went undetected for a few frames on the motorcycle clip.
    d->cell = (d->px + 32 + 31) / 32 * 32;
    const int nT = std::clamp(d->p.tiles, 1, kCols * kRows);
    for (int i = 0; i < nT; ++i)
        d->tiles.push_back(View::make(-180.0 + 360.0 * i / nT, d->p.tilePitchDeg, d->p.tileFovDeg, d->px, d->px));
    // Warm-up: the first inference builds the pipelines.
    d->detect(std::vector<uint8_t>(size_t(d->canvasW()) * d->canvasH() * 4, 114));
    return true;
}

bool PlateScanner::run(FrameSource& src, const std::atomic<bool>* stop,
                       const std::function<void(int64_t tMs)>& progress, std::string& err)
{
    Impl& m = *d;
    const auto wall0 = Clock::now();
    auto stopped = [stop] { return stop && stop->load(); };
    const std::vector<ScanView> tileViews = m.tileViews();

    // Frame -> tiles rendered (the source keeps the frame for the quad views).
    auto begin = [&](Pending& pf) -> int { // 1 = ok, 0 = end, -1 = error
        if (stopped())
            return 0;
        if (!src.next(pf.tMs, pf.M, err))
            return err.empty() ? 0 : -1;
        const auto t0 = Clock::now();
        pf.tiles = std::make_shared<std::vector<uint8_t>>();
        const bool ok = src.render(m.canvasW(), m.canvasH(), tileViews, *pf.tiles, err);
        m.stats.renderMs += msSince(t0);
        return ok ? 1 : -1;
    };
    auto quadViews = [&](Pending& pf) {
        const auto t0 = Clock::now();
        const bool ok = m.renderQuadViews(src, pf, err);
        src.release();
        m.stats.renderMs += msSince(t0);
        return ok;
    };
    auto launch = [&](const Pending& pf) {
        return std::async(std::launch::async, [&m, tiles = pf.tiles] {
            const auto t0 = Clock::now();
            std::vector<SphereBox> b = m.detect(*tiles);
            return std::make_pair(std::move(b), msSince(t0));
        });
    };

    // Two detections in flight: frame f+1's starts as soon as its tiles are
    // rendered, while f's is still running -- the GPU stays busy between
    // ncnn's layers.
    Pending cur, nxt;
    int r = begin(cur);
    if (r <= 0)
        return r == 0;
    if (!quadViews(cur))
        return false;
    auto fut = launch(cur);
    bool ok = true;
    for (;;) {
        const auto tq = Clock::now();
        m.stepQuads(cur);
        m.stats.quadMs += msSince(tq);
        const int rn = begin(nxt); // the next frame's tiles, while the detector works
        if (rn < 0)
            ok = false;
        decltype(fut) futNext;
        if (rn > 0)
            futNext = launch(nxt);
        const auto tw = Clock::now();
        auto [boxes, detMs] = fut.get();
        m.stats.waitMs += msSince(tw);
        m.stats.detectMs += detMs;
        const auto ts = Clock::now();
        m.settle(cur, boxes);
        m.stats.quadMs += msSince(ts);
        if (progress)
            progress(cur.tMs);
        if (rn <= 0)
            break;
        fut = std::move(futNext);
        if (!quadViews(nxt)) {
            fut.wait();
            ok = false;
            break;
        }
        std::swap(cur, nxt);
    }
    src.release();
    m.stats.wallMs += msSince(wall0);
    return ok;
}

bool PlateScanner::follow(FrameSource& src, const insv::BlurQuad& seed, const FollowParams& fp,
                          const std::atomic<bool>* stop,
                          const std::function<void(int64_t tMs, bool detected)>& progress, FollowResult& out,
                          std::string& err)
{
    Impl& m = *d;
    out = FollowResult{};
    const int tilePx = std::max(32, fp.tilePx / 32 * 32);
    const int canvasW = tilePx, canvasH = tilePx + kQuadPx;

    Quad cur;                 // the plate (levelled frame, not grown)
    std::deque<cv::Vec3d> path; // its centre on the last few frames, for the prediction
    QuadTracker t;
    bool tracking = false;
    int hold = -1, sinceLocated = 0; // frames since detected or quad-verified
    std::vector<uint8_t> canvas;
    for (int f = 0;; ++f) {
        if (stop && stop->load())
            break;
        int64_t tMs = 0;
        float M[9];
        if (!src.next(tMs, M, err)) {
            if (f == 0 && !err.empty())
                return false;
            err.clear();
            break;
        }
        if (f == 0) // the seed, into the levelled frame (M: levelled -> sphere)
            for (int k = 0; k < 4; ++k) {
                const float* s = seed.corner[k];
                cur[k] = cv::normalize(cv::Vec3d(M[0] * s[0] + M[3] * s[1] + M[6] * s[2],
                                                 M[1] * s[0] + M[4] * s[1] + M[7] * s[2],
                                                 M[2] * s[0] + M[5] * s[1] + M[8] * s[2]));
            }
        // Where the plate should be: moved on as it moved last frame.
        // Moved on at its mean speed over the last (up to) 4 frames, at most
        // 1.5 deg a frame: one frame's step was too noisy to carry it on
        // through a hold (stick swing, a re-detection on the plate's edge),
        // and a hold carried on at a bad step swept the blur across the view.
        Quad pred = cur;
        if (path.size() >= 2) {
            const size_t k = std::min<size_t>(path.size() - 1, 4);
            pred = rotateQuad(cur, path[path.size() - 1 - k], path.back(), 1.0 / k, 1.5 * kDegToRad);
        }
        const SphereBox pb = boxOfQuad(pred);
        const double fov = std::clamp(pb.w * kRadToDeg * tilePx / fp.platePx, fp.minTileFovDeg, fp.maxTileFovDeg);
        const View tile = View::make(pb.lon * kRadToDeg, pb.lat * kRadToDeg, fov, tilePx, tilePx);
        const View qv = viewForQuad(pred);
        std::vector<ScanView> sv(2);
        sv[0] = {tile.lonDeg, tile.latDeg, tile.hfovDeg, 0, 0, tilePx, tilePx};
        sv[1] = {qv.lonDeg, qv.latDeg, qv.hfovDeg, 0, tilePx, kQuadPx, kQuadPx};
        const auto t0 = Clock::now();
        if (!src.render(canvasW, canvasH, sv, canvas, err)) {
            src.release();
            return f > 0;
        }
        src.release();
        m.stats.renderMs += msSince(t0);
        cv::Mat gray;
        cv::cvtColor(cv::Mat(canvasH, canvasW, CV_8UC4, canvas.data())(cv::Rect(0, tilePx, kQuadPx, kQuadPx)), gray,
                     cv::COLOR_RGBA2GRAY);

        if (tracking && hold < 0)
            stepQuad(t, qv, gray);
        const auto td = Clock::now();
        std::vector<SphereBox> dets;
        for (const TileDet& x : nms(m.infer(canvas.data(), canvasW, tilePx, tilePx, fp.conf), 0.45f))
            dets.push_back(toSphere(tile, x.box, x.score));
        m.stats.detectMs += msSince(td);

        // The detection that is this plate: near the prediction and about its
        // size. On the first frame the user's box only says roughly where:
        // its centre inside the box, no wider than it. The gate widens with
        // the frames since the plate was last located -- detected, or the
        // quad tracker's step verified -- not just detected: while the quad
        // has it the prediction is sure (cars clip, 7:19.6, followed
        // backwards: a pickup's plate, quad-tracked for 3 frames, then hidden
        // by a car crossing in front, went over to that car's plate 1.4-1.8
        // deg away).
        const SphereBox* best = nullptr;
        double bestCost = 1e9;
        const double size = std::max(pb.w, pb.h);
        const bool quadSure = tracking && hold < 0 && t.stepOk && t.stepNcc >= m.p.quadNcc;
        for (const SphereBox& b : dets) {
            const double dist = angleBetween(b, pb);
            double allowed, cost;
            if (f == 0) {
                allowed = 0.5 * size;
                if (b.w > 1.2 * pb.w)
                    continue;
            } else {
                allowed = quadSure ? 0.75 * size
                                   : std::min(0.75 * size + 0.5 * (1 + sinceLocated) * kDegToRad, 10.0 * kDegToRad);
                if (b.w < 0.5 * pb.w || b.w > 2.0 * pb.w)
                    continue;
            }
            if (dist > allowed)
                continue;
            cost = dist / allowed - 0.5 * b.score;
            if (cost < bestCost) {
                bestCost = cost;
                best = &b;
            }
        }

        Quad region;
        RegionKind kind;
        if (best) {
            ++out.detections;
            tracking = anchorQuad(t, *best, qv, gray);
            cur = quadFromBox(*best);
            region = quadFromBox(*best, m.p.dilateW, m.p.dilateH);
            kind = RegionKind::Detected;
            hold = -1;
            sinceLocated = 0;
        } else if (f == 0) {
            // Nothing found in the box: trust it -- the user says it's a plate.
            tracking = anchorQuad(t, cur, pb.h / std::max(pb.w, 1e-6), qv, gray);
            region = growQuad(cur, 1.15);
            kind = RegionKind::Tracked;
        } else if (tracking && hold < 0 && t.stepOk &&
                   (t.stepNcc >= m.p.quadNcc || t.unverified + 1 <= m.p.quadMaxUnverified) &&
                   t.sinceAnchor + 1 <= fp.maxWithoutDetection) {
            t.unverified = t.stepNcc < m.p.quadNcc ? t.unverified + 1 : 0;
            t.q = t.stepQ;
            t.view = qv;
            t.gray = gray;
            ++t.sinceAnchor;
            cur = t.q;
            region = growQuad(cur, std::min(1.15 + 0.01 * t.sinceAnchor, 1.4) + 0.15 * t.unverified);
            kind = t.unverified ? RegionKind::Unverified : RegionKind::Tracked;
            sinceLocated = t.unverified ? sinceLocated + 1 : 0;
        } else {
            // Lost: carried on as it last moved, growing, while the detector
            // keeps looking there (a car crossing the view moves ~1 deg a
            // frame; held still, it was out of reach in a few frames).
            if (hold < 0)
                hold = fp.hold;
            tracking = false;
            if (hold == 0) {
                out.lost = true;
                break;
            }
            cur = pred;
            const int k = fp.hold - hold + 1;
            region = growQuad(cur, 1.4 + 0.15 * std::min(k, 5));
            kind = RegionKind::Filled;
            --hold;
            ++sinceLocated;
        }

        ScanFrame sf;
        sf.tMs = tMs;
        BlurRegion r;
        r.kind = kind;
        for (int k = 0; k < 4; ++k)
            for (int i = 0; i < 3; ++i)
                r.quad.corner[k][i] = static_cast<float>(M[i * 3] * region[k][0] + M[i * 3 + 1] * region[k][1] +
                                                         M[i * 3 + 2] * region[k][2]);
        sf.regions.push_back(r);
        out.frames.push_back(std::move(sf));
        ++m.stats.frames;

        path.push_back(centroidDir(cur));
        if (path.size() > 5)
            path.pop_front();
        if (progress)
            progress(tMs, best != nullptr);
    }
    src.release();
    return true;
}

PlateScan PlateScanner::finish(double fps)
{
    Impl& m = *d;
    PlateScan scan;
    scan.fps = fps;
    const std::string& mp = m.p.modelParam;
    scan.model = mp.substr(mp.find_last_of('/') + 1);
    const int n = static_cast<int>(m.frames.size());
    if (n == 0)
        return scan;
    scan.spans.push_back({m.frames.front().tMs, m.frames.back().tMs});

    // Levelled regions per frame first; mapped to the sphere at the end.
    std::vector<std::vector<std::tuple<Quad, RegionKind, int>>> regions(n); // quad, kind, track
    std::vector<std::map<int, std::pair<Quad, RegionKind>>> quadOf(n); // per frame: track -> quad
    for (int f = 0; f < n; ++f)
        for (const QuadRec& q : m.frames[f].quads)
            quadOf[f][q.track] = {q.q, q.kind};

    const double dw = m.p.dilateW, dh = m.p.dilateH;
    for (int ti = 0; ti < static_cast<int>(m.tracks.size()); ++ti) {
        const Track& t = m.tracks[ti];
        if (static_cast<int>(t.obs.size()) < m.p.minHits)
            continue;
        ++scan.plates;
        int lastQuad = -1;
        for (int f = t.obs.back().first; f < n; ++f)
            if (quadOf[f].count(ti))
                lastQuad = f;
        const int first = t.obs.front().first, last = t.obs.back().first;
        const int from = std::max(0, first - m.p.lead);
        const int to = std::min(n - 1, std::max(last + m.p.tail, lastQuad));
        size_t oi = 0; // the observation at or before f
        for (int f = from; f <= to; ++f) {
            while (oi + 1 < t.obs.size() && t.obs[oi + 1].first <= f)
                ++oi;
            const auto& ob = t.obs[oi];
            if (ob.first == f) { // detected
                regions[f].push_back({quadFromBox(ob.second, dw, dh), RegionKind::Detected, ti});
                continue;
            }
            if (const auto it = quadOf[f].find(ti); it != quadOf[f].end()) {
                regions[f].push_back({it->second.first, it->second.second, ti});
                continue;
            }
            if (f < first) { // lead: held at the first detection
                regions[f].push_back({quadFromBox(t.obs.front().second, dw, dh), RegionKind::Filled, ti});
            } else if (f > last) { // tail
                if (f <= last + m.p.tail)
                    regions[f].push_back({quadFromBox(t.obs.back().second, dw, dh), RegionKind::Filled, ti});
            } else { // between detections ob and the next
                const auto& nx = t.obs[oi + 1];
                if (nx.first - ob.first - 1 <= m.p.maxGap)
                    regions[f].push_back(
                        {quadFromBox(lerpBox(ob.second, nx.second, double(f - ob.first) / (nx.first - ob.first)),
                                     dw, dh),
                         RegionKind::Filled, ti});
            }
        }
    }

    scan.frames.resize(n);
    for (int f = 0; f < n; ++f) {
        ScanFrame& sf = scan.frames[f];
        sf.tMs = m.frames[f].tMs;
        const float* M = m.frames[f].M;
        for (const auto& [q, kind, track] : regions[f]) {
            BlurRegion r;
            r.kind = kind;
            r.track = track;
            for (int k = 0; k < 4; ++k)
                for (int i = 0; i < 3; ++i)
                    r.quad.corner[k][i] =
                        static_cast<float>(M[i * 3] * q[k][0] + M[i * 3 + 1] * q[k][1] + M[i * 3 + 2] * q[k][2]);
            sf.regions.push_back(r);
        }
    }
    return scan;
}

} // namespace privacy
