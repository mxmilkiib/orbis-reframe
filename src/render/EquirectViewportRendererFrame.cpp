// Before any Qt header (EquirectViewportRenderer.h has the reason).
#include <vulkan/vulkan.h>

#include "EquirectViewportRenderer.h"

#include "../core/Log.h"

#include <QMutexLocker>
#include <QVersionNumber>

#include <unistd.h> // dup / close: imported memory FDs

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

// One frame: the picture (an uploaded equirect or the stitcher's lens
// frames), the overlays, the Track subject box and the privacy regions,
// recorded into the frame's command buffer; and the frame stats.

namespace {

// A region outline as the view shows it: its quad's corners taken from the
// sphere frame into the view (the inverse of equirect.frag's ray: stab^T,
// then yaw, pitch and roll undone), clipped to what lies in front of the
// camera and projected into the output rect, px. Up to 6 corners (4, plus
// one per edge the clip cuts). Fewer than 3 = nothing to draw.
int projectOutline(const EquirectViewportWindow::RegionOutline &o, const float stabRm[9],
                   float yaw, float pitch, float roll, float fovY, float aspect, float rectW,
                   float rectH, float out[12])
{
    const float cy = std::cos(yaw), sy = std::sin(yaw);
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const float cr = std::cos(roll), sr = std::sin(roll);
    float v[4][3];
    for (int i = 0; i < 4; ++i) {
        const float *d = o.corner[i];
        // stab^T * d (stab is a rotation; rayWorld = stab * rayPosed).
        float x = stabRm[0] * d[0] + stabRm[3] * d[1] + stabRm[6] * d[2];
        float y = stabRm[1] * d[0] + stabRm[4] * d[1] + stabRm[7] * d[2];
        float z = stabRm[2] * d[0] + stabRm[5] * d[1] + stabRm[8] * d[2];
        // Undo the yaw (about Y), the pitch (about X), the roll (about Z).
        float x1 = x * cy - z * sy, z1 = x * sy + z * cy;
        float y2 = y * cp + z1 * sp, z2 = -y * sp + z1 * cp;
        v[i][0] = x1 * cr + y2 * sr;
        v[i][1] = -x1 * sr + y2 * cr;
        v[i][2] = z2;
    }
    // Keep z <= -kNear (in front); Sutherland-Hodgman against that plane.
    constexpr float kNear = 1e-3f;
    float c[6][3];
    int n = 0;
    for (int i = 0; i < 4; ++i) {
        const float *a = v[i], *b = v[(i + 1) % 4];
        const bool ina = a[2] <= -kNear, inb = b[2] <= -kNear;
        if (ina)
            std::copy(a, a + 3, c[n++]);
        if (ina != inb) {
            const float t = (-kNear - a[2]) / (b[2] - a[2]);
            for (int k = 0; k < 3; ++k)
                c[n][k] = a[k] + t * (b[k] - a[k]);
            ++n;
        }
    }
    const float t = std::tan(fovY * 0.5f);
    for (int i = 0; i < n; ++i) {
        const float ndcX = (c[i][0] / -c[i][2]) / (t * aspect);
        const float ndcY = -(c[i][1] / -c[i][2]) / t;
        out[i * 2] = (ndcX + 1.0f) * 0.5f * rectW;
        out[i * 2 + 1] = (ndcY + 1.0f) * 0.5f * rectH;
    }
    return n;
}

} // namespace

