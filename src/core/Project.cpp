#include "Project.h"

#include "MediaPool.h"
#include "Timeline.h"

#include <limits>
#include <cmath>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <exception>
#include <functional>
#include <vector>

using nlohmann::json;

namespace {

// Store a source path relative to the project directory when it sits inside
// it, absolute otherwise -- so a project folder stays portable but a clip
// elsewhere on disk still resolves.
QString storedPath(const QString &sourcePath, const QDir &projectDir)
{
    const QString abs = QFileInfo(sourcePath).absoluteFilePath();
    const QString rel = projectDir.relativeFilePath(abs);
    if (!rel.startsWith(QStringLiteral("../")) && !QDir::isAbsolutePath(rel))
        return rel;
    return abs;
}

QString resolvedPath(const QString &stored, const QDir &projectDir)
{
    if (QDir::isAbsolutePath(stored))
        return QDir::cleanPath(stored);
    return QDir::cleanPath(projectDir.absoluteFilePath(stored));
}

const char *lensGuardName(stitch::PreviewStitchConfig::LensGuard g)
{
    return g == stitch::PreviewStitchConfig::LensGuard::None       ? "none"
           : g == stitch::PreviewStitchConfig::LensGuard::Standard ? "standard"
                                                                  : "premium";
}

// An unknown or missing name leaves `g` as it is.
void lensGuardFromJson(const json &j, stitch::PreviewStitchConfig::LensGuard &g)
{
    const std::string guard = j.value("lensGuard", std::string());
    if (guard == "none")
        g = stitch::PreviewStitchConfig::LensGuard::None;
    else if (guard == "standard")
        g = stitch::PreviewStitchConfig::LensGuard::Standard;
    else if (guard == "premium")
        g = stitch::PreviewStitchConfig::LensGuard::Premium;
}

json previewStitchToJson(const stitch::PreviewStitchConfig &c)
{
    return json{
        {"quality", c.quality == stitch::PreviewStitchConfig::Quality::OpticalFlow ? "optflow"
                                                                               : "template"},
        {"width", c.width},
        {"height", c.height},
        {"lensGuard", lensGuardName(c.lensGuard)},
        {"depthSeam", c.depthSeam},
    };
}

stitch::PreviewStitchConfig previewStitchFromJson(const json &j)
{
    stitch::PreviewStitchConfig c; // defaults for any missing field
    if (!j.is_object())
        return c;
    c.quality = j.value("quality", std::string("template")) == "optflow"
                    ? stitch::PreviewStitchConfig::Quality::OpticalFlow
                    : stitch::PreviewStitchConfig::Quality::Template;
    c.width = j.value("width", c.width);
    c.height = j.value("height", c.height);
    lensGuardFromJson(j, c.lensGuard);
    c.depthSeam = j.value("depthSeam", c.depthSeam);
    return c;
}

json stabilizationToJson(const insv::StabConfig &c)
{
    return json{
        {"mode", insv::stabModeToString(c.mode)},
        {"windowYawSec", c.windowYawSec},
        {"windowPitchSec", c.windowPitchSec},
        {"windowRollSec", c.windowRollSec},
        {"horizonLockAmount", c.horizonLockAmount},
        {"horizonGain", c.horizonGain},
        {"accelLowPassHz", c.accelLowPassHz},
        {"pitchOffsetRad", c.pitchOffsetRad},
        {"rollOffsetRad", c.rollOffsetRad},
        {"autoLevel", c.autoLevel},
    };
}

json outputToJson(const OutputSettings &o)
{
    const char *aspect = o.aspect == OutputSettings::Aspect::Portrait9x16 ? "9:16"
                         : o.aspect == OutputSettings::Aspect::Square1x1  ? "1:1"
                         : o.aspect == OutputSettings::Aspect::Classic4x3 ? "4:3"
                                                                          : "16:9";
    return json{{"aspect", aspect}, {"width", o.width}, {"height", o.height}, {"fps", o.fps}};
}

OutputSettings outputFromJson(const json &j)
{
    OutputSettings o; // defaults for any missing field
    if (!j.is_object())
        return o;
    const std::string aspect = j.value("aspect", std::string("16:9"));
    if (aspect == "9:16")
        o.aspect = OutputSettings::Aspect::Portrait9x16;
    else if (aspect == "1:1")
        o.aspect = OutputSettings::Aspect::Square1x1;
    else if (aspect == "4:3")
        o.aspect = OutputSettings::Aspect::Classic4x3;
    const auto def = OutputSettings::presets(o.aspect)[OutputSettings::defaultPresetIndex()];
    o.width = j.value("width", def.w);
    o.height = j.value("height", def.h);
    if (o.width < 16 || o.height < 16) { // nonsense -> the aspect's default size
        o.width = def.w;
        o.height = def.h;
    }
    o.fps = std::max(0.0, j.value("fps", 0.0));
    return o;
}

insv::StabConfig stabilizationFromJson(const json &j)
{
    insv::StabConfig c; // defaults for any missing field
    if (!j.is_object())
        return c;
    c.mode = insv::stabModeFromString(j.value("mode", std::string("off")));
    c.windowYawSec = j.value("windowYawSec", c.windowYawSec);
    c.windowPitchSec = j.value("windowPitchSec", c.windowPitchSec);
    c.windowRollSec = j.value("windowRollSec", c.windowRollSec);
    c.horizonLockAmount = j.value("horizonLockAmount", c.horizonLockAmount);
    c.horizonGain = j.value("horizonGain", c.horizonGain);
    c.accelLowPassHz = j.value("accelLowPassHz", c.accelLowPassHz);
    c.pitchOffsetRad = j.value("pitchOffsetRad", c.pitchOffsetRad);
    c.rollOffsetRad = j.value("rollOffsetRad", c.rollOffsetRad);
    c.autoLevel = j.value("autoLevel", c.autoLevel); // absent (older files) => on
    return c;
}

json reframeToJson(const ReframeTrack &track)
{
    json keys = json::array();
    for (const ReframeKey &k : track.keys()) {
        json jk{{"tMs", static_cast<qint64>(k.timeMs)},
                {"yaw", k.pose.yaw},
                {"pitch", k.pose.pitch},
                {"roll", k.pose.roll},
                {"fovY", k.pose.fovY}};
        if (k.ease != keytrack::Ease::Linear)
            jk["ease"] = keytrack::easeName(k.ease);
        keys.push_back(std::move(jk));
    }
    return json{{"keys", std::move(keys)}};
}

json colorGradeToJson(const ColorGrade &g, const QDir *projectDir)
{
    json j{
        {"exposureEv", g.exposureEv},
        {"contrast", g.contrast},
        {"saturation", g.saturation},
        {"temperature", g.temperature},
    };
    if (!g.lut.isEmpty()) {
        j["lut"] = (projectDir ? storedPath(g.lut, *projectDir) : g.lut).toStdString();
        j["lutStrength"] = g.lutStrength;
    }
    return j;
}

ColorGrade colorGradeFromJson(const json &j, const QDir *projectDir)
{
    ColorGrade g; // defaults for any missing field
    if (!j.is_object())
        return g;
    g.exposureEv = j.value("exposureEv", g.exposureEv);
    g.contrast = j.value("contrast", g.contrast);
    g.saturation = j.value("saturation", g.saturation);
    g.temperature = j.value("temperature", g.temperature);
    const QString lut = QString::fromStdString(j.value("lut", std::string()));
    if (!lut.isEmpty()) {
        g.lut = projectDir ? resolvedPath(lut, *projectDir) : lut;
        g.lutStrength = j.value("lutStrength", g.lutStrength);
    }
    return g;
}

// "noise": {"mode": "wind" | "windGeneral" | "hiss" | "voice" |
// "voiceAmbience", "strength": 0.7} -- written only when on. "wind" is the
// motorcycle one (it came first).
json noiseToJson(const insv::NoiseReduction &n)
{
    const char *mode = "wind";
    switch (n.mode) {
    case insv::NoiseMode::Wind: mode = "windGeneral"; break;
    case insv::NoiseMode::Hiss: mode = "hiss"; break;
    case insv::NoiseMode::Voice: mode = "voice"; break;
    case insv::NoiseMode::VoiceAmbience: mode = "voiceAmbience"; break;
    default: break;
    }
    return json{{"mode", mode}, {"strength", n.strength}};
}

insv::NoiseReduction noiseFromJson(const json &j)
{
    insv::NoiseReduction n; // off unless a known mode is given
    if (!j.is_object())
        return n;
    const std::string mode = j.value("mode", std::string());
    if (mode == "wind")
        n.mode = insv::NoiseMode::MotorcycleWind;
    else if (mode == "windGeneral")
        n.mode = insv::NoiseMode::Wind;
    else if (mode == "hiss")
        n.mode = insv::NoiseMode::Hiss;
    else if (mode == "voice")
        n.mode = insv::NoiseMode::Voice;
    else if (mode == "voiceAmbience")
        n.mode = insv::NoiseMode::VoiceAmbience;
    n.strength = std::clamp(j.value("strength", n.strength), 0.0f, 1.0f);
    return n;
}

json blurRemovalsToJson(const std::vector<privacy::BlurRemoval> &removals)
{
    json arr = json::array();
    for (const privacy::BlurRemoval &r : removals) {
        json path = json::array();
        for (const privacy::BlurRemoval::Point &p : r.path)
            path.push_back({p.tMs, p.dir[0], p.dir[1], p.dir[2]});
        arr.push_back(json{{"fromMs", r.fromMs}, {"toMs", r.toMs}, {"wholeTrack", r.wholeTrack},
                           {"path", std::move(path)}});
    }
    return arr;
}

// Corner coordinates in millionths (unit vectors: ~0.00006 deg), as
// integers -- a followed plate is a region per frame, and full floats made
// a 20 s plate ~150 KB of project.
json blurAdditionsToJson(const std::vector<privacy::BlurAddition> &additions)
{
    json arr = json::array();
    for (const privacy::BlurAddition &a : additions) {
        json frames = json::array();
        for (const privacy::BlurAddition::Frame &f : a.frames) {
            json jf = json::array({f.tMs, f.kind});
            for (int k = 0; k < 4; ++k)
                for (int i = 0; i < 3; ++i)
                    jf.push_back(std::lround(f.corner[k][i] * 1e6));
            frames.push_back(std::move(jf));
        }
        const char *timing = a.timing == privacy::StitchTiming::Uncorrected ? "uncorrected"
                             : a.timing == privacy::StitchTiming::NoGyro    ? "noGyro"
                                                                            : "corrected";
        arr.push_back(json{{"frames", std::move(frames)}, {"timing", timing}});
    }
    return arr;
}

// Malformed frames / entries are skipped.
std::vector<privacy::BlurAddition> blurAdditionsFromJson(const json &j)
{
    std::vector<privacy::BlurAddition> out;
    if (!j.is_array())
        return out;
    for (const json &ja : j) {
        if (!ja.is_object() || !ja.contains("frames") || !ja.at("frames").is_array())
            continue;
        privacy::BlurAddition a;
        if (const auto t = ja.find("timing"); t != ja.end() && t->is_string())
            a.timing = *t == "uncorrected" ? privacy::StitchTiming::Uncorrected
                       : *t == "noGyro"    ? privacy::StitchTiming::NoGyro
                                           : privacy::StitchTiming::Corrected;
        for (const json &jf : ja.at("frames")) {
            if (!jf.is_array() || jf.size() != 14 ||
                !std::all_of(jf.begin(), jf.end(), [](const json &v) { return v.is_number(); }))
                continue;
            privacy::BlurAddition::Frame f;
            f.tMs = jf[0].get<int64_t>();
            f.kind = static_cast<uint8_t>(std::clamp(jf[1].get<int>(), 0, 3));
            for (int k = 0; k < 4; ++k)
                for (int i = 0; i < 3; ++i)
                    f.corner[k][i] = static_cast<float>(jf[2 + k * 3 + i].get<double>() * 1e-6);
            if (a.frames.empty() || f.tMs > a.frames.back().tMs)
                a.frames.push_back(f);
        }
        if (!a.frames.empty())
            out.push_back(std::move(a));
    }
    return out;
}

// Malformed entries are skipped (a removal that matches nothing is harmless,
// a dropped one only brings a blur back).
std::vector<privacy::BlurRemoval> blurRemovalsFromJson(const json &j)
{
    std::vector<privacy::BlurRemoval> out;
    if (!j.is_array())
        return out;
    for (const json &jr : j) {
        if (!jr.is_object() || !jr.contains("path") || !jr.at("path").is_array())
            continue;
        privacy::BlurRemoval r;
        r.fromMs = jr.value("fromMs", int64_t(0));
        r.toMs = jr.value("toMs", int64_t(0));
        r.wholeTrack = jr.value("wholeTrack", true);
        for (const json &jp : jr.at("path")) {
            if (!jp.is_array() || jp.size() != 4 || !jp[0].is_number() || !jp[1].is_number() ||
                !jp[2].is_number() || !jp[3].is_number())
                continue;
            privacy::BlurRemoval::Point p;
            p.tMs = jp[0].get<int64_t>();
            for (int k = 0; k < 3; ++k)
                p.dir[k] = jp[k + 1].get<float>();
            r.path.push_back(p);
        }
        if (!r.path.empty())
            out.push_back(std::move(r));
    }
    return out;
}

// Parsed form of one clip, before it's committed to the Timeline -- so a
// malformed file can be rejected without half-destroying the current model.
struct ParsedClip {
    QString sourcePath; // already resolved to absolute
    qint64 trimInMs = 0;
    qint64 trimOutMs = 0;
    qint64 sourceDurationMs = 0;
    double fps = 0.0;
    int sourceWidth = 0;
    int sourceHeight = 0;
    bool mediaMissing = false;
    std::vector<ReframeKey> reframeKeys;
    double audioGainDb = 0.0;
    bool audioMuted = false;
    int audioTrack = 0;
    insv::NoiseReduction noise;
    ColorGrade color;
    insv::StabConfig stabilization;
    bool blurPlates = false;
    QString plateScan;
    std::vector<privacy::BlurRemoval> blurRemovals;
    std::vector<privacy::BlurAddition> blurAdditions;
    double speed = 1.0;
    double syncAdjustMs = std::numeric_limits<double>::quiet_NaN();
    std::array<double, 3> seamAdjustDeg{0.0, 0.0, 0.0};
    int seamAdjustGuard = -1;
    stitch::PreviewStitchConfig::LensGuard lensGuard = stitch::PreviewStitchConfig::LensGuard::Premium;
    bool depthSeam = true;
    bool flat = false;
    ClipItem::Fit fit = ClipItem::Fit::Bars;
};

// `linkedIndex`: the main-track index of the clip it's linked to, -1 if
// none -- ids don't survive a save, indices do.
json trackClipToJson(const TrackClip &c, const QDir *projectDir, int linkedIndex)
{
    const std::string source =
        (projectDir ? storedPath(c.sourcePath, *projectDir) : c.sourcePath).toStdString();
    json j{
        {"source", source},
        {"stream", c.stream},
        {"sourceDurationMs", static_cast<qint64>(c.sourceDurationMs)},
        {"startMs", static_cast<qint64>(c.startMs)},
        {"trimInMs", static_cast<qint64>(c.trimInMs)},
        {"trimOutMs", static_cast<qint64>(c.trimOutMs)},
        {"speed", c.speed},
        {"gainDb", c.gainDb},
        {"muted", c.muted},
        {"fadeInMs", static_cast<qint64>(c.fadeInMs)},
        {"fadeOutMs", static_cast<qint64>(c.fadeOutMs)},
    };
    if (c.noise.mode != insv::NoiseMode::Off)
        j["noise"] = noiseToJson(c.noise);
    if (linkedIndex >= 0) {
        j["linkedClip"] = linkedIndex;
        j["syncOffsetMs"] = static_cast<qint64>(c.syncOffsetMs);
    }
    return j;
}

TrackClip trackClipFromJson(const json &j, const QDir *projectDir)
{
    TrackClip c;
    const QString stored = QString::fromStdString(j.value("source", std::string()));
    c.sourcePath = projectDir ? resolvedPath(stored, *projectDir) : stored;
    c.stream = j.value("stream", -1);
    c.sourceDurationMs = j.value("sourceDurationMs", qint64(0));
    c.startMs = j.value("startMs", qint64(0));
    c.trimInMs = j.value("trimInMs", qint64(0));
    c.trimOutMs = j.value("trimOutMs", c.sourceDurationMs);
    c.speed = j.value("speed", 1.0);
    c.gainDb = j.value("gainDb", 0.0);
    c.muted = j.value("muted", false);
    c.fadeInMs = j.value("fadeInMs", qint64(0));
    c.fadeOutMs = j.value("fadeOutMs", qint64(0));
    if (j.contains("noise"))
        c.noise = noiseFromJson(j.at("noise"));
    c.mediaMissing = !QFileInfo::exists(c.sourcePath);
    // The linked main-track clip's index + 1 for now (0 = none); applyDocument
    // maps it to that clip's new id.
    c.linkedClipId = j.value("linkedClip", -1) + 1;
    c.syncOffsetMs = j.value("syncOffsetMs", qint64(0));
    return c;
}

json lookToJson(const inset::Look &l)
{
    return json{{"centerX", l.centerX}, {"centerY", l.centerY}, {"width", l.width},
                {"opacity", l.opacity}};
}

inset::Look lookFromJson(const json &j)
{
    inset::Look l;
    l.centerX = j.value("centerX", l.centerX);
    l.centerY = j.value("centerY", l.centerY);
    l.width = j.value("width", l.width);
    l.opacity = j.value("opacity", l.opacity);
    return l;
}

// A video-track clip: the track-clip fields plus its size and inset.
json colorToJson(uint32_t rgb)
{
    return QStringLiteral("#%1").arg(rgb & 0xffffff, 6, 16, QLatin1Char('0')).toStdString();
}

// "#rrggbb" -> rgb; `fallback` if it isn't one.
uint32_t colorFromJson(const json &j, const char *key, uint32_t fallback)
{
    const QString col = QString::fromStdString(j.value(key, std::string()));
    bool ok = false;
    const uint rgb = col.startsWith(QLatin1Char('#')) ? col.mid(1).toUInt(&ok, 16) : 0;
    return ok ? (rgb & 0xffffff) : fallback;
}

json titleToJson(const title::Style &t)
{
    return json{{"text", t.text.toStdString()}, {"font", t.font.toStdString()},
                {"bold", t.bold}, {"italic", t.italic}, {"color", colorToJson(t.rgb)}};
}

title::Style titleFromJson(const json &j)
{
    title::Style t;
    t.text = QString::fromStdString(j.value("text", std::string()));
    t.font = QString::fromStdString(j.value("font", t.font.toStdString()));
    t.bold = j.value("bold", t.bold);
    t.italic = j.value("italic", t.italic);
    t.rgb = colorFromJson(j, "color", t.rgb);
    return t;
}

// A video-track clip: the track-clip fields plus its size and inset; a
// title has its style instead of a source and size.
json videoClipToJson(const TrackClip &c, const QDir *projectDir)
{
    json j = trackClipToJson(c, projectDir, -1);
    j.erase("stream");
    j.erase("gainDb");
    j.erase("muted");
    j.erase("noise");
    if (c.isTitle()) {
        j.erase("source");
        j["title"] = titleToJson(*c.title);
    } else {
        j["sourceWidth"] = c.sourceWidth;
        j["sourceHeight"] = c.sourceHeight;
    }
    json in = lookToJson(c.look);
    in["cornerRadius"] = c.frame.cornerRadius;
    in["borderWidth"] = c.frame.borderWidth;
    in["borderColor"] = colorToJson(c.frame.borderRgb);
    j["inset"] = std::move(in);
    json keys = json::array();
    for (const InsetKey &k : c.insetKeys.keys()) {
        json jk = lookToJson(k.look);
        jk["tMs"] = static_cast<qint64>(k.timeMs);
        if (k.ease != keytrack::Ease::Linear)
            jk["ease"] = keytrack::easeName(k.ease);
        keys.push_back(std::move(jk));
    }
    j["insetKeys"] = std::move(keys);
    return j;
}

TrackClip videoClipFromJson(const json &j, const QDir *projectDir)
{
    TrackClip c = trackClipFromJson(j, projectDir);
    c.stream = -1;
    c.linkedClipId = 0;
    c.sourceWidth = j.value("sourceWidth", 0);
    c.sourceHeight = j.value("sourceHeight", 0);
    if (j.contains("title") && j.at("title").is_object()) {
        c.title = titleFromJson(j.at("title"));
        c.sourcePath.clear();
        c.mediaMissing = false;
    }
    if (j.contains("inset") && j.at("inset").is_object()) {
        const json &in = j.at("inset");
        c.look = lookFromJson(in);
        c.frame.cornerRadius = in.value("cornerRadius", c.frame.cornerRadius);
        c.frame.borderWidth = in.value("borderWidth", c.frame.borderWidth);
        c.frame.borderRgb = colorFromJson(in, "borderColor", c.frame.borderRgb);
    }
    if (j.contains("insetKeys") && j.at("insetKeys").is_array()) {
        for (const json &jk : j.at("insetKeys"))
            c.insetKeys.upsert(jk.value("tMs", qint64(0)), lookFromJson(jk), 0);
        // Once every key is in: a key's ease needs the next key there.
        for (const json &jk : j.at("insetKeys"))
            c.insetKeys.setEase(jk.value("tMs", qint64(0)), 0,
                                keytrack::easeFromName(jk.value("ease", std::string())));
    }
    return c;
}

const char *mediaKindName(MediaItem::Kind kind)
{
    switch (kind) {
    case MediaItem::Kind::Clip360:
        return "360";
    case MediaItem::Kind::Video:
        return "video";
    case MediaItem::Kind::Audio:
        break;
    }
    return "audio";
}

// A project from before version 10 (see kFormatVersion).
void convertClipSound(Timeline *timeline, const std::function<int(const QString &)> &counter,
                      const std::vector<ParsedClip> &parsed, const std::vector<int> &clipIds,
                      bool mainMuted)
{
    // Each clip played one of its streams itself (pc.audioTrack): now every
    // stream is a linked clip on its stream track. The one it played gets
    // its level, mute and noise reduction (and the video track's "M"); the
    // others are muted, so the project sounds as it did. A stream already put
    // on a track by hand ("Put the other mic on an audio track") moves to its
    // stream track too, with its track's level and mute folded into its own;
    // a track left empty by that goes.
    QList<int> emptied;
    for (size_t i = 0; i < parsed.size() && i < clipIds.size(); ++i) {
        const ParsedClip &pc = parsed[i];
        struct Placed {
            int id, stream;
            double gainDb;
            bool muted;
        };
        QList<Placed> placed; // its streams on free tracks
        int streams = counter && !pc.mediaMissing ? counter(pc.sourcePath)
                                                  : std::max(1, pc.audioTrack + 1);
        for (int t = 0; t < timeline->trackCount(); ++t) {
            const Track &track = timeline->trackAt(t);
            if (track.kind != Track::Kind::Audio || track.role != Track::Role::Free)
                continue;
            for (const TrackClip &a : track.clips)
                if (a.linkedClipId == clipIds[i] && a.stream >= 0) {
                    placed.append({a.id, a.stream, a.gainDb + track.gainDb,
                                   a.muted || track.muted});
                    if (!emptied.contains(track.id))
                        emptied.append(track.id);
                    if (pc.mediaMissing || !counter)
                        streams = std::max(streams, a.stream + 1);
                }
        }
        for (const Placed &a : placed) {
            const Track::Role role = Track::roleOfStream(a.stream, streams);
            if (role == Track::Role::Free)
                continue;
            int trackId = timeline->streamTrackId(role);
            if (!trackId)
                trackId = timeline->addTrack(Track::Kind::Audio, QString(), role);
            const TrackClip *c = timeline->trackClipById(a.id);
            timeline->moveTrackClip(a.id, trackId, c ? c->startMs : 0);
            timeline->setAudioClipLevel(a.id, a.gainDb, a.muted);
        }

        const QList<int> ids = timeline->addClipSound(clipIds[i], streams);
        const int picked = pc.audioTrack < streams ? pc.audioTrack : 0;
        for (int s = 0; s < ids.size(); ++s) {
            if (!ids[s])
                continue;
            if (s == picked) {
                timeline->setAudioClipLevel(ids[s], pc.audioGainDb, pc.audioMuted || mainMuted);
                timeline->setAudioClipNoise(ids[s], pc.noise);
            } else {
                timeline->setAudioClipLevel(ids[s], 0.0, true);
            }
        }
    }
    for (int trackId : emptied)
        if (const Track *t = timeline->trackById(trackId); t && t->clips.isEmpty())
            timeline->removeTrack(trackId);
}

} // namespace

