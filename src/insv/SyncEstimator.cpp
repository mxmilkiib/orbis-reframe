// SPDX-License-Identifier: MIT OR Apache-2.0
#include "SyncEstimator.h"

#include "GyroStabilizer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>

#include <opencv2/features2d.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/video/tracking.hpp>

namespace insv {

namespace {

constexpr double kPi = std::numbers::pi;
const double kNaN = std::numeric_limits<double>::quiet_NaN();

using Quat = std::array<double, 4>; // w, x, y, z; body -> world

// v' = q v q*
void rotate(const Quat& q, const double v[3], double out[3])
{
    const double w = q[0], x = q[1], y = q[2], z = q[3];
    const double m[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)},
                            {2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)},
                            {2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)}};
    for (int r = 0; r < 3; ++r)
        out[r] = m[r][0] * v[0] + m[r][1] * v[1] + m[r][2] * v[2];
}

void rotateInv(const Quat& q, const double v[3], double out[3])
{
    rotate({q[0], -q[1], -q[2], -q[3]}, v, out);
}

// Equirect convention of the stitch: dir = (cos lat sin lon, sin lat, -cos lat cos lon).
void pixelToDir(double x, double y, int w, int h, double d[3])
{
    const double lon = ((x + 0.5) / w - 0.5) * 2.0 * kPi;
    const double lat = (0.5 - (y + 0.5) / h) * kPi;
    d[0] = std::cos(lat) * std::sin(lon);
    d[1] = std::sin(lat);
    d[2] = -std::cos(lat) * std::cos(lon);
}

void dirToPixel(const double d[3], int w, int h, double& x, double& y)
{
    const double lon = std::atan2(d[0], -d[2]);
    const double lat = std::asin(std::clamp(d[1], -1.0, 1.0));
    x = (lon / (2.0 * kPi) + 0.5) * w - 0.5;
    y = (0.5 - lat / kPi) * h - 0.5;
}

// Remove a centred moving average (NaN-aware) from a series in place.
void highPass(std::vector<double>& s, int win)
{
    const int n = static_cast<int>(s.size());
    std::vector<double> out(n, kNaN);
    const int half = win / 2;
    for (int i = 0; i < n; ++i) {
        if (std::isnan(s[i]))
            continue;
        double sum = 0.0;
        int cnt = 0;
        for (int j = std::max(0, i - half); j <= std::min(n - 1, i + half); ++j)
            if (!std::isnan(s[j])) {
                sum += s[j];
                ++cnt;
            }
        out[i] = s[i] - sum / cnt;
    }
    s.swap(out);
}

} // namespace

struct SyncEstimator::Impl {
    Params p;
    int w = 0, h = 0;
    struct Interval {
        double t0 = 0.0, t1 = 0.0;
        std::vector<float> flow; // cellsX*cellsY*2, NaN where unmeasured
    };
    std::vector<std::vector<Interval>> windows;
    cv::Mat prev;
    double prevT = 0.0;
    bool havePrev = false;
};

SyncEstimator::SyncEstimator() : SyncEstimator(Params{}) {}
SyncEstimator::SyncEstimator(const Params& params) : m(std::make_unique<Impl>()) { m->p = params; }
SyncEstimator::~SyncEstimator() = default;

void SyncEstimator::beginWindow()
{
    m->windows.emplace_back();
    m->havePrev = false;
}

