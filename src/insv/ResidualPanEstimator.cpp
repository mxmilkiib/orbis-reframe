// SPDX-License-Identifier: MIT OR Apache-2.0
#include "ResidualPanEstimator.h"

#include "GyroStabilizer.h" // insv::leanWindowFromStrength

#include <opencv2/core.hpp>
#include <opencv2/features2d.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/video.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <vector>

namespace insv {

namespace {

constexpr double kPi = std::numbers::pi;

struct V3 {
    double x, y, z;
};

// Equirect pixel -> unit direction. Inverse of src/render/shaders/equirect.frag:
//   longitude = atan(x, -z),  latitude = asin(y)
//   u = lon/(2pi) + 0.5,  v = 0.5 - lat/pi
V3 dirFromEquirect(double px, double py, double W, double H)
{
    const double lon = (px / W - 0.5) * 2.0 * kPi;
    const double lat = (0.5 - py / H) * kPi;
    const double cl = std::cos(lat);
    return {cl * std::sin(lon), std::sin(lat), -cl * std::cos(lon)};
}

// m^T * v. GyroStabilizer::correctionMatrix returns M such that the shader
// forms the equirect sample direction as pose*(M*viewRay); to go the other
// way -- an equirect direction back to the *stabilized* view ray -- apply M^T.
V3 rot3T(const float m[3][3], const V3& v)
{
    return {m[0][0] * v.x + m[1][0] * v.y + m[2][0] * v.z,
            m[0][1] * v.x + m[1][1] * v.y + m[2][1] * v.z,
            m[0][2] * v.x + m[1][2] * v.y + m[2][2] * v.z};
}

double wrapPi(double a)
{
    while (a > kPi)
        a -= 2.0 * kPi;
    while (a < -kPi)
        a += 2.0 * kPi;
    return a;
}

double median(std::vector<double>& v)
{
    if (v.empty())
        return 0.0;
    const size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + mid, v.end());
    const double m = v[mid];
    if (v.size() % 2 == 1)
        return m;
    const double lo = *std::max_element(v.begin(), v.begin() + mid);
    return 0.5 * (m + lo);
}

void identity3(float m[3][3])
{
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            m[r][c] = (r == c) ? 1.0f : 0.0f;
}

} // namespace

struct ResidualPanEstimator::Impl {
    Params p;

    cv::Mat prevGray;
    double prevT = 0.0;
    float prevGyro[3][3];
    bool have = false;

    double accYaw = 0.0;   // accumulated apparent forward-view pan, radians
    double accPitch = 0.0;

    std::vector<double> t;       // interval end time, seconds
    std::vector<double> panYaw;  // accYaw at t[i]
    std::vector<double> panPitch;
    std::vector<int> matches;    // features surviving in interval i
};

ResidualPanEstimator::ResidualPanEstimator() : ResidualPanEstimator(Params{}) {}

ResidualPanEstimator::ResidualPanEstimator(const Params& params) : d(std::make_unique<Impl>())
{
    d->p = params;
    identity3(d->prevGyro);
}

ResidualPanEstimator::~ResidualPanEstimator() = default;