Project::Project(QObject *parent)
    : QObject(parent)
    , m_timeline(new Timeline(this))
    , m_mediaPool(new MediaPool(this))
{
    connect(m_mediaPool, &MediaPool::changed, this, [this] {
        if (m_rebuilding)
            return;
        setModified(true);
        onUndoableEdited();
    });
    // Whatever goes on the timeline is in the pool too.
    connect(m_timeline, &Timeline::clipInserted, this, [this] { addTimelineSourcesToPool(); });
    connect(m_timeline, &Timeline::tracksChanged, this, [this] { addTimelineSourcesToPool(); });

    const auto edited = [this] { onTimelineEdited(); onUndoableEdited(); };
    connect(m_timeline, &Timeline::clipInserted, this, edited);
    connect(m_timeline, &Timeline::clipRemoved, this, edited);
    connect(m_timeline, &Timeline::clipsReordered, this, edited);
    connect(m_timeline, &Timeline::clipTrimChanged, this, edited);
    connect(m_timeline, &Timeline::clipReframeChanged, this, edited);
    connect(m_timeline, &Timeline::clipColorChanged, this, edited);
    connect(m_timeline, &Timeline::clipStabilizationChanged, this, edited);
    connect(m_timeline, &Timeline::clipPrivacyChanged, this, edited);
    connect(m_timeline, &Timeline::clipSpeedChanged, this, edited);
    connect(m_timeline, &Timeline::clipStitchChanged, this, edited);
    connect(m_timeline, &Timeline::tracksChanged, this, edited);
    connect(m_timeline, &Timeline::transitionsChanged, this, edited);
    // A measured sync or seam fine-tune is saved with the project but isn't
    // an edit: nothing to undo, and not an unsaved change.
    connect(m_timeline, &Timeline::clipSyncChanged, this, [this](int id) { onMeasured(id); });
    connect(m_timeline, &Timeline::clipSeamChanged, this, [this](int id) { onMeasured(id); });

    m_undoDebounce.setSingleShot(true);
    connect(&m_undoDebounce, &QTimer::timeout, this, &Project::commitPendingUndoPoint);
    m_lastSettledSnapshot = snapshot(); // baseline for the empty starting project
}

