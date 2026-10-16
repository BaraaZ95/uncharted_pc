// SPDX-License-Identifier: GPL-2.0-or-later
// Exercises the production target/resolve path on Vulkan (Lavapipe works, no game/window).
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "common/slot_vector.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_scene_resolution.h"
#include "video_core/texture_cache/blit_helper.h"
#include <vk_mem_alloc.h>

int main() {
    using namespace Vulkan;
    Instance instance(0, false);
    // Calls emitted by this executable use a local dispatcher. libbbgpu.so initializes
    // its own default dispatcher while constructing Instance.
    static vk::detail::DynamicLoader loader;
    vk::detail::DispatchLoaderDynamic dispatch;
    dispatch.init(
        loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));
    dispatch.init(instance.GetInstance());
    dispatch.init(instance.GetDevice());
    Scheduler scheduler(instance);
    Runtime runtime(instance, scheduler);
    Common::SlotVector<VideoCore::ImageView> views;
    Common::SlotVector<VideoCore::Image> images;
    VideoCore::ImageInfo ci;
    ci.size = {1920,1080,1};
    ci.resources = {1,1};
    ci.type = AmdGpu::ImageType::Color2D;
    ci.pixel_format = vk::Format::eR8G8B8A8Unorm;
    ci.num_bits = 32;
    const auto color_id = images.insert(instance, runtime, views, ci);
    auto di = ci;
    di.pixel_format = vk::Format::eD32SfloatS8Uint;
    di.props.is_depth = di.props.has_stencil = true;
    const auto depth_id = images.insert(instance, runtime, views, di);
    SceneTargets targets(instance, scheduler, runtime, [&](VideoCore::ImageId id, u64 uid) {
        if (!images.is_allocated(id)) return static_cast<VideoCore::Image*>(nullptr);
        return !uid || images[id].image_uid == uid ? &images[id] : nullptr;
    });
    const bool color_eligible = targets.Eligible(images[color_id]);
    const bool depth_eligible = targets.Eligible(images[depth_id]);
    std::fprintf(stderr, "Scene target eligibility: color=%d depth=%d\n",
                 color_eligible, depth_eligible);
    if (!color_eligible || !depth_eligible) {
        std::puts("Scene targets: unsupported format blit, live scaling skipped");
        return 0;
    }

    VkBuffer readback{};
    VmaAllocation allocation{};
    VmaAllocationInfo ai{};
    const VkBufferCreateInfo bi{.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size=16, .usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const VmaAllocationCreateInfo ac{.flags=VMA_ALLOCATION_CREATE_MAPPED_BIT |
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT, .usage=VMA_MEMORY_USAGE_AUTO,
        .requiredFlags=VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
    assert(vmaCreateBuffer(instance.GetAllocator(), &bi, &ac, &readback, &allocation, &ai) == VK_SUCCESS);
    const auto pixel = [&](vk::Image image, vk::ImageAspectFlagBits aspect,
                           vk::ImageLayout layout, u32 w, u32 h) {
        scheduler.EndRendering();
        const auto cmd = scheduler.CommandBuffer();
        const vk::MemoryBarrier2 b{.srcStageMask=vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask=vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask=vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask=vk::AccessFlagBits2::eTransferRead};
        cmd.pipelineBarrier2({.memoryBarrierCount=1,.pMemoryBarriers=&b}, dispatch);
        const vk::BufferImageCopy region{.imageSubresource={aspect,0,0,1},
            .imageOffset={s32(w-1),s32(h-1),0},.imageExtent={1,1,1}};
        cmd.copyImageToBuffer(image, layout, readback, region, dispatch);
        const vk::MemoryBarrier2 host{.srcStageMask=vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask=vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask=vk::PipelineStageFlagBits2::eHost,
            .dstAccessMask=vk::AccessFlagBits2::eHostRead};
        cmd.pipelineBarrier2({.memoryBarrierCount=1,.pMemoryBarriers=&host}, dispatch);
        scheduler.Finish();
        u32 result;
        std::memcpy(&result, ai.pMappedData, 4);
        return result;
    };
    VideoCore::ImageViewInfo cv;
    cv.format = ci.pixel_format;
    auto dv = cv;
    dv.format = di.pixel_format;
    const auto clear_native = [&](u32 rgba, u32 stencil = 7) {
        vk::ClearValue clear{};
        clear.color.float32 = std::array{float(rgba & 255)/255, float((rgba>>8)&255)/255,
                                        float((rgba>>16)&255)/255, 1.0f};
        runtime.ClearImage(&images[color_id], {}, clear);
        runtime.Transit(&images[depth_id], vk::ImageLayout::eTransferDstOptimal,
            vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite);
        runtime.FlushBarriers();
        scheduler.CommandBuffer().clearDepthStencilImage(
            images[depth_id].GetImage(), vk::ImageLayout::eTransferDstOptimal,
            vk::ClearDepthStencilValue{0.25f, stencil},
            vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eDepth |
                vk::ImageAspectFlagBits::eStencil, 0, 1, 0, 1}, dispatch);
    };
    constexpr u32 stencil_values[] = {0, 255, 128, 85, 170, 7, 13, 255, 0, 170};
    u32 stencil_index = 0;
    for (int preset : {3,1,4,2,0,3,0,1,0,3}) {
        const u32 copied_stencil = stencil_values[stencil_index++];
        const u32 resolved_stencil = copied_stencil ^ 255;
        const auto output = stencil_index > 7 ? SceneResolution::Size{3840,2160}
                                             : SceneResolution::Size{};
        const auto size = SceneResolution::ForPreset(preset, output);
        targets.SetSize(size);
        assert(SceneResolution::Unpack(SceneResolution::Pack(size)) == size);
        clear_native(0xffff0000, copied_stencil); // blue
        auto c = targets.Read(color_id,cv);
        assert(pixel(c.image,vk::ImageAspectFlagBits::eColor,c.layout,size.width,size.height)==0xffff0000);
        auto d = targets.Read(depth_id,dv);
        const u32 depth_bits = pixel(d.image,vk::ImageAspectFlagBits::eDepth,d.layout,size.width,size.height);
        assert(std::bit_cast<float>(depth_bits)==0.25f);
        assert((pixel(d.image,vk::ImageAspectFlagBits::eStencil,d.layout,size.width,size.height)&255)==copied_stencil);

        c = targets.Attachment(color_id,cv);
        d = targets.Attachment(depth_id,dv);
        auto cmd = scheduler.CommandBuffer();
        const vk::RenderingAttachmentInfo color{.imageView=c.view,.imageLayout=c.layout,
            .loadOp=vk::AttachmentLoadOp::eClear,.storeOp=vk::AttachmentStoreOp::eStore,
            .clearValue=vk::ClearValue{.color={.float32=std::array{1.f,0.f,0.f,1.f}}}};
        const vk::RenderingAttachmentInfo depth{.imageView=d.view,.imageLayout=d.layout,
            .loadOp=vk::AttachmentLoadOp::eClear,.storeOp=vk::AttachmentStoreOp::eStore,
            .clearValue=vk::ClearValue{.depthStencil={0.75f,resolved_stencil}}};
        cmd.beginRendering({.renderArea={{0,0},{size.width,size.height}},.layerCount=1,
            .colorAttachmentCount=1,.pColorAttachments=&color,
            .pDepthAttachment=&depth,.pStencilAttachment=&depth}, dispatch);
        cmd.endRendering(dispatch);
        runtime.Transit(&images[color_id],vk::ImageLayout::eTransferSrcOptimal,
            vk::PipelineStageFlagBits2::eTransfer,vk::AccessFlagBits2::eTransferRead);
        runtime.FlushBarriers();
        assert(pixel(images[color_id].GetImage(),vk::ImageAspectFlagBits::eColor,
                     vk::ImageLayout::eTransferSrcOptimal,1920,1080)==0xff0000ff);
        runtime.Transit(&images[depth_id],vk::ImageLayout::eTransferSrcOptimal,
            vk::PipelineStageFlagBits2::eTransfer,vk::AccessFlagBits2::eTransferRead);
        runtime.FlushBarriers();
        assert(std::bit_cast<float>(pixel(images[depth_id].GetImage(),vk::ImageAspectFlagBits::eDepth,
                     vk::ImageLayout::eTransferSrcOptimal,1920,1080))==0.75f);
        assert((pixel(images[depth_id].GetImage(),vk::ImageAspectFlagBits::eStencil,
                     vk::ImageLayout::eTransferSrcOptimal,1920,1080)&255)==resolved_stencil);
        clear_native(0xff00ff00); // overwrite after reduced render: proxy must be invalidated
        c=targets.Read(color_id,cv);
        assert(pixel(c.image,vk::ImageAspectFlagBits::eColor,c.layout,size.width,size.height)==0xff00ff00);
        std::printf("Scene targets: %ux%u color/depth/stencil roundtrip PASS\n",size.width,size.height);
    }

    // Half-resolution mip chain (the game's bloom pyramid): one proxy per level, scaled by the
    // scene's factor, resolved into its own native level.
    auto hi = ci;
    hi.size = {960, 540, 1};
    hi.resources = {4, 1};
    const auto half_id = images.insert(instance, runtime, views, hi);
    const auto scene = SceneResolution::ForPreset(4); // 640x360 (x3 of 1080p)
    targets.SetSize(scene);
    auto& half = images[half_id];
    assert(targets.Eligible(half) && !targets.EligibleScene(half));
    assert((targets.ProxySize(half, 0) == SceneResolution::Size{320, 180}));
    assert((targets.ProxySize(half, 2) == SceneResolution::Size{80, 45}));
    auto lv = cv;
    lv.range.base.level = 2;
    const auto proxy = targets.Attachment(half_id, lv);
    {
        auto cmd = scheduler.CommandBuffer();
        const vk::RenderingAttachmentInfo color{.imageView=proxy.view,.imageLayout=proxy.layout,
            .loadOp=vk::AttachmentLoadOp::eClear,.storeOp=vk::AttachmentStoreOp::eStore,
            .clearValue=vk::ClearValue{.color={.float32=std::array{0.f,1.f,0.f,1.f}}}};
        cmd.beginRendering({.renderArea={{0,0},{80,45}},.layerCount=1,
            .colorAttachmentCount=1,.pColorAttachments=&color}, dispatch);
        cmd.endRendering(dispatch);
    }
    assert(targets.ProxyCurrent(half, 2, 1) && !targets.ProxyCurrent(half, 0, 4));
    // A native access resolves the level: its last texel (239, 134) is the proxy's colour.
    runtime.Transit(&half, vk::ImageLayout::eTransferSrcOptimal,
                    vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
    runtime.FlushBarriers();
    {
        scheduler.EndRendering();
        const auto cmd = scheduler.CommandBuffer();
        const vk::MemoryBarrier2 b{.srcStageMask=vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask=vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask=vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask=vk::AccessFlagBits2::eTransferRead};
        cmd.pipelineBarrier2({.memoryBarrierCount=1,.pMemoryBarriers=&b}, dispatch);
        const vk::BufferImageCopy region{.imageSubresource={vk::ImageAspectFlagBits::eColor,2,0,1},
            .imageOffset={239,134,0},.imageExtent={1,1,1}};
        cmd.copyImageToBuffer(half.GetImage(), vk::ImageLayout::eTransferSrcOptimal, readback,
                              region, dispatch);
        scheduler.Finish();
        u32 result;
        std::memcpy(&result, ai.pMappedData, 4);
        assert(result == 0xff00ff00);
    }
    std::puts("Scene targets: half-resolution mip level proxy roundtrip PASS");

    // Hi-res output (BB_HIRES_OUTPUT): proxies larger than the native image.
    targets.SetSize({3840, 2160});
    // A compute write adopted into the proxy: no fill, current only once marked, and dropped by
    // the next native write.
    clear_native(0xff000000);
    const auto adopted = targets.AdoptWrite(color_id, cv);
    assert(adopted && adopted->layout == vk::ImageLayout::eGeneral);
    assert(!targets.ProxyCurrent(images[color_id]));
    targets.MarkAdopted(color_id);
    assert(targets.ProxyCurrent(images[color_id]));
    clear_native(0xff00ff00);
    assert(!targets.ProxyCurrent(images[color_id]));
    // Linear resolve: a native pixel averages its 2x2 block of the proxy (left column red,
    // right column black: half red). The default is nearest.
    targets.linear_resample = true;
    clear_native(0xff000000);
    const auto enlarged = targets.Attachment(color_id, cv);
    {
        auto cmd = scheduler.CommandBuffer();
        const vk::RenderingAttachmentInfo color{.imageView=enlarged.view,.imageLayout=enlarged.layout,
            .loadOp=vk::AttachmentLoadOp::eClear,.storeOp=vk::AttachmentStoreOp::eStore,
            .clearValue=vk::ClearValue{.color={.float32=std::array{0.f,0.f,0.f,1.f}}}};
        cmd.beginRendering({.renderArea={{0,0},{3840,2160}},.layerCount=1,
            .colorAttachmentCount=1,.pColorAttachments=&color}, dispatch);
        const vk::ClearAttachment red{.aspectMask=vk::ImageAspectFlagBits::eColor,
            .colorAttachment=0,
            .clearValue=vk::ClearValue{.color={.float32=std::array{1.f,0.f,0.f,1.f}}}};
        const vk::ClearRect block{.rect={{3838,2158},{1,2}},.baseArrayLayer=0,.layerCount=1};
        cmd.clearAttachments(red, block, dispatch);
        cmd.endRendering(dispatch);
    }
    runtime.Transit(&images[color_id],vk::ImageLayout::eTransferSrcOptimal,
        vk::PipelineStageFlagBits2::eTransfer,vk::AccessFlagBits2::eTransferRead);
    runtime.FlushBarriers();
    const u32 averaged = pixel(images[color_id].GetImage(),vk::ImageAspectFlagBits::eColor,
                               vk::ImageLayout::eTransferSrcOptimal,1920,1080) & 255;
    if (instance.GetPhysicalDevice().getFormatProperties(ci.pixel_format).optimalTilingFeatures &
        vk::FormatFeatureFlagBits::eSampledImageFilterLinear) {
        assert(averaged >= 126 && averaged <= 129);
    }
    targets.linear_resample = false;
    // The cap on images with proxies, and the 960x540 switch.
    targets.SetSize({2560, 1440});
    const auto other_id = images.insert(instance, runtime, views, ci);
    targets.max_proxies = 1;
    assert(targets.Eligible(images[color_id]) && targets.Eligible(images[other_id]));
    (void)targets.Attachment(color_id, cv);
    assert(targets.Eligible(images[color_id]) && !targets.Eligible(images[other_id]));
    assert(!targets.AdoptWrite(other_id, cv));
    // A level load recreates targets: the proxy of a destroyed image is freed, which makes
    // room under the cap again.
    assert(targets.PruneStale() == 0);
    const auto gone_id = images.insert(instance, runtime, views, ci);
    targets.max_proxies = 0;
    (void)targets.Attachment(gone_id, cv);
    scheduler.Finish();
    images.erase(gone_id);
    assert(targets.PruneStale() == 1 && targets.PruneStale() == 0);
    targets.max_proxies = 2;
    assert(targets.Eligible(images[other_id]));
    targets.max_proxies = 0;
    assert(targets.Eligible(images[other_id]));
    assert(targets.Eligible(images[half_id]));
    targets.half_res = false;
    assert(!targets.Eligible(images[half_id]) && targets.Eligible(images[color_id]));
    targets.half_res = true;
    std::puts("Scene targets: hi-res output proxies (adopt, linear resolve, cap) PASS");
    scheduler.Finish();
    vmaDestroyBuffer(instance.GetAllocator(),readback,allocation);
}
