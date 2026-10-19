#include "ClipItem.h"

#include <algorithm>
#include <cmath>

void ColorGrade::clamp()
{
    exposureEv = std::clamp(exposureEv, kMinExposureEv, kMaxExposureEv);
    contrast = std::clamp(contrast, kMinPercent, kMaxPercent);
    saturation = std::clamp(saturation, kMinPercent, kMaxPercent);
    temperature = std::clamp(temperature, kMinPercent, kMaxPercent);
    lutStrength = std::clamp(lutStrength, 0.0f, kMaxPercent);
}

color::Grade ColorGrade::render(std::string *lutError) const
{
    color::Grade g;
    g.adjust = packed();
    if (!lut.isEmpty()) {
        g.lut = color::cachedCube(lut.toStdString(), lutError);
        g.lutMix = lutStrength / 100.0f;
    }
    return g;
}

qint64 ClipItem::frameDurationMs() const
{
    return std::max<qint64>(1, std::llround(1000.0 / fps));
}

void ClipItem::resetEditsToFullSource()
{
    trimInMs = 0;
    trimOutMs = sourceDurationMs;
    reframe.clear();
}

bool ClipItem::setRange(qint64 inMs, qint64 outMs)
{
    // A clip shorter than kMinTrimMs can't satisfy the gap; just span it whole.
    if (sourceDurationMs <= kMinTrimMs) {
        inMs = 0;
        outMs = sourceDurationMs;
    } else {
        inMs = std::clamp<qint64>(inMs, 0, sourceDurationMs - kMinTrimMs);
        outMs = std::clamp<qint64>(outMs, inMs + kMinTrimMs, sourceDurationMs);
    }
    if (inMs == trimInMs && outMs == trimOutMs)
        return false;
    trimInMs = inMs;
    trimOutMs = outMs;
    return true;
}

bool ClipItem::setTrimIn(qint64 positionMs)
{
    const qint64 clamped = std::clamp<qint64>(positionMs, 0, sourceDurationMs);
    return setRange(std::min(clamped, trimOutMs - kMinTrimMs), trimOutMs);
}

bool ClipItem::setTrimOut(qint64 positionMs)
{
    const qint64 clamped = std::clamp<qint64>(positionMs, 0, sourceDurationMs);
    return setRange(trimInMs, std::max(clamped, trimInMs + kMinTrimMs));
}

bool ClipItem::clearTrim()
{
    return setRange(0, sourceDurationMs);
}

bool ClipItem::upsertReframeKey(qint64 sourceMs, const ReframePose &pose)
{
    if (flat)
        return false; // a flat clip isn't reframed
    // upsert always lands a key (insert, or overwrite an existing pose).
    reframe.upsert(sourceMs, pose, frameDurationMs());
    return true;
}

bool ClipItem::removeReframeKeyNear(qint64 sourceMs)
{
    return reframe.removeNear(sourceMs, kReframeDeleteSnapMs);
}

bool ClipItem::clearReframe()
{
    if (reframe.isEmpty())
        return false;
    reframe.clear();
    return true;
}

bool ClipItem::setSpeed(double s)
{
    if (!std::isfinite(s))
        return false;
    s = std::clamp(s, kMinSpeed, kMaxSpeed);
    if (std::abs(s - speed) < 1e-9)
        return false;
    speed = s;
    return true;
}

bool ClipItem::setFit(Fit f)
{
    if (fit == f)
        return false;
    fit = f;
    return true;
}

bool ClipItem::setBlurPlates(bool on)
{
    if (flat)
        return false; // the blur works on the sphere
    if (blurPlates == on)
        return false;
    blurPlates = on;
    return true;
}

bool ClipItem::setPlateScan(const QString &key)
{
    if (flat || plateScan == key)
        return false;
    plateScan = key;
    return true;
}

bool ClipItem::setBlurAdditions(const std::vector<privacy::BlurAddition> &additions)
{
    if (flat)
        return false;
    if (blurAdditions == additions)
        return false;
    blurAdditions = additions;
    return true;
}

bool ClipItem::setBlurRemovals(const std::vector<privacy::BlurRemoval> &removals)
{
    if (flat)
        return false;
    if (blurRemovals == removals)
        return false;
    blurRemovals = removals;
    return true;
}

bool ClipItem::setColorGrade(const ColorGrade &grade)
{
    ColorGrade clamped = grade;
    clamped.clamp();
    if (clamped == color)
        return false;
    color = clamped;
    return true;
}

bool ClipItem::setStabilization(const insv::StabConfig &config)
{
    if (flat)
        return false; // no gyro data
    if (config == stabilization)
        return false;
    stabilization = config;
    return true;
}
