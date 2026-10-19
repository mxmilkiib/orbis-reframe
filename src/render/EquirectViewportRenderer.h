#pragma once

// The viewport's Vulkan renderer (QVulkanWindowRenderer), private to the
// viewport: EquirectViewportWindow.cpp makes it, EquirectViewportRenderer.cpp
// holds its pipelines and resources, EquirectViewportRendererFrame.cpp
// records each frame.

// Before any Qt header: QVulkanInstance defines VK_NO_PROTOTYPES, and the
// renderer calls the Vulkan loader directly -- Qt's QVulkanDeviceFunctions
// are private ABI in Debian's Qt, which would pin the .deb to one Qt release.
#include <vulkan/vulkan.h>

#include "EquirectViewportWindow.h"
#include "VulkanLut.h"
#include "EmbeddedShaders.h"
#include "FrameStats.h"

#include <QVulkanWindowRenderer>

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <vector>

struct PushConstants {
    float yaw;
    float pitch;
    float roll;
    float fovY;
    float aspect;
    float _pad[3];       // std140: mat3 below aligns at offset 32
    float stab[12];      // 3 columns, each a vec3 padded to 16 bytes (col-major)
    float colorGrade[4]; // exposureEv, contrast%, saturation%, temperature%
    float colorLut[4];   // render::VulkanLutStore::pushValues
};

// fisheye_view.frag's push constants: the same view block plus the lens gains.
struct LensPushConstants {
    PushConstants view;
    float gains[4]; // x, y: photometric gain of lens 0 / 1; z: plane scale, w: chroma midpoint
};
static_assert(sizeof(LensPushConstants) == 128, "LensPushConstants must match fisheye_view.frag");

// inset.frag's push constants.
struct InsetPushConstants {
    float rect[4];   // x, y, w, h in output-rect px
    float origin[2]; // output rect's top-left, framebuffer px
    float radius;
    float border;
    float borderColor[4]; // rgb, a = opacity
    float grade[4];       // ColorGrade::packed(), all 0 = none
    float lut[4];         // render::VulkanLutStore::pushValues
};
static_assert(sizeof(InsetPushConstants) == 80, "InsetPushConstants must match inset.frag");

// marker.frag's push constants (the Track subject box).
struct MarkerPushConstants {
    float rect[4];   // x, y, w, h in output-rect px
    float origin[2]; // output rect's top-left, framebuffer px
    float border;
    float fill;
    float color[4]; // rgb, a = outline opacity
};
static_assert(sizeof(MarkerPushConstants) == 48, "MarkerPushConstants must match marker.frag");

// region.frag's push constants (the privacy-blur region outlines).
struct RegionPushConstants {
    float origin[2]; // output rect's top-left, framebuffer px
    float border;
    float count;     // corners in use
    float color[4];  // rgb, a = opacity
    float pts[12];   // up to 6 corners in output-rect px
};
static_assert(sizeof(RegionPushConstants) == 80, "RegionPushConstants must match region.frag");

// Rendering-only concern: a pipeline that samples an equirect texture through a
// fullscreen-triangle reprojection shader. See EquirectViewportWindow.h for why
// frames are pushed in from outside rather than pulled from an IStitchSource
// here.
class EquirectViewportRenderer : public QVulkanWindowRenderer {
public:
    explicit EquirectViewportRenderer(EquirectViewportWindow *window)
        : m_window(window)
    {
    }

    void initResources() override;
    void releaseResources() override;
    void startNextFrame() override;

private:
    VkShaderModule loadShader(render::shaders::Spirv spirv) const;
    void createSampler();
    void createDescriptorSet();
    void createPipeline();
    // (Re)creates the sampled image + staging ring when the frame size changes
    // (or on the first call), and points the descriptor set at the new view.
    void ensureTextureResources(uint32_t width, uint32_t height);
    void destroyTextureResources();
    // Records the barrier + buffer->image copy for staging slot `stagingSlot`
    // (already filled by the caller) into `cmd`. No submit, no wait.
    void recordTextureCopy(VkCommandBuffer cmd, uint32_t stagingSlot);
    // Blocking single upload for initResources only (placeholder / retained
    // frame restore) -- uses a one-time command buffer + vkQueueWaitIdle.
    void uploadTextureImmediate(int width, int height, const std::vector<uint8_t> &rgba);
    VkCommandBuffer beginOneTimeCommands() const;
    void endOneTimeCommands(VkCommandBuffer cmd) const;
    // A fullscreen-triangle graphics pipeline with equirect.vert and `frag`.
    // `blend`: straight alpha blending over what's drawn (the insets).
    VkPipeline makePipeline(render::shaders::Spirv frag, VkPipelineLayout layout, bool blend = false);

    // --- Picture-in-picture insets (inset.frag) ---
    struct InsetSlot {
        VkImage img = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        uint32_t w = 0, h = 0;
        bool undefined = true;
        // One staging buffer per frame in flight, like the main texture.
        std::vector<VkBuffer> staging;
        std::vector<VkDeviceMemory> stagingMem;
        std::vector<void *> stagingMap;
        VkDescriptorSet set = VK_NULL_HANDLE;
        uint64_t key = 0; // frameKey of the picture in `img`
    };
    void createInsetResources();
    void destroyInsetResources();
    // (Re)size slot `i` for a w x h picture; waits for the device if it had
    // to recreate it.
    void ensureInsetSlot(InsetSlot &slot, uint32_t w, uint32_t h);
    void destroyInsetSlot(InsetSlot &slot);
    // Upload `rgba` into the slot's image, recorded into `cmd`.
    void recordInsetUpload(VkCommandBuffer cmd, InsetSlot &slot, const std::vector<uint8_t> &rgba);

