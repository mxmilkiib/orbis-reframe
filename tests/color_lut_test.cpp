// Colour LUTs: the .cube reader (sizes, domain, comments, what it refuses),
// the trilinear lookup the GPU's sampler matches, the per-path cache, and a
// clip's ColorGrade carrying one (render(), equality).
// Standalone; the exit code is the failure count.

#include "support/ModelTest.h"

#include "ColorLut.h"

#include <QTemporaryDir>

#include <string>

namespace {

// A size^3 .cube whose output is f(r, g, b) at each grid point.
template <typename F>
std::string cubeText(int size, F f, const std::string &header = {})
{
    std::string s = "# made by color_lut_test\nTITLE \"test\"\n" + header +
                    "LUT_3D_SIZE " + std::to_string(size) + "\n";
    for (int b = 0; b < size; ++b)
        for (int g = 0; g < size; ++g)
            for (int r = 0; r < size; ++r) {
                float out[3];
                f(r / float(size - 1), g / float(size - 1), b / float(size - 1), out);
                s += std::to_string(out[0]) + " " + std::to_string(out[1]) + " " +
                     std::to_string(out[2]) + "\n";
            }
    return s;
}

bool near(float a, float b, float tol = 1e-4f)
{
    return std::abs(a - b) <= tol;
}

void testParse()
{
    std::string err;
    // Red fastest: entry (r=1, g=0, b=0) is the second line.
    auto lut = color::parseCube(cubeText(2, [](float r, float g, float b, float *o) {
                                    o[0] = r, o[1] = g, o[2] = b;
                                }),
                                &err);
    CHECK(lut && err.empty());
    CHECK(lut->size == 2 && lut->title == "test" && lut->rgb.size() == 8 * 3);
    CHECK(lut->rgb[3] == 1.0f && lut->rgb[4] == 0.0f && lut->rgb[5] == 0.0f);
    CHECK(lut->domainMin == 0.0f && lut->domainMax == 1.0f);

    // CRLF line ends, comments after values, blank lines, keywords we skip.
    std::string crlf = "TITLE \"x\"\r\nLUT_IN_VIDEO_RANGE\r\n\r\nLUT_3D_SIZE 2\r\n";
    for (int i = 0; i < 8; ++i)
        crlf += "0.5 0.5 0.5 # grey\r\n";
    lut = color::parseCube(crlf, &err);
    CHECK(lut && lut->rgb[0] == 0.5f);

    // A domain, the same for all three.
    lut = color::parseCube(cubeText(2, [](float r, float g, float b, float *o) {
                               o[0] = r, o[1] = g, o[2] = b;
                           },
                                    "DOMAIN_MIN 0 0 0\nDOMAIN_MAX 2 2 2\n"),
                           &err);
    CHECK(lut && lut->domainMax == 2.0f);

    // Refused, with a reason.
    CHECK(!color::parseCube("LUT_1D_SIZE 4\n0 0 0\n", &err) && err.find("1D") != std::string::npos);
    CHECK(!color::parseCube("TITLE \"empty\"\n", &err) && !err.empty());
    CHECK(!color::parseCube("LUT_3D_SIZE 2\n0 0 0\n", &err) &&
          err.find("expected 8") != std::string::npos);
    CHECK(!color::parseCube("LUT_3D_SIZE 1\n", &err));
    CHECK(!color::parseCube("LUT_3D_SIZE 2\n0 0\n", &err) && err.find("line 2") == 0);
    CHECK(!color::parseCube("0 0 0\nLUT_3D_SIZE 2\n", &err));
    CHECK(!color::parseCube(cubeText(2, [](float, float, float, float *o) { o[0] = o[1] = o[2] = 0; },
                                     "DOMAIN_MIN 0 0 0\nDOMAIN_MAX 1 2 1\n"),
                            &err) &&
          err.find("DOMAIN") != std::string::npos);
    std::string extra = cubeText(2, [](float, float, float, float *o) { o[0] = o[1] = o[2] = 0; });
    CHECK(!color::parseCube(extra + "0 0 0\n", &err));
}

void testApply()
{
    std::string err;
    // Identity at any size: the lookup gives the input back.
    auto id = color::parseCube(cubeText(5, [](float r, float g, float b, float *o) {
                                   o[0] = r, o[1] = g, o[2] = b;
                               }),
                               &err);
    float in[3] = {0.3f, 0.71f, 0.05f}, out[3];
    id->apply(in, out);
    CHECK(near(out[0], 0.3f) && near(out[1], 0.71f) && near(out[2], 0.05f));
    // Out of range clamps to the edge.
    float hi[3] = {1.5f, -0.2f, 1.0f};
    id->apply(hi, out);
    CHECK(near(out[0], 1.0f) && near(out[1], 0.0f) && near(out[2], 1.0f));

    // Trilinear: a LUT that's linear in its input is exact between points.
    auto lin = color::parseCube(cubeText(3, [](float r, float g, float b, float *o) {
                                    o[0] = 1.0f - r;
                                    o[1] = 0.5f * g + 0.25f * b;
                                    o[2] = (r + g + b) / 3.0f;
                                }),
                                &err);
    lin->apply(in, out);
    CHECK(near(out[0], 0.7f) && near(out[1], 0.5f * 0.71f + 0.25f * 0.05f) &&
          near(out[2], (0.3f + 0.71f + 0.05f) / 3.0f));

    // A domain of [0, 2]: input 1 is the middle of the grid.
    auto wide = color::parseCube(cubeText(3, [](float r, float g, float b, float *o) {
                                     o[0] = r, o[1] = g, o[2] = b;
                                 },
                                          "DOMAIN_MIN 0 0 0\nDOMAIN_MAX 2 2 2\n"),
                                 &err);
    float one[3] = {1.0f, 1.0f, 0.5f};
    wide->apply(one, out);
    CHECK(near(out[0], 0.5f) && near(out[2], 0.25f));
}

void testCacheAndGrade()
{
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    const QString path = tmp.path() + "/Invert.cube";
    auto write = [&](float scale) {
        QFile f(path);
        CHECK(f.open(QIODevice::WriteOnly));
        f.write(QByteArray::fromStdString(cubeText(2, [scale](float r, float g, float b, float *o) {
            o[0] = (1 - r) * scale, o[1] = (1 - g) * scale, o[2] = (1 - b) * scale;
        })));
    };
    write(1.0f);
    std::string err;
    auto a = color::cachedCube(path.toStdString(), &err);
    CHECK(a && color::cachedCube(path.toStdString()) == a); // read once
    CHECK(!color::cachedCube((tmp.path() + "/none.cube").toStdString(), &err) &&
          err.find("missing") != std::string::npos);

    // A clip's grade: render() loads it; strength becomes the mix.
    ColorGrade g;
    CHECK(g.render().isNeutral());
    g.lut = path;
    g.lutStrength = 40.0f;
    const color::Grade r = g.render();
    CHECK(r.lut == a && near(r.lutMix, 0.4f) && r.hasLut() && !r.isNeutral());
    // Equality: the LUT counts, its strength only with a LUT.
    ColorGrade h = g;
    CHECK(h == g);
    h.lutStrength = 50.0f;
    CHECK(h != g);
    ColorGrade none1, none2;
    none2.lutStrength = 20.0f;
    CHECK(none1 == none2);
    // clamp() keeps the strength in 0..100.
    h.lutStrength = 150.0f;
    h.clamp();
    CHECK(h.lutStrength == 100.0f);
    // A missing file: no LUT applied, and why.
    g.lut = tmp.path() + "/gone.cube";
    std::string why;
    CHECK(!g.render(&why).lut && !why.empty());
}

} // namespace

int main()
{
    testParse();
    testApply();
    testCacheAndGrade();
    return modeltest::finish("color_lut_test");
}
