#include "VulkanFisheyeStitcherImpl.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <numbers>
#include <thread>
#include <vector>

// The per-frame work: lens frames in (uploaded RGB, or the decoder's own
// images), the stitch, measure and seam passes, and the reprojection to a
// flat view or the lens frames playback shows. The device, pipelines and
// buffers they run on are VulkanFisheyeStitcherSetup.cpp's.

namespace render {

namespace {

constexpr uint32_t kLocalSize = 8;

double msBetween(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b)
{
    return std::chrono::duration<double, std::milli>(b - a).count();
}

void packParams(const insv::StitchGpuParams& p, int eqW, int eqH, ShaderParams& sp)
{
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            sp.w2l0[r][c] = p.worldToLens[0][r * 3 + c];
            sp.w2l1[r][c] = p.worldToLens[1][r * 3 + c];
            sp.outRot[r][c] = p.outRot[r * 3 + c];
        }
        sp.w2l0[r][3] = sp.w2l1[r][3] = sp.outRot[r][3] = 0.0f;
    }
    sp.cxcy[0] = p.cx[0];
    sp.cxcy[1] = p.cy[0];
    sp.cxcy[2] = p.cx[1];
    sp.cxcy[3] = p.cy[1];
    sp.seam[0] = p.seamRad;
    sp.seam[1] = p.bandRad;
    sp.seam[2] = p.thetaMaxRad;
    sp.seam[3] = p.thetaRef;
    sp.edge[0] = p.edgeStartFrac;
    sp.edge[1] = p.edgeEndFrac;
    sp.edge[2] = static_cast<float>(p.photometric);
    sp.edge[3] = 0.0f;
    sp.dims[0] = p.fisheyeW;
    sp.dims[1] = p.fisheyeH;
    sp.dims[2] = eqW;
    sp.dims[3] = eqH;
    sp.meiA[0] = p.xi[0];
    sp.meiA[1] = p.xi[1];
    sp.meiA[2] = p.fx[0];
    sp.meiA[3] = p.fx[1];
    sp.meiB[0] = p.fy[0];
    sp.meiB[1] = p.fy[1];
    sp.meiB[2] = 0.0f;
    sp.meiB[3] = static_cast<float>(p.accessory);
    for (int k = 0; k < 4; ++k) {
        sp.kd0[k] = p.kdist[0][k];
        sp.kd1[k] = p.kdist[1][k];
    }
    float* pd[2] = {sp.pd0, sp.pd1};
    for (int i = 0; i < 2; ++i) {
        pd[i][0] = p.pdist[i][0];
        pd[i][1] = p.pdist[i][1];
        pd[i][2] = p.sdist[i][0];
        pd[i][3] = p.sdist[i][1];
        sp.sd[2 * i + 0] = p.sdist[i][2];
        sp.sd[2 * i + 1] = p.sdist[i][3];
    }
    const float acc[12] = {p.accessoryPoly[0], p.accessoryPoly[1], p.accessoryPoly[2],
                           p.accessoryPoly[3], p.accessoryPoly[4], p.accessoryPoly[5],
                           p.barePoly[0],      p.barePoly[1],      p.barePoly[2],
                           p.barePoly[3],      p.barePoly[4],      p.barePoly[5]};
    std::memcpy(sp.accA, acc, sizeof sp.accA);
    std::memcpy(sp.accB, acc + 4, sizeof sp.accB);
    std::memcpy(sp.accC, acc + 8, sizeof sp.accC);
    // The capture-time rotations arrive in the equirect frame; the shader's
    // lens rays are outRot * eqDir, so carry them into that frame.
    auto toStitchWorld = [&p](const float r[3], float out[4]) {
        for (int k = 0; k < 3; ++k)
            out[k] = p.outRot[k * 3 + 0] * r[0] + p.outRot[k * 3 + 1] * r[1] + p.outRot[k * 3 + 2] * r[2];
        out[3] = 0.0f;
    };
    constexpr int kTaps = insv::StitchGpuParams::kRsTaps;
    for (int i = 0; i < 2; ++i)
        for (int k = 0; k < kTaps; ++k)
            toStitchWorld(p.rsRot[i][k], sp.rsRot[i * kTaps + k]);
    sp.rs[0] = static_cast<float>(p.rsActive);
    sp.rs[1] = static_cast<float>(p.rsUseColumns);
    sp.rs[2] = sp.rs[3] = 0.0f;
    for (int k = 0; k < 3; ++k) {
        sp.seamA[k] = p.seamAxis[k];
        sp.seamU[k] = p.seamU[k];
        sp.seamV[k] = p.seamV[k];
    }
    sp.seamA[3] = static_cast<float>(p.depthActive);
    sp.seamU[3] = p.halfBaseline;
    sp.seamV[3] = p.depthRampRad;
    sp.seamC[0] = static_cast<float>(insv::kSeamQMin);
    sp.seamC[1] = static_cast<float>(insv::kSeamQMax);
    sp.seamC[2] = static_cast<float>(insv::kSeamPatchElDeg * std::numbers::pi / 180.0);
    sp.seamC[3] = 0.0f;
    std::memcpy(sp.seamQ, p.seamQ, sizeof sp.seamQ);
    // Privacy blur quads, sphere frame -> stitch world (rotations keep the
    // tangent-plane coordinates, so only the three axes move).
    const int nBlur = std::clamp(p.blurCount, 0, insv::kMaxBlurQuads);
    sp.blur0[0] = static_cast<float>(nBlur);
    sp.blur0[1] = sp.blur0[2] = sp.blur0[3] = 0.0f;
    std::memset(sp.blurQ, 0, sizeof sp.blurQ);
    for (int k = 0; k < nBlur; ++k) {
        const insv::BlurQuadFrame& b = p.blur[k];
        float(*q)[4] = &sp.blurQ[k * 6];
        toStitchWorld(b.c, q[0]);
        toStitchWorld(b.r, q[1]);
        toStitchWorld(b.u, q[2]);
        q[0][3] = b.cells[0];
        q[1][3] = b.cells[1];
        q[2][3] = b.cosRadius;
        q[3][0] = b.p[0][0];
        q[3][1] = b.p[0][1];
        q[3][2] = b.p[1][0];
        q[3][3] = b.p[1][1];
        q[4][0] = b.p[2][0];
        q[4][1] = b.p[2][1];
        q[4][2] = b.p[3][0];
        q[4][3] = b.p[3][1];
        q[5][0] = b.scale;
    }
}