void SyncEstimator::addFrame(const uint8_t* gray, int w, int h, double tSec)
{
    if (m->windows.empty())
        beginWindow();
    const cv::Mat cur = cv::Mat(h, w, CV_8UC1, const_cast<uint8_t*>(gray)).clone();
    if (!m->havePrev || w != m->w || h != m->h) {
        m->w = w;
        m->h = h;
        m->prev = cur;
        m->prevT = tSec;
        m->havePrev = true;
        return;
    }
    const Params& p = m->p;
    // Features away from the caps and the longitude wrap.
    cv::Mat mask = cv::Mat::zeros(h, w, CV_8UC1);
    const int capPx = static_cast<int>(h * (0.5 - p.latMaxDeg / 180.0));
    const int edgePx = w / 50;
    mask(cv::Rect(edgePx, capPx, w - 2 * edgePx, h - 2 * capPx)).setTo(255);
    std::vector<cv::Point2f> p0, p1, pb;
    cv::goodFeaturesToTrack(m->prev, p0, p.maxFeatures, 0.003, 5.0, mask);

    const int ncell = p.cellsX * p.cellsY;
    Impl::Interval iv;
    iv.t0 = m->prevT;
    iv.t1 = tSec;
    iv.flow.assign(static_cast<size_t>(ncell) * 2, std::numeric_limits<float>::quiet_NaN());
    if (p0.size() >= 20) {
        std::vector<uint8_t> st, st2;
        std::vector<float> err;
        const cv::Size win(21, 21);
        cv::calcOpticalFlowPyrLK(m->prev, cur, p0, p1, st, err, win, 3);
        cv::calcOpticalFlowPyrLK(cur, m->prev, p1, pb, st2, err, win, 3);
        std::vector<std::vector<float>> fx(ncell), fy(ncell);
        for (size_t i = 0; i < p0.size(); ++i) {
            if (!st[i] || !st2[i] || cv::norm(pb[i] - p0[i]) > 0.3)
                continue;
            const int cx = std::clamp(static_cast<int>(p0[i].x / w * p.cellsX), 0, p.cellsX - 1);
            const int cy = std::clamp(static_cast<int>(p0[i].y / h * p.cellsY), 0, p.cellsY - 1);
            fx[cy * p.cellsX + cx].push_back(p1[i].x - p0[i].x);
            fy[cy * p.cellsX + cx].push_back(p1[i].y - p0[i].y);
        }
        for (int c = 0; c < ncell; ++c) {
            if (static_cast<int>(fx[c].size()) < p.minFeaturesPerCell)
                continue;
            auto median = [](std::vector<float>& v) {
                std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
                return v[v.size() / 2];
            };
            iv.flow[c * 2 + 0] = median(fx[c]);
            iv.flow[c * 2 + 1] = median(fy[c]);
        }
    }
    m->windows.back().push_back(std::move(iv));
    m->prev = cur;
    m->prevT = tSec;
}

