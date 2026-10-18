// Exportpipe::FlatVideoReader and InsetCompositor on generated video, plus the
// CPU export's colour grade (ReframeRenderer). Writes small MPEG-4 files whose
// frames each carry their index in their colour, then checks frame timing,
// seeking, rotation and the NV12 blend. Exit code is the failure count.

#include "FlatVideoReader.h"
#include "InsetCompositor.h"
#include "InsetGeometry.h"
#include "ReframeRenderer.h"
#include "support/TestMedia.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                                 \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::printf("FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond);                           \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

constexpr int kW = 320, kH = 180, kFps = 25, kFrames = 50;

// Frame i's luma: distinct steps a lossy encode can't blur together.
int lumaFor(int i) { return 20 + i * 4; }

// `rotationCw`: tag the stream with a display matrix. `split`: left half dark,
// right half bright (to see which way a rotation went) instead of solid.
bool writeVideo(const std::string &path, int rotationCw, bool split)
{
    testmedia::VideoSpec spec;
    spec.width = kW;
    spec.height = kH;
    spec.fps = kFps;
    spec.frames = kFrames;
    spec.rotationCw = rotationCw;
    spec.luma = [split](int i, int x, int) {
        return uint8_t(split ? (x < kW / 2 ? 40 : 220) : lumaFor(i));
    };
    return testmedia::writeVideo(path, spec);
}

// Which frame a decoded solid picture is, from its (full-range RGB) grey.
int frameIndexOf(const exportpipe::VideoFrame &f)
{
    const uint8_t *p = &f.rgba[(size_t(f.height / 2) * f.width + f.width / 2) * 4];
    // Limited-range luma L -> full-range grey (L - 16) * 255 / 219.
    const double luma = 16.0 + p[1] * 219.0 / 255.0;
    return static_cast<int>(std::lround((luma - 20.0) / 4.0));
}

void testReader(const std::string &dir)
{
    const std::string path = dir + "/solid.mp4";
    CHECK(writeVideo(path, 0, false));
    exportpipe::FlatVideoReader::Info info;
    std::string err;
    CHECK(exportpipe::FlatVideoReader::probe(path, info, err));
    CHECK(info.width == kW && info.height == kH);
    CHECK(std::abs(info.durationMs - 2000) <= 40 && std::abs(info.fps - kFps) < 0.01);

    exportpipe::FlatVideoReader r;
    CHECK(r.open(path, err));
    const exportpipe::VideoFrame *f = r.frameAt(0);
    CHECK(f && f->ptsMs == 0 && frameIndexOf(*f) == 0);
    f = r.frameAt(1000); // forward
    CHECK(f && f->ptsMs == 1000 && frameIndexOf(*f) == 25);
    f = r.frameAt(1030); // same frame still showing
    CHECK(f && f->ptsMs == 1000);
    f = r.frameAt(500); // backwards: seeks
    CHECK(f && f->ptsMs == 480 && frameIndexOf(*f) == 12);
    f = r.frameAt(1900);
    CHECK(f && frameIndexOf(*f) == 47);
    // Past the end the last frame holds. (The encoder's last frame doesn't
    // decode -- ffprobe misses it too -- so that's frame 48.)
    f = r.frameAt(5000);
    CHECK(f && frameIndexOf(*f) >= 48);

    CHECK(r.seek(800));
    exportpipe::VideoFrame v;
    CHECK(r.readNext(v) && v.ptsMs == 800 && frameIndexOf(v) == 20);
    CHECK(r.readNext(v) && v.ptsMs == 840 && frameIndexOf(v) == 21);
    f = r.frameAt(900); // continues from where readNext left off
    CHECK(f && f->ptsMs == 880);

    r.setOutputSize(160, 90);
    f = r.frameAt(900);
    CHECK(f && f->width == 160 && f->height == 90 && f->rgba.size() == 160u * 90 * 4);
}