void Project::addTimelineSourcesToPool(bool rebuilt)
{
    // Mid-rebuild the timeline is half old, half new (clear() empties the
    // tracks before the clips): the rebuild calls this once at its end.
    if (m_rebuilding && !rebuilt)
        return;
    for (int i = 0; i < m_timeline->count(); ++i) {
        const ClipItem &c = m_timeline->clipAt(i);
        if (!m_mediaPool->contains(c.sourcePath))
            m_mediaPool->add({c.sourcePath,
                              c.flat ? MediaItem::Kind::Video : MediaItem::Kind::Clip360,
                              c.sourceDurationMs, c.sourceWidth, c.sourceHeight, c.fps});
    }
    for (int i = 0; i < m_timeline->trackCount(); ++i) {
        const Track &t = m_timeline->trackAt(i);
        const bool video = t.kind == Track::Kind::Video;
        for (const TrackClip &c : t.clips) {
            if (c.isTitle() || m_mediaPool->contains(c.sourcePath))
                continue;
            // A camera mic on an audio track is the .insv's sound: the pool
            // lists the clip, whose duration and size the main track knows.
            MediaItem m{c.sourcePath, video ? MediaItem::Kind::Video : MediaItem::Kind::Audio,
                        c.sourceDurationMs};
            if (video) {
                m.width = c.sourceWidth;
                m.height = c.sourceHeight;
            } else if (media::kindForPath(c.sourcePath) == MediaItem::Kind::Clip360) {
                m.kind = MediaItem::Kind::Clip360;
                for (int j = 0; j < m_timeline->count(); ++j)
                    if (m_timeline->clipAt(j).sourcePath == c.sourcePath) {
                        const ClipItem &v = m_timeline->clipAt(j);
                        m.width = v.sourceWidth;
                        m.height = v.sourceHeight;
                        m.fps = v.fps;
                        break;
                    }
            }
            m_mediaPool->add(m);
        }
    }
}