SyncEstimator::Result SyncEstimator::estimate(const GyroStabilizer& gyro, double readoutSec,
                                              const RowFn& rowOf) const
{
    Result res;
    const Params& p = m->p;
    const int w = m->w, h = m->h;
    if (w <= 0 || !gyro.ready())
        return res;
    const int ncell = p.cellsX * p.cellsY;
    const int nstep = static_cast<int>(std::lround(2.0 * p.searchMs / p.stepMs)) + 1;

    // Per-cell centre direction, pixel and row time offset.
    struct Cell {
        double d[3];
        double x, y;
        double rowSec;
        bool ok;
    };
    std::vector<Cell> cells(ncell);
    for (int c = 0; c < ncell; ++c) {
        Cell& cl = cells[c];
        cl.x = (c % p.cellsX + 0.5) / p.cellsX * w - 0.5;
        cl.y = (c / p.cellsX + 0.5) / p.cellsY * h - 0.5;
        pixelToDir(cl.x, cl.y, w, h, cl.d);
        double row = 0.5;
        cl.ok = std::fabs(std::asin(cl.d[1])) < p.latMaxDeg * kPi / 180.0 && rowOf && rowOf(cl.d, row);
        cl.rowSec = (row - 0.5) * readoutSec;
    }

    // Measured flow per cell, high-passed per window: [cell][interval] x/y.
    std::vector<std::vector<double>> mx(ncell), my(ncell);
    size_t nIntervals = 0;
    for (const auto& win : m->windows) {
        for (int c = 0; c < ncell; ++c) {
            std::vector<double> sx(win.size()), sy(win.size());
            for (size_t k = 0; k < win.size(); ++k) {
                sx[k] = win[k].flow[c * 2 + 0];
                sy[k] = win[k].flow[c * 2 + 1];
            }
            highPass(sx, p.highPassFrames);
            highPass(sy, p.highPassFrames);
            mx[c].insert(mx[c].end(), sx.begin(), sx.end());
            my[c].insert(my[c].end(), sy.begin(), sy.end());
        }
        nIntervals += win.size();
    }
    res.intervals = static_cast<int>(nIntervals);
    if (res.intervals < p.minIntervals)
        return res;

    // Residual per cell and step.
    std::vector<std::vector<double>> resid(ncell, std::vector<double>(nstep, kNaN));
    std::vector<double> rawFlow(ncell, kNaN);
    for (int c = 0; c < ncell; ++c) {
        const Cell& cl = cells[c];
        if (!cl.ok)
            continue;
        int valid = 0;
        double raw = 0.0;
        for (size_t k = 0; k < mx[c].size(); ++k)
            if (!std::isnan(mx[c][k])) {
                raw += mx[c][k] * mx[c][k] + my[c][k] * my[c][k];
                ++valid;
            }
        if (valid < p.minIntervals || raw <= 0.0)
            continue;
        rawFlow[c] = std::sqrt(raw / valid);
        for (int s = 0; s < nstep; ++s) {
            const double adj = (-p.searchMs + s * p.stepMs) * 1e-3 + cl.rowSec;
            // Predicted rotational flow per interval, then high-passed per window.
            std::vector<double> px, py;
            px.reserve(nIntervals);
            py.reserve(nIntervals);
            for (const auto& win : m->windows) {
                std::vector<double> wx(win.size()), wy(win.size());
                for (size_t k = 0; k < win.size(); ++k) {
                    const Quat qa = gyro.rawOrientation(win[k].t0 + adj);
                    const Quat qb = gyro.rawOrientation(win[k].t1 + adj);
                    double world[3], d1[3], x1, y1;
                    rotate(qa, cl.d, world);
                    rotateInv(qb, world, d1);
                    dirToPixel(d1, w, h, x1, y1);
                    double dx = x1 - cl.x;
                    if (dx > w / 2.0)
                        dx -= w;
                    else if (dx < -w / 2.0)
                        dx += w;
                    wx[k] = dx;
                    wy[k] = y1 - cl.y;
                }
                highPass(wx, p.highPassFrames);
                highPass(wy, p.highPassFrames);
                px.insert(px.end(), wx.begin(), wx.end());
                py.insert(py.end(), wy.begin(), wy.end());
            }
            double e = 0.0;
            int n = 0;
            for (size_t k = 0; k < px.size(); ++k)
                if (!std::isnan(mx[c][k])) {
                    const double ex = mx[c][k] - px[k], ey = my[c][k] - py[k];
                    e += ex * ex + ey * ey;
                    ++n;
                }
            resid[c][s] = std::sqrt(e / n);
        }
    }

    // Keep the cells the gyro explains; sum their normalized residual curves.
    res.curve.assign(nstep, 0.0);
    for (int c = 0; c < ncell; ++c) {
        if (std::isnan(rawFlow[c]))
            continue;
        const double best = *std::min_element(resid[c].begin(), resid[c].end());
        if (best / rawFlow[c] >= p.goodRatio)
            continue;
        for (int s = 0; s < nstep; ++s)
            res.curve[s] += resid[c][s] / rawFlow[c];
        ++res.cellsUsed;
    }
    if (res.cellsUsed < p.minCells)
        return res;
    for (double& v : res.curve)
        v /= res.cellsUsed;

    const int bi = static_cast<int>(std::min_element(res.curve.begin(), res.curve.end()) - res.curve.begin());
    if (bi == 0 || bi == nstep - 1)
        return res; // at the edge of the search: no reliable minimum
    double off = 0.0;
    const double a = res.curve[bi - 1], b = res.curve[bi], c = res.curve[bi + 1];
    if (a - 2 * b + c > 0.0)
        off = 0.5 * (a - c) / (a - 2 * b + c);
    res.adjustSec = (-p.searchMs + (bi + off) * p.stepMs) * 1e-3;
    const double atZero = res.curve[nstep / 2];
    res.improvement = atZero > 0.0 ? 1.0 - b / atZero : 0.0;
    res.ok = true;
    return res;
}

} // namespace insv