void testRotation(const std::string &dir)
{
    const std::string path = dir + "/rot90.mp4";
    CHECK(writeVideo(path, 90, true));
    exportpipe::FlatVideoReader r;
    std::string err;
    CHECK(r.open(path, err));
    CHECK(r.info().width == kH && r.info().height == kW); // portrait
    const exportpipe::VideoFrame *f = r.frameAt(0);
    CHECK(f && f->width == kH && f->height == kW);
    // Turned clockwise: the dark left half is now on top.
    auto grey = [&](int x, int y) { return f->rgba[(size_t(y) * f->width + x) * 4 + 1]; };
    CHECK(grey(kH / 2, 20) < 60 && grey(kH / 2, kW - 20) > 200);
}

void testCompositor(const std::string &dir)
{
    const std::string path = dir + "/solid.mp4";
    constexpr int W = 640, H = 360;
    std::vector<uint8_t> y(size_t(W) * H, 200), uv(size_t(W) * H / 2, 128);

    exportpipe::InsetClipJob j;
    j.path = path;
    j.startMs = 1000;
    j.trimInMs = 0;
    j.trimOutMs = 2000;
    j.aspect = double(kW) / kH;
    j.lookAt = [](int64_t) {
        inset::Look l;
        l.centerX = 0.5f;
        l.centerY = 0.5f;
        l.width = 0.5f;
        return l;
    };
    exportpipe::InsetCompositor comp({j}, W, H);
    std::string err;

    // Before the clip starts: untouched.
    CHECK(comp.composite(500, y.data(), W, uv.data(), W, err));
    CHECK(y[size_t(H / 2) * W + W / 2] == 200);

    // 500 ms in: frame 12 fills the rect (160, 90) .. (480, 270).
    CHECK(comp.composite(1500, y.data(), W, uv.data(), W, err));
    const int centre = y[size_t(H / 2) * W + W / 2];
    CHECK(std::abs(centre - lumaFor(12)) <= 3);
    CHECK(y[size_t(H / 2) * W + 100] == 200 && y[size_t(40) * W + W / 2] == 200); // outside
    CHECK(std::abs(y[size_t(91) * W + 161] - lumaFor(12)) <= 3); // just inside the corner
    CHECK(std::abs(int(uv[size_t(H / 4) * W + W / 2]) - 128) <= 2); // grey stays grey

    // Half opacity blends halfway; an edge pixel is part-covered.
    std::vector<uint8_t> y2(size_t(W) * H, 200);
    exportpipe::VideoFrame pic;
    pic.width = pic.height = 4;
    pic.rgba.assign(4 * 4 * 4, 0); // black (limited luma 16), opaque
    for (size_t i = 3; i < pic.rgba.size(); i += 4)
        pic.rgba[i] = 255;
    inset::Rect rect;
    rect.x = 100.5;
    rect.y = 100;
    rect.w = 100;
    rect.h = 50;
    exportpipe::InsetCompositor::blendNv12(pic.rgba.data(), 4, 4, rect, 0.5f, 0xffffff, y2.data(),
                                           W, uv.data(), W, W, H);
    CHECK(y2[size_t(120) * W + 150] == 108); // (200 + 16) / 2
    CHECK(y2[size_t(120) * W + 100] == 154); // half-covered: 200 - 0.5 * 0.5 * 184
    CHECK(y2[size_t(120) * W + 99] == 200);

    // A border: its colour at the edge, the picture inside.
    std::vector<uint8_t> y3(size_t(W) * H, 128);
    rect.x = 100;
    rect.border = 4;
    exportpipe::InsetCompositor::blendNv12(pic.rgba.data(), 4, 4, rect, 1.0f, 0xffffff, y3.data(),
                                           W, uv.data(), W, W, H);
    CHECK(y3[size_t(120) * W + 101] == 235); // white border
    CHECK(y3[size_t(120) * W + 150] == 16);  // black picture

    // A title's picture: premultiplied, transparent around the letters. A
    // white left half over a clear right half leaves the right untouched.
    {
        exportpipe::InsetClipJob t;
        t.startMs = 0;
        t.trimInMs = 0;
        t.trimOutMs = 1000;
        t.aspect = 2.0;
        auto still = std::make_shared<std::vector<uint8_t>>(size_t(8) * 4 * 4, 0);
        for (int py = 0; py < 4; ++py)
            for (int px = 0; px < 4; ++px) {
                uint8_t *p = still->data() + (size_t(py) * 8 + px) * 4;
                p[0] = p[1] = p[2] = p[3] = 255;
            }
        t.still = still;
        t.stillW = 8;
        t.stillH = 4;
        t.lookAt = [](int64_t) {
            inset::Look l;
            l.centerX = 0.5f;
            l.centerY = 0.5f;
            l.width = 0.5f; // rect (160, 110) .. (480, 270)
            return l;
        };
        std::vector<uint8_t> y4(size_t(W) * H, 100), uv4(size_t(W) * H / 2, 128);
        exportpipe::InsetCompositor tc({t}, W, H);
        CHECK(tc.composite(500, y4.data(), W, uv4.data(), W, err));
        CHECK(y4[size_t(H / 2) * W + 200] == 235);          // white, opaque
        CHECK(y4[size_t(H / 2) * W + 440] == 100);          // clear: what was under it
        CHECK(std::abs(int(uv4[size_t(H / 4) * W + 440]) - 128) <= 1);
        CHECK(tc.composite(1500, y4.data(), W, uv4.data(), W, err)); // past it: no file to read
    }

    // A clip whose file is gone fails the export with the path.
    exportpipe::InsetClipJob bad = j;
    bad.path = dir + "/missing.mp4";
    exportpipe::InsetCompositor comp2({bad}, W, H);
    CHECK(!comp2.composite(1500, y.data(), W, uv.data(), W, err) && !err.empty());
}