void ResidualPanEstimator::addFrame(const uint8_t* gray, int w, int h, double tSec,
                                    const float gyro[3][3])
{
    const cv::Mat cur(h, w, CV_8UC1, const_cast<uint8_t*>(gray));

    if (!d->have) {
        cur.copyTo(d->prevGray);
        d->prevT = tSec;
        std::copy(&gyro[0][0], &gyro[0][0] + 9, &d->prevGyro[0][0]);
        d->have = true;
        return;
    }

    const double W = w, H = h;
    const int poleMargin = static_cast<int>(d->p.poleMarginFrac * H);

    std::vector<cv::Point2f> p0;
    cv::Mat mask(h, w, CV_8U, cv::Scalar(255));
    if (poleMargin > 0 && poleMargin * 2 < h) {
        mask.rowRange(0, poleMargin).setTo(0);
        mask.rowRange(h - poleMargin, h).setTo(0);
    }
    cv::goodFeaturesToTrack(d->prevGray, p0, d->p.maxFeatures, d->p.qualityLevel,
                            d->p.minDistancePx, mask);

    std::vector<double> dlons, dlats;
    if (static_cast<int>(p0.size()) >= d->p.minMatches) {
        std::vector<cv::Point2f> p1, pb;
        std::vector<uchar> st, st2;
        std::vector<float> e0, e1;
        cv::calcOpticalFlowPyrLK(d->prevGray, cur, p0, p1, st, e0, cv::Size(21, 21), 3);
        cv::calcOpticalFlowPyrLK(cur, d->prevGray, p1, pb, st2, e1, cv::Size(21, 21), 3);

        dlons.reserve(p0.size());
        dlats.reserve(p0.size());
        for (size_t k = 0; k < p0.size(); ++k) {
            if (!st[k] || !st2[k])
                continue;
            if (cv::norm(pb[k] - p0[k]) > d->p.fwdBwdTolPx)
                continue;
            if (cv::norm(p1[k] - p0[k]) > 0.30 * W) // seam wrap / bad track
                continue;

            V3 a = dirFromEquirect(p0[k].x, p0[k].y, W, H);
            V3 b = dirFromEquirect(p1[k].x, p1[k].y, W, H);
            a = rot3T(d->prevGyro, a); // both endpoints into the gyro-stabilized
            b = rot3T(gyro, b);        // view frame
            if (b.z >= d->p.forwardZMax)
                continue;

            const double lonA = std::atan2(a.x, -a.z);
            const double latA = std::asin(std::clamp(a.y, -1.0, 1.0));
            const double lonB = std::atan2(b.x, -b.z);
            const double latB = std::asin(std::clamp(b.y, -1.0, 1.0));
            dlons.push_back(wrapPi(lonB - lonA));
            dlats.push_back(latB - latA);
        }
    }

    double mLon = 0.0, mLat = 0.0;
    const int nm = static_cast<int>(dlons.size());
    if (nm >= d->p.minMatches) {
        mLon = median(dlons);
        mLat = median(dlats);
        // A real forward-view pan between two video frames is well under a
        // degree; anything past that is a bad median (moving object, low
        // texture, motion blur) and would permanently offset the accumulator.
        mLon = std::clamp(mLon, -d->p.maxPanPerFrameRad, d->p.maxPanPerFrameRad);
        mLat = std::clamp(mLat, -d->p.maxPanPerFrameRad, d->p.maxPanPerFrameRad);
    }
    d->accYaw += mLon;
    d->accPitch += mLat;
    d->t.push_back(tSec);
    d->panYaw.push_back(d->accYaw);
    d->panPitch.push_back(d->accPitch);
    d->matches.push_back(nm);

    cur.copyTo(d->prevGray);
    d->prevT = tSec;
    std::copy(&gyro[0][0], &gyro[0][0] + 9, &d->prevGyro[0][0]);
}

int ResidualPanEstimator::intervalCount() const
{
    return static_cast<int>(d->t.size());
}

double ResidualPanEstimator::medianMatches() const
{
    if (d->matches.empty())
        return 0.0;
    std::vector<double> m(d->matches.begin(), d->matches.end());
    return median(m);
}

const std::vector<double>& ResidualPanEstimator::accumulatedYaw() const { return d->panYaw; }
const std::vector<double>& ResidualPanEstimator::accumulatedPitch() const { return d->panPitch; }
const std::vector<double>& ResidualPanEstimator::times() const { return d->t; }

ResidualTrack ResidualPanEstimator::finish(float strength01, double fps) const
{
    ResidualTrack rt;
    const size_t n = d->t.size();
    if (n < 2 || fps <= 0.0)
        return rt;

    // Longer window == more aggressive high-pass (keeps only the very slow
    // motion). `leanWindowFromStrength` is a half-window here, matching the
    // original spike (`--win 0.9` felt right => strength 0.5).
    const float winSec = leanWindowFromStrength(strength01);
    const int half = std::max(1, static_cast<int>(std::lround(winSec * fps)));

    rt.t = d->t;
    rt.dyaw.resize(n);
    rt.dpitch.resize(n);
    for (size_t i = 0; i < n; ++i) {
        double sY = 0.0, sP = 0.0;
        int c = 0;
        const long lo = std::max<long>(0, static_cast<long>(i) - half);
        const long hi = std::min<long>(static_cast<long>(n) - 1, static_cast<long>(i) + half);
        for (long j = lo; j <= hi; ++j) {
            sY += d->panYaw[j];
            sP += d->panPitch[j];
            ++c;
        }
        const double smY = sY / c, smP = sP / c;
        rt.dyaw[i] = static_cast<float>(smY - d->panYaw[i]);
        rt.dpitch[i] = static_cast<float>(smP - d->panPitch[i]);
    }
    return rt;
}

} // namespace insv
