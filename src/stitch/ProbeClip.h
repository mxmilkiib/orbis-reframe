#pragma once

#include <cstdint>
#include <string>

// Lightweight source-clip probe: native resolution / fps / duration, so a
// caller can work out the output frame count it must bake poses for (and fill
// ClipItem's source-media facts on import). Backed by insv::InsvDemux -- opens
// the container and reads stream 0's parameters, no decode, no stitch.

namespace stitch {

struct ClipProbe {
    int width = 0;
    int height = 0;
    double fps = 0.0;
    int64_t duration_ms = 0;
    int bitDepth = 0; // bits per component (the X6 records HEVC Main 10)
};

bool ProbeClip(const std::string& clip_path, ClipProbe& out, std::string& err);

} // namespace stitch