int Project::relinkMedia(const QString &from, const MediaItem &to)
{
    // The pool first: the timeline's change notices add sources missing from
    // it, and `to` is there by then.
    m_mediaPool->relink(from, to);
    return m_timeline->relinkSource(from, to.path);
}

QHash<QString, bool> Project::recheckMissingMedia()
{
    // One look per file; the pool lists every timeline source, but a clip
    // checked here doesn't depend on that.
    QHash<QString, bool> missing;
    const auto look = [&](const QString &path) {
        if (!path.isEmpty() && !missing.contains(path))
            missing.insert(path, !QFileInfo::exists(path));
    };
    for (const MediaItem &m : m_mediaPool->items())
        look(m.path);
    for (int i = 0; i < m_timeline->count(); ++i)
        look(m_timeline->clipAt(i).sourcePath);
    for (int i = 0; i < m_timeline->trackCount(); ++i)
        for (const TrackClip &c : m_timeline->trackAt(i).clips)
            look(c.sourcePath);

    QHash<QString, bool> changed;
    for (const QString &path : m_mediaPool->setMissing(missing))
        changed.insert(path, missing.value(path));
    for (const QString &path : m_timeline->setSourcesMissing(missing))
        changed.insert(path, missing.value(path));
    return changed;
}

void Project::onTimelineEdited()
{
    if (!m_rebuilding)
        setModified(true);
}

void Project::onMeasured(int clipId)
{
    const ClipItem *clip = m_timeline->clipById(clipId);
    if (!m_rebuilding && clip)
        writeMeasured(*clip);
}

void Project::writeMeasured(const ClipItem &clip)
{
    if (m_filePath.isEmpty())
        return;
    QFile in(m_filePath);
    if (!in.open(QIODevice::ReadOnly | QIODevice::Text))
        return;
    json doc;
    try {
        doc = json::parse(in.readAll().toStdString());
    } catch (const std::exception &) {
        return;
    }
    in.close();
    if (!doc.is_object() || !doc.contains("timeline") || !doc.at("timeline").is_object() ||
        !doc.at("timeline").contains("clips") || !doc.at("timeline").at("clips").is_array())
        return;

    const QDir projectDir = QFileInfo(m_filePath).absoluteDir();
    bool changed = false;
    for (json &jc : doc["timeline"]["clips"]) {
        if (!jc.is_object())
            continue;
        const QString source =
            resolvedPath(QString::fromStdString(jc.value("source", std::string())), projectDir);
        if (source != clip.sourcePath)
            continue;
        json updated = jc;
        if (!std::isnan(clip.syncAdjustMs))
            updated["syncAdjustMs"] = clip.syncAdjustMs;
        if (clip.seamAdjustGuard >= 0)
            updated["seamAdjust"] = json{
                {"deg", {clip.seamAdjustDeg[0], clip.seamAdjustDeg[1], clip.seamAdjustDeg[2]}},
                {"lensGuard", clip.seamAdjustGuard}};
        if (updated != jc) {
            jc = std::move(updated);
            changed = true;
        }
    }
    if (!changed)
        return;
    const QByteArray text = QByteArray::fromStdString(doc.dump()) + "\n";
    QSaveFile out(m_filePath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text) || out.write(text) != text.size() ||
        !out.commit())
        return;
    emit measurementsWritten();
}

