#include "PlateTracking.h"

#include <opencv2/core/version.hpp>
#if CV_VERSION_MAJOR >= 5
#include <opencv2/features.hpp> // goodFeaturesToTrack (imgproc before 5)
#include <opencv2/geometry.hpp> // findHomography, estimateAffinePartial2D (calib3d)
#else
#include <opencv2/calib3d.hpp>
#endif
#include <opencv2/imgproc.hpp>
#include <opencv2/video/tracking.hpp>

#include <algorithm>
#include <cmath>

namespace privacy::plates {

cv::Vec3d dirOf(double lon, double lat)
{
    return {std::cos(lat) * std::sin(lon), std::sin(lat), -std::cos(lat) * std::cos(lon)};
}

void lonLatOf(const cv::Vec3d& d, double& lon, double& lat)
{
    lon = std::atan2(d[0], -d[2]);
    lat = std::asin(std::clamp(d[1], -1.0, 1.0));
}

void basisAt(double lon, double lat, cv::Vec3d& fwd, cv::Vec3d& right, cv::Vec3d& up)
{
    fwd = dirOf(lon, lat);
    right = cv::Vec3d(std::cos(lon), 0.0, std::sin(lon));
    up = right.cross(fwd);
}

double angleBetween(const SphereBox& a, const SphereBox& b)
{
    return std::acos(std::clamp(dirOf(a.lon, a.lat).dot(dirOf(b.lon, b.lat)), -1.0, 1.0));
}

Quad quadFromBox(const SphereBox& b, double dilW, double dilH)
{
    cv::Vec3d f, r, u;
    basisAt(b.lon, b.lat, f, r, u);
    const double tx = std::tan(std::min(0.5 * b.w * dilW, 1.4)), ty = std::tan(std::min(0.5 * b.h * dilH, 1.4));
    return {cv::normalize(f - r * tx + u * ty), cv::normalize(f + r * tx + u * ty),
            cv::normalize(f + r * tx - u * ty), cv::normalize(f - r * tx - u * ty)};
}

cv::Vec3d centroidDir(const Quad& q)
{
    return cv::normalize(q[0] + q[1] + q[2] + q[3]);
}

Quad growQuad(const Quad& q, double s)
{
    const cv::Vec3d c = centroidDir(q);
    Quad r;
    for (int i = 0; i < 4; ++i) {
        const cv::Vec3d p = q[i] / q[i].dot(c);
        r[i] = cv::normalize(c + (p - c) * s);
    }
    return r;
}

double angleOf(const cv::Vec3d& a, const cv::Vec3d& b)
{
    return std::acos(std::clamp(a.dot(b), -1.0, 1.0));
}

double quadSize(const Quad& q)
{
    return std::max(angleOf(q[0], q[2]), angleOf(q[1], q[3]));
}

SphereBox lerpBox(const SphereBox& a, const SphereBox& b, double t)
{
    SphereBox r;
    r.lon = a.lon + std::remainder(b.lon - a.lon, 2 * kPi) * t;
    r.lat = a.lat + (b.lat - a.lat) * t;
    r.w = a.w + (b.w - a.w) * t;
    r.h = a.h + (b.h - a.h) * t;
    return r;
}

View viewForQuad(const Quad& q)
{
    double lon, lat;
    lonLatOf(centroidDir(q), lon, lat);
    const double fov = std::clamp(quadSize(q) * kRadToDeg * 3.0, 2.0, 70.0);
    return View::make(lon * kRadToDeg, lat * kRadToDeg, fov, kQuadPx, kQuadPx);
}

bool projectQuad(const View& v, const Quad& q, std::vector<cv::Point2f>& px)
{
    px.resize(4);
    for (int i = 0; i < 4; ++i) {
        if (!v.project(q[i], px[i]))
            return false;
        px[i] -= cv::Point2f(0.5f, 0.5f);
    }
    return true;
}

bool insideImage(const std::vector<cv::Point2f>& px, int w, int h)
{
    for (const cv::Point2f& p : px)
        if (p.x < 0 || p.y < 0 || p.x > w - 1 || p.y > h - 1)
            return false;
    return true;
}

cv::Mat rectify(const cv::Mat& gray, const std::vector<cv::Point2f>& quadPx, cv::Size sz)
{
    const std::vector<cv::Point2f> dst{
        {0, 0}, {float(sz.width), 0}, {float(sz.width), float(sz.height)}, {0, float(sz.height)}};
    cv::Mat out;
    cv::warpPerspective(gray, out, cv::getPerspectiveTransform(quadPx, dst), sz, cv::INTER_AREA,
                        cv::BORDER_REPLICATE);
    return out;
}

bool warpView(const cv::Mat& img, const View& a, const View& b, cv::Mat& out)
{
    const std::vector<cv::Point2f> dst{{0, 0}, {float(b.w - 1), 0}, {float(b.w - 1), float(b.h - 1)}, {0, float(b.h - 1)}};
    std::vector<cv::Point2f> src(4);
    for (int i = 0; i < 4; ++i) {
        if (!a.project(b.ray(dst[i].x + 0.5, dst[i].y + 0.5), src[i]))
            return false;
        src[i] -= cv::Point2f(0.5f, 0.5f);
    }
    cv::warpPerspective(img, out, cv::getPerspectiveTransform(dst, src), cv::Size(b.w, b.h),
                        cv::INTER_LINEAR | cv::WARP_INVERSE_MAP, cv::BORDER_REPLICATE);
    return true;
}

double nccInner(const cv::Mat& a, const cv::Mat& b)
{
    const cv::Rect in(a.cols / 10, a.rows / 10, a.cols * 8 / 10, a.rows * 8 / 10);
    cv::Mat r;
    cv::matchTemplate(a(in), b(in), r, cv::TM_CCOEFF_NORMED);
    return r.at<float>(0, 0);
}

bool anchorQuad(QuadTracker& t, const Quad& q, double hOverW, const View& v, const cv::Mat& gray)
{
    std::vector<cv::Point2f> px;
    if (!projectQuad(v, q, px) || !insideImage(px, v.w, v.h))
        return false;
    t.q = q;
    t.view = v;
    t.gray = gray;
    t.tmplSize = cv::Size(64, std::clamp(static_cast<int>(std::lround(64.0 * hOverW)), 16, 128));
    t.tmpl = rectify(gray, px, t.tmplSize);
    t.sinceAnchor = t.unverified = 0;
    t.holdLeft = -1;
    return true;
}

bool anchorQuad(QuadTracker& t, const SphereBox& b, const View& v, const cv::Mat& gray)
{
    return anchorQuad(t, quadFromBox(b), b.h / std::max(b.w, 1e-6), v, gray);
}

SphereBox boxOfQuad(const Quad& q)
{
    SphereBox b;
    lonLatOf(centroidDir(q), b.lon, b.lat);
    b.w = 0.5 * (angleOf(q[0], q[1]) + angleOf(q[3], q[2]));
    b.h = 0.5 * (angleOf(q[0], q[3]) + angleOf(q[1], q[2]));
    return b;
}

Quad rotateQuad(const Quad& q, const cv::Vec3d& a, const cv::Vec3d& b, double scale,
                double maxAngle)
{
    cv::Vec3d axis = a.cross(b);
    const double s = cv::norm(axis);
    if (s < 1e-9)
        return q;
    axis *= std::min(std::atan2(s, a.dot(b)) * scale, maxAngle) / s;
    cv::Matx33d R;
    cv::Rodrigues(axis, R);
    Quad r;
    for (int i = 0; i < 4; ++i)
        r[i] = R * q[i];
    return r;
}

void stepQuad(QuadTracker& t, const View& v, const cv::Mat& cur)
{
    t.stepOk = false;
    cv::Mat prev;
    if (!warpView(t.gray, t.view, v, prev))
        return;
    std::vector<cv::Point2f> qp;
    if (!projectQuad(v, t.q, qp))
        return;
    cv::Mat mask = cv::Mat::zeros(prev.size(), CV_8U);
    const cv::Point2f c = (qp[0] + qp[1] + qp[2] + qp[3]) * 0.25f;
    std::vector<cv::Point> mp;
    for (const auto& p : qp)
        mp.emplace_back(c + (p - c) * 1.15f); // the plate and its frame
    cv::fillConvexPoly(mask, mp, cv::Scalar(255));
    std::vector<cv::Point2f> p0;
    cv::goodFeaturesToTrack(prev, p0, 150, 0.005, 3, mask, 5);
    if (p0.size() < 4)
        return;
    std::vector<cv::Point2f> p1, pb;
    std::vector<uchar> st, stb;
    std::vector<float> err;
    cv::calcOpticalFlowPyrLK(prev, cur, p0, p1, st, err, cv::Size(21, 21), 3);
    cv::calcOpticalFlowPyrLK(cur, prev, p1, pb, stb, err, cv::Size(21, 21), 3);
    std::vector<cv::Point2f> a0, a1;
    for (size_t i = 0; i < p0.size(); ++i)
        if (st[i] && stb[i] && cv::norm(pb[i] - p0[i]) < 1.0) {
            a0.push_back(p0[i]);
            a1.push_back(p1[i]);
        }
    cv::Mat H;
    if (a0.size() >= 8) {
        std::vector<uchar> inl;
        H = cv::findHomography(a0, a1, cv::RANSAC, 1.5, inl);
        if (!H.empty() && cv::countNonZero(inl) < 6)
            H.release();
    }
    if (H.empty() && a0.size() >= 3) { // fewer points: similarity only
        const cv::Mat A = cv::estimateAffinePartial2D(a0, a1);
        if (!A.empty()) {
            H = cv::Mat::eye(3, 3, CV_64F);
            A.copyTo(H(cv::Rect(0, 0, 3, 2)));
        }
    }
    if (H.empty())
        return;
    std::vector<cv::Point2f> q1;
    cv::perspectiveTransform(qp, q1, H);
    const double area0 = cv::contourArea(qp), area1 = cv::contourArea(q1);
    if (!cv::isContourConvex(q1) || area1 < 0.5 * area0 || area1 > 2.0 * area0 || !insideImage(q1, v.w, v.h))
        return;
    for (int i = 0; i < 4; ++i)
        t.stepQ[i] = v.ray(q1[i].x + 0.5, q1[i].y + 0.5);
    t.stepNcc = nccInner(rectify(cur, q1, t.tmplSize), t.tmpl);
    t.stepOk = true;
}

} // namespace privacy::plates