// NV12 frames (Format::Nv12): via RGBA for a small clip (untagged, so
// BT.601 by its size), straight from the decoder for an HD one (BT.709),
// and turned for a rotated one.
void testReaderNv12(const std::string &dir)
{
    std::string err;
    exportpipe::FlatVideoReader r;
    CHECK(r.open(dir + "/solid.mp4", err));
    r.setOutputFormat(exportpipe::FlatVideoReader::Format::Nv12);
    const exportpipe::VideoFrame *f = r.frameAt(1000);
    CHECK(f && f->width == kW && f->height == kH && f->nv12.size() >= size_t(kW) * kH * 3 / 2);
    if (f) {
        CHECK(std::abs(f->nv12[size_t(kH / 2) * kW + kW / 2] - lumaFor(25)) <= 2); // grey
        CHECK(std::abs(f->nv12[size_t(kW) * kH + size_t(kH / 4) * kW + kW / 2] - 128) <= 2);
    }
    r.setOutputSize(161, 91); // NV12 wants even
    f = r.frameAt(1000);
    CHECK(f && f->width == 162 && f->height == 92);

    const std::string hd = dir + "/hd.mp4";
    testmedia::VideoSpec spec;
    spec.width = 1280;
    spec.height = 720;
    spec.frames = 5;
    spec.luma = [](int, int x, int) { return uint8_t(x < 640 ? 40 : 220); };
    CHECK(testmedia::writeVideo(hd, spec));
    exportpipe::FlatVideoReader h;
    CHECK(h.open(hd, err));
    h.setOutputFormat(exportpipe::FlatVideoReader::Format::Nv12);
    f = h.frameAt(0);
    CHECK(f && f->width == 1280 && f->height == 720);
    if (f) {
        CHECK(std::abs(f->nv12[360 * 1280 + 100] - 40) <= 3);
        CHECK(std::abs(f->nv12[360 * 1280 + 1200] - 220) <= 3);
        CHECK(std::abs(f->nv12[1280 * 720 + 180 * 1280 + 200] - 128) <= 2);
    }

    exportpipe::FlatVideoReader rot;
    CHECK(rot.open(dir + "/rot90.mp4", err));
    rot.setOutputFormat(exportpipe::FlatVideoReader::Format::Nv12);
    f = rot.frameAt(0);
    CHECK(f && f->width == kH && f->height == kW);
    if (f) { // the dark left half on top
        CHECK(f->nv12[size_t(20) * kH + kH / 2] < 60);
        CHECK(f->nv12[size_t(kW - 20) * kH + kH / 2] > 200);
    }
}