EquirectViewportRenderer::LensImport *
EquirectViewportRenderer::importFor(const render::LensFrame &f)
{
    const render::LensSlot &slot = *f.slot;
    auto it = m_imports.find(slot.id);
    if (it != m_imports.end())
        return &it->second;
    if (std::memcmp(slot.deviceUuid, m_deviceUuid, sizeof m_deviceUuid) != 0) {
        std::fprintf(stderr, "[viewport] lens frames come from another GPU; can't import\n");
        return nullptr;
    }

    VkDevice device = m_window->device();
    LensImport imp;
    for (int i = 0; i < render::LensSlot::kImages; ++i) {
        const bool uv = (i & 1) != 0;
        const bool deep = slot.bitDepth > 8; // 10-bit: 16-bit planes
        const VkFormat fmt = deep ? (uv ? VK_FORMAT_R16G16_UNORM : VK_FORMAT_R16_UNORM)
                                  : (uv ? VK_FORMAT_R8G8_UNORM : VK_FORMAT_R8_UNORM);
        // Must match the exporter's image exactly (VulkanFisheyeStitcher).
        VkExternalMemoryImageCreateInfo emi{};
        emi.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
        emi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkImageCreateInfo ii{};
        ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ii.pNext = &emi;
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = fmt;
        ii.extent = {static_cast<uint32_t>(uv ? slot.lensW / 2 : slot.lensW),
                     static_cast<uint32_t>(uv ? slot.lensH / 2 : slot.lensH), 1};
        ii.mipLevels = 1;
        ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        bool ok = vkCreateImage(device, &ii, nullptr, &imp.img[i]) == VK_SUCCESS;

        VkMemoryRequirements req{};
        uint32_t type = UINT32_MAX;
        if (ok) {
            vkGetImageMemoryRequirements(device, imp.img[i], &req);
            for (uint32_t t = 0; t < m_memProps.memoryTypeCount && type == UINT32_MAX; ++t)
                if ((req.memoryTypeBits & (1u << t)) &&
                    (m_memProps.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
                    type = t;
            ok = type != UINT32_MAX;
        }
        if (ok) {
            const int fd = ::dup(slot.fd[i]); // the import takes ownership of this copy
            VkMemoryDedicatedAllocateInfo ded{};
            ded.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
            ded.image = imp.img[i];
            VkImportMemoryFdInfoKHR imf{};
            imf.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
            imf.pNext = &ded;
            imf.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
            imf.fd = fd;
            VkMemoryAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            ai.pNext = &imf;
            ai.allocationSize = slot.memSize[i];
            ai.memoryTypeIndex = type;
            ok = fd >= 0 &&
                 vkAllocateMemory(device, &ai, nullptr, &imp.mem[i]) == VK_SUCCESS;
            if (!ok && fd >= 0)
                ::close(fd); // not consumed on failure
        }
        if (ok)
            ok = vkBindImageMemory(device, imp.img[i], imp.mem[i], 0) == VK_SUCCESS;
        if (ok) {
            VkImageViewCreateInfo vi{};
            vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            vi.image = imp.img[i];
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = fmt;
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            ok = vkCreateImageView(device, &vi, nullptr, &imp.view[i]) == VK_SUCCESS;
        }
        if (!ok) {
            std::fprintf(stderr, "[viewport] importing a lens frame failed; back to the "
                                 "equirect preview\n");
            destroyImport(imp);
            return nullptr;
        }
    }
    return &m_imports.emplace(slot.id, imp).first->second;
}

void EquirectViewportRenderer::retireLens(std::shared_ptr<const render::LensFrame> f)
{
    if (!f)
        return;
    // A frame recorded now finishes before the one concurrentFrameCount()
    // frames later starts (Qt waits on that frame slot's fence).
    const uint64_t frames = static_cast<uint64_t>(std::max(1, m_window->concurrentFrameCount()));
    m_retired.push_back({std::move(f), m_frameCounter + frames});
}

void EquirectViewportRenderer::recordTextureCopy(VkCommandBuffer cmd, uint32_t stagingSlot)
{
    VkImageMemoryBarrier toTransferDst{};
    toTransferDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toTransferDst.oldLayout = m_textureUndefined ? VK_IMAGE_LAYOUT_UNDEFINED
                                                 : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toTransferDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransferDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransferDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransferDst.image = m_textureImage;
    toTransferDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toTransferDst.srcAccessMask = m_textureUndefined ? 0 : VK_ACCESS_SHADER_READ_BIT;
    toTransferDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    const VkPipelineStageFlags srcStage = m_textureUndefined
        ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
        : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    vkCmdPipelineBarrier(cmd, srcStage, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                                     0, nullptr, 1, &toTransferDst);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {m_texWidth, m_texHeight, 1};
    vkCmdCopyBufferToImage(cmd, m_stagingBuffers[stagingSlot], m_textureImage,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier toShaderRead{};
    toShaderRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toShaderRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toShaderRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toShaderRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toShaderRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toShaderRead.image = m_textureImage;
    toShaderRead.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toShaderRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toShaderRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                                     nullptr, 1, &toShaderRead);

    m_textureUndefined = false;
}

void EquirectViewportRenderer::recordInsetUpload(VkCommandBuffer cmd, InsetSlot &slot,
                                                 const std::vector<uint8_t> &rgba)
{
    const uint32_t frame = static_cast<uint32_t>(m_window->currentFrame());
    std::memcpy(slot.stagingMap[frame], rgba.data(), std::min<size_t>(rgba.size(), size_t(slot.w) * slot.h * 4));

    VkImageMemoryBarrier toDst{};
    toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toDst.oldLayout = slot.undefined ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.srcQueueFamilyIndex = toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.image = slot.img;
    toDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toDst.srcAccessMask = slot.undefined ? 0 : VK_ACCESS_SHADER_READ_BIT;
    toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,
                                     slot.undefined ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                                    : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                     &toDst);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {slot.w, slot.h, 1};
    vkCmdCopyBufferToImage(cmd, slot.staging[frame], slot.img,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    VkImageMemoryBarrier toRead = toDst;
    toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                                     nullptr, 1, &toRead);
    slot.undefined = false;
}

void EquirectViewportRenderer::statsFrameStart(bool newPicture)
{
    Stats &s = m_stats;
    const auto now = render::framestats::Clock::now();
    if (s.lastRender != render::framestats::Clock::time_point{})
        s.interval.add(render::framestats::MsBetween(s.lastRender, now));
    else
        s.windowStart = now;
    s.lastRender = now;
    if (newPicture) {
        if (s.rendersOfCurrent > 0)
            ++s.shownFor[std::min(s.rendersOfCurrent, 5) - 1];
        s.rendersOfCurrent = 0;
        ++s.newFrames;
    }
    ++s.rendersOfCurrent;
    const float pose[4] = {m_window->Yaw(), m_window->Pitch(), m_window->Roll(), m_window->FovY()};
    if (!std::equal(pose, pose + 4, s.lastPose)) {
        ++s.poseChanges;
        std::copy(pose, pose + 4, s.lastPose);
    }
}

void EquirectViewportRenderer::statsFrameEnd(render::framestats::Clock::time_point start)
{
    Stats &s = m_stats;
    const auto now = render::framestats::Clock::now();
    s.cpu.add(render::framestats::MsBetween(start, now));
    if (render::framestats::MsBetween(s.windowStart, now) < 1000.0)
        return;
    std::fprintf(stderr,
                 "[stats] viewport: renders %zu | interval mean %.2f p99 %.2f max %.2f ms | "
                 "cpu mean %.2f max %.2f ms | new frames %d, shown for 1/2/3/4/5+ renders: "
                 "%d/%d/%d/%d/%d | pose changes %d\n",
                 s.cpu.count(), s.interval.mean(), s.interval.pct(0.99), s.interval.max(),
                 s.cpu.mean(), s.cpu.max(), s.newFrames, s.shownFor[0], s.shownFor[1],
                 s.shownFor[2], s.shownFor[3], s.shownFor[4], s.poseChanges);
    s.interval.clear();
    s.cpu.clear();
    s.newFrames = s.poseChanges = 0;
    std::fill(std::begin(s.shownFor), std::end(s.shownFor), 0);
    s.windowStart = now;
}

void EquirectViewportRenderer::startNextFrame()
{
    const auto statsStart = render::framestats::Clock::now();
    VkCommandBuffer commandBuffer = m_window->currentCommandBuffer();
    ++m_frameCounter;
    // QVulkanWindow's default render pass has no external subpass dependency,
    // so its layout transitions at vkCmdBeginRenderPass aren't ordered after
    // (a) the swapchain acquire -- waited on at COLOR_ATTACHMENT_OUTPUT -- or
    // (b) the previous frame's store to the one depth-stencil image all
    // frames share (sync validation: WRITE_AFTER_READ vs
    // vkAcquireNextImageKHR, WRITE_AFTER_WRITE vs the last storeOp). One
    // barrier from those stages / writes to everything after covers both.
    VkMemoryBarrier attachments{};
    attachments.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    attachments.srcAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    attachments.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &attachments, 0, nullptr, 0, nullptr);
    // Lens frames no frame in flight can still be reading go back to their ring.
    while (!m_retired.empty() && m_retired.front().releaseAt <= m_frameCounter)
        m_retired.pop_front();

    int width = 0, height = 0;
    std::vector<uint8_t> rgba;
    std::shared_ptr<const render::LensFrame> lens;
    const bool newPicture = m_window->TakePendingFrame(width, height, rgba, lens);
    if (render::framestats::Enabled())
        statsFrameStart(newPicture);
    if (newPicture) {
        if (lens) {
            retireLens(std::move(m_currentLens));
            m_currentLens = std::move(lens);
            m_lensMode = true;
        } else {
            // Upload recorded into this frame's own command buffer, before the
            // render pass begins -- no separate submit, no wait-idle.
            ensureTextureResources(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
            const uint32_t slot = static_cast<uint32_t>(m_window->currentFrame());
            std::memcpy(m_stagingMapped[slot], rgba.data(), rgba.size());
            recordTextureCopy(commandBuffer, slot);
            m_lastWidth = width;
            m_lastHeight = height;
            m_lastRgba = std::move(rgba);
            retireLens(std::move(m_currentLens));
            m_currentLens.reset();
            m_lensMode = false;
        }
    }

    // Lens mode: this frame's descriptor set -> the current pair's (imported)
    // images and its stitch params.
    const uint32_t frameSlot = static_cast<uint32_t>(m_window->currentFrame());
    LensImport *imp = nullptr;
    if (m_lensMode && m_currentLens && m_lensCapable) {
        imp = importFor(*m_currentLens);
        if (!imp) {
            m_lensCapable = false;
            m_window->setLensFramesSupported(false); // the controller falls back
        }
    }
    if (imp) {
        static bool announced = false;
        if (!announced) {
            announced = true;
            core::log::Info("[viewport] drawing the view straight from the fisheyes (%dx%d "
                            "lenses)\n",
                            m_currentLens->slot->lensW, m_currentLens->slot->lensH);
        }
        if (imp->acquiredFill != m_currentLens->fill) {
            // Take the freshly written images over from the exporting device.
            VkImageMemoryBarrier acq[render::LensSlot::kImages]{};
            for (int i = 0; i < render::LensSlot::kImages; ++i) {
                acq[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                acq[i].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                acq[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                acq[i].srcAccessMask = 0;
                acq[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                acq[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
                acq[i].dstQueueFamilyIndex = m_window->graphicsQueueFamilyIndex();
                acq[i].image = imp->img[i];
                acq[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            }
            vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                                             0, nullptr, render::LensSlot::kImages, acq);
            imp->acquiredFill = m_currentLens->fill;
        }
        imp->lastUsed = m_frameCounter;
        std::memcpy(m_lensUboMap[frameSlot], m_currentLens->params.data(),
                    render::LensFrame::kParamsBytes);
        VkDescriptorBufferInfo uboInfo{m_lensUbo[frameSlot], 0, render::LensFrame::kParamsBytes};
        VkDescriptorImageInfo imgInfo[4];
        VkWriteDescriptorSet w[5]{};
        w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[0].dstSet = m_lensSets[frameSlot];
        w[0].dstBinding = 0;
        w[0].descriptorCount = 1;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[0].pBufferInfo = &uboInfo;
        for (int i = 0; i < 4; ++i) {
            imgInfo[i] = {m_lensSampler, imp->view[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            w[1 + i] = w[0];
            w[1 + i].dstBinding = static_cast<uint32_t>(1 + i);
            w[1 + i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[1 + i].pBufferInfo = nullptr;
            w[1 + i].pImageInfo = &imgInfo[i];
        }
        vkUpdateDescriptorSets(m_window->device(), 5, w, 0, nullptr);
    }

    // Drop imports of slots not drawn for a while (a trimmed ring, a closed
    // clip). None of them is in flight any more.
    for (auto it = m_imports.begin(); it != m_imports.end();) {
        const bool current = m_currentLens && it->first == m_currentLens->slot->id;
        if (!current && it->second.lastUsed + 120 < m_frameCounter) {
            destroyImport(it->second);
            it = m_imports.erase(it);
        } else {
            ++it;
        }
    }

    // Insets: upload any new pictures (outside the render pass).
    std::vector<EquirectViewportWindow::InsetDraw> insets;
    if (m_insetPipeline != VK_NULL_HANDLE)
        insets = m_window->RenderInsets();
    if (insets.size() > size_t(EquirectViewportWindow::kMaxInsets))
        insets.resize(EquirectViewportWindow::kMaxInsets);
    for (size_t i = 0; i < insets.size(); ++i) {
        const auto &d = insets[i];
        InsetSlot &slot = m_insetSlots[i];
        if (!d.rgba || d.width <= 0 || d.height <= 0 ||
            d.rgba->size() < size_t(d.width) * d.height * 4)
            continue;
        if (slot.key == d.frameKey && slot.w == uint32_t(d.width) && slot.h == uint32_t(d.height))
            continue;
        ensureInsetSlot(slot, uint32_t(d.width), uint32_t(d.height));
        recordInsetUpload(commandBuffer, slot, *d.rgba);
        slot.key = d.frameKey;
    }

    // The grades' LUTs: uploaded on first use (their own submissions), so
    // before the render pass is recorded.
    const color::Grade grade = m_window->CurrentColorGrade();
    const VkDescriptorSet lutSet = m_luts.setFor(grade.hasLut() ? grade.lut : nullptr);
    VkDescriptorSet insetLutSets[EquirectViewportWindow::kMaxInsets] = {};
    for (size_t i = 0; i < insets.size(); ++i)
        insetLutSets[i] =
            m_luts.setFor(insets[i].colorGrade.hasLut() ? insets[i].colorGrade.lut : nullptr);

    const QSize size = m_window->swapChainImageSize();

    // One clear value per attachment the default render pass clears: colour,
    // depth-stencil, and the multisample colour when MSAA is on
    // (VUID-VkRenderPassBeginInfo-clearValueCount-00902).
    VkClearValue clearValues[3]{};
    const float *surround = m_window->SurroundColor();
    clearValues[0].color = {{surround[0], surround[1], surround[2], 1.0f}};
    clearValues[1].depthStencil = {1.0f, 0};
    clearValues[2].color = clearValues[0].color;

    VkRenderPassBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    beginInfo.renderPass = m_window->defaultRenderPass();
    beginInfo.framebuffer = m_window->currentFramebuffer();
    beginInfo.renderArea.extent.width = static_cast<uint32_t>(size.width());
    beginInfo.renderArea.extent.height = static_cast<uint32_t>(size.height());
    beginInfo.clearValueCount = m_window->sampleCountFlagBits() > VK_SAMPLE_COUNT_1_BIT ? 3 : 2;
    beginInfo.pClearValues = clearValues;

    vkCmdBeginRenderPass(commandBuffer, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);
    // Without the equirect texture's set (its creation failed, logged in
    // initResources) the equirect preview isn't drawn, only the surround;
    // the view straight from the fisheyes still is.
    const bool drawPicture =
        imp || (m_pipeline != VK_NULL_HANDLE && m_descriptorSet != VK_NULL_HANDLE);
    if (imp) {
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_lensPipeline);
        const VkDescriptorSet sets[2] = {m_lensSets[frameSlot], lutSet};
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                            m_lensPipelineLayout, 0, 2, sets, 0, nullptr);
    } else if (drawPicture) {
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
        const VkDescriptorSet sets[2] = {m_descriptorSet, lutSet};
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                            m_pipelineLayout, 0, 2, sets, 0, nullptr);
    }

    // Letterbox / pillarbox the picture to the project's output aspect: the
    // render pass clears the whole window to black, the draw covers only the
    // centred output-shaped rect.
    const float winAspect =
        size.height() > 0 ? static_cast<float>(size.width()) / size.height() : 1.0f;
    const float outAspect = m_window->OutputAspect() > 0.0f ? m_window->OutputAspect() : winAspect;
    int rectW = size.width();
    int rectH = size.height();
    if (outAspect > winAspect)
        rectH = std::max(1, static_cast<int>(std::lround(size.width() / outAspect)));
    else
        rectW = std::max(1, static_cast<int>(std::lround(size.height() * outAspect)));
    const int rectX = (size.width() - rectW) / 2;
    const int rectY = (size.height() - rectH) / 2;

    VkViewport viewport{};
    viewport.x = static_cast<float>(rectX);
    viewport.y = static_cast<float>(rectY);
    viewport.width = static_cast<float>(rectW);
    viewport.height = static_cast<float>(rectH);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset.x = rectX;
    scissor.offset.y = rectY;
    scissor.extent.width = static_cast<uint32_t>(rectW);
    scissor.extent.height = static_cast<uint32_t>(rectH);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    PushConstants pc{};
    pc.yaw = m_window->Yaw();
    pc.pitch = m_window->Pitch();
    pc.roll = m_window->Roll();
    pc.fovY = m_window->FovY();
    pc.aspect = outAspect;
    // Stabilization matrix, row-major in the window -> col-major (3 padded
    // vec3 columns) for std140.
    float stabRm[9];
    m_window->StabRowMajor(stabRm);
    for (int col = 0; col < 3; ++col) {
        pc.stab[col * 4 + 0] = stabRm[0 * 3 + col];
        pc.stab[col * 4 + 1] = stabRm[1 * 3 + col];
        pc.stab[col * 4 + 2] = stabRm[2 * 3 + col];
        pc.stab[col * 4 + 3] = 0.0f;
    }
    std::copy(grade.adjust.begin(), grade.adjust.end(), pc.colorGrade);
    render::VulkanLutStore::pushValues(grade.hasLut() ? grade.lut.get() : nullptr, grade.lutMix,
                                       pc.colorLut);
    if (imp) {
        LensPushConstants lpc{};
        lpc.view = pc;
        lpc.gains[0] = m_currentLens->gain[0];
        lpc.gains[1] = m_currentLens->gain[1];
        // The planes' sample scale and full-range chroma midpoint
        // (fisheye_view.frag): 8-bit 1 and 128/255; 10-bit in 16-bit planes
        // (the value at the top) 65535/65472, so 1023 reads as 1, and 512/1023.
        const bool deep = m_currentLens->slot->bitDepth > 8;
        lpc.gains[2] = deep ? 65535.0f / 65472.0f : 1.0f;
        lpc.gains[3] = deep ? 512.0f / 1023.0f : 128.0f / 255.0f;
        vkCmdPushConstants(commandBuffer, m_lensPipelineLayout,
                                        VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(lpc), &lpc);
    } else if (drawPicture) {
        vkCmdPushConstants(commandBuffer, m_pipelineLayout,
                                        VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstants), &pc);
    }

    if (drawPicture)
        vkCmdDraw(commandBuffer, 3, 1, 0, 0);

    // The privacy-blur region outlines, on the picture (under the insets):
    // each scissored to its own bounds, so a pass costs only its pixels.
    const auto &outlines = m_window->Outlines();
    if (m_regionPipeline && !outlines.empty()) {
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                      m_regionPipeline);
        const float border = 2.0f * float(m_window->devicePixelRatio());
        for (const auto &o : outlines) {
            RegionPushConstants rpc{};
            const int n = projectOutline(o, stabRm, pc.yaw, pc.pitch, pc.roll, pc.fovY, outAspect,
                                         float(rectW), float(rectH), rpc.pts);
            if (n < 3)
                continue;
            float lo[2] = {rpc.pts[0], rpc.pts[1]}, hi[2] = {lo[0], lo[1]};
            for (int i = 1; i < n; ++i)
                for (int k = 0; k < 2; ++k) {
                    lo[k] = std::min(lo[k], rpc.pts[i * 2 + k]);
                    hi[k] = std::max(hi[k], rpc.pts[i * 2 + k]);
                }
            const float pad = border + 1.0f;
            const int x0 = std::max(0, int(std::floor(lo[0] - pad)));
            const int y0 = std::max(0, int(std::floor(lo[1] - pad)));
            const int x1 = std::min(rectW, int(std::ceil(std::min(hi[0] + pad, 1e6f))));
            const int y1 = std::min(rectH, int(std::ceil(std::min(hi[1] + pad, 1e6f))));
            if (x1 <= x0 || y1 <= y0)
                continue;
            VkRect2D rs{};
            rs.offset.x = rectX + x0;
            rs.offset.y = rectY + y0;
            rs.extent.width = static_cast<uint32_t>(x1 - x0);
            rs.extent.height = static_cast<uint32_t>(y1 - y0);
            vkCmdSetScissor(commandBuffer, 0, 1, &rs);
            rpc.origin[0] = float(rectX);
            rpc.origin[1] = float(rectY);
            rpc.border = border;
            rpc.count = float(n);
            std::copy(o.rgb, o.rgb + 3, rpc.color);
            rpc.color[3] = 1.0f;
            vkCmdPushConstants(commandBuffer, m_regionPipelineLayout,
                                           VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(rpc), &rpc);
            vkCmdDraw(commandBuffer, 3, 1, 0, 0);
        }
        vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    }

    // The insets over the view, bottom-most first, placed in the output rect
    // exactly as the export places them (inset::place).
    bool insetBound = false;
    for (size_t i = 0; i < insets.size(); ++i) {
        const auto &d = insets[i];
        const InsetSlot &slot = m_insetSlots[i];
        if (!slot.img || slot.key != d.frameKey)
            continue;
        const inset::Rect r = inset::place(d.look, d.frame, d.aspect, rectW, rectH);
        if (r.empty() || d.look.opacity <= 0.0f)
            continue;
        if (!insetBound) {
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                          m_insetPipeline);
            insetBound = true;
        }
        const VkDescriptorSet sets[2] = {slot.set, insetLutSets[i]};
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                            m_insetPipelineLayout, 0, 2, sets, 0, nullptr);
        InsetPushConstants ipc{};
        ipc.rect[0] = float(r.x);
        ipc.rect[1] = float(r.y);
        ipc.rect[2] = float(r.w);
        ipc.rect[3] = float(r.h);
        ipc.origin[0] = float(rectX);
        ipc.origin[1] = float(rectY);
        ipc.radius = float(r.radius);
        ipc.border = float(r.border);
        ipc.borderColor[0] = ((d.frame.borderRgb >> 16) & 0xff) / 255.0f;
        ipc.borderColor[1] = ((d.frame.borderRgb >> 8) & 0xff) / 255.0f;
        ipc.borderColor[2] = (d.frame.borderRgb & 0xff) / 255.0f;
        ipc.borderColor[3] = d.look.opacity;
        std::copy(d.colorGrade.adjust.begin(), d.colorGrade.adjust.end(), ipc.grade);
        render::VulkanLutStore::pushValues(d.colorGrade.hasLut() ? d.colorGrade.lut.get() : nullptr,
                                           d.colorGrade.lutMix, ipc.lut);
        vkCmdPushConstants(commandBuffer, m_insetPipelineLayout,
                                       VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ipc), &ipc);
        vkCmdDraw(commandBuffer, 3, 1, 0, 0);
    }

    // The Track subject box, over everything.
    float marker[4];
    EquirectViewportWindow::MarkerStyle markerStyle{};
    if (m_markerPipeline && m_window->Marker(marker, &markerStyle)) {
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                      m_markerPipeline);
        MarkerPushConstants mpc{};
        mpc.rect[0] = marker[0] * rectW;
        mpc.rect[1] = marker[1] * rectH;
        mpc.rect[2] = marker[2] * rectW;
        mpc.rect[3] = marker[3] * rectH;
        mpc.origin[0] = float(rectX);
        mpc.origin[1] = float(rectY);
        mpc.border = 2.0f * float(m_window->devicePixelRatio());
        // The theme's colours while drawing, tracking, lost.
        const float *col = m_window->MarkerColor(markerStyle);
        mpc.fill = markerStyle == EquirectViewportWindow::MarkerStyle::Select ? 0.12f : 0.0f;
        mpc.color[0] = col[0];
        mpc.color[1] = col[1];
        mpc.color[2] = col[2];
        mpc.color[3] = 1.0f;
        vkCmdPushConstants(commandBuffer, m_markerPipelineLayout,
                                       VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(mpc), &mpc);
        vkCmdDraw(commandBuffer, 3, 1, 0, 0);
    }

    vkCmdEndRenderPass(commandBuffer);

    if (render::framestats::Enabled())
        statsFrameEnd(statsStart);
    m_window->frameReady();
    m_window->requestUpdate();
}
