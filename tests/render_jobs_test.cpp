// src/ui/RenderJobs: the edit model as playback, the export and a
// screenshot are handed it -- the audio mix, the overlays (what's left out,
// in which order), titles, transitions (their layers and crossfading
// sound), a main-track clip's export job, and the suggested file names. No
// GPU, no media.

#include "RenderJobs.h"

#include "PrivacyBlur.h"
#include "../core/ClipItem.h"
#include "../core/ProjectFolder.h"
#include "../core/Timeline.h"

#include <QGuiApplication>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QTemporaryDir>

#include <cmath>
#include <cstdio>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                                 \
    do {                                                                                            \
        if (!(cond)) {                                                                              \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);          \
            ++g_failures;                                                                           \
        }                                                                                           \
    } while (0)

bool near(double a, double b, double eps = 1e-4) { return std::abs(a - b) < eps; }

void testMix()
{
    Timeline t;
    const int music = t.addTrack(Track::Kind::Audio, "Music");
    const int muted = t.addTrack(Track::Kind::Audio, "Muted");
    t.setTrackAudio(music, -6.0, false);
    t.setTrackAudio(muted, 0.0, true);
    const int a = t.addAudioClip(music, "/m/a.wav", 5000, 1000);
    t.setAudioClipLevel(a, -6.0, false);
    t.setTrackClipFades(a, 500, 250);
    const int quiet = t.addAudioClip(music, "/m/quiet.wav", 1000, 8000);
    t.setAudioClipLevel(quiet, 0.0, true);
    t.addAudioClip(music, "/m/gone.wav", 1000, 10000);
    t.addAudioClip(muted, "/m/b.wav", 1000, 0);
    t.setSourcesMissing({{"/m/gone.wav", true}});

    const std::vector<insv::MixClip> mix = renderjobs::mixClips(t);
    CHECK(mix.size() == 1); // muted track, muted clip and missing file left out
    if (mix.size() == 1) {
        const insv::MixClip &m = mix[0];
        CHECK(m.path == "/m/a.wav");
        CHECK(m.startFrame == 1000 * 48 && m.lengthFrames == 5000 * 48 && m.inFrame == 0);
        CHECK(near(m.gain, std::pow(10.0, -12.0 / 20.0))); // clip + track
        CHECK(m.fadeInFrames == 500 * 48 && m.fadeOutFrames == 250 * 48);
    }
}

void testOverlays()
{
    Timeline t;
    const int v1 = t.addTrack(Track::Kind::Video, "V1");
    const int v2 = t.addTrack(Track::Kind::Video, "V2");
    const int hidden = t.addTrack(Track::Kind::Video, "Hidden");
    t.setTrackAudio(hidden, 0.0, true);
    const int c1 = t.addVideoClip(v1, "/v/one.mp4", 4000, 0, 1920, 1080);
    const int c2 = t.addVideoClip(v2, "/v/two.mp4", 4000, 500, 1080, 1920);
    t.addVideoClip(hidden, "/v/hidden.mp4", 4000, 0, 1920, 1080);
    t.addVideoClip(v2, "/v/gone.mp4", 1000, 6000, 1920, 1080);
    t.setSourcesMissing({{"/v/gone.mp4", true}});
    inset::Look keyed;
    keyed.centerX = 0.25f;
    t.upsertTrackClipInsetKey(c2, 1000, keyed);

    const PrivacyBlur privacy;
    const std::vector<InsetFeed::Clip> feed = renderjobs::insetFeed(t, 1920, 1080, privacy);
    const std::vector<exportpipe::InsetClipJob> jobs = renderjobs::insetJobs(t, 1920, 1080, privacy);
    CHECK(feed.size() == 2 && jobs.size() == 2);
    if (feed.size() != 2 || jobs.size() != 2)
        return;
    // Bottom-most first: the track listed last draws first.
    const int i1 = t.trackIndexOfId(v1), i2 = t.trackIndexOfId(v2);
    const int first = i1 > i2 ? c1 : c2;
    CHECK(feed[0].id == first);
    CHECK(jobs[0].path == (first == c1 ? "/v/one.mp4" : "/v/two.mp4"));
    for (size_t k = 0; k < feed.size(); ++k) {
        CHECK(jobs[k].path == feed[k].path.toStdString());
        CHECK(jobs[k].startMs == feed[k].startMs && near(jobs[k].aspect, feed[k].aspect));
    }
    const size_t k2 = feed[0].id == c2 ? 0 : 1;
    CHECK(near(feed[k2].aspect, 1080.0 / 1920.0));
    CHECK(near(jobs[k2].lookAt(1000).centerX, 0.25f)); // its key, sampled
    CHECK(jobs[1 - k2].lookAt(1000).centerX != 0.25f); // the other keeps its own look
}