// Interleave an RGB24 buffer into RGBA8 (opaque alpha), parallel over rows.
void expandRgbToRgba(const uint8_t* src, uint8_t* dst, int w, int h)
{
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    std::atomic<int> nextRow{0};
    auto worker = [&] {
        for (int y = nextRow.fetch_add(1); y < h; y = nextRow.fetch_add(1)) {
            const uint8_t* s = src + static_cast<size_t>(y) * w * 3;
            uint8_t* dpix = dst + static_cast<size_t>(y) * w * 4;
            for (int x = 0; x < w; ++x) {
                dpix[0] = s[0];
                dpix[1] = s[1];
                dpix[2] = s[2];
                dpix[3] = 255;
                s += 3;
                dpix += 4;
            }
        }
    };
    std::vector<std::thread> pool;
    for (unsigned i = 0; i < hw; ++i)
        pool.emplace_back(worker);
    for (auto& t : pool)
        t.join();
}

} // namespace

void VulkanFisheyeStitcher::Impl::recordStitchPasses(int eqW, int eqH)
{
    vkCmdFillBuffer(cmd, gainBuf, 0, 12, 0);
    VkBufferMemoryBarrier zb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    zb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    zb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    zb.srcQueueFamilyIndex = zb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    zb.buffer = gainBuf;
    zb.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1, &zb, 0, nullptr);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 0, 1, &descSet, 0,
                            nullptr);
    const uint32_t gx = (static_cast<uint32_t>(eqW) + kLocalSize - 1) / kLocalSize;
    const uint32_t gy = (static_cast<uint32_t>(eqH) + kLocalSize - 1) / kLocalSize;

    int32_t pass = 0;
    vkCmdPushConstants(cmd, pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &pass);
    vkCmdDispatch(cmd, gx, gy, 1);

    VkBufferMemoryBarrier mb = zb;
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1, &mb, 0, nullptr);

    pass = 1;
    vkCmdPushConstants(cmd, pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &pass);
    vkCmdDispatch(cmd, gx, gy, 1);
}