void Project::setAudioStreamCounter(std::function<int(const QString &)> counter)
{
    m_audioStreamCounter = std::move(counter);
    m_timeline->setAudioStreamCounter(m_audioStreamCounter);
}

void Project::setModified(bool modified)
{
    if (m_modified == modified)
        return;
    m_modified = modified;
    emit modifiedChanged(m_modified);
}

void Project::setFilePath(const QString &path)
{
    if (m_filePath == path)
        return;
    m_filePath = path;
    emit filePathChanged(m_filePath);
}

void Project::setPreviewStitch(const stitch::PreviewStitchConfig &config)
{
    if (m_previewStitch == config)
        return;
    m_previewStitch = config;
    emit previewStitchChanged(m_previewStitch);
    if (!m_rebuilding) {
        setModified(true);
        onUndoableEdited();
    }
}

void Project::setOutput(const OutputSettings &output)
{
    if (m_output == output)
        return;
    m_output = output;
    emit outputChanged(m_output);
    if (!m_rebuilding) {
        setModified(true);
        onUndoableEdited();
    }
}

void Project::newProject()
{
    m_rebuilding = true;
    m_mediaPool->clear();
    m_timeline->clear();
    setPreviewStitch(stitch::PreviewStitchConfig{});
    setOutput(OutputSettings{});
    m_rebuilding = false;
    setFilePath(QString());
    setModified(false);
    clearUndoHistory();
    emit projectReset();
}

int Project::resetWithClip(const QString &sourcePath, qint64 durationMs, double fps, int width,
                           int height, int audioStreams)
{
    m_rebuilding = true;
    m_mediaPool->clear();
    m_timeline->clear();
    setPreviewStitch(stitch::PreviewStitchConfig{});
    setOutput(OutputSettings{});
    const int id = m_timeline->appendClip(sourcePath, durationMs, fps, width, height, audioStreams);
    addTimelineSourcesToPool(true);
    m_rebuilding = false;

    setFilePath(QString());
    setModified(false);
    clearUndoHistory();
    emit projectReset();
    return id;
}

QByteArray Project::buildDocument(const QDir *projectDir) const
{
    json clips = json::array();
    bool anyFlat = false;
    bool anyLut = false;
    // The first 360 clip's stitch settings, and whether every 360 clip has
    // them (else an older build, which reads one setting for all, misreads).
    const ClipItem *firstStitched = nullptr;
    bool mixedStitch = false;
    for (int i = 0; i < m_timeline->count(); ++i) {
        const ClipItem &c = m_timeline->clipAt(i);
        const std::string source =
            (projectDir ? storedPath(c.sourcePath, *projectDir) : c.sourcePath).toStdString();
        if (c.flat) {
            anyFlat = true;
            clips.push_back({
                {"source", source},
                {"flat", true},
                {"trimInMs", static_cast<qint64>(c.trimInMs)},
                {"trimOutMs", static_cast<qint64>(c.trimOutMs)},
                {"sourceDurationMs", static_cast<qint64>(c.sourceDurationMs)},
                {"fps", c.fps},
                {"sourceWidth", c.sourceWidth},
                {"sourceHeight", c.sourceHeight},
                {"color", colorGradeToJson(c.color, projectDir)},
                {"speed", c.speed},
            });
            anyLut = anyLut || !c.color.lut.isEmpty();
            if (c.fit == ClipItem::Fit::Fill)
                clips.back()["fit"] = "fill";
            continue;
        }
        clips.push_back({
            {"source", source},
            {"trimInMs", static_cast<qint64>(c.trimInMs)},
            {"trimOutMs", static_cast<qint64>(c.trimOutMs)},
            {"sourceDurationMs", static_cast<qint64>(c.sourceDurationMs)},
            {"fps", c.fps},
            {"sourceWidth", c.sourceWidth},
            {"sourceHeight", c.sourceHeight},
            {"reframe", reframeToJson(c.reframe)},
            {"color", colorGradeToJson(c.color, projectDir)},
            {"speed", c.speed},
            {"stabilization", stabilizationToJson(c.stabilization)},
            {"stitch", json{{"lensGuard", lensGuardName(c.lensGuard)}, {"depthSeam", c.depthSeam}}},
        });
        anyLut = anyLut || !c.color.lut.isEmpty();
        if (!firstStitched)
            firstStitched = &c;
        else if (c.lensGuard != firstStitched->lensGuard || c.depthSeam != firstStitched->depthSeam)
            mixedStitch = true;
        if (c.blurPlates || !c.plateScan.isEmpty() || !c.blurRemovals.empty() ||
            !c.blurAdditions.empty()) {
            json &jp = clips.back()["privacy"] = json::object();
            if (c.blurPlates)
                jp["plates"] = true;
            if (!c.plateScan.isEmpty())
                jp["scan"] = c.plateScan.toStdString();
            if (!c.blurRemovals.empty())
                jp["removed"] = blurRemovalsToJson(c.blurRemovals);
            if (!c.blurAdditions.empty())
                jp["added"] = blurAdditionsToJson(c.blurAdditions);
        }
        if (!std::isnan(c.syncAdjustMs))
            clips.back()["syncAdjustMs"] = c.syncAdjustMs; // absent until measured
        if (c.seamAdjustGuard >= 0) // absent until measured
            clips.back()["seamAdjust"] = json{
                {"deg", {c.seamAdjustDeg[0], c.seamAdjustDeg[1], c.seamAdjustDeg[2]}},
                {"lensGuard", c.seamAdjustGuard}};
    }

    // Each at its cut: the index of the clip before it.
    json transitions = json::array();
    for (int i = 0; i + 1 < m_timeline->count(); ++i)
        if (const Transition *t = m_timeline->transitionAfter(m_timeline->clipAt(i).id))
            transitions.push_back({{"cut", i},
                                   {"type", Transition::typeName(t->type)},
                                   {"durationMs", static_cast<qint64>(t->durationMs)}});

    json tracks = json::array();
    bool anyTitle = false;
    for (int i = 0; i < m_timeline->trackCount(); ++i) {
        const Track &t = m_timeline->trackAt(i);
        const bool video = t.kind == Track::Kind::Video;
        json tc = json::array();
        for (const TrackClip &c : t.clips) {
            if (video) {
                anyTitle |= c.isTitle();
                tc.push_back(videoClipToJson(c, projectDir));
                continue;
            }
            tc.push_back(trackClipToJson(
                c, projectDir, c.linkedClipId ? m_timeline->indexOfId(c.linkedClipId) : -1));
            int overlayTrackId = 0; // linked to an overlay clip: [track, clip] index
            if (c.linkedClipId && m_timeline->trackClipById(c.linkedClipId, &overlayTrackId)) {
                const Track &ot = *m_timeline->trackById(overlayTrackId);
                const auto at = std::find_if(ot.clips.begin(), ot.clips.end(),
                                             [&](const TrackClip &o) { return o.id == c.linkedClipId; });
                tc.back()["linkedOverlay"] = {m_timeline->trackIndexOfId(overlayTrackId),
                                              static_cast<int>(at - ot.clips.begin())};
                tc.back()["syncOffsetMs"] = static_cast<qint64>(c.syncOffsetMs);
            }
        }
        tracks.push_back({
            {"kind", video ? "video" : "audio"},
            {"name", t.name.toStdString()},
            {"gainDb", t.gainDb},
            {"muted", t.muted},
            {"clips", std::move(tc)},
        });
        if (t.role == Track::Role::CameraMic)
            tracks.back()["role"] = "cameraMic";
        else if (t.role == Track::Role::ExternalMic)
            tracks.back()["role"] = "externalMic";
        else if (t.role == Track::Role::OverlayAudio) {
            tracks.back()["role"] = "overlayAudio";
            tracks.back()["owner"] = m_timeline->trackIndexOfId(t.ownerTrackId);
        }
        // An undo snapshot keeps the track's id, so whatever the view keys by
        // it (a folded overlay track) survives the undo; not in the file.
        if (!projectDir)
            tracks.back()["id"] = t.id;
    }

    json media = json::array();
    for (const MediaItem &m : m_mediaPool->items()) {
        media.push_back({
            {"source", (projectDir ? storedPath(m.path, *projectDir) : m.path).toStdString()},
            {"kind", mediaKindName(m.kind)},
            {"durationMs", m.durationMs},
        });
        if (m.width > 0 && m.height > 0) {
            media.back()["width"] = m.width;
            media.back()["height"] = m.height;
        }
        if (m.fps > 0)
            media.back()["fps"] = m.fps;
    }

    // The project-wide stitch settings an older build reads: the clips' own.
    stitch::PreviewStitchConfig stitchForOlder = m_previewStitch;
    if (firstStitched) {
        stitchForOlder.lensGuard = firstStitched->lensGuard;
        stitchForOlder.depthSeam = firstStitched->depthSeam;
    }
    json doc = {
        {"format", kFormatTag},
        {"version", anyLut                  ? kLutFormatVersion
                    : !transitions.empty()    ? kTransitionFormatVersion
                    : anyTitle                ? kTitleFormatVersion
                    : anyFlat || mixedStitch ? kFlatClipFormatVersion
                                             : kPlainFormatVersion},
        {"previewStitch", previewStitchToJson(stitchForOlder)},
        {"output", outputToJson(m_output)},
        {"timeline",
         {{"clips", std::move(clips)},
          {"tracks", std::move(tracks)}}},
    };
    if (!transitions.empty())
        doc["timeline"]["transitions"] = std::move(transitions);
    if (!media.empty())
        doc["media"] = std::move(media);
    return QByteArray::fromStdString(doc.dump());
}