    // --- Drawing lens frames (fisheye_view.frag) ---
    struct LensImport {
        VkImage img[render::LensSlot::kImages] = {};
        VkDeviceMemory mem[render::LensSlot::kImages] = {};
        VkImageView view[render::LensSlot::kImages] = {};
        uint64_t acquiredFill = 0; // fill last acquired from the exporter
        uint64_t lastUsed = 0;     // m_frameCounter
    };
    // Pipeline, layouts, per-frame UBOs / sets; decides m_lensCapable and
    // tells the window. Leaves the lens path off (not fatal) on any failure.
    void createLensResources();
    void destroyLensResources();
    // The (cached) import of `f`'s slot on this device; nullptr if it can't be
    // imported.
    LensImport *importFor(const render::LensFrame &f);
    void destroyImport(LensImport &imp);
    // Hand `f` back once every GPU frame that may use it has finished.
    void retireLens(std::shared_ptr<const render::LensFrame> f);

    EquirectViewportWindow *m_window;

    // The grade's LUTs: set 1 of the equirect, lens and inset pipelines.
    render::VulkanLutStore m_luts;

    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet m_descriptorSet = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;

    VkImage m_textureImage = VK_NULL_HANDLE;
    VkDeviceMemory m_textureMemory = VK_NULL_HANDLE;
    VkImageView m_textureView = VK_NULL_HANDLE;
    uint32_t m_texWidth = 0;
    uint32_t m_texHeight = 0;
    // True when m_textureImage has been (re)created but not written yet, so the
    // next upload transitions it from UNDEFINED rather than SHADER_READ_ONLY.
    bool m_textureUndefined = true;

    // Persistently-mapped host-visible staging buffers, one per frame in
    // flight. A per-frame texture upload is then just a memcpy into the current
    // slot + a copy recorded into that frame's own command buffer -- no
    // per-frame allocation and no vkQueueWaitIdle. Recreated (with the image)
    // only when the frame size changes.
    std::vector<VkBuffer> m_stagingBuffers;
    std::vector<VkDeviceMemory> m_stagingMemory;
    std::vector<void *> m_stagingMapped;

    // Last frame uploaded, retained so it can be re-uploaded if Vulkan
    // resources are torn down and rebuilt (Qt does this on some resize /
    // surface-recreation paths, notably under Wayland). Without this, a
    // paused frame vanishes on resize -- nothing re-pushes it, since the
    // playback clock only calls SetFrame() while playing or just after a seek.
    int m_lastWidth = 0;
    int m_lastHeight = 0;
    std::vector<uint8_t> m_lastRgba;

    // --- lens frames ---
    bool m_lensCapable = false;
    uint8_t m_deviceUuid[16] = {};
    VkPhysicalDeviceMemoryProperties m_memProps{};
    VkDescriptorSetLayout m_lensSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_lensPool = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> m_lensSets; // one per frame in flight
    std::vector<VkBuffer> m_lensUbo;         // the stitch Params, one per frame in flight
    std::vector<VkDeviceMemory> m_lensUboMem;
    std::vector<void *> m_lensUboMap;
    VkPipelineLayout m_lensPipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_lensPipeline = VK_NULL_HANDLE;
    VkSampler m_lensSampler = VK_NULL_HANDLE;
    std::map<uint64_t, LensImport> m_imports; // by LensSlot::id
    // What's on screen in lens mode. Survives releaseResources (like
    // m_lastRgba) so a rebuilt device re-imports it.
    std::shared_ptr<const render::LensFrame> m_currentLens;
    bool m_lensMode = false;
    struct Retired {
        std::shared_ptr<const render::LensFrame> frame;
        uint64_t releaseAt; // m_frameCounter at which no frame in flight uses it
    };
    std::deque<Retired> m_retired;
    uint64_t m_frameCounter = 0;

    // ORBIS_FRAME_STATS (FrameStats.h): render pacing, and for how many
    // renders each new picture stayed on screen.
    struct Stats {
        render::framestats::Clock::time_point windowStart{}, lastRender{};
        render::framestats::Samples interval, cpu;
        int newFrames = 0;
        int rendersOfCurrent = 0;
        int shownFor[5] = {}; // 1, 2, 3, 4, 5+ renders
        int poseChanges = 0;
        float lastPose[4] = {};
    } m_stats;
    void statsFrameStart(bool newPicture);
    void statsFrameEnd(render::framestats::Clock::time_point start);

    // --- insets ---
    InsetSlot m_insetSlots[EquirectViewportWindow::kMaxInsets];
    VkSampler m_insetSampler = VK_NULL_HANDLE;
    VkDescriptorPool m_insetPool = VK_NULL_HANDLE;
    VkPipelineLayout m_insetPipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_insetPipeline = VK_NULL_HANDLE;

    // The Track subject box (marker.frag): no descriptors, push constants only.
    VkPipelineLayout m_markerPipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_markerPipeline = VK_NULL_HANDLE;
    // The privacy-blur region outlines (region.frag): push constants only.
    VkPipelineLayout m_regionPipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_regionPipeline = VK_NULL_HANDLE;
};
