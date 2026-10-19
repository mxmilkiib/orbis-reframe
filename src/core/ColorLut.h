#pragma once

// 3D colour LUTs (.cube files) and the grade the renderers apply: a LUT, then
// the Colour panel's four adjustments. No Qt, no Vulkan, so every layer that
// grades (core, the GPU stitch, the CPU export) can use it.
//
// The .cube format (Adobe / Resolve): optional TITLE, LUT_3D_SIZE N (2..256),
// DOMAIN_MIN / DOMAIN_MAX r g b (default 0 0 0 / 1 1 1), then N^3 lines of
// "r g b" output values with red changing fastest. '#' starts a comment. A
// 1D-only file (LUT_1D_SIZE) is refused: camera makers ship 3D ones.

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace color {

struct Lut3D {
    static constexpr int kMinSize = 2;
    static constexpr int kMaxSize = 256;

    std::string title;              // TITLE, empty when the file has none
    int size = 0;                   // N: N^3 entries
    float domainMin = 0.0f;         // the input range, the same for r, g and b
    float domainMax = 1.0f;
    std::vector<float> rgb;         // N^3 * 3, red fastest, then green, then blue

    // The output at input `in` (in [domainMin, domainMax], clamped), trilinear
    // between the eight nearest entries -- what the GPU's linear sampler on a
    // 3D texture does.
    void apply(const float in[3], float out[3]) const;
};

// Parse a .cube file's text. nullptr with *error set (in words for the user)
// when it isn't one we can use.
std::shared_ptr<const Lut3D> parseCube(const std::string &text, std::string *error = nullptr);
// Read and parse the file at `path` (UTF-8).
std::shared_ptr<const Lut3D> loadCube(const std::string &path, std::string *error = nullptr);

// The loaded LUTs, by path: a file read once is shared by every clip and
// renderer that uses it (a 65^3 file is ~275k lines). Reloaded when the file
// changes on disk. Thread-safe.
std::shared_ptr<const Lut3D> cachedCube(const std::string &path, std::string *error = nullptr);

// What a renderer applies to a clip: the LUT (when there is one), mixed with
// the original by `lutMix`, then the four adjustments
// (src/render/shaders/color_grade.glsl and exportpipe::gradeRgb()).
struct Grade {
    // {exposureEv, contrast%, saturation%, temperature%}, all 0 = neutral.
    std::array<float, 4> adjust{};
    std::shared_ptr<const Lut3D> lut; // nullptr = none
    float lutMix = 1.0f;              // 0..1, how much of the LUT's output

    bool hasLut() const { return lut && lutMix > 0.0f; }
    bool isNeutral() const { return !hasLut() && adjust == std::array<float, 4>{}; }
    bool operator==(const Grade &o) const
    {
        return adjust == o.adjust && lut == o.lut && (!lut || lutMix == o.lutMix);
    }
    bool operator!=(const Grade &o) const { return !(*this == o); }
};

} // namespace color