bool Project::applyDocument(const QByteArray &text, const QDir *projectDir, QString *error)
{
    std::vector<ParsedClip> parsed;
    std::vector<Track> parsedTracks; // ids unassigned; clips carry everything else
    // Per parsed track and clip: the [track, clip] index of the overlay clip
    // it's linked to, or {-1, -1}.
    std::vector<std::vector<std::pair<int, int>>> overlayLinks;
    stitch::PreviewStitchConfig parsedStitch;
    OutputSettings parsedOutput;
    bool parsedMainMuted = false;
    struct ParsedTransition {
        int cut = 0; // index of the clip before it
        Transition::Type type = Transition::Type::Crossfade;
        qint64 durationMs = Transition::kDefaultMs;
    };
    std::vector<ParsedTransition> parsedTransitions;
    int parsedVersion = kFormatVersion;
    QList<MediaItem> parsedMedia;
    try {
        const json doc = json::parse(text.begin(), text.end());

        const std::string format =
            doc.is_object() ? doc.value("format", std::string()) : std::string();
        if (format != kFormatTag) {
            if (error)
                *error = QStringLiteral("not an orbis-reframe project");
            return false;
        }
        const int version = doc.value("version", 0);
        if (version < kOldestFormatVersion || version > kFormatVersion) {
            if (error)
                *error = QStringLiteral("unsupported project version %1").arg(version);
            return false;
        }

        parsedVersion = version;
        if (doc.contains("previewStitch"))
            parsedStitch = previewStitchFromJson(doc.at("previewStitch"));
        // Version 8: one setting for the whole project, which every clip takes.
        insv::StabConfig projectStab;
        if (version == 8 && doc.contains("stabilization"))
            projectStab = stabilizationFromJson(doc.at("stabilization"));
        if (doc.contains("output"))
            parsedOutput = outputFromJson(doc.at("output"));

        const json &clips = doc.at("timeline").at("clips");
        for (const json &jc : clips) {
            ParsedClip pc;
            const QString stored = QString::fromStdString(jc.value("source", std::string()));
            pc.sourcePath = projectDir ? resolvedPath(stored, *projectDir) : stored;
            pc.sourceDurationMs = jc.value("sourceDurationMs", qint64(0));
            pc.fps = jc.value("fps", 0.0);
            if (!(pc.fps > 0.0)) {
                if (error)
                    *error = QStringLiteral("clip %1 has no frame rate").arg(stored);
                return false;
            }
            pc.sourceWidth = jc.value("sourceWidth", 0);
            pc.sourceHeight = jc.value("sourceHeight", 0);
            pc.trimInMs = jc.value("trimInMs", qint64(0));
            pc.trimOutMs = jc.value("trimOutMs", pc.sourceDurationMs);
            pc.audioGainDb = jc.value("audioGainDb", 0.0);
            pc.audioMuted = jc.value("audioMuted", false);
            pc.audioTrack = jc.value("audioTrack", 0);
            if (jc.contains("noise"))
                pc.noise = noiseFromJson(jc.at("noise"));
            if (jc.contains("color"))
                pc.color = colorGradeFromJson(jc.at("color"), projectDir);
            pc.speed = jc.value("speed", 1.0);
            pc.flat = jc.value("flat", false);
            if (pc.flat && jc.value("fit", std::string()) == "fill")
                pc.fit = ClipItem::Fit::Fill;
            pc.stabilization = projectStab;
            if (version >= 9 && jc.contains("stabilization"))
                pc.stabilization = stabilizationFromJson(jc.at("stabilization"));
            if (jc.contains("privacy") && jc.at("privacy").is_object()) {
                pc.blurPlates = jc.at("privacy").value("plates", false);
                pc.plateScan =
                    QString::fromStdString(jc.at("privacy").value("scan", std::string()));
                if (jc.at("privacy").contains("removed"))
                    pc.blurRemovals = blurRemovalsFromJson(jc.at("privacy").at("removed"));
                if (jc.at("privacy").contains("added"))
                    pc.blurAdditions = blurAdditionsFromJson(jc.at("privacy").at("added"));
            }
            // Before per-clip stitch settings: the project's, for every clip.
            pc.lensGuard = parsedStitch.lensGuard;
            pc.depthSeam = parsedStitch.depthSeam;
            if (jc.contains("stitch") && jc.at("stitch").is_object()) {
                lensGuardFromJson(jc.at("stitch"), pc.lensGuard);
                pc.depthSeam = jc.at("stitch").value("depthSeam", pc.depthSeam);
            }
            if (jc.contains("syncAdjustMs") && jc.at("syncAdjustMs").is_number())
                pc.syncAdjustMs = jc.at("syncAdjustMs").get<double>();
            if (jc.contains("seamAdjust") && jc.at("seamAdjust").is_object()) {
                const json &js = jc.at("seamAdjust");
                if (js.contains("deg") && js.at("deg").is_array() && js.at("deg").size() == 3 &&
                    js.contains("lensGuard") && js.at("lensGuard").is_number_integer()) {
                    for (int k = 0; k < 3; ++k)
                        pc.seamAdjustDeg[k] = js.at("deg")[k].get<double>();
                    pc.seamAdjustGuard = js.at("lensGuard").get<int>();
                }
            }
            pc.mediaMissing = !QFileInfo::exists(pc.sourcePath);
            if (jc.contains("reframe") && jc.at("reframe").contains("keys")) {
                for (const json &jk : jc.at("reframe").at("keys")) {
                    ReframeKey k;
                    k.timeMs = jk.value("tMs", qint64(0));
                    k.pose.yaw = jk.value("yaw", 0.0f);
                    k.pose.pitch = jk.value("pitch", 0.0f);
                    k.pose.roll = jk.value("roll", 0.0f);
                    k.pose.fovY = jk.value("fovY", 1.309f);
                    k.ease = keytrack::easeFromName(jk.value("ease", std::string()));
                    pc.reframeKeys.push_back(k);
                }
            }
            parsed.push_back(std::move(pc));
        }

        // Extra tracks. An unknown track kind is skipped rather than failing
        // the load.
        const json &jtl = doc.at("timeline");
        parsedMainMuted = jtl.value("mainMuted", false);
        if (jtl.contains("transitions") && jtl.at("transitions").is_array())
            for (const json &jt : jtl.at("transitions")) {
                ParsedTransition pt;
                pt.cut = jt.value("cut", -1);
                pt.type = Transition::typeFromName(jt.value("type", std::string()).c_str());
                pt.durationMs = jt.value("durationMs", Transition::kDefaultMs);
                parsedTransitions.push_back(pt);
            }
        if (jtl.contains("tracks") && jtl.at("tracks").is_array()) {
            for (const json &jt : jtl.at("tracks")) {
                const std::string kind = jt.value("kind", std::string());
                if (kind != "audio" && kind != "video")
                    continue;
                Track t;
                t.kind = kind == "video" ? Track::Kind::Video : Track::Kind::Audio;
                t.name = QString::fromStdString(jt.value("name", std::string()));
                t.gainDb = jt.value("gainDb", 0.0);
                t.muted = jt.value("muted", false);
                t.id = projectDir ? 0 : jt.value("id", 0); // see buildDocument
                const std::string role = jt.value("role", std::string());
                if (t.kind == Track::Kind::Audio && role == "cameraMic")
                    t.role = Track::Role::CameraMic;
                else if (t.kind == Track::Kind::Audio && role == "externalMic")
                    t.role = Track::Role::ExternalMic;
                else if (t.kind == Track::Kind::Audio && role == "overlayAudio") {
                    t.role = Track::Role::OverlayAudio;
                    t.ownerTrackId = jt.value("owner", -1) + 1; // the owner's index + 1 for now
                }
                std::vector<std::pair<int, int>> links;
                if (jt.contains("clips"))
                    for (const json &jc : jt.at("clips")) {
                        t.clips.push_back(t.kind == Track::Kind::Video
                                              ? videoClipFromJson(jc, projectDir)
                                              : trackClipFromJson(jc, projectDir));
                        std::pair<int, int> link{-1, -1};
                        if (t.kind == Track::Kind::Audio && jc.contains("linkedOverlay")) {
                            const json &jl = jc.at("linkedOverlay");
                            if (jl.is_array() && jl.size() == 2)
                                link = {jl[0].get<int>(), jl[1].get<int>()};
                        }
                        links.push_back(link);
                    }
                parsedTracks.push_back(std::move(t));
                overlayLinks.push_back(std::move(links));
            }
        }

        // The media pool. An unknown kind is skipped like an unknown track.
        if (doc.contains("media") && doc.at("media").is_array()) {
            for (const json &jm : doc.at("media")) {
                const std::string kind = jm.value("kind", std::string());
                MediaItem m;
                if (kind == "360")
                    m.kind = MediaItem::Kind::Clip360;
                else if (kind == "video")
                    m.kind = MediaItem::Kind::Video;
                else if (kind == "audio")
                    m.kind = MediaItem::Kind::Audio;
                else
                    continue;
                const QString stored = QString::fromStdString(jm.value("source", std::string()));
                if (stored.isEmpty())
                    continue;
                m.path = projectDir ? resolvedPath(stored, *projectDir) : stored;
                m.durationMs = jm.value("durationMs", qint64(0));
                m.width = jm.value("width", 0);
                m.height = jm.value("height", 0);
                m.fps = jm.value("fps", 0.0);
                m.missing = !QFileInfo::exists(m.path);
                parsedMedia.append(m);
            }
        }
    } catch (const std::exception &e) {
        if (error)
            *error = QStringLiteral("could not be read as a project: %1")
                         .arg(QString::fromUtf8(e.what()));
        return false;
    }

    // Everything parsed -- now it's safe to replace the model. The pool first:
    // rebuilding the timeline adds the sources it lacks (an older project).
    m_rebuilding = true;
    m_mediaPool->setItems(parsedMedia);
    m_timeline->clear();
    setPreviewStitch(parsedStitch);
    setOutput(parsedOutput);
    std::vector<int> clipIds; // main-track index -> new id, for linked audio
    for (const ParsedClip &pc : parsed) {
        const int id = m_timeline->appendClip(pc.sourcePath, pc.sourceDurationMs, pc.fps,
                                              pc.sourceWidth, pc.sourceHeight, 0, pc.flat);
        m_timeline->setClipFit(id, pc.fit);
        // appendClip set the trim to the whole source; narrow it to the saved
        // range (in then out, so the min-gap clamp doesn't fight a valid pair).
        m_timeline->setClipTrimIn(id, pc.trimInMs);
        m_timeline->setClipTrimOut(id, pc.trimOutMs);
        m_timeline->setClipColor(id, pc.color);
        m_timeline->setClipStabilization(id, pc.stabilization);
        m_timeline->setClipBlurPlates(id, pc.blurPlates);
        m_timeline->setClipPlateScan(id, pc.plateScan);
        m_timeline->setClipBlurRemovals(id, pc.blurRemovals);
        m_timeline->setClipBlurAdditions(id, pc.blurAdditions);
        m_timeline->setClipSpeed(id, pc.speed);
        if (!pc.flat)
            m_timeline->setSourceStitch(pc.sourcePath, pc.lensGuard, pc.depthSeam);
        if (!std::isnan(pc.syncAdjustMs))
            m_timeline->setSourceSyncAdjust(pc.sourcePath, pc.syncAdjustMs);
        if (pc.seamAdjustGuard >= 0)
            m_timeline->setSourceSeamAdjust(pc.sourcePath, pc.seamAdjustDeg, pc.seamAdjustGuard);
        for (const ReframeKey &k : pc.reframeKeys)
            m_timeline->upsertClipReframeKey(id, k.timeMs, k.pose);
        for (const ReframeKey &k : pc.reframeKeys) // once the next key is in
            if (k.ease != keytrack::Ease::Linear)
                m_timeline->setClipReframeEase(id, k.timeMs, k.ease);
        m_timeline->setClipMediaMissing(id, pc.mediaMissing);
        clipIds.push_back(id);
    }
    for (const ParsedTransition &pt : parsedTransitions) // (one past the last clip: refused)
        if (pt.cut >= 0 && pt.cut < static_cast<int>(clipIds.size()))
            m_timeline->setTransition(clipIds[pt.cut], pt.type, pt.durationMs);
    // Overlay clips before any audio, so sound linked to one finds its new
    // id; overlay audio tracks after their overlay tracks (one whose overlay
    // track is missing becomes a free audio track).
    std::vector<int> trackIds(parsedTracks.size(), 0);
    std::vector<std::vector<int>> overlayIds(parsedTracks.size()); // [track][clip] -> new id
    for (bool overlayAudio : {false, true})
        for (size_t i = 0; i < parsedTracks.size(); ++i) {
            const Track &t = parsedTracks[i];
            if ((t.role == Track::Role::OverlayAudio) != overlayAudio)
                continue;
            if (overlayAudio) {
                const int owner = t.ownerTrackId - 1;
                const bool ok = owner >= 0 && owner < static_cast<int>(parsedTracks.size()) &&
                                parsedTracks[owner].kind == Track::Kind::Video;
                trackIds[i] = m_timeline->addTrack(t.kind, t.name,
                                                   ok ? t.role : Track::Role::Free,
                                                   ok ? trackIds[owner] : 0, t.id);
            } else {
                trackIds[i] = m_timeline->addTrack(t.kind, t.name, t.role, 0, t.id);
            }
            m_timeline->setTrackAudio(trackIds[i], t.gainDb, t.muted);
            if (t.kind == Track::Kind::Video)
                for (const TrackClip &c : t.clips)
                    overlayIds[i].push_back(m_timeline->insertTrackClip(trackIds[i], c));
        }
    for (size_t i = 0; i < parsedTracks.size(); ++i) {
        const Track &t = parsedTracks[i];
        if (t.kind == Track::Kind::Video)
            continue;
        for (int k = 0; k < t.clips.size(); ++k) {
            TrackClip c = t.clips[k];
            const int idx = c.linkedClipId - 1;
            c.linkedClipId = idx >= 0 && idx < static_cast<int>(clipIds.size()) ? clipIds[idx] : 0;
            const auto [ot, oc] = overlayLinks[i][k];
            if (ot >= 0 && ot < static_cast<int>(overlayIds.size()) && oc >= 0 &&
                oc < static_cast<int>(overlayIds[ot].size()))
                c.linkedClipId = overlayIds[ot][oc];
            m_timeline->insertTrackClip(trackIds[i], c);
        }
    }
    // A free track that held only overlays' sound (from before overlay
    // audio tracks; it moved to them) goes.
    for (size_t i = 0; i < parsedTracks.size(); ++i)
        if (const Track *t = m_timeline->trackById(trackIds[i]);
            t && t->group() == Track::Group::Audio && t->clips.isEmpty() &&
            !parsedTracks[i].clips.isEmpty())
            m_timeline->removeTrack(trackIds[i]);
    if (parsedVersion < 10)
        convertClipSound(m_timeline, m_audioStreamCounter, parsed, clipIds, parsedMainMuted);
    addTimelineSourcesToPool(true); // an older project's sources; not an edit
    m_rebuilding = false;
    emit projectReset();
    return true;
}