// A title is its text's picture, placed by its text size; blank text
// draws nothing.
void testTitles()
{
    Timeline t;
    t.appendClip("/c/ride.insv", 20000, 30.0);
    const int v = t.addTrack(Track::Kind::Video, "Text");
    title::Style style;
    style.text = "Hi";
    const int id = t.addTitleClip(v, 1000, style);
    title::Style blank;
    blank.text = "  ";
    t.addTitleClip(v, 8000, blank);

    const PrivacyBlur privacy;
    const std::vector<InsetFeed::Clip> feed = renderjobs::insetFeed(t, 1920, 1080, privacy);
    const std::vector<exportpipe::InsetClipJob> jobs = renderjobs::insetJobs(t, 1920, 1080, privacy);
    CHECK(feed.size() == 1 && jobs.size() == 1);
    if (feed.size() != 1 || jobs.size() != 1)
        return;
    const InsetFeed::Clip &f = feed[0];
    CHECK(f.id == id && f.path.isEmpty() && f.still && f.stillW > 0 && f.stillH > 0);
    CHECK(f.stillW > f.stillH); // "Hi" is wider than tall
    // Some of the picture is text, some clear.
    int opaque = 0, clear = 0;
    for (size_t i = 3; i < f.still->size(); i += 4)
        (*f.still)[i] == 255 ? ++opaque : (*f.still)[i] == 0 ? ++clear : 0;
    CHECK(opaque > 0 && clear > 0);
    const exportpipe::InsetClipJob &j = jobs[0];
    CHECK(j.still && near(j.aspect, double(j.stillW) / j.stillH));
    // The picture's height follows the text size: ~8% of 1080 for the font,
    // plus the line's leading and padding.
    const inset::Look l = j.lookAt(t.trackClipById(id)->trimInMs);
    const inset::Rect r = inset::place(l, j.frame, j.aspect, 1920, 1080);
    CHECK(r.h > 0.08 * 1080 && r.h < 0.08 * 1080 * 2.0);
    CHECK(near(r.x + r.w / 2, 960.0, 1.0) && near(r.y + r.h / 2, 540.0, 1.0));
    // Playback's picture is smaller or the same, placed the same.
    CHECK(near(double(f.stillW) / f.stillH, j.aspect, 0.05));
    CHECK(near(l.width, f.look.width * f.stillWidthPerSize, 0.01));
}

