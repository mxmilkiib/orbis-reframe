#include "ColorLut.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

namespace color {

namespace {

void setError(std::string *error, const std::string &text)
{
    if (error)
        *error = text;
}

bool isSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

// Up to `n` floats from `s`; how many were read, and whether anything but
// space followed them.
int readFloats(const char *s, float *out, int n, bool *trailing)
{
    int got = 0;
    while (got < n) {
        while (isSpace(*s))
            ++s;
        if (!*s)
            break;
        char *end = nullptr;
        errno = 0;
        const float v = std::strtof(s, &end);
        if (end == s || errno == ERANGE || !std::isfinite(v))
            break;
        out[got++] = v;
        s = end;
    }
    while (isSpace(*s))
        ++s;
    *trailing = *s != '\0';
    return got;
}

} // namespace

void Lut3D::apply(const float in[3], float out[3]) const
{
    const int n = size;
    const float scale = float(n - 1) / (domainMax - domainMin);
    int i0[3], i1[3];
    float f[3];
    for (int c = 0; c < 3; ++c) {
        const float x = std::clamp((in[c] - domainMin) * scale, 0.0f, float(n - 1));
        i0[c] = std::min(int(x), n - 2);
        i1[c] = i0[c] + 1;
        f[c] = x - float(i0[c]);
    }
    auto at = [&](int r, int g, int b, int c) {
        return rgb[((size_t(b) * n + g) * n + r) * 3 + c];
    };
    for (int c = 0; c < 3; ++c) {
        const float c00 = at(i0[0], i0[1], i0[2], c) * (1 - f[0]) + at(i1[0], i0[1], i0[2], c) * f[0];
        const float c10 = at(i0[0], i1[1], i0[2], c) * (1 - f[0]) + at(i1[0], i1[1], i0[2], c) * f[0];
        const float c01 = at(i0[0], i0[1], i1[2], c) * (1 - f[0]) + at(i1[0], i0[1], i1[2], c) * f[0];
        const float c11 = at(i0[0], i1[1], i1[2], c) * (1 - f[0]) + at(i1[0], i1[1], i1[2], c) * f[0];
        const float c0 = c00 * (1 - f[1]) + c10 * f[1];
        const float c1 = c01 * (1 - f[1]) + c11 * f[1];
        out[c] = c0 * (1 - f[2]) + c1 * f[2];
    }
}

std::shared_ptr<const Lut3D> parseCube(const std::string &text, std::string *error)
{
    auto lut = std::make_shared<Lut3D>();
    float dmin[3] = {0, 0, 0}, dmax[3] = {1, 1, 1};
    size_t want = 0;
    int lineNo = 0;
    std::string line;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos)
            end = text.size();
        line.assign(text, pos, end - pos);
        pos = end + 1;
        ++lineNo;
        if (const size_t hash = line.find('#'); hash != std::string::npos)
            line.resize(hash);
        const char *s = line.c_str();
        while (isSpace(*s))
            ++s;
        if (!*s)
            continue;
        const auto bad = [&](const std::string &why) {
            setError(error, "line " + std::to_string(lineNo) + ": " + why);
            return nullptr;
        };
        if ((*s >= '0' && *s <= '9') || *s == '-' || *s == '+' || *s == '.') {
            if (want == 0)
                return bad("values before LUT_3D_SIZE");
            float v[3];
            bool trailing = false;
            if (readFloats(s, v, 3, &trailing) != 3 || trailing)
                return bad("expected three numbers");
            if (lut->rgb.size() >= want * 3)
                return bad("more entries than LUT_3D_SIZE says");
            lut->rgb.insert(lut->rgb.end(), v, v + 3);
            continue;
        }
        const char *word = s;
        while (*s && !isSpace(*s))
            ++s;
        const std::string key(word, s);
        if (!lut->rgb.empty())
            return bad("\"" + key + "\" after the values");
        bool trailing = false;
        if (key == "TITLE") {
            std::string t = s;
            const size_t a = t.find('"'), b = t.rfind('"');
            lut->title = a != std::string::npos && b > a ? t.substr(a + 1, b - a - 1) : std::string();
        } else if (key == "LUT_3D_SIZE") {
            float v = 0;
            if (readFloats(s, &v, 1, &trailing) != 1 || trailing || v != std::floor(v) ||
                v < Lut3D::kMinSize || v > Lut3D::kMaxSize)
                return bad("LUT_3D_SIZE must be a whole number from 2 to 256");
            lut->size = int(v);
            want = size_t(lut->size) * lut->size * lut->size;
            lut->rgb.reserve(want * 3);
        } else if (key == "LUT_1D_SIZE") {
            return bad("this is a 1D LUT; only 3D LUTs (LUT_3D_SIZE) are supported");
        } else if (key == "DOMAIN_MIN" || key == "DOMAIN_MAX") {
            float *d = key == "DOMAIN_MIN" ? dmin : dmax;
            if (readFloats(s, d, 3, &trailing) != 3 || trailing)
                return bad(key + " needs three numbers");
        } else if (key == "LUT_3D_INPUT_RANGE") {
            float v[2];
            if (readFloats(s, v, 2, &trailing) != 2 || trailing)
                return bad("LUT_3D_INPUT_RANGE needs two numbers");
            std::fill(dmin, dmin + 3, v[0]);
            std::fill(dmax, dmax + 3, v[1]);
        }
        // Other keywords (LUT_IN_VIDEO_RANGE, ...) don't change the mapping
        // for full-range input; skip them.
    }
    if (lut->size == 0) {
        setError(error, "no LUT_3D_SIZE: not a 3D .cube LUT");
        return nullptr;
    }
    if (lut->rgb.size() != want * 3) {
        setError(error, "expected " + std::to_string(want) + " entries for LUT_3D_SIZE " +
                            std::to_string(lut->size) + ", found " +
                            std::to_string(lut->rgb.size() / 3));
        return nullptr;
    }
    if (dmin[0] != dmin[1] || dmin[0] != dmin[2] || dmax[0] != dmax[1] || dmax[0] != dmax[2]) {
        setError(error, "a different DOMAIN for red, green and blue isn't supported");
        return nullptr;
    }
    if (!(dmax[0] > dmin[0])) {
        setError(error, "DOMAIN_MAX must be above DOMAIN_MIN");
        return nullptr;
    }
    lut->domainMin = dmin[0];
    lut->domainMax = dmax[0];
    return lut;
}