bool Project::save(const QString &path, QString *error)
{
    const QByteArray text = fileContents(path);

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error)
            *error = QStringLiteral("Cannot open %1 for writing: %2").arg(path, file.errorString());
        return false;
    }
    if (file.write(text) != text.size() || !file.commit()) {
        if (error)
            *error = QStringLiteral("Failed to write %1: %2").arg(path, file.errorString());
        return false;
    }

    setFilePath(path);
    setModified(false);
    return true;
}

bool Project::copyFile(const QString &from, const QString &to, QString *error)
{
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    QFile in(from);
    if (!in.open(QIODevice::ReadOnly | QIODevice::Text))
        return fail(QStringLiteral("Cannot open %1: %2").arg(from, in.errorString()));
    json doc;
    try {
        doc = json::parse(in.readAll().toStdString());
    } catch (const std::exception &e) {
        return fail(QStringLiteral("%1 is not a valid project file: %2").arg(from, e.what()));
    }
    // Every media path in every version is a "source" string. A clip's
    // "lut" stays as it is: a project folder's copy brings its luts/ along,
    // and a loose project's copy goes beside it.
    const QDir fromDir = QFileInfo(from).absoluteDir();
    const QDir toDir = QFileInfo(to).absoluteDir();
    std::function<void(json &)> relocate = [&](json &j) {
        if (j.is_object()) {
            for (auto it = j.begin(); it != j.end(); ++it) {
                if (it.key() == "source" && it->is_string()) {
                    const QString stored = QString::fromStdString(it->get<std::string>());
                    if (!stored.isEmpty() && !QDir::isAbsolutePath(stored))
                        *it = storedPath(resolvedPath(stored, fromDir), toDir).toStdString();
                } else {
                    relocate(*it);
                }
            }
        } else if (j.is_array()) {
            for (json &e : j)
                relocate(e);
        }
    };
    relocate(doc);

    const QByteArray text = QByteArray::fromStdString(doc.dump()) + "\n";
    QSaveFile out(to);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text))
        return fail(QStringLiteral("Cannot open %1 for writing: %2").arg(to, out.errorString()));
    if (out.write(text) != text.size() || !out.commit())
        return fail(QStringLiteral("Failed to write %1: %2").arg(to, out.errorString()));
    return true;
}