bool VulkanFisheyeStitcher::stitch(const uint8_t* rgb0, const uint8_t* rgb1,
                                   const insv::StitchGpuParams& params, int eqW, int eqH,
                                   std::vector<uint8_t>& outRgba, std::string& err)
{
    if (!m_ready) {
        err = "stitcher not initialised";
        return false;
    }
    const int fwv = params.fisheyeW;
    const int fhv = params.fisheyeH;
    if (fwv <= 0 || fhv <= 0 || eqW <= 0 || eqH <= 0) {
        err = "bad dimensions";
        return false;
    }
    if (!d->ensureResources(fwv, fhv, eqW, eqH, err))
        return false;

    const size_t oneRgba = static_cast<size_t>(fwv) * fhv * 4;
    expandRgbToRgba(rgb0, static_cast<uint8_t*>(d->stagingMap), fwv, fhv);
    expandRgbToRgba(rgb1, static_cast<uint8_t*>(d->stagingMap) + oneRgba, fwv, fhv);

    ShaderParams sp{};
    packParams(params, eqW, eqH, sp);
    std::memcpy(d->uboMap, &sp, sizeof(sp));

    vkResetCommandBuffer(d->cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(d->cmd, &bi);
    d->tsReset();

    VkImageMemoryBarrier toDst[2]{};
    for (int i = 0; i < 2; ++i) {
        toDst[i] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toDst[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toDst[i].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toDst[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDst[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDst[i].image = d->fisheye[i];
        toDst[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        toDst[i].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    }
    vkCmdPipelineBarrier(d->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 2, toDst);

    for (int i = 0; i < 2; ++i) {
        VkBufferImageCopy region{};
        region.bufferOffset = i == 0 ? 0 : oneRgba;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {static_cast<uint32_t>(fwv), static_cast<uint32_t>(fhv), 1};
        vkCmdCopyBufferToImage(d->cmd, d->staging, d->fisheye[i],
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }

    VkImageMemoryBarrier toRead[2]{};
    for (int i = 0; i < 2; ++i) {
        toRead[i] = toDst[i];
        toRead[i].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toRead[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toRead[i].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toRead[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    }
    vkCmdPipelineBarrier(d->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 2, toRead);

    d->tsWrite(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0);
    d->recordStitchPasses(eqW, eqH);
    d->tsWrite(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 1);

    VkBufferMemoryBarrier toHost{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    toHost.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.buffer = d->outBuf;
    toHost.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(d->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                         0, nullptr, 1, &toHost, 0, nullptr);

    vkEndCommandBuffer(d->cmd);

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &d->cmd;
    if (d->submit(si) != VK_SUCCESS) {
        err = "vkQueueSubmit failed";
        return false;
    }
    vkWaitForFences(d->device, 1, &d->fence, VK_TRUE, UINT64_MAX);
    vkResetFences(d->device, 1, &d->fence);

    const size_t outBytes = static_cast<size_t>(eqW) * eqH * 4;
    outRgba.resize(outBytes);
    std::memcpy(outRgba.data(), d->outMap, outBytes);
    return true;
}

bool VulkanFisheyeStitcher::stitchVk(const VulkanLensInput& f0, const VulkanLensInput& f1,
                                     const insv::StitchGpuParams& params, int eqW, int eqH,
                                     std::vector<uint8_t>& outRgba, VulkanLensOutcome* r0,
                                     VulkanLensOutcome* r1, std::string& err)
{
    if (!m_ready || !d->nv12) {
        err = "stitchVk requires initAdopt()";
        return false;
    }
    if (eqW <= 0 || eqH <= 0) {
        err = "bad equirect dimensions";
        return false;
    }
    if (!d->ensureOutput(eqW, eqH, err))
        return false;

    const VkImageView v0 = d->nv12ViewFor(f0.image, f0.format);
    const VkImageView v1 = d->nv12ViewFor(f1.image, f1.format);
    if (!v0 || !v1) {
        err = "vkCreateImageView (ycbcr) failed";
        return false;
    }

    ShaderParams sp{};
    packParams(params, eqW, eqH, sp);
    std::memcpy(d->uboMap, &sp, sizeof(sp));

    VkDescriptorImageInfo img0{VK_NULL_HANDLE, v0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo img1{VK_NULL_HANDLE, v1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w[2]{};
    w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[0].dstSet = d->descSet;
    w[0].dstBinding = 1;
    w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[0].pImageInfo = &img0;
    w[1] = w[0];
    w[1].dstBinding = 2;
    w[1].pImageInfo = &img1;
    vkUpdateDescriptorSets(d->device, 2, w, 0, nullptr);

    vkResetCommandBuffer(d->cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(d->cmd, &bi);
    d->tsReset();

    const VulkanLensInput* in[2] = {&f0, &f1};
    VkImageMemoryBarrier toRead[2]{};
    for (int i = 0; i < 2; ++i) {
        toRead[i] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toRead[i].oldLayout = in[i]->layout;
        toRead[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toRead[i].srcAccessMask = 0; // covered by the semaphore wait
        toRead[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toRead[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toRead[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toRead[i].image = in[i]->image;
        toRead[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    vkCmdPipelineBarrier(d->cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 2, toRead);

    d->tsWrite(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0);
    d->recordStitchPasses(eqW, eqH);
    d->tsWrite(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 1);

    VkBufferMemoryBarrier toHost{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    toHost.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.buffer = d->outBuf;
    toHost.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(d->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                         0, nullptr, 1, &toHost, 0, nullptr);

    vkEndCommandBuffer(d->cmd);

    // Wait on both decode semaphores at their current value; signal each +1.
    const VkSemaphore waitSems[2] = {f0.semaphore, f1.semaphore};
    const uint64_t waitVals[2] = {f0.semWaitValue, f1.semWaitValue};
    const uint64_t signalVals[2] = {f0.semWaitValue + 1, f1.semWaitValue + 1};
    const VkPipelineStageFlags waitStages[2] = {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
    VkTimelineSemaphoreSubmitInfo ts{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    ts.waitSemaphoreValueCount = 2;
    ts.pWaitSemaphoreValues = waitVals;
    ts.signalSemaphoreValueCount = 2;
    ts.pSignalSemaphoreValues = signalVals;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.pNext = &ts;
    si.waitSemaphoreCount = 2;
    si.pWaitSemaphores = waitSems;
    si.pWaitDstStageMask = waitStages;
    si.signalSemaphoreCount = 2;
    si.pSignalSemaphores = waitSems;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &d->cmd;
    const auto tSubmit = std::chrono::steady_clock::now();
    if (d->submit(si) != VK_SUCCESS) {
        err = "vkQueueSubmit failed";
        return false;
    }

    if (r0) { r0->semSignalValue = signalVals[0]; r0->newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; }
    if (r1) { r1->semSignalValue = signalVals[1]; r1->newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; }

    vkWaitForFences(d->device, 1, &d->fence, VK_TRUE, 5'000'000'000ULL);
    vkResetFences(d->device, 1, &d->fence);
    const auto tDone = std::chrono::steady_clock::now();
    d->tsRead(false);

    const size_t outBytes = static_cast<size_t>(eqW) * eqH * 4;
    outRgba.resize(outBytes);
    std::memcpy(outRgba.data(), d->outMap, outBytes);
    d->timing.fenceWaitMs = msBetween(tSubmit, tDone);
    d->timing.copyMs = msBetween(tDone, std::chrono::steady_clock::now());
    return true;
}

bool VulkanFisheyeStitcher::stitchAndReframeVk(const VulkanLensInput& f0, const VulkanLensInput& f1,
                                               const insv::StitchGpuParams& params, int eqW, int eqH,
                                               int outW, int outH, const float pose[4],
                                               const float stab[9], const float colorGrade[4],
                                               std::vector<uint8_t>& outNv12, VulkanLensOutcome* r0,
                                               VulkanLensOutcome* r1, std::string& err)
{
    if (!m_ready || !d->nv12) {
        err = "stitchAndReframeVk requires initAdopt()";
        return false;
    }
    if (!m_reframeReady) {
        err = "reframe reproject pipeline not built";
        return false;
    }
    if (eqW <= 0 || eqH <= 0 || outW <= 0 || outH <= 0 || (outW & 3) != 0 || (outH & 1) != 0) {
        err = "bad dimensions (outW must be a multiple of 4, outH a multiple of 2)";
        return false;
    }
    if (!d->ensureOutput(eqW, eqH, err) ||
        !d->ensureFlat(static_cast<VkDeviceSize>(outW) * outH * 3 / 2, err))
        return false;

    const VkImageView v0 = d->nv12ViewFor(f0.image, f0.format);
    const VkImageView v1 = d->nv12ViewFor(f1.image, f1.format);
    if (!v0 || !v1) {
        err = "vkCreateImageView (ycbcr) failed";
        return false;
    }

    ShaderParams sp{};
    packParams(params, eqW, eqH, sp);
    std::memcpy(d->uboMap, &sp, sizeof(sp));

    // Stitch set: point the lens bindings at this pair.
    {
        VkDescriptorImageInfo img0{VK_NULL_HANDLE, v0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo img1{VK_NULL_HANDLE, v1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w[2]{};
        w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[0].dstSet = d->descSet;
        w[0].dstBinding = 1;
        w[0].descriptorCount = 1;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[0].pImageInfo = &img0;
        w[1] = w[0];
        w[1].dstBinding = 2;
        w[1].pImageInfo = &img1;
        vkUpdateDescriptorSets(d->device, 2, w, 0, nullptr);
    }
    // Reproject set: equirect OutBuf in, NV12 flatBuf out.
    {
        VkDescriptorBufferInfo eqInfo{d->outBuf, 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo flatInfo{d->flatBuf, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w[2]{};
        w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[0].dstSet = d->rfSet;
        w[0].dstBinding = 0;
        w[0].descriptorCount = 1;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[0].pBufferInfo = &eqInfo;
        w[1] = w[0];
        w[1].dstBinding = 1;
        w[1].pBufferInfo = &flatInfo;
        vkUpdateDescriptorSets(d->device, 2, w, 0, nullptr);
    }

    vkResetCommandBuffer(d->cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(d->cmd, &bi);
    d->tsReset();

    const VulkanLensInput* in[2] = {&f0, &f1};
    VkImageMemoryBarrier toRead[2]{};
    for (int i = 0; i < 2; ++i) {
        toRead[i] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toRead[i].oldLayout = in[i]->layout;
        toRead[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toRead[i].srcAccessMask = 0; // covered by the semaphore wait
        toRead[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toRead[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toRead[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toRead[i].image = in[i]->image;
        toRead[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    vkCmdPipelineBarrier(d->cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 2, toRead);

    d->tsWrite(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0);
    d->recordStitchPasses(eqW, eqH);
    d->tsWrite(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 1);

    // Equirect just written by the stitch -> readable by the reproject.
    VkBufferMemoryBarrier eqRW{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    eqRW.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    eqRW.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    eqRW.srcQueueFamilyIndex = eqRW.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    eqRW.buffer = d->outBuf;
    eqRW.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(d->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1, &eqRW, 0, nullptr);

    ReframePush rp{};
    rp.eqW = eqW;
    rp.eqH = eqH;
    rp.outW = outW;
    rp.outH = outH;
    rp.yaw = pose ? pose[0] : 0.0f;
    rp.pitch = pose ? pose[1] : 0.0f;
    rp.roll = pose ? pose[2] : 0.0f;
    rp.fovY = pose ? pose[3] : 1.309f;
    rp.aspect = static_cast<float>(outW) / static_cast<float>(outH);
    // Row-major stab -> col-major padded vec3 columns; nullptr => identity.
    for (int col = 0; col < 3; ++col)
        for (int row = 0; row < 3; ++row)
            rp.stab[col * 4 + row] = stab ? stab[row * 3 + col] : (row == col ? 1.0f : 0.0f);
    for (int i = 0; i < 4; ++i)
        rp.colorGrade[i] = colorGrade ? colorGrade[i] : 0.0f;

    vkCmdBindPipeline(d->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d->rfPipeline);
    vkCmdBindDescriptorSets(d->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d->rfPipeLayout, 0, 1, &d->rfSet,
                            0, nullptr);
    vkCmdPushConstants(d->cmd, d->rfPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(rp), &rp);
    const uint32_t gx = (static_cast<uint32_t>(outW / 4) + 15) / 16;
    const uint32_t gy = (static_cast<uint32_t>(outH) + 3) / 4;
    vkCmdDispatch(d->cmd, gx, gy, 1);
    d->tsWrite(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 2);

    VkBufferMemoryBarrier toHost{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    toHost.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    toHost.srcQueueFamilyIndex = toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.buffer = d->flatBuf;
    toHost.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(d->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                         0, nullptr, 1, &toHost, 0, nullptr);

    vkEndCommandBuffer(d->cmd);

    const VkSemaphore waitSems[2] = {f0.semaphore, f1.semaphore};
    const uint64_t waitVals[2] = {f0.semWaitValue, f1.semWaitValue};
    const uint64_t signalVals[2] = {f0.semWaitValue + 1, f1.semWaitValue + 1};
    const VkPipelineStageFlags waitStages[2] = {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
    VkTimelineSemaphoreSubmitInfo ts{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    ts.waitSemaphoreValueCount = 2;
    ts.pWaitSemaphoreValues = waitVals;
    ts.signalSemaphoreValueCount = 2;
    ts.pSignalSemaphoreValues = signalVals;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.pNext = &ts;
    si.waitSemaphoreCount = 2;
    si.pWaitSemaphores = waitSems;
    si.pWaitDstStageMask = waitStages;
    si.signalSemaphoreCount = 2;
    si.pSignalSemaphores = waitSems;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &d->cmd;
    const auto tSubmit = std::chrono::steady_clock::now();
    if (d->submit(si) != VK_SUCCESS) {
        err = "vkQueueSubmit failed";
        return false;
    }

    if (r0) { r0->semSignalValue = signalVals[0]; r0->newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; }
    if (r1) { r1->semSignalValue = signalVals[1]; r1->newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; }

    vkWaitForFences(d->device, 1, &d->fence, VK_TRUE, 5'000'000'000ULL);
    vkResetFences(d->device, 1, &d->fence);
    const auto tDone = std::chrono::steady_clock::now();
    d->tsRead(true);

    const size_t nbytes = static_cast<size_t>(outW) * outH * 3 / 2;
    outNv12.resize(nbytes);
    std::memcpy(outNv12.data(), d->flatMap, nbytes);
    d->timing.fenceWaitMs = msBetween(tSubmit, tDone);
    d->timing.copyMs = msBetween(tDone, std::chrono::steady_clock::now());
    return true;
}

bool VulkanFisheyeStitcher::renderViewVk(const VulkanLensInput& f0, const VulkanLensInput& f1,
                                         const insv::StitchGpuParams& params, int measureW,
                                         int measureH, int outW, int outH, const float pose[4],
                                         const float stab[9], const float colorGrade[4],
                                         ViewFormat fmt, std::vector<uint8_t>& out,
                                         VulkanLensOutcome* r0, VulkanLensOutcome* r1,
                                         std::string& err)
{
    ViewRect v;
    if (pose)
        std::copy(pose, pose + 4, v.pose);
    v.w = outW;
    v.h = outH;
    return renderViewsImpl(f0, f1, params, measureW, measureH, 0, 0, {v}, stab, colorGrade, fmt,
                           nullptr, out, nullptr, r0, r1, err);
}

bool VulkanFisheyeStitcher::renderViewVkToImage(const VulkanLensInput& f0,
                                                const VulkanLensInput& f1,
                                                const insv::StitchGpuParams& params,
                                                int measureW, int measureH, int outW, int outH,
                                                const float pose[4], const float stab[9],
                                                const float colorGrade[4], FlatImageTarget& dst,
                                                VulkanLensOutcome* r0, VulkanLensOutcome* r1,
                                                std::string& err)
{
    if (dst.nbImages < 1 || dst.nbImages > 2) {
        err = "FlatImageTarget needs 1 or 2 images";
        return false;
    }
    ViewRect v;
    if (pose)
        std::copy(pose, pose + 4, v.pose);
    v.w = outW;
    v.h = outH;
    std::vector<uint8_t> unused;
    return renderViewsImpl(f0, f1, params, measureW, measureH, 0, 0, {v}, stab, colorGrade,
                           ViewFormat::Nv12, nullptr, unused, &dst, r0, r1, err);
}

bool VulkanFisheyeStitcher::renderViewsVk(const VulkanLensInput& f0, const VulkanLensInput& f1,
                                          const insv::StitchGpuParams& params, int measureW,
                                          int measureH, int canvasW, int canvasH,
                                          const std::vector<ViewRect>& views, const float stab[9],
                                          uint32_t clearRgba, std::vector<uint8_t>& out,
                                          VulkanLensOutcome* r0, VulkanLensOutcome* r1,
                                          std::string& err)
{
    if (canvasW <= 0 || canvasH <= 0 || (canvasW & 3) != 0 || views.empty()) {
        err = "bad canvas (width a multiple of 4, at least one view)";
        return false;
    }
    for (const ViewRect& v : views)
        if (v.x < 0 || v.y < 0 || (v.x & 3) != 0 || v.x + v.w > canvasW || v.y + v.h > canvasH) {
            err = "view outside the canvas (or x not a multiple of 4)";
            return false;
        }
    return renderViewsImpl(f0, f1, params, measureW, measureH, canvasW, canvasH, views, stab,
                           nullptr, ViewFormat::Rgba, &clearRgba, out, nullptr, r0, r1, err);
}

bool VulkanFisheyeStitcher::renderViewsImpl(const VulkanLensInput& f0, const VulkanLensInput& f1,
                                            const insv::StitchGpuParams& params, int measureW,
                                            int measureH, int canvasW, int canvasH,
                                            const std::vector<ViewRect>& views,
                                            const float stab[9], const float colorGrade[4],
                                            ViewFormat fmt, const uint32_t* clear,
                                            std::vector<uint8_t>& out, FlatImageTarget* img,
                                            VulkanLensOutcome* r0, VulkanLensOutcome* r1,
                                            std::string& err)
{
    if (!m_ready || !d->nv12 || !m_viewReady) {
        err = "renderViewVk requires initAdopt() and the fused view pipeline";
        return false;
    }
    // A single view is its own canvas.
    const bool single = canvasW == 0;
    const int outW = single ? views[0].w : canvasW;
    const int outH = single ? views[0].h : canvasH;
    for (const ViewRect& v : views)
        if (v.w <= 0 || v.h <= 0 || (v.w & 3) != 0 || (v.h & 1) != 0) {
            err = "bad dimensions (outW must be a multiple of 4, outH a multiple of 2)";
            return false;
        }
    if (measureW <= 0 || measureH <= 0 || outW <= 0 || outH <= 0) {
        err = "bad dimensions (outW must be a multiple of 4, outH a multiple of 2)";
        return false;
    }
    const VkDeviceSize outBytes = fmt == ViewFormat::Rgba
                                      ? static_cast<VkDeviceSize>(outW) * outH * 4
                                      : static_cast<VkDeviceSize>(outW) * outH * 3 / 2;
    if (!d->ensureUbo(err) || !d->ensureFlat(outBytes, err))
        return false;

    const VkImageView v0 = d->nv12ViewFor(f0.image, f0.format);
    const VkImageView v1 = d->nv12ViewFor(f1.image, f1.format);
    if (!v0 || !v1) {
        err = "vkCreateImageView (ycbcr) failed";
        return false;
    }

    // dims.zw = the measure grid: the equirect the stitch would have made, so
    // the photometric gains come out the same as on the equirect path.
    ShaderParams sp{};
    packParams(params, measureW, measureH, sp);
    std::memcpy(d->uboMap, &sp, sizeof(sp));

    {
        VkDescriptorImageInfo img0{VK_NULL_HANDLE, v0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo img1{VK_NULL_HANDLE, v1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorBufferInfo flatInfo{d->flatBuf, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w[3]{};
        w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[0].dstSet = d->vwSet;
        w[0].dstBinding = 1;
        w[0].descriptorCount = 1;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[0].pImageInfo = &img0;
        w[1] = w[0];
        w[1].dstBinding = 2;
        w[1].pImageInfo = &img1;
        w[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[2].dstSet = d->vwSet;
        w[2].dstBinding = 3;
        w[2].descriptorCount = 1;
        w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[2].pBufferInfo = &flatInfo;
        vkUpdateDescriptorSets(d->device, 3, w, 0, nullptr);
    }

    vkResetCommandBuffer(d->cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(d->cmd, &bi);
    d->tsReset();

    const VulkanLensInput* in[2] = {&f0, &f1};
    VkImageMemoryBarrier toRead[2]{};
    for (int i = 0; i < 2; ++i) {
        toRead[i] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toRead[i].oldLayout = in[i]->layout;
        toRead[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toRead[i].srcAccessMask = 0; // covered by the semaphore wait
        toRead[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toRead[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toRead[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toRead[i].image = in[i]->image;
        toRead[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    vkCmdPipelineBarrier(d->cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 2, toRead);

    d->tsWrite(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0);

    vkCmdFillBuffer(d->cmd, d->gainBuf, 0, 12, 0);
    if (clear) // the canvas background, where no view goes
        vkCmdFillBuffer(d->cmd, d->flatBuf, 0, outBytes, *clear);
    VkBufferMemoryBarrier zb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    zb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    zb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    zb.srcQueueFamilyIndex = zb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    zb.buffer = d->gainBuf;
    zb.size = VK_WHOLE_SIZE;
    VkBufferMemoryBarrier fb = zb;
    fb.buffer = d->flatBuf;
    vkCmdPipelineBarrier(d->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, clear ? 2 : 1,
                         clear ? std::array<VkBufferMemoryBarrier, 2>{zb, fb}.data() : &zb, 0,
                         nullptr);

    vkCmdBindPipeline(d->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d->vwPipeline);
    vkCmdBindDescriptorSets(d->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d->vwPipeLayout, 0, 1,
                            &d->vwSet, 0, nullptr);

    ViewPush vp{};
    vp.outW = outW;
    vp.outH = outH;
    vp.fmt = fmt == ViewFormat::Rgba ? 1 : 0;
    for (int col = 0; col < 3; ++col)
        for (int row = 0; row < 3; ++row)
            vp.stab[col * 4 + row] = stab ? stab[row * 3 + col] : (row == col ? 1.0f : 0.0f);
    for (int i = 0; i < 4; ++i)
        vp.colorGrade[i] = colorGrade ? colorGrade[i] : 0.0f;

    // Pass 0: one invocation per measure-grid point (every 8th texel).
    vp.pass = 0;
    vkCmdPushConstants(d->cmd, d->vwPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(vp), &vp);
    const uint32_t mgx = (static_cast<uint32_t>(measureW) + 7) / 8;
    const uint32_t mgy = (static_cast<uint32_t>(measureH) + 7) / 8;
    vkCmdDispatch(d->cmd, (mgx + 15) / 16, (mgy + 3) / 4, 1);

    VkBufferMemoryBarrier mb = zb;
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(d->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1, &mb, 0, nullptr);

    // Pass 1: each view, 4 pixels per invocation.
    vp.pass = 1;
    for (const ViewRect& v : views) {
        vp.outW = v.w;
        vp.outH = v.h;
        vp.yaw = v.pose[0];
        vp.pitch = v.pose[1];
        vp.roll = v.pose[2];
        vp.fovY = v.pose[3];
        vp.aspect = static_cast<float>(v.w) / static_cast<float>(v.h);
        vp.ox = single ? 0 : v.x;
        vp.oy = single ? 0 : v.y;
        vp.stride = single ? 0 : canvasW;
        vkCmdPushConstants(d->cmd, d->vwPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(vp), &vp);
        vkCmdDispatch(d->cmd, (static_cast<uint32_t>(v.w / 4) + 15) / 16,
                      (static_cast<uint32_t>(v.h) + 3) / 4, 1);
    }
    d->tsWrite(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 1);

    // Host read-back (flatBuf -> `out`), or the on-device copy into `img`'s
    // images -- flatBuf is tightly packed NV12 either way.
    if (img) {
        VkBufferMemoryBarrier toCopy{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        toCopy.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        toCopy.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toCopy.srcQueueFamilyIndex = toCopy.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toCopy.buffer = d->flatBuf;
        toCopy.size = VK_WHOLE_SIZE;
        VkImageMemoryBarrier toDst[2]{};
        const bool multiplane = img->nbImages == 1;
        for (int i = 0; i < img->nbImages; ++i) {
            toDst[i] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            toDst[i].srcAccessMask = static_cast<VkAccessFlags>(img->access[i]);
            toDst[i].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toDst[i].oldLayout = img->layout[i];
            toDst[i].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toDst[i].srcQueueFamilyIndex = toDst[i].dstQueueFamilyIndex =
                VK_QUEUE_FAMILY_IGNORED;
            toDst[i].image = img->img[i];
            toDst[i].subresourceRange = {multiplane ? VkImageAspectFlags(VK_IMAGE_ASPECT_PLANE_0_BIT |
                                                                         VK_IMAGE_ASPECT_PLANE_1_BIT)
                                                    : VK_IMAGE_ASPECT_COLOR_BIT,
                                         0, 1, 0, 1};
        }
        vkCmdPipelineBarrier(d->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &toCopy,
                             img->nbImages, toDst);

        // NV12 planes: Y at offset 0 (outW x outH), interleaved UV at
        // outW*outH (outW/2 x outH/2 in texel pairs).
        VkBufferImageCopy y{0, 0, 0,
                            {multiplane ? VkImageAspectFlags(VK_IMAGE_ASPECT_PLANE_0_BIT)
                                        : VK_IMAGE_ASPECT_COLOR_BIT,
                             0, 0, 1},
                            {0, 0, 0},
                            {static_cast<uint32_t>(outW), static_cast<uint32_t>(outH), 1}};
        VkBufferImageCopy uv = y;
        uv.bufferOffset = static_cast<VkDeviceSize>(outW) * outH;
        uv.imageSubresource.aspectMask =
            multiplane ? VkImageAspectFlags(VK_IMAGE_ASPECT_PLANE_1_BIT)
                       : VK_IMAGE_ASPECT_COLOR_BIT;
        uv.imageExtent = {static_cast<uint32_t>(outW / 2), static_cast<uint32_t>(outH / 2), 1};
        vkCmdCopyBufferToImage(d->cmd, d->flatBuf, img->img[0],
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &y);
        vkCmdCopyBufferToImage(d->cmd, d->flatBuf, img->img[multiplane ? 0 : 1],
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &uv);
        for (int i = 0; i < img->nbImages; ++i) {
            img->layout[i] = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            img->access[i] = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        }
    } else {
        VkBufferMemoryBarrier toHost{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        toHost.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        toHost.srcQueueFamilyIndex = toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toHost.buffer = d->flatBuf;
        toHost.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(d->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &toHost, 0, nullptr);
    }

    vkEndCommandBuffer(d->cmd);

    // The lens pair's semaphores, plus the target image's own timeline
    // semaphore (its current value could still gate a previous reader).
    VkSemaphore waitSems[4] = {f0.semaphore, f1.semaphore};
    uint64_t waitVals[4] = {f0.semWaitValue, f1.semWaitValue};
    uint64_t signalVals[4] = {f0.semWaitValue + 1, f1.semWaitValue + 1};
    VkPipelineStageFlags waitStages[4] = {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
    int nbDst = img ? img->nbImages : 0;
    for (int i = 0; i < nbDst; ++i) {
        waitSems[2 + i] = img->sem[i];
        waitVals[2 + i] = img->semValue[i];
        signalVals[2 + i] = img->semValue[i] + 1;
        waitStages[2 + i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    }
    VkTimelineSemaphoreSubmitInfo ts{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    ts.waitSemaphoreValueCount = 2 + nbDst;
    ts.pWaitSemaphoreValues = waitVals;
    ts.signalSemaphoreValueCount = 2 + nbDst;
    ts.pSignalSemaphoreValues = signalVals;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.pNext = &ts;
    si.waitSemaphoreCount = 2 + nbDst;
    si.pWaitSemaphores = waitSems;
    si.pWaitDstStageMask = waitStages;
    si.signalSemaphoreCount = 2 + nbDst;
    si.pSignalSemaphores = waitSems;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &d->cmd;
    const auto tSubmit = std::chrono::steady_clock::now();
    if (d->submit(si) != VK_SUCCESS) {
        err = "vkQueueSubmit failed";
        return false;
    }

    if (r0) { r0->semSignalValue = signalVals[0]; r0->newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; }
    if (r1) { r1->semSignalValue = signalVals[1]; r1->newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; }
    for (int i = 0; i < nbDst; ++i)
        img->semValue[i] = signalVals[2 + i];

    vkWaitForFences(d->device, 1, &d->fence, VK_TRUE, 5'000'000'000ULL);
    vkResetFences(d->device, 1, &d->fence);
    const auto tDone = std::chrono::steady_clock::now();
    d->tsRead(false);

    if (!img) {
        out.resize(static_cast<size_t>(outBytes));
        std::memcpy(out.data(), d->flatMap, static_cast<size_t>(outBytes));
    }
    d->timing.fenceWaitMs = msBetween(tSubmit, tDone);
    d->timing.copyMs = msBetween(tDone, std::chrono::steady_clock::now());
    return true;
}

bool VulkanFisheyeStitcher::measureSeamVk(const VulkanLensInput& f0, const VulkanLensInput& f1,
                                          const insv::StitchGpuParams& params,
                                          insv::SeamCosts& out, VulkanLensOutcome* r0,
                                          VulkanLensOutcome* r1, std::string& err)
{
    if (!m_ready || !d->nv12) {
        err = "measureSeamVk requires initAdopt()";
        return false;
    }
    // Pass 2 lives in both the stitch and the fused view shader. Prefer the
    // view pipeline: its binding 3 only needs a token buffer, where the
    // stitch's wants the whole equirect.
    const bool viaView = m_viewReady;
    if (!d->ensureUbo(err) ||
        !(viaView ? d->ensureFlat(16, err) : d->ensureOutput(std::max(d->ew, 16), std::max(d->eh, 8), err)))
        return false;
    const VkImageView v0 = d->nv12ViewFor(f0.image, f0.format);
    const VkImageView v1 = d->nv12ViewFor(f1.image, f1.format);
    if (!v0 || !v1) {
        err = "vkCreateImageView (ycbcr) failed";
        return false;
    }
    ShaderParams sp{};
    packParams(params, 16, 8, sp); // no equirect involved; dims.zw unused by pass 2
    std::memcpy(d->uboMap, &sp, sizeof(sp));

    const VkDescriptorSet set = viaView ? d->vwSet : d->descSet;
    {
        VkDescriptorImageInfo img0{VK_NULL_HANDLE, v0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo img1{VK_NULL_HANDLE, v1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorBufferInfo flatInfo{d->flatBuf, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w[3]{};
        w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[0].dstSet = set;
        w[0].dstBinding = 1;
        w[0].descriptorCount = 1;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[0].pImageInfo = &img0;
        w[1] = w[0];
        w[1].dstBinding = 2;
        w[1].pImageInfo = &img1;
        w[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[2].dstSet = set;
        w[2].dstBinding = 3;
        w[2].descriptorCount = 1;
        w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[2].pBufferInfo = &flatInfo;
        vkUpdateDescriptorSets(d->device, viaView ? 3 : 2, w, 0, nullptr);
    }

    VkCommandBuffer cmd = d->cmd;
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    const VulkanLensInput* in[2] = {&f0, &f1};
    VkImageMemoryBarrier toRead[2]{};
    for (int i = 0; i < 2; ++i) {
        toRead[i] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toRead[i].oldLayout = in[i]->layout;
        toRead[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toRead[i].srcAccessMask = 0; // covered by the semaphore wait
        toRead[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toRead[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toRead[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toRead[i].image = in[i]->image;
        toRead[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 2, toRead);

    if (viaView) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d->vwPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d->vwPipeLayout, 0, 1, &set, 0,
                                nullptr);
        ViewPush vp{};
        vp.pass = 2;
        vkCmdPushConstants(cmd, d->vwPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(vp), &vp);
    } else {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d->pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d->pipeLayout, 0, 1, &set, 0,
                                nullptr);
        const int32_t pass = 2;
        vkCmdPushConstants(cmd, d->pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &pass);
    }
    // One 64-invocation workgroup per (bin, candidate).
    vkCmdDispatch(cmd, insv::kSeamDepthBins, insv::kSeamDepthCands, 1);

    VkBufferMemoryBarrier toHost{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    toHost.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    toHost.srcQueueFamilyIndex = toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.buffer = d->seamBuf;
    toHost.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0,
                         nullptr, 1, &toHost, 0, nullptr);
    vkEndCommandBuffer(cmd);

    const VkSemaphore waitSems[2] = {f0.semaphore, f1.semaphore};
    const uint64_t waitVals[2] = {f0.semWaitValue, f1.semWaitValue};
    const uint64_t signalVals[2] = {f0.semWaitValue + 1, f1.semWaitValue + 1};
    const VkPipelineStageFlags waitStages[2] = {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
    VkTimelineSemaphoreSubmitInfo ts{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    ts.waitSemaphoreValueCount = 2;
    ts.pWaitSemaphoreValues = waitVals;
    ts.signalSemaphoreValueCount = 2;
    ts.pSignalSemaphoreValues = signalVals;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.pNext = &ts;
    si.waitSemaphoreCount = 2;
    si.pWaitSemaphores = waitSems;
    si.pWaitDstStageMask = waitStages;
    si.signalSemaphoreCount = 2;
    si.pSignalSemaphores = waitSems;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if (d->submit(si) != VK_SUCCESS) {
        err = "vkQueueSubmit failed";
        return false;
    }
    if (r0) { r0->semSignalValue = signalVals[0]; r0->newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; }
    if (r1) { r1->semSignalValue = signalVals[1]; r1->newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; }
    vkWaitForFences(d->device, 1, &d->fence, VK_TRUE, 5'000'000'000ULL);
    vkResetFences(d->device, 1, &d->fence);

    const float* c = static_cast<const float*>(d->seamMap);
    for (int b = 0; b < insv::kSeamDepthBins; ++b)
        for (int k = 0; k < insv::kSeamDepthCands; ++k) {
            const float* v = c + 2 * (b * insv::kSeamDepthCands + k);
            out.ncc[b][k] = v[0];
            out.texture[b][k] = v[1];
        }
    return true;
}

bool VulkanFisheyeStitcher::fillLensFrameVk(const VulkanLensInput& f0, const VulkanLensInput& f1,
                                            const insv::StitchGpuParams& params, int measureW,
                                            int measureH, LensFrame& out, VulkanLensOutcome* r0,
                                            VulkanLensOutcome* r1, std::string& err)
{
    if (!lensExportReady()) {
        err = "fillLensFrameVk requires initAdopt(), the view pipeline and external memory";
        return false;
    }
    if (!(f0.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ||
        !(f1.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
        err = "decoder images can't be copied from (no TRANSFER_SRC usage)";
        return false;
    }
    const int lw = f0.width, lh = f0.height;
    if (lw <= 0 || lh <= 0 || f1.width != lw || f1.height != lh || (lw & 1) || (lh & 1) ||
        measureW <= 0 || measureH <= 0) {
        err = "bad lens / measure dimensions";
        return false;
    }
    // Binding 3 of the view set must point somewhere even though pass 0
    // doesn't write it.
    if (!d->ensureUbo(err) || !d->ensureFlat(16, err))
        return false;

    const VkImageView v0 = d->nv12ViewFor(f0.image, f0.format);
    const VkImageView v1 = d->nv12ViewFor(f1.image, f1.format);
    if (!v0 || !v1) {
        err = "vkCreateImageView (ycbcr) failed";
        return false;
    }

    LensRingCore::Slot* slot = d->acquireLensSlot(lw, lh, err);
    if (!slot)
        return false;
    // Hand the slot back if anything below fails.
    struct Release {
        LensRingCore* ring;
        LensRingCore::Slot* slot;
        bool armed = true;
        ~Release()
        {
            if (!armed)
                return;
            std::lock_guard<std::mutex> lk(ring->mx);
            slot->busy = false;
            ring->cv.notify_all();
        }
    } release{d->ring.get(), slot};

    ShaderParams sp{};
    packParams(params, measureW, measureH, sp);
    std::memcpy(d->uboMap, &sp, sizeof(sp));
    {
        VkDescriptorImageInfo img0{VK_NULL_HANDLE, v0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo img1{VK_NULL_HANDLE, v1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorBufferInfo flatInfo{d->flatBuf, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w[3]{};
        w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[0].dstSet = d->vwSet;
        w[0].dstBinding = 1;
        w[0].descriptorCount = 1;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[0].pImageInfo = &img0;
        w[1] = w[0];
        w[1].dstBinding = 2;
        w[1].pImageInfo = &img1;
        w[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[2].dstSet = d->vwSet;
        w[2].dstBinding = 3;
        w[2].descriptorCount = 1;
        w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[2].pBufferInfo = &flatInfo;
        vkUpdateDescriptorSets(d->device, 3, w, 0, nullptr);
    }

    VkCommandBuffer cmd = d->cmd;
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    d->tsReset();

    auto imgBarrier = [](VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags src,
                         VkAccessFlags dst) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = from;
        b.newLayout = to;
        b.srcAccessMask = src;
        b.dstAccessMask = dst;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        return b;
    };

    // Decoded pair -> sampled, for the measure pass.
    const VulkanLensInput* in[2] = {&f0, &f1};
    VkImageMemoryBarrier toRead[2];
    for (int i = 0; i < 2; ++i)
        toRead[i] = imgBarrier(in[i]->image, in[i]->layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                               0, VK_ACCESS_SHADER_READ_BIT); // src covered by the semaphore wait
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 2, toRead);

    d->tsWrite(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0);

    // Photometric measure: the view pipeline's pass 0 over the equirect grid.
    vkCmdFillBuffer(cmd, d->gainBuf, 0, 12, 0);
    VkBufferMemoryBarrier zb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    zb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    zb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    zb.srcQueueFamilyIndex = zb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    zb.buffer = d->gainBuf;
    zb.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                         0, nullptr, 1, &zb, 0, nullptr);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d->vwPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d->vwPipeLayout, 0, 1, &d->vwSet,
                            0, nullptr);
    ViewPush vp{};
    vp.pass = 0;
    vkCmdPushConstants(cmd, d->vwPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(vp), &vp);
    const uint32_t mgx = (static_cast<uint32_t>(measureW) + 7) / 8;
    const uint32_t mgy = (static_cast<uint32_t>(measureH) + 7) / 8;
    vkCmdDispatch(cmd, (mgx + 15) / 16, (mgy + 3) / 4, 1);
    VkBufferMemoryBarrier toHostGain = zb;
    toHostGain.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toHostGain.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0,
                         nullptr, 1, &toHostGain, 0, nullptr);

    // Decoded pair -> transfer source; the slot's images -> transfer target
    // (old contents discarded, so no ownership acquire is needed).
    VkImageMemoryBarrier pre[2 + LensSlot::kImages];
    for (int i = 0; i < 2; ++i)
        pre[i] = imgBarrier(in[i]->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                            VK_ACCESS_TRANSFER_READ_BIT);
    for (int i = 0; i < LensSlot::kImages; ++i)
        pre[2 + i] = imgBarrier(slot->img[i], VK_IMAGE_LAYOUT_UNDEFINED,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                                VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                         2 + LensSlot::kImages, pre);

    for (int lens = 0; lens < 2; ++lens) {
        for (int plane = 0; plane < 2; ++plane) {
            VkImageCopy c{};
            c.srcSubresource = {plane == 0 ? VK_IMAGE_ASPECT_PLANE_0_BIT : VK_IMAGE_ASPECT_PLANE_1_BIT,
                                0, 0, 1};
            c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            c.extent = {static_cast<uint32_t>(plane == 0 ? lw : lw / 2),
                        static_cast<uint32_t>(plane == 0 ? lh : lh / 2), 1};
            vkCmdCopyImage(cmd, in[lens]->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           slot->img[lens * 2 + plane], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        }
    }

    // Release the slot's images to the importing device.
    VkImageMemoryBarrier rel[LensSlot::kImages];
    for (int i = 0; i < LensSlot::kImages; ++i) {
        rel[i] = imgBarrier(slot->img[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, 0);
        rel[i].srcQueueFamilyIndex = d->queueFamily;
        rel[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    }
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                         0, nullptr, 0, nullptr, LensSlot::kImages, rel);
    d->tsWrite(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 1);
    vkEndCommandBuffer(cmd);

    const VkSemaphore waitSems[2] = {f0.semaphore, f1.semaphore};
    const uint64_t waitVals[2] = {f0.semWaitValue, f1.semWaitValue};
    const uint64_t signalVals[2] = {f0.semWaitValue + 1, f1.semWaitValue + 1};
    const VkPipelineStageFlags waitStages[2] = {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
    VkTimelineSemaphoreSubmitInfo ts{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    ts.waitSemaphoreValueCount = 2;
    ts.pWaitSemaphoreValues = waitVals;
    ts.signalSemaphoreValueCount = 2;
    ts.pSignalSemaphoreValues = signalVals;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.pNext = &ts;
    si.waitSemaphoreCount = 2;
    si.pWaitSemaphores = waitSems;
    si.pWaitDstStageMask = waitStages;
    si.signalSemaphoreCount = 2;
    si.pSignalSemaphores = waitSems;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    const auto tSubmit = std::chrono::steady_clock::now();
    if (d->submit(si) != VK_SUCCESS) {
        err = "vkQueueSubmit failed";
        return false;
    }
    if (r0) { r0->semSignalValue = signalVals[0]; r0->newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; }
    if (r1) { r1->semSignalValue = signalVals[1]; r1->newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; }
    vkWaitForFences(d->device, 1, &d->fence, VK_TRUE, 5'000'000'000ULL);
    vkResetFences(d->device, 1, &d->fence);
    const auto tDone = std::chrono::steady_clock::now();
    d->tsRead(false);

    // The shader's photoGains(), on the CPU.
    const uint32_t* g = static_cast<const uint32_t*>(d->gainMap);
    constexpr float kGainFp = 4.0f;
    float g0 = 1.0f, g1 = 1.0f;
    if (params.photometric && g[2] > 64u) {
        const float m0 = float(g[0]) / (kGainFp * float(g[2]));
        const float m1 = float(g[1]) / (kGainFp * float(g[2]));
        if (m0 > 1.0f && m1 > 1.0f) {
            const float r = std::sqrt(m1 / m0);
            g0 = std::clamp(r, 0.8f, 1.25f);
            g1 = std::clamp(1.0f / r, 0.8f, 1.25f);
        }
    }

    out.fill = ++slot->fill;
    std::memcpy(out.params.data(), &sp, sizeof(sp));
    out.gain[0] = g0;
    out.gain[1] = g1;
    // The handle: hands the slot back to the ring when its last copy goes.
    std::shared_ptr<LensRingCore> ring = d->ring;
    out.slot = std::shared_ptr<const LensSlot>(&slot->pub, [ring, slot](const LensSlot*) {
        std::lock_guard<std::mutex> lk(ring->mx);
        slot->busy = false;
        ring->cv.notify_all();
    });
    release.armed = false;

    d->timing.fenceWaitMs = msBetween(tSubmit, tDone);
    d->timing.copyMs = 0.0;
    return true;
}

const StitchTiming& VulkanFisheyeStitcher::lastTiming() const
{
    return d->timing;
}

} // namespace render