// A transition: the other clip as a layer over each half, blending through
// the cut; the clips' own sound running on past it, crossfading.
void testTransitions()
{
    Timeline t;
    const int a = t.appendClip("/c/a.insv", 20000, 30.0, 7680, 3840, 1);
    const int b = t.appendClip("/c/b.insv", 20000, 30.0, 7680, 3840, 1);
    const int c = t.appendClip("/c/c.mp4", 20000, 30.0, 1920, 1080, 0, true);
    t.setClipTrimIn(a, 2000);
    t.setClipTrimOut(a, 10000); // 8 s
    t.setClipTrimIn(b, 3000);
    t.setClipTrimOut(b, 13000);
    t.setClipSpeed(b, 2.0); // 5 s
    t.setClipTrimIn(c, 4000);
    t.setTransition(a, Transition::Type::Crossfade, 1000); // the cut at 8 s
    t.setTransition(b, Transition::Type::Crossfade, 2000); // the cut at 13 s

    const std::vector<renderjobs::TransitionLayer> layers = renderjobs::transitionLayers(t);
    CHECK(layers.size() == 4);
    if (layers.size() != 4)
        return;
    // a | b: b from before its trim-in (at double speed), blending in to
    // half; then a past its trim-out, blending out from half.
    const renderjobs::TransitionLayer &in = layers[0], &out = layers[1];
    CHECK(in.clipIndex == 1 && in.startMs == 7500 && in.trimInMs == 2000 && in.trimOutMs == 3000);
    CHECK(in.rampFrom == 0.0f && in.rampTo == 0.5f);
    CHECK(out.clipIndex == 0 && out.startMs == 8000 && out.trimInMs == 10000 &&
          out.trimOutMs == 10500);
    CHECK(out.rampFrom == 0.5f && out.rampTo == 0.0f);
    CHECK(in.id < 0 && out.id < 0 && in.id != out.id && layers[2].id != in.id);
    CHECK(in.overClipId == a && out.overClipId == b); // each over the other clip
    CHECK(layers[2].clipIndex == 2 && layers[2].startMs == 12000 && layers[3].clipIndex == 1);

    const PrivacyBlur privacy;
    const std::vector<InsetFeed::Clip> feed = renderjobs::insetFeed(t, 1920, 1080, privacy);
    const std::vector<exportpipe::InsetClipJob> jobs = renderjobs::insetJobs(t, 1920, 1080, privacy);
    // The flat clip's own layer, then the four transition layers.
    CHECK(feed.size() == 5 && jobs.size() == 5);
    if (feed.size() != 5 || jobs.size() != 5)
        return;
    CHECK(feed[0].id == c && !feed[0].picture);
    const InsetFeed::Clip &f = feed[1];
    CHECK(f.id == in.id && f.fullFrame && f.picture && !f.pictureKey.isEmpty());
    CHECK(f.startMs == 7500 && f.trimInMs == 2000 && f.speed == 2.0 && f.spanMs() == 500);
    CHECK(near(f.aspect, 1920.0 / 1080.0) && near(f.look.width, 1.0));
    CHECK(f.rampFrom == 0.0f && f.rampTo == 0.5f);
    CHECK(jobs[1].picture && jobs[1].rampTo == 0.5f && jobs[1].startMs == 7500);
    // b | c: the flat clip blends in as its file's frames.
    CHECK(feed[3].path == "/c/c.mp4" && !feed[3].picture && feed[3].trimInMs == 3000);
    // A new setting of the clip it shows: a new picture.
    const QString key = f.pictureKey;
    ColorGrade g;
    g.exposureEv = 0.5f;
    t.setClipColor(b, g);
    CHECK(renderjobs::insetFeed(t, 1920, 1080, privacy)[1].pictureKey != key);

    // The sound: a's runs half the transition past the cut and fades out
    // over all of it, b's starts half of it early and fades in.
    std::vector<insv::MixClip> mix = renderjobs::mixClips(t);
    const auto clipOf = [&](const char *path) -> const insv::MixClip * {
        for (const insv::MixClip &m : mix)
            if (m.path == path)
                return &m;
        return nullptr;
    };
    const insv::MixClip *ma = clipOf("/c/a.insv");
    const insv::MixClip *mb = clipOf("/c/b.insv");
    CHECK(ma && mb);
    if (!ma || !mb)
        return;
    CHECK(ma->startFrame == 0 && ma->lengthFrames == 8500 * 48 && ma->fadeOutFrames == 1000 * 48);
    CHECK(ma->fadeInFrames == 0);
    CHECK(mb->startFrame == 7500 * 48 && mb->inFrame == 2000 * 48 && mb->fadeInFrames == 1000 * 48);
    // ... and at its own end into c (a 2 s transition).
    CHECK(mb->lengthFrames == (500 + 5000 + 1000) * 48 && mb->fadeOutFrames == 2000 * 48);

    // One that can't play (no footage before c's start) has no layers.
    t.setClipTrimIn(c, 0);
    CHECK(renderjobs::transitionLayers(t).size() == 2);
    mix = renderjobs::mixClips(t);
    mb = clipOf("/c/b.insv");
    CHECK(mb && mb->fadeOutFrames == 0);

    // A dip to black (b | c still can't play): black over a | b, in up to
    // the cut and out after it; the sound fades out and in, no longer.
    t.setTransition(a, Transition::Type::DipToBlack, 1000);
    const std::vector<renderjobs::TransitionLayer> dip = renderjobs::transitionLayers(t);
    CHECK(dip.size() == 2);
    if (dip.size() != 2)
        return;
    CHECK(dip[0].clipIndex == -1 && dip[0].startMs == 7500 &&
          dip[0].trimOutMs - dip[0].trimInMs == 500);
    CHECK(dip[0].rampFrom == 0.0f && dip[0].rampTo == 1.0f);
    CHECK(dip[0].overClipId == a && dip[1].overClipId == b);
    CHECK(dip[1].clipIndex == -1 && dip[1].startMs == 8000 && dip[1].rampFrom == 1.0f &&
          dip[1].rampTo == 0.0f);
    const std::vector<InsetFeed::Clip> dipFeed = renderjobs::insetFeed(t, 1920, 1080, privacy);
    const std::vector<exportpipe::InsetClipJob> dipJobs =
        renderjobs::insetJobs(t, 1920, 1080, privacy);
    CHECK(dipFeed.size() == 3 && dipJobs.size() == 3);
    if (dipFeed.size() != 3 || dipJobs.size() != 3)
        return;
    const InsetFeed::Clip &black = dipFeed[1];
    CHECK(black.id == dip[0].id && black.still && black.fullFrame && black.speed == 1.0);
    CHECK(black.spanMs() == 500 && black.rampTo == 1.0f && near(black.look.width, 1.0));
    CHECK(black.stillW > 0 && black.still->size() == size_t(black.stillW * black.stillH * 4));
    CHECK(black.still->at(0) == 0 && black.still->at(3) == 255); // opaque black
    CHECK(dipJobs[1].still == black.still && dipJobs[1].rampTo == 1.0f &&
          dipJobs[1].startMs == 7500);
    mix = renderjobs::mixClips(t);
    ma = clipOf("/c/a.insv");
    mb = clipOf("/c/b.insv");
    CHECK(ma && ma->lengthFrames == 8000 * 48 && ma->fadeOutFrames == 500 * 48);
    CHECK(mb && mb->startFrame == 8000 * 48 && mb->inFrame == 3000 * 48 &&
          mb->fadeInFrames == 500 * 48);
}