QByteArray Project::fileContents(const QString &projectFile) const
{
    const QDir projectDir = QFileInfo(projectFile).absoluteDir();
    return buildDocument(&projectDir) + "\n";
}

bool Project::recover(const QString &autosaveFile, const QString &projectFile, QString *error)
{
    QFile file(autosaveFile);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error)
            *error = QStringLiteral("Cannot open %1: %2").arg(autosaveFile, file.errorString());
        return false;
    }
    const QDir projectDir = QFileInfo(projectFile).absoluteDir();
    QString innerError;
    if (!applyDocument(file.readAll(), &projectDir, &innerError)) {
        if (error)
            *error = QStringLiteral("%1 %2").arg(autosaveFile, innerError);
        return false;
    }
    setFilePath(projectFile);
    setModified(true);
    clearUndoHistory();
    return true;
}

bool Project::load(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error)
            *error = QStringLiteral("Cannot open %1: %2").arg(path, file.errorString());
        return false;
    }
    const QByteArray bytes = file.readAll();
    const QDir projectDir = QFileInfo(path).absoluteDir();

    QString innerError;
    if (!applyDocument(bytes, &projectDir, &innerError)) {
        if (error)
            *error = QStringLiteral("%1 %2").arg(path, innerError);
        return false;
    }

    setFilePath(path);
    setModified(false);
    clearUndoHistory();
    return true;
}

QSet<QString> Project::plateScansInUse() const
{
    QSet<QString> keys;
    for (int i = 0; i < m_timeline->count(); ++i)
        if (!m_timeline->clipAt(i).plateScan.isEmpty())
            keys.insert(m_timeline->clipAt(i).plateScan);
    QList<QByteArray> steps = m_undoStack + m_redoStack;
    steps.append(m_lastSettledSnapshot); // a burst not settled yet undoes to it
    for (const QByteArray &bytes : steps) {
        const json doc = json::parse(bytes.toStdString(), nullptr, false);
        if (!doc.is_object() || !doc.contains("timeline") || !doc.at("timeline").contains("clips"))
            continue;
        for (const json &jc : doc.at("timeline").at("clips"))
            if (jc.contains("privacy") && jc.at("privacy").is_object())
                if (const std::string k = jc.at("privacy").value("scan", std::string()); !k.empty())
                    keys.insert(QString::fromStdString(k));
    }
    return keys;
}

QByteArray Project::snapshot() const
{
    return buildDocument(nullptr);
}

void Project::restoreSnapshot(const QByteArray &bytes)
{
    applyDocument(bytes, nullptr, nullptr); // generated by us -- never expected to fail
    setModified(true); // may or may not match the saved file; treat it as unsaved either way
}

void Project::clearUndoHistory()
{
    m_undoDebounce.stop();
    m_pendingDirty = false;
    m_undoStack.clear();
    m_redoStack.clear();
    m_lastSettledSnapshot = snapshot();
    emit canUndoChanged(false);
    emit canRedoChanged(false);
}

void Project::onUndoableEdited()
{
    if (m_rebuilding)
        return;
    if (!m_pendingDirty) {
        m_pendingDirty = true;
        if (m_undoStack.isEmpty())
            emit canUndoChanged(true);
        if (!m_redoStack.isEmpty()) {
            m_redoStack.clear();
            emit canRedoChanged(false);
        }
    }
    m_undoDebounce.start(kUndoCoalesceMs);
}

void Project::commitPendingUndoPoint()
{
    if (!m_pendingDirty)
        return;
    m_undoDebounce.stop();
    m_pendingDirty = false;

    const QByteArray now = snapshot();
    if (now == m_lastSettledSnapshot) {
        if (m_undoStack.isEmpty())
            emit canUndoChanged(false); // the whole burst netted out to nothing
        return;
    }
    m_undoStack.push_back(m_lastSettledSnapshot);
    while (m_undoStack.size() > kMaxUndoDepth)
        m_undoStack.removeFirst();
    m_lastSettledSnapshot = now;
}

void Project::undo()
{
    commitPendingUndoPoint(); // don't drop an in-flight edit under Ctrl+Z
    if (m_undoStack.isEmpty())
        return;
    m_redoStack.push_back(m_lastSettledSnapshot);
    const QByteArray prev = m_undoStack.takeLast();
    restoreSnapshot(prev);
    m_lastSettledSnapshot = prev;
    emit canUndoChanged(!m_undoStack.isEmpty());
    emit canRedoChanged(true);
}

void Project::redo()
{
    commitPendingUndoPoint();
    if (m_redoStack.isEmpty())
        return;
    m_undoStack.push_back(m_lastSettledSnapshot);
    const QByteArray next = m_redoStack.takeLast();
    restoreSnapshot(next);
    m_lastSettledSnapshot = next;
    emit canUndoChanged(true);
    emit canRedoChanged(!m_redoStack.isEmpty());
}