// A flat clip filling the frame is copied in as NV12, not blended: the same
// picture to its edges, graded too, and a fill's overhang cropped.
void testFullFrameCopy(const std::string &dir)
{
    constexpr int W = 640, H = 360;
    auto job = [](const std::string &path, double aspect, bool fill) {
        exportpipe::InsetClipJob j;
        j.path = path;
        j.trimOutMs = 2000;
        j.aspect = aspect;
        const inset::Look look = inset::fitted(aspect, W, H, fill);
        j.lookAt = [look](int64_t) { return look; };
        return j;
    };
    std::string err;
    std::vector<uint8_t> y(size_t(W) * H, 200), uv(size_t(W) * H / 2, 60);
    exportpipe::InsetCompositor comp({job(dir + "/solid.mp4", double(kW) / kH, false)}, W, H);
    CHECK(comp.composite(1000, y.data(), W, uv.data(), W, err));
    for (const size_t at : {size_t(0), size_t(W - 1), size_t(H / 2) * W + W / 2, size_t(H) * W - 1})
        CHECK(std::abs(y[at] - lumaFor(25)) <= 3);
    CHECK(std::abs(uv[0] - 128) <= 2 && std::abs(uv.back() - 128) <= 2);

    // Graded: what the screenshot's blend from RGBA gives, in NV12's luma.
    exportpipe::InsetClipJob g = job(dir + "/solid.mp4", double(kW) / kH, false);
    g.colorGrade = {1.0f, 0.0f, 0.0f, 0.0f}; // +1 EV
    exportpipe::InsetCompositor graded({g}, W, H);
    CHECK(graded.composite(1000, y.data(), W, uv.data(), W, err));
    std::vector<uint8_t> rgb(size_t(W) * H * 3, 0);
    exportpipe::InsetCompositor gradedRgb({g}, W, H);
    CHECK(gradedRgb.compositeRgb(1000, rgb.data(), W * 3, 3, err));
    const uint8_t *px = &rgb[(size_t(H / 2) * W + W / 2) * 3];
    const double luma709 =
        16.0 + 219.0 * (0.2126 * px[0] + 0.7152 * px[1] + 0.0722 * px[2]) / 255.0;
    CHECK(y[size_t(H / 2) * W + W / 2] > lumaFor(25) + 10);
    // (The copy decodes at the rect's size, the blend at the source's:
    // swscale's scaled YUV -> RGB comes out ~2 levels darker -- the ffmpeg
    // command line's too -- doubled by the +1 EV.)
    CHECK(std::abs(y[size_t(H / 2) * W + W / 2] - luma709) <= 5);

    // A 4:3 clip filling the 16:9 frame: 640 x 480, 60 rows cropped off the
    // top and bottom -- its dark top half ends at output row 180.
    const std::string tall = dir + "/tall.mp4";
    testmedia::VideoSpec spec;
    spec.width = 240;
    spec.height = 180;
    spec.frames = 5;
    spec.luma = [](int, int, int py) { return uint8_t(py < 90 ? 40 : 220); };
    CHECK(testmedia::writeVideo(tall, spec));
    exportpipe::InsetCompositor fill({job(tall, 4.0 / 3.0, true)}, W, H);
    std::fill(y.begin(), y.end(), 0);
    CHECK(fill.composite(0, y.data(), W, uv.data(), W, err));
    CHECK(std::abs(y[W / 2] - 40) <= 4 && std::abs(y[size_t(170) * W + W / 2] - 40) <= 4);
    CHECK(std::abs(y[size_t(190) * W + W / 2] - 220) <= 4);
    CHECK(std::abs(y[size_t(H - 1) * W + W / 2] - 220) <= 4);
}