// Playback draws a transition's layer only over the main-track clip it
// blends with, and a little past its span: the main picture may lag the
// clock at the cut, and the layer goes with the picture, not the clock.
void testLayerOverMainClip()
{
    Timeline t;
    const int a = t.appendClip("/c/a.insv", 20000, 30.0, 7680, 3840, 1);
    const int b = t.appendClip("/c/b.insv", 20000, 30.0, 7680, 3840, 1);
    t.setClipTrimOut(a, 8000);
    t.setTransition(a, Transition::Type::DipToBlack, 1000); // black 7.5-8 s over a, 8-8.5 s over b
    InsetFeed feed;
    feed.setClips(renderjobs::insetFeed(t, 1920, 1080, PrivacyBlur()));
    const auto drawn = [&](qint64 ms, int mainClipId) {
        return feed.frameFor(ms, false, mainClipId);
    };
    CHECK(drawn(7600, a).size() == 1);
    CHECK(drawn(7600, b).empty() && drawn(7600, 0).empty());
    // Just past the cut, the main track still showing a: still a's layer,
    // at its end (all black).
    const std::vector<EquirectViewportWindow::InsetDraw> late = drawn(8010, a);
    CHECK(late.size() == 1 && late[0].look.opacity > 0.99f && late[0].locked);
    // b on screen: the layer going out over it.
    const std::vector<EquirectViewportWindow::InsetDraw> over = drawn(8010, b);
    CHECK(over.size() == 1 && over[0].look.opacity > 0.9f);
    CHECK(over.size() == 1 && late.size() == 1 && over[0].clipId != late[0].clipId);
    // b showing a frame early: already b's layer, at its start.
    CHECK(drawn(7990, b).size() == 1);
    // Well outside: none.
    CHECK(drawn(7000, a).empty() && drawn(9000, b).empty());
}

void testMissingSources()
{
    Timeline t;
    t.appendClip("/c/here.insv", 20000, 30.0);
    t.appendClip("/c/gone.insv", 20000, 30.0);
    t.appendClip("/c/gone.insv", 20000, 30.0); // the same file twice: listed once
    const int music = t.addTrack(Track::Kind::Audio, "Music");
    const int mutedTrack = t.addTrack(Track::Kind::Audio, "Muted");
    t.setTrackAudio(mutedTrack, 0.0, true);
    t.addAudioClip(music, "/m/song.wav", 5000, 0);
    const int quiet = t.addAudioClip(music, "/m/quiet.wav", 5000, 6000);
    t.setAudioClipLevel(quiet, 0.0, true);
    t.addAudioClip(mutedTrack, "/m/off.wav", 5000, 0);
    const int v1 = t.addTrack(Track::Kind::Video, "V1");
    const int hidden = t.addTrack(Track::Kind::Video, "Hidden");
    t.setTrackAudio(hidden, 0.0, true);
    t.addVideoClip(v1, "/v/inset.mp4", 4000, 0, 1920, 1080);
    t.addVideoClip(hidden, "/v/hidden.mp4", 4000, 0, 1920, 1080);

    CHECK(renderjobs::missingSources(t).isEmpty());

    t.setSourcesMissing({{"/c/gone.insv", true},
                         {"/m/song.wav", true},
                         {"/m/quiet.wav", true},
                         {"/m/off.wav", true},
                         {"/v/inset.mp4", true},
                         {"/v/hidden.mp4", true}});
    // Muted clip, muted track and hidden track aren't exported: not listed.
    CHECK((renderjobs::missingSources(t) ==
           QStringList{"/c/gone.insv", "/m/song.wav", "/v/inset.mp4"}));
}