std::shared_ptr<const Lut3D> loadCube(const std::string &path, std::string *error)
{
    std::ifstream in(std::filesystem::path(path), std::ios::binary);
    if (!in) {
        setError(error, "can't open the file");
        return nullptr;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return parseCube(ss.str(), error);
}

std::shared_ptr<const Lut3D> cachedCube(const std::string &path, std::string *error)
{
    struct Entry {
        std::filesystem::file_time_type mtime;
        std::uintmax_t bytes = 0;
        std::shared_ptr<const Lut3D> lut;
        std::string error;
    };
    static std::mutex mutex;
    static std::map<std::string, Entry> cache;

    std::error_code ec;
    const auto p = std::filesystem::path(path);
    const auto mtime = std::filesystem::last_write_time(p, ec);
    const std::uintmax_t bytes = ec ? 0 : std::filesystem::file_size(p, ec);
    if (ec) {
        setError(error, "the file is missing");
        return nullptr;
    }
    std::lock_guard lock(mutex);
    auto it = cache.find(path);
    if (it == cache.end() || it->second.mtime != mtime || it->second.bytes != bytes) {
        Entry e{mtime, bytes, nullptr, {}};
        e.lut = loadCube(path, &e.error);
        it = cache.insert_or_assign(path, std::move(e)).first;
    }
    if (!it->second.lut)
        setError(error, it->second.error);
    return it->second.lut;
}

} // namespace color