// The CPU export's colour grade (ReframeRenderer's gradeRgb(), the twin of
// src/render/shaders/color_grade.glsl): a flat equirect reprojects to the same
// flat colour, graded. Expected values worked out by hand from the formula.
std::array<int, 3> gradedFlat(std::array<uint8_t, 3> rgb, std::array<float, 4> grade)
{
    constexpr int kEqW = 16, kEqH = 8, kOut = 4;
    std::vector<uint8_t> eq(size_t(kEqW) * kEqH * 4);
    for (size_t i = 0; i < eq.size(); i += 4)
        eq[i] = rgb[0], eq[i + 1] = rgb[1], eq[i + 2] = rgb[2], eq[i + 3] = 255;
    std::vector<uint8_t> out(size_t(kOut) * kOut * 4);
    exportpipe::ReframeView view;
    std::copy(grade.begin(), grade.end(), view.colorGrade);
    exportpipe::RenderRectilinear(eq.data(), kEqW, kEqH, out.data(), kOut, kOut, view);
    return {out[0], out[1], out[2]};
}

void testColorGrade()
{
    using RGB = std::array<int, 3>;
    CHECK((gradedFlat({60, 90, 120}, {0, 0, 0, 0}) == RGB{60, 90, 120})); // neutral
    // exposure +1, contrast +30, saturation -40, temperature +60
    CHECK((gradedFlat({60, 90, 120}, {1, 30, -40, 60}) == RGB{185, 194, 204}));
    // exposure -1, contrast -50, saturation +100, temperature -100
    CHECK((gradedFlat({60, 90, 120}, {-1, -50, 100, -100}) == RGB{55, 90, 125}));
    CHECK((gradedFlat({100, 150, 200}, {1, 30, -40, 60}) == RGB{255, 255, 255})); // clamps
}

} // namespace

// A layer whose pictures come from a PictureSource (a transition's other
// clip, a 360 clip's view): asked for at its source time and at the rect's
// size, faded by its opacity ramp; one that can't open fails the frame.
namespace {

struct FakePicture : exportpipe::PictureSource {
    std::vector<int64_t> *asked;
    bool ok;
    int w = 0, h = 0;
    exportpipe::VideoFrame frame;
    std::string error;
    FakePicture(std::vector<int64_t> *a, bool o) : asked(a), ok(o) {}
    bool open(std::string &err) override
    {
        if (!ok)
            err = "no such clip";
        return ok;
    }
    double fps() const override { return 30.0; }
    void setOutputSize(int width, int height) override
    {
        w = width;
        h = height;
    }
    const exportpipe::VideoFrame *frameAt(int64_t ms) override
    {
        asked->push_back(ms);
        frame.ptsMs = ms;
        frame.width = w;
        frame.height = h;
        frame.rgba.assign(size_t(w) * h * 4, 255); // white
        return &frame;
    }
    bool readNext(exportpipe::VideoFrame &) override { return false; }
    const std::string &lastError() const override { return error; }
};

} // namespace

