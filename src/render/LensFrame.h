#pragma once

#include <array>
#include <cstdint>
#include <memory>

// A decoded fisheye pair handed from a stitch source's Vulkan device (FFmpeg's
// decoder device) to the viewport's (QVulkanWindow's), so the viewport can draw
// the view straight from the lenses at present time.
//
// The pair lives in a ring slot of four plain images on the decoder device --
// lens 0 Y (R8_UNORM, lensW x lensH), lens 0 CbCr (R8G8_UNORM, half size),
// lens 1 Y, lens 1 CbCr; R16_UNORM / R16G16_UNORM for 10-bit pictures (P010:
// the 10 bits at the top of each 16) -- each in its own dedicated, exportable allocation
// (VK_KHR_external_memory_fd, opaque FD). The viewport imports each slot once
// by FD and creates identical images over the memory. Plain Vulkan / POSIX,
// no Qt, no FFmpeg: shared by render_gpu_stitch (exporter) and the viewport
// (importer).

namespace render {

// One ring slot. Created and owned by the exporter; read-only to importers.
struct LensSlot {
    // Image order: lens0 Y, lens0 CbCr, lens1 Y, lens1 CbCr.
    static constexpr int kImages = 4;
    int fd[kImages] = {-1, -1, -1, -1}; // exporter keeps these; importers dup()
    uint64_t memSize[kImages] = {};     // allocation size of each image
    int lensW = 0;                      // Y extent; CbCr is (lensW/2, lensH/2)
    int lensH = 0;
    int bitDepth = 8;                   // 8 (R8 / R8G8 planes) or 10 (R16 / R16G16)
    uint64_t id = 0;                    // process-unique, the importer's cache key
    uint8_t deviceUuid[16] = {};        // exporter's VkPhysicalDeviceIDProperties::deviceUUID;
                                        // an importer on another GPU can't use the FDs
};

// One decoded frame, ready to draw. Holding it keeps its slot reserved: the
// exporter won't rewrite the slot until every copy of `slot` is gone, so an
// importer holds on until its last GPU frame using the slot has retired.
struct LensFrame {
    std::shared_ptr<const LensSlot> slot;
    // Bumps every time this slot is rewritten: an importer that sees a new
    // value must do the queue-family acquire (from VK_QUEUE_FAMILY_EXTERNAL)
    // before sampling.
    uint64_t fill = 0;
    // The stitch's std140 Params UBO for this frame (geometry, lens model,
    // per-frame capture-time taps, the depth-aware seam's table, the privacy
    // blur quads), packed by the exporter.
    static constexpr int kParamsBytes = 7344;
    std::array<uint8_t, kParamsBytes> params{};
    // Photometric gains for lens 0 / 1, measured on the exporter.
    float gain[2] = {1.0f, 1.0f};
    int64_t timestamp_ms = 0;
};

} // namespace render