void testClipJob(const QString &dir)
{
    Timeline t;
    const int id = t.appendClip("/c/ride.insv", 20000, 30.0);
    t.setClipTrimIn(id, 2000);
    t.setClipTrimOut(id, 9000);
    t.setClipSpeed(id, 2.0);
    ColorGrade g;
    g.exposureEv = 1.0f;
    t.setClipColor(id, g);
    t.setSourceSeamAdjust("/c/ride.insv", {-0.5, 0.25, 0.1}, 2);
    PrivacyBlur privacy;
    privacy.scans().setDirectory(dir);

    const ClipItem &c = t.clipAt(0);
    ReframeExportController::ClipJob job = renderjobs::clipJob(c, t, privacy);
    CHECK(job.clipPath == "/c/ride.insv" && job.trimInMs == 2000 && job.trimOutMs == 9000);
    CHECK(job.speed == 2.0 && job.colorGrade[0] == 1.0f);
    CHECK(job.seamAdjustDeg[1] == 0.25); // measured under Premium (2), the clip's
    CHECK(job.lensGuard == stitch::PreviewStitchConfig::LensGuard::Premium && job.depthSeam);
    CHECK(!job.stab);    // stabilization off
    CHECK(!job.poseAt);  // no keys: looking straight ahead
    CHECK(!job.blur);    // no blur
    t.setSourceStitch("/c/ride.insv", stitch::PreviewStitchConfig::LensGuard::Standard, false);
    job = renderjobs::clipJob(t.clipAt(0), t, privacy);
    CHECK(job.lensGuard == stitch::PreviewStitchConfig::LensGuard::Standard && !job.depthSeam);
    CHECK(job.seamAdjustDeg[1] == 0.0); // measured under another guard
    t.setSourceStitch("/c/ride.insv", stitch::PreviewStitchConfig::LensGuard::Premium, true);

    t.upsertClipReframeKey(id, 3000, {0.5f, 0.0f, 0.0f, 1.2f});
    job = renderjobs::clipJob(t.clipAt(0), t, privacy);
    CHECK(job.poseAt);
    if (job.poseAt) {
        float pose[4] = {};
        job.poseAt(5000, pose);
        CHECK(near(pose[0], 0.5f) && near(pose[3], 1.2f));
    }
}

void testSuggestedPaths(const QString &dir)
{
    CHECK(renderjobs::suggestedExportPath(QString()) == QDir::homePath() + "/timeline.mp4");
    const QString file = projectfolder::create(dir, "Ride");
    CHECK(!file.isEmpty());
    const QString first = renderjobs::suggestedExportPath(file);
    CHECK(first == QDir(projectfolder::exportsDir(file)).filePath("Ride.mp4"));
    QFile f(first);
    CHECK(f.open(QIODevice::WriteOnly));
    f.close();
    CHECK(renderjobs::suggestedExportPath(file) ==
          QDir(projectfolder::exportsDir(file)).filePath("Ride-2.mp4"));

    const QString shot = renderjobs::suggestedScreenshotPath(file, "/c/ride.insv", 62003);
    CHECK(shot == QDir(projectfolder::screenshotsDir(file)).filePath("Ride_01m02s003.png"));
    CHECK(renderjobs::suggestedScreenshotPath(QString(), "/c/ride.insv", -5)
              .endsWith("/ride_00m00s000.png")); // unsaved: named after the clip
}

} // namespace

int main(int argc, char **argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv); // fonts, for the titles
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    testMix();
    testMissingSources();
    testOverlays();
    testTitles();
    testTransitions();
    testLayerOverMainClip();
    testClipJob(tmp.path());
    testSuggestedPaths(tmp.path());
    if (g_failures == 0)
        std::printf("render_jobs_test: all checks passed\n");
    else
        std::printf("render_jobs_test: %d check(s) failed\n", g_failures);
    return g_failures;
}