void testPictureLayer()
{
    constexpr int W = 64, H = 36;
    std::vector<int64_t> asked;
    exportpipe::InsetClipJob j;
    j.path = "/c/b.insv";
    j.startMs = 1000;
    j.trimInMs = 4000;
    j.trimOutMs = 6000;
    j.speed = 2.0; // 1 s on the timeline
    j.aspect = double(W) / H;
    j.rampFrom = 0.0f;
    j.rampTo = 0.5f;
    j.lookAt = [](int64_t) { return inset::fitted(double(W) / H, W, H, false); };
    j.picture = [&asked] { return std::make_unique<FakePicture>(&asked, true); };
    exportpipe::InsetCompositor comp({j}, W, H);
    std::vector<uint8_t> y(size_t(W) * H), uv(size_t(W) * H / 2);
    std::string err;
    const auto lumaAt = [&](int64_t ms) {
        std::fill(y.begin(), y.end(), 16); // black
        std::fill(uv.begin(), uv.end(), 128);
        CHECK(comp.composite(ms, y.data(), W, uv.data(), W, err));
        return int(y[size_t(H / 2) * W + W / 2]);
    };
    CHECK(lumaAt(500) == 16 && asked.empty()); // before it
    CHECK(lumaAt(1000) == 16);                 // opacity 0 at its start
    CHECK(std::abs(lumaAt(1500) - (16 + 219 / 4)) <= 2); // a quarter
    CHECK(std::abs(lumaAt(1990) - (16 + 219 / 2)) <= 3); // nearly half at its end
    CHECK(lumaAt(2000) == 16); // past it
    CHECK(asked.size() >= 2 && asked.back() == 4000 + 990 * 2); // its source time
    exportpipe::InsetClipJob bad = j;
    bad.picture = [&asked] { return std::make_unique<FakePicture>(&asked, false); };
    exportpipe::InsetCompositor failing({bad}, W, H);
    CHECK(!failing.composite(1500, y.data(), W, uv.data(), W, err));
    CHECK(err.find("/c/b.insv") != std::string::npos && err.find("no such clip") != std::string::npos);
}

// inset::fitted: a flat main-track clip's full-frame layer.
void testFitted()
{
    // 16:9 into 16:9: the whole frame either way
    CHECK(inset::fitted(16.0 / 9.0, 1920, 1080, false).width == 1.0f);
    CHECK(inset::fitted(16.0 / 9.0, 1920, 1080, true).width == 1.0f);
    // portrait 9:16 into 16:9: bars at the sides, or as wide as the frame
    const inset::Look bars = inset::fitted(9.0 / 16.0, 1920, 1080, false);
    const inset::Rect r = inset::place(bars, {}, 9.0 / 16.0, 1920, 1080);
    CHECK(std::abs(r.h - 1080) < 0.5 && std::abs(r.x - (1920 - r.w) / 2) < 0.5 && r.y > -0.5);
    const inset::Rect f =
        inset::place(inset::fitted(9.0 / 16.0, 1920, 1080, true), {}, 9.0 / 16.0, 1920, 1080);
    CHECK(std::abs(f.w - 1920) < 0.5 && f.h > 1080 && std::abs(f.y + (f.h - 1080) / 2) < 0.5);
    // 16:9 into a 9:16 output: bars above and below, or cropped at the sides
    const inset::Rect w =
        inset::place(inset::fitted(16.0 / 9.0, 1080, 1920, false), {}, 16.0 / 9.0, 1080, 1920);
    CHECK(std::abs(w.w - 1080) < 0.5 && w.h < 1920);
    CHECK(inset::fitted(16.0 / 9.0, 1080, 1920, true).width > 3.0f);
}

int main()
{
    const char *tmp = std::getenv("TMPDIR");
    std::string dir = std::string(tmp && *tmp ? tmp : "/tmp") + "/inset_test_XXXXXX";
    if (!mkdtemp(dir.data())) {
        std::printf("inset_test: cannot create a temp dir\n");
        return 1;
    }
    testReader(dir);
    testRotation(dir);
    testCompositor(dir);
    testReaderNv12(dir);
    testFullFrameCopy(dir);
    testColorGrade();
    testFitted();
    testPictureLayer();
    if (!std::getenv("INSET_TEST_KEEP")) {
        for (const char *f : {"/solid.mp4", "/rot90.mp4", "/hd.mp4", "/tall.mp4"})
            std::remove((dir + f).c_str());
        std::remove(dir.c_str());
    } else {
        std::printf("inset_test: files kept in %s\n", dir.c_str());
    }

    if (g_failures == 0)
        std::printf("inset_test: all checks passed\n");
    else
        std::printf("inset_test: %d check(s) failed\n", g_failures);
    return g_failures;
}
