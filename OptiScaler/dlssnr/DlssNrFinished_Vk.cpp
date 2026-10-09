#include "pch.h"
#include "DlssNrFinished_Vk.h"
#include "DlssNr_Image_Vk.h"
#include "DlssNr_Status.h"
#include <Config.h>
#include <hooks/VulkanwDx12_Hooks.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace DlssNr
{
namespace
{
std::recursive_mutex finishedMutex;
std::vector<FinishedVk*> owners;
struct Swapchain
{
    VkDevice device = VK_NULL_HANDLE;
    VkSwapchainKHR handle = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR space {};
    VkExtent2D size {};
    VkImageUsageFlags usage = 0;
    bool concurrent = false; // images shared with every queue family (see FinishedVkSwapchainFamilies)
    std::vector<VkImage> images;
} screen;
struct PrivateQueue
{
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = UINT32_MAX;
} privateQueue;
std::string status = "Waiting for a finished Vulkan picture.";
// Learned from submissions: games may evaluate DLSS on a compute queue and present from the graphics queue.
std::map<VkQueue, uint32_t> queueFamilies;
uint64_t presentEpoch = 0;
// Diagnostics ([DlssNr] FinishedDiagnostics): every early exit is counted, logged with its details the first
// time it happens, and summarised every 600 presents. Behaviour does not change.
std::map<std::string, uint64_t> diagCounts;
// Present-hook CPU time (lock wait included), summarised with the diagnostics.
struct HookTimes
{
    double maxMs = 0, maxLockMs = 0;
    uint64_t over4 = 0, over16 = 0, over50 = 0;
} hookTimes, captureTimes;
// GPU time of the present-time command buffer (copies, model and composition), summarised with the diagnostics.
double presentGpuMs = 0, presentGpuMaxMs = 0;
uint64_t presentGpuSamples = 0;
bool DiagOn() { return Config::Instance()->DlssNrFinishedDiagnostics.value_or_default(); }
void Diag(const std::string& key, const std::string& detail = {})
{
    if (!DiagOn())
        return;
    if (diagCounts[key]++ == 0)
        LOG_INFO("DLSS-NR Vulkan finished picture diagnostic: {}{}{}", key, detail.empty() ? "" : " -- ", detail);
}
void DiagSummary()
{
    if (!DiagOn())
        return;
    std::string line;
    for (const auto& [key, count] : diagCounts)
        line += std::format("{}{}={}", line.empty() ? "" : ", ", key, count);
    LOG_INFO("DLSS-NR Vulkan finished picture diagnostic summary after {} presents: {}", presentEpoch,
             line.empty() ? "nothing recorded" : line);
    auto times = [](const char* name, HookTimes& t)
    {
        LOG_INFO("DLSS-NR Vulkan finished picture {} CPU time over the last 600 presents: max {:.2f} ms (lock wait "
                 "max {:.2f} ms), >4 ms {}, >16 ms {}, >50 ms {}",
                 name, t.maxMs, t.maxLockMs, t.over4, t.over16, t.over50);
        t = {};
    };
    times("present hook", hookTimes);
    times("capture", captureTimes);
    const auto nr = ReadStatus(Backend::Vulkan);
    LOG_INFO("DLSS-NR Vulkan finished picture GPU time over the last 600 presents: present-time work mean {:.2f} ms "
             "max {:.2f} ms ({} samples); NR model pass (either mode) {:.2f} ms",
             presentGpuSamples ? presentGpuMs / presentGpuSamples : 0.0, presentGpuMaxMs, presentGpuSamples,
             nr.gpuTime.value_or(0.0));
    presentGpuMs = presentGpuMaxMs = 0;
    presentGpuSamples = 0;
}
void RecordTime(HookTimes& t, std::chrono::steady_clock::time_point start, std::chrono::steady_clock::time_point locked)
{
    const auto now = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(now - start).count();
    const double lockMs = std::chrono::duration<double, std::milli>(locked - start).count();
    t.maxMs = std::max(t.maxMs, ms);
    t.maxLockMs = std::max(t.maxLockMs, lockMs);
    t.over4 += ms > 4;
    t.over16 += ms > 16;
    t.over50 += ms > 50;
}
void Say(const char* message)
{
    if (status != message)
    {
        status = message;
        LOG_INFO("DLSS-NR Vulkan finished picture: {}", message);
    }
}
void Transition(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier b { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.oldLayout = from;
    b.newLayout = to;
    b.image = image;
    b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.srcAccessMask = from == VK_IMAGE_LAYOUT_UNDEFINED || from == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                          ? 0
                          : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask =
        to == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR ? 0 : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
}
void Transition(VkCommandBuffer cmd, ImageVk& image, VkImageLayout to)
{
    Transition(cmd, image.info.Image, image.layout, to);
    image.layout = to;
}
// Same-format copy: a transfer command, which compute queues also accept.
void Copy(VkCommandBuffer cmd, VkImage from, VkImage to, VkExtent2D size)
{
    VkImageCopy region {};
    region.srcSubresource = region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.extent = { size.width, size.height, 1 };
    vkCmdCopyImage(cmd, from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}
void Blit(VkCommandBuffer cmd, VkImage from, VkImage to, VkExtent2D size)
{
    VkImageBlit region {};
    region.srcSubresource = region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.srcOffsets[1] = region.dstOffsets[1] = { (int) size.width, (int) size.height, 1 };
    vkCmdBlitImage(cmd, from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &region, VK_FILTER_NEAREST);
}
} // namespace
struct FinishedVk::Impl
{
    DlssNr_Vk& shader;
    VkDevice device;
    VkPhysicalDevice physical;
    struct Slot
    {
        // Per frame in flight: the game overwrites its depth and motion before the picture is presented.
        ImageVk depth, motion;
        DlssNrFrameInfo_Vk frame {};
        VkInstance instance = VK_NULL_HANDLE;
        VkCommandBuffer producer = VK_NULL_HANDLE, cmd = VK_NULL_HANDLE;
        VkCommandPool pool = VK_NULL_HANDLE, producerPool = VK_NULL_HANDLE;
        // Present-time work runs on the present queue; when its family differs from the capture's, it gets its
        // own pool.
        VkCommandPool presentPool = VK_NULL_HANDLE;
        VkCommandBuffer presentCmd = VK_NULL_HANDLE;
        bool timed = false; // its timestamp pair holds a result not yet read
        uint32_t presentFamily = UINT32_MAX;
        VkQueue queue = VK_NULL_HANDLE;
        VkEvent captured = VK_NULL_HANDLE;
        VkFence done = VK_NULL_HANDLE;
        uint32_t family = UINT32_MAX;
        uint64_t epoch = 0, serial = 0;
        bool pending = false, submitted = false, validCapture = false;
    };
    // A slot holds one frame's depth and motion copies (about 24 MB at 1080p) from capture until its present-time
    // work has finished on the GPU. The GPU runs a few frames behind, so four slots ran out on about a quarter of
    // the frames in Indiana Jones, and those frames went out without NR.
    std::array<Slot, 8> slots;
    // Screen-sized work images, shared by all slots: present-time work is recorded and submitted one frame after
    // another on the present queue, and every Transition is a full barrier, so one set serves every frame.
    struct Work
    {
        ImageVk input, linear, output, encoded;
        // Compute route: the screen picture copied in and out in the screen's own format (no blits on compute).
        ImageVk screenIn, screenOut;
        VkQueue queue = VK_NULL_HANDLE;
        VkFence lastDone = VK_NULL_HANDLE;
    } work;
    std::vector<VkSemaphore> presentReady;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    uint64_t serial = 0, frames = 0;
    // Two timestamps per slot around the present-time work; a slot is reused only after its fence, so its last
    // pair is complete when it is read.
    VkQueryPool queryPool = VK_NULL_HANDLE;
    double timestampPeriod = 0;
    Impl(DlssNr_Vk& s, VkDevice d, VkPhysicalDevice p) : shader(s), device(d), physical(p)
    {
        VkPhysicalDeviceProperties props {};
        vkGetPhysicalDeviceProperties(physical, &props);
        timestampPeriod = props.limits.timestampPeriod;
        VkQueryPoolCreateInfo qi { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = static_cast<uint32_t>(2 * slots.size());
        if (vkCreateQueryPool(device, &qi, nullptr, &queryPool) != VK_SUCCESS)
            queryPool = VK_NULL_HANDLE;
    }
    ~Impl()
    {
        if (vkDeviceWaitIdle(device) != VK_SUCCESS)
            return;
        if (queryPool)
            vkDestroyQueryPool(device, queryPool, nullptr);
        for (auto& s : slots)
        {
            s.depth.Destroy(device);
            s.motion.Destroy(device);
            if (s.captured)
                vkDestroyEvent(device, s.captured, nullptr);
            if (s.done)
                vkDestroyFence(device, s.done, nullptr);
            if (s.pool)
                vkDestroyCommandPool(device, s.pool, nullptr);
            if (s.presentPool)
                vkDestroyCommandPool(device, s.presentPool, nullptr);
        }
        work.input.Destroy(device);
        work.screenIn.Destroy(device);
        work.screenOut.Destroy(device);
        work.linear.Destroy(device);
        work.output.Destroy(device);
        work.encoded.Destroy(device);
        for (auto semaphore : presentReady)
            if (semaphore)
                vkDestroySemaphore(device, semaphore, nullptr);
    }
    void Capture(VkCommandBuffer cmd, const VkImageInfo& depth, const VkImageInfo& motion,
                 const DlssNrFrameInfo_Vk& frame, VkInstance instance)
    {
        Diag("capture: called");
        if (!Config::Instance()->DlssNrEnabled.value_or_default() ||
            !Config::Instance()->DlssNrFinishedPicture.value_or_default())
        {
            Diag("capture: NR or finished picture off");
            for (auto& s : slots)
                if (s.submitted)
                    s.pending = false;
            return;
        }
        if (screen.device != device || !screen.handle)
        {
            Diag("capture: no registered swapchain", std::format("screen device {} vs NR device {}, swapchain {}",
                                                                 (void*) screen.device, (void*) device,
                                                                 (void*) screen.handle));
            Say("Enable NR before creating the Vulkan swapchain; restart the game if NR was enabled during play.");
            return;
        }
        if (Config::Instance()->DlssNrRunBeforeSr.value_or_default() ||
            Config::Instance()->DlssNrDeferredDlss.value_or_default())
        {
            Say("Native Vulkan finished-picture NR runs at presentation; disable Generate model before upscale and "
                "Generate before SR, apply after SR.");
            return;
        }
        // Games can evaluate DLSS more than once per frame (Indiana Jones: a small second view). Only the call
        // that produces the screen-sized picture belongs to the frame being presented.
        if (frame.OutputWidth != screen.size.width || frame.OutputHeight != screen.size.height)
        {
            Diag(std::format("capture: skipped, DLSS output {}x{} is not the screen size {}x{}", frame.OutputWidth,
                             frame.OutputHeight, screen.size.width, screen.size.height));
            return;
        }
        if (!depth.ImageView || !motion.ImageView)
        {
            Diag("capture: no depth or motion view");
            Say("Waiting for Vulkan depth and movement data.");
            return;
        }
        auto family = Vulkan_wDx12::cmdBufferStateTracker.GetCommandBufferQueueFamily(cmd);
        auto level = Vulkan_wDx12::cmdBufferStateTracker.GetCommandBufferLevel(cmd);
        if (!family || !level || *level != VK_COMMAND_BUFFER_LEVEL_PRIMARY)
        {
            Diag("capture: command buffer not tracked or secondary",
                 std::format("family {}, level {}", family ? (int) *family : -1, level ? (int) *level : -1));
            Say("Waiting for a tracked primary Vulkan command buffer.");
            return;
        }
        Slot* next = nullptr;
        for (auto& s : slots)
        {
            if (s.pending && s.submitted && s.epoch + 1 < presentEpoch &&
                vkGetEventStatus(device, s.captured) == VK_EVENT_SET)
                s.pending = false;
            if (!s.pending && (!s.done || vkGetFenceStatus(device, s.done) == VK_SUCCESS) &&
                (!s.submitted || vkGetEventStatus(device, s.captured) == VK_EVENT_SET))
            {
                next = &s;
                break;
            }
        }
        if (!next)
        {
            std::string slotsState;
            for (auto& sl : slots)
                slotsState += std::format("[pending {} submitted {} epoch {}] ", sl.pending, sl.submitted, sl.epoch);
            Diag("capture: no free slot", std::format("present epoch {}, {}", presentEpoch, slotsState));
            return;
        }
        auto& s = *next;
        if (s.pool && s.family != *family)
        {
            Diag("capture: slot pool is for another queue family",
                 std::format("slot family {}, command buffer family {}", s.family, *family));
            return;
        }
        if (!s.pool)
        {
            VkCommandPoolCreateInfo pi { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
            pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            pi.queueFamilyIndex = *family;
            if (vkCreateCommandPool(device, &pi, nullptr, &s.pool) != VK_SUCCESS)
                return;
            s.family = *family;
            VkCommandBufferAllocateInfo ai { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            ai.commandPool = s.pool;
            ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            ai.commandBufferCount = 1;
            VkFenceCreateInfo fi { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            VkEventCreateInfo ei { VK_STRUCTURE_TYPE_EVENT_CREATE_INFO };
            if (vkAllocateCommandBuffers(device, &ai, &s.cmd) != VK_SUCCESS ||
                vkCreateFence(device, &fi, nullptr, &s.done) != VK_SUCCESS ||
                vkCreateEvent(device, &ei, nullptr, &s.captured) != VK_SUCCESS)
            {
                Diag("capture: slot command buffer, fence or event creation failed");
                return;
            }
        }
        if (!s.cmd || !s.done || !s.captured)
        {
            Diag("capture: slot objects missing");
            return;
        }
        if (!s.depth.Ensure(device, physical, depth.Width, depth.Height, VK_FORMAT_R32_SFLOAT, true) ||
            !s.motion.Ensure(device, physical, motion.Width, motion.Height, VK_FORMAT_R32G32_SFLOAT, true))
        {
            Diag("capture: depth/motion copy images could not be created",
                 std::format("depth {}x{}, motion {}x{}", depth.Width, depth.Height, motion.Width, motion.Height));
            return;
        }
        if (vkResetEvent(device, s.captured) != VK_SUCCESS)
        {
            Diag("capture: vkResetEvent failed");
            return;
        }
        auto copy = [&](const VkImageInfo& source, ImageVk& dest, bool readWrite)
        {
            Transition(cmd, dest, VK_IMAGE_LAYOUT_GENERAL);
            DlssNrConstants c {};
            c.Mode = DlssNrMode_Downsample;
            c.Width = source.Width;
            c.Height = source.Height;
            return shader.Dispatch(cmd, c, c.Width, c.Height, source.ImageView, VK_NULL_HANDLE, VK_NULL_HANDLE,
                                   VK_NULL_HANDLE, dest.info.ImageView, VK_NULL_HANDLE,
                                   readWrite ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        };
        s.validCapture = copy(depth, s.depth, frame.DepthReadWrite) && copy(motion, s.motion, frame.MotionReadWrite);
        vkCmdSetEvent(cmd, s.captured, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        s.frame = frame;
        s.frame.PreExposure = 1.0f;
        s.frame.FinishedPicture = true;
        s.frame.BeforeUpscale = false;
        s.frame.DepthReadWrite = s.frame.MotionReadWrite = false;
        s.instance = instance;
        s.producer = cmd;
        s.producerPool = Vulkan_wDx12::cmdBufferStateTracker.GetCommandBufferPool(cmd);
        s.queue = VK_NULL_HANDLE;
        s.epoch = presentEpoch;
        s.serial = ++serial;
        s.pending = true;
        s.submitted = false;
        Diag(s.validCapture ? "capture: recorded" : "capture: recorded but the depth/motion copy failed",
             std::format("output {}x{}, screen {}x{}, depth {}x{}, family {}", frame.OutputWidth, frame.OutputHeight,
                         screen.size.width, screen.size.height, depth.Width, depth.Height, *family));
    }
    bool Present(VkQueue queue, VkPresentInfoKHR* present)
    {
        Diag("present: called");
        if (!Config::Instance()->DlssNrEnabled.value_or_default() ||
            !Config::Instance()->DlssNrFinishedPicture.value_or_default())
        {
            Diag("present: NR or finished picture off");
            return false;
        }
        if (screen.device != device || present->swapchainCount != 1 || present->pSwapchains[0] != screen.handle)
        {
            Diag("present: other device or swapchain",
                 std::format("screen device {} vs NR device {}, {} swapchain(s), presented {} vs registered {}",
                             (void*) screen.device, (void*) device, present->swapchainCount,
                             present->swapchainCount ? (void*) present->pSwapchains[0] : nullptr,
                             (void*) screen.handle));
            return false;
        }
        const uint32_t index = present->pImageIndices[0];
        if (index >= screen.images.size())
        {
            Diag("present: image index out of range", std::format("{} of {}", index, screen.images.size()));
            return false;
        }
        Slot* latest = nullptr;
        for (auto& s : slots)
            if (s.pending && s.submitted && s.validCapture && s.epoch + 1 >= presentEpoch &&
                s.frame.OutputWidth == screen.size.width && s.frame.OutputHeight == screen.size.height &&
                (!latest || s.serial > latest->serial))
                latest = &s;
        if (!latest)
        {
            std::string slotsState;
            for (auto& sl : slots)
                slotsState += std::format("[pending {} submitted {} valid {} sameQueue {} epoch {} output {}x{}] ",
                                          sl.pending, sl.submitted, sl.validCapture, sl.queue == queue, sl.epoch,
                                          sl.frame.OutputWidth, sl.frame.OutputHeight);
            Diag("present: no captured frame matches",
                 std::format("present queue {}, epoch {}, screen {}x{}, {}", (void*) queue, presentEpoch,
                             screen.size.width, screen.size.height, slotsState));
            return false;
        }
        auto& s = *latest;
        const auto presentFamilyIt = queueFamilies.find(queue);
        if (presentFamilyIt == queueFamilies.end())
        {
            Diag("present: present queue family unknown", std::format("queue {}", (void*) queue));
            return false;
        }
        const uint32_t presentFamily = presentFamilyIt->second;
        if (s.queue != queue)
            Diag("present: capture and present on different queues",
                 std::format("capture queue {} family {}, present queue {} family {}", (void*) s.queue, s.family,
                             (void*) queue, presentFamily));
        // Compute route: OptiScaler's own compute queue, swapchain images shared across families, an SDR screen
        // format a compute shader can write. Otherwise the work runs on the present queue.
        VkFormatProperties props {};
        vkGetPhysicalDeviceFormatProperties(physical, screen.format, &props);
        constexpr VkFormatFeatureFlags computeFeatures =
            VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
            VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        const bool useCompute = privateQueue.device == device && privateQueue.queue && screen.concurrent &&
                                screen.space == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
                                (screen.format == VK_FORMAT_B8G8R8A8_UNORM ||
                                 screen.format == VK_FORMAT_R8G8B8A8_UNORM ||
                                 screen.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ||
                                 screen.format == VK_FORMAT_A2R10G10B10_UNORM_PACK32) &&
                                (props.optimalTilingFeatures & computeFeatures) == computeFeatures;
        Diag(useCompute ? "present: work on OptiScaler's compute queue" : "present: work on the present queue",
             std::format("private queue {}, concurrent swapchain {}, format {}", (void*) privateQueue.queue,
                         screen.concurrent, (int) screen.format));
        const uint32_t workFamily = useCompute ? privateQueue.family : presentFamily;
        VkQueue workQueue = useCompute ? privateQueue.queue : queue;
        // The game's own synchronisation orders its DLSS work before the present, so the capture is complete.
        VkCommandPool pool = s.pool;
        VkCommandBuffer cmd = s.cmd;
        if (workFamily != s.family)
        {
            if (s.presentPool && s.presentFamily != workFamily)
            {
                vkDestroyCommandPool(device, s.presentPool, nullptr);
                s.presentPool = VK_NULL_HANDLE;
                s.presentCmd = VK_NULL_HANDLE;
            }
            if (!s.presentPool)
            {
                VkCommandPoolCreateInfo pi { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
                pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
                pi.queueFamilyIndex = workFamily;
                VkCommandBufferAllocateInfo ai { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
                ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                ai.commandBufferCount = 1;
                if (vkCreateCommandPool(device, &pi, nullptr, &s.presentPool) != VK_SUCCESS)
                {
                    Diag("present: present-family command pool creation failed");
                    return false;
                }
                ai.commandPool = s.presentPool;
                if (vkAllocateCommandBuffers(device, &ai, &s.presentCmd) != VK_SUCCESS)
                {
                    Diag("present: present-family command buffer allocation failed");
                    vkDestroyCommandPool(device, s.presentPool, nullptr);
                    s.presentPool = VK_NULL_HANDLE;
                    return false;
                }
                s.presentFamily = workFamily;
            }
            pool = s.presentPool;
            cmd = s.presentCmd;
        }
        if ((screen.usage & (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) !=
            (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
        {
            Diag("present: swapchain lacks transfer usage", std::format("usage 0x{:x}", (unsigned) screen.usage));
            Say("The Vulkan swapchain does not support finished-picture transfers.");
            return false;
        }
        const bool pq = screen.space == VK_COLOR_SPACE_HDR10_ST2084_EXT;
        const bool scrgb = screen.space == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT;
        const bool srgb = screen.format == VK_FORMAT_R8G8B8A8_SRGB || screen.format == VK_FORMAT_B8G8R8A8_SRGB ||
                          screen.format == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
        if (!pq && !scrgb && screen.space != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        {
            Diag("present: unsupported colour space", std::format("{}", (int) screen.space));
            Say("This Vulkan screen colour space is not supported.");
            return false;
        }
        if (!useCompute &&
            (props.optimalTilingFeatures & (VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT)) !=
                (VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT))
        {
            Diag("present: screen format cannot be blitted", std::format("format {}", (int) screen.format));
            return false;
        }
        if (swapchain != screen.handle)
        {
            if (vkDeviceWaitIdle(device) != VK_SUCCESS)
            {
                Diag("present: vkDeviceWaitIdle failed");
                return false;
            }
            for (auto semaphore : presentReady)
                if (semaphore)
                    vkDestroySemaphore(device, semaphore, nullptr);
            presentReady.assign(screen.images.size(), VK_NULL_HANDLE);
            swapchain = screen.handle;
        }
        if (!presentReady[index])
        {
            VkSemaphoreCreateInfo ci { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
            if (vkCreateSemaphore(device, &ci, nullptr, &presentReady[index]) != VK_SUCCESS)
            {
                Diag("present: semaphore creation failed");
                return false;
            }
        }
        // The shared work images must be idle before they are recreated. A present-queue change is only counted:
        // waiting on the CPU there stalls the frame.
        const bool resize = work.input.Valid() && (work.input.info.Width != screen.size.width ||
                                                   work.input.info.Height != screen.size.height);
        if (work.queue && work.queue != workQueue)
            Diag("present: present-time work queue changed since the last finished frame");
        if (work.lastDone && resize && vkWaitForFences(device, 1, &work.lastDone, VK_TRUE, 1000000000) != VK_SUCCESS)
        {
            Diag("present: waiting for the previous present-time work failed");
            return false;
        }
        auto ensure = [&](ImageVk& image)
        {
            const bool existed = image.Valid();
            const bool ok =
                image.Ensure(device, physical, screen.size.width, screen.size.height, VK_FORMAT_R16G16B16A16_SFLOAT);
            if (ok && !existed)
                LOG_INFO("DLSS-NR Vulkan finished picture: allocated a {}x{} work image (~{} MB)", screen.size.width,
                         screen.size.height, (uint64_t) screen.size.width * screen.size.height * 8 / (1024 * 1024));
            return ok;
        };
        auto ensureScreen = [&](ImageVk& image)
        { return image.Ensure(device, physical, screen.size.width, screen.size.height, screen.format); };
        if (useCompute ? (!ensureScreen(work.screenIn) || !ensureScreen(work.screenOut) || !ensure(work.output))
                       : (!ensure(work.input) || !ensure(work.output) ||
                          (pq && (!ensure(work.linear) || !ensure(work.encoded)))))
        {
            Diag("present: screen-size work images could not be created");
            return false;
        }
        if (vkResetCommandPool(device, pool, 0) != VK_SUCCESS)
        {
            Diag("present: vkResetCommandPool failed");
            return false;
        }
        VkCommandBufferBeginInfo bi { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS)
        {
            Diag("present: vkBeginCommandBuffer failed");
            return false;
        }
        const uint32_t query = static_cast<uint32_t>(&s - slots.data()) * 2;
        if (queryPool)
        {
            uint64_t ticks[2] {};
            if (s.timed && vkGetQueryPoolResults(device, queryPool, query, 2, sizeof(ticks), ticks, sizeof(uint64_t),
                                                 VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
                ticks[1] > ticks[0])
            {
                const double ms = (double) (ticks[1] - ticks[0]) * timestampPeriod / 1e6;
                if (ms < 1000.0)
                {
                    presentGpuMs += ms;
                    presentGpuMaxMs = std::max(presentGpuMaxMs, ms);
                    ++presentGpuSamples;
                }
            }
            s.timed = false;
            vkCmdResetQueryPool(cmd, queryPool, query, 2);
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queryPool, query);
        }
        Transition(cmd, screen.images[index], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        ImageVk* input = useCompute ? &work.screenIn : &work.input;
        Transition(cmd, *input, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        if (useCompute)
            Copy(cmd, screen.images[index], work.screenIn.info.Image, screen.size);
        else
            Blit(cmd, screen.images[index], work.input.info.Image, screen.size);
        Transition(cmd, *input, VK_IMAGE_LAYOUT_GENERAL);
        Transition(cmd, work.output, VK_IMAGE_LAYOUT_GENERAL);
        Transition(cmd, s.depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        Transition(cmd, s.motion, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        bool colorReady = true;
        if (pq && !useCompute)
        {
            Transition(cmd, work.linear, VK_IMAGE_LAYOUT_GENERAL);
            DlssNrConstants conversion {};
            conversion.Width = screen.size.width;
            conversion.Height = screen.size.height;
            Transition(cmd, work.input, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            colorReady = shader.Dispatch(cmd, conversion, conversion.Width, conversion.Height, work.input.info.ImageView,
                                         VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, work.linear.info.ImageView,
                                         VK_NULL_HANDLE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, true);
            input = &work.linear;
        }
        auto frame = s.frame;
        frame.ColourIsLinearHdr = pq || scrgb || srgb;
        frame.WhitePointOverride = (pq || scrgb) ? 203.0f / 80.0f : srgb ? 1.0f : 0.0f;
        bool ran = false;
        if (colorReady)
            shader.Dispatch(cmd, input->info, s.depth.info, s.motion.info, work.output.info, frame, s.instance,
                            VK_IMAGE_LAYOUT_GENERAL, &ran);
        ImageVk* result = &work.output;
        if (useCompute && ran)
        {
            // Back to the screen's format with the plain copy shader; compute queues cannot blit.
            Transition(cmd, work.output, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            Transition(cmd, work.screenOut, VK_IMAGE_LAYOUT_GENERAL);
            DlssNrConstants copy {};
            copy.Mode = DlssNrMode_Downsample;
            copy.Width = screen.size.width;
            copy.Height = screen.size.height;
            colorReady = shader.Dispatch(cmd, copy, copy.Width, copy.Height, work.output.info.ImageView, VK_NULL_HANDLE,
                                         VK_NULL_HANDLE, VK_NULL_HANDLE, work.screenOut.info.ImageView, VK_NULL_HANDLE,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            result = &work.screenOut;
        }
        if (pq && ran && !useCompute)
        {
            Transition(cmd, work.output, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            Transition(cmd, work.encoded, VK_IMAGE_LAYOUT_GENERAL);
            DlssNrConstants conversion {};
            conversion.Mode = 1;
            conversion.Width = screen.size.width;
            conversion.Height = screen.size.height;
            colorReady = shader.Dispatch(
                cmd, conversion, conversion.Width, conversion.Height, work.output.info.ImageView, VK_NULL_HANDLE,
                work.input.info.ImageView, VK_NULL_HANDLE, work.encoded.info.ImageView, VK_NULL_HANDLE,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, true);
            result = &work.encoded;
        }
        // The original presentable image is only modified after a successful model evaluation.
        if (ran && colorReady && Config::Instance()->DlssNrApplyModel.value_or_default())
        {
            Transition(cmd, *result, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            Transition(cmd, screen.images[index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            if (useCompute)
                Copy(cmd, result->info.Image, screen.images[index], screen.size);
            else
                Blit(cmd, result->info.Image, screen.images[index], screen.size);
            Transition(cmd, screen.images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        }
        else
            Transition(cmd, screen.images[index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        if (queryPool)
        {
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool, query + 1);
            s.timed = true;
        }
        if (vkEndCommandBuffer(cmd) != VK_SUCCESS)
        {
            Diag("present: vkEndCommandBuffer failed");
            return false;
        }
        if (vkResetFences(device, 1, &s.done) != VK_SUCCESS)
        {
            Diag("present: vkResetFences failed");
            return false;
        }
        std::vector<VkPipelineStageFlags> stages(present->waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        VkSubmitInfo submit { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.waitSemaphoreCount = present->waitSemaphoreCount;
        submit.pWaitSemaphores = present->pWaitSemaphores;
        submit.pWaitDstStageMask = stages.data();
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &presentReady[index];
        if (vkQueueSubmit(workQueue, 1, &submit, s.done) != VK_SUCCESS)
        {
            Say("Vulkan finished-picture submission failed.");
            return false;
        }
        work.queue = workQueue;
        work.lastDone = s.done;
        for (auto& old : slots)
            if (old.submitted && old.serial <= s.serial)
                old.pending = false;
        present->waitSemaphoreCount = 1;
        present->pWaitSemaphores = &presentReady[index];
        Diag(ran ? "present: NR applied" : "present: submitted without a model result",
             std::format("colour ready {}, apply model {}", colorReady,
                         Config::Instance()->DlssNrApplyModel.value_or_default()));
        Say(ran ? "Applying NR to the finished Vulkan picture." : "Preparing NR for the finished Vulkan picture.");
        if (ran && (++frames == 1 || frames % 300 == 0))
            LOG_INFO("DLSS-NR Vulkan finished picture: {} frames, {}x{}", frames, screen.size.width,
                     screen.size.height);
        return true;
    }
};
FinishedVk::FinishedVk(DlssNr_Vk& shader, VkDevice device, VkPhysicalDevice physical)
    : impl(std::make_unique<Impl>(shader, device, physical))
{
    std::lock_guard lock(finishedMutex);
    owners.push_back(this);
}
FinishedVk::~FinishedVk()
{
    std::lock_guard lock(finishedMutex);
    std::erase(owners, this);
    impl.reset();
}
void FinishedVk::Capture(VkCommandBuffer cmd, const VkImageInfo& depth, const VkImageInfo& motion,
                         const DlssNrFrameInfo_Vk& frame, VkInstance instance)
{
    const auto start = std::chrono::steady_clock::now();
    std::lock_guard lock(finishedMutex);
    const auto locked = std::chrono::steady_clock::now();
    impl->Capture(cmd, depth, motion, frame, instance);
    if (DiagOn())
        RecordTime(captureTimes, start, locked);
}
void FinishedVk::Submitted(VkQueue queue, VkCommandBuffer cmd)
{
    for (auto& s : impl->slots)
        if (s.pending && !s.submitted && s.producer == cmd)
        {
            s.submitted = true;
            s.queue = queue;
            Diag("submit: capture's command buffer submitted", std::format("queue {}", (void*) queue));
        }
}
void FinishedVk::Reset(VkCommandBuffer cmd)
{
    for (auto& s : impl->slots)
        if (s.pending && !s.submitted && s.producer == cmd)
        {
            s.pending = false;
            Diag("reset: capture dropped, its command buffer was reset before submission");
        }
}
void FinishedVk::ResetPool(VkCommandPool pool)
{
    for (auto& s : impl->slots)
        if (s.pending && !s.submitted && s.producerPool == pool)
        {
            s.pending = false;
            Diag("reset: capture dropped, its command pool was reset before submission");
        }
}
bool FinishedVk::Present(VkQueue queue, VkPresentInfoKHR* present) { return impl->Present(queue, present); }
void FinishedVkSwapchain(VkDevice device, VkSwapchainKHR swapchain, const VkSwapchainCreateInfoKHR& info)
{
    if (State::Instance().isShuttingDown)
        return;
    std::lock_guard lock(finishedMutex);
    screen = {};
    screen.device = device;
    screen.handle = swapchain;
    screen.format = info.imageFormat;
    screen.space = info.imageColorSpace;
    screen.size = info.imageExtent;
    screen.usage = info.imageUsage;
    if (info.imageSharingMode == VK_SHARING_MODE_CONCURRENT && privateQueue.device == device)
        for (uint32_t i = 0; i < info.queueFamilyIndexCount; ++i)
            screen.concurrent |= info.pQueueFamilyIndices[i] == privateQueue.family;
    uint32_t count = 0;
    if (vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr) != VK_SUCCESS)
        return;
    screen.images.resize(count);
    vkGetSwapchainImagesKHR(device, swapchain, &count, screen.images.data());
}
bool FinishedVkRequestQueue(VkPhysicalDevice physical, const VkDeviceCreateInfo& info, FinishedVkQueueRequest& out)
{
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
    // A compute family without graphics: the asynchronous queues the game's own DLSS work tends to use.
    for (uint32_t f = 0; f < count; ++f)
    {
        const auto flags = families[f].queueFlags;
        if (!(flags & VK_QUEUE_COMPUTE_BIT) || (flags & VK_QUEUE_GRAPHICS_BIT))
            continue;
        uint32_t requested = 0;
        for (uint32_t i = 0; i < info.queueCreateInfoCount; ++i)
            if (info.pQueueCreateInfos[i].queueFamilyIndex == f)
                requested += info.pQueueCreateInfos[i].queueCount;
        if (requested >= families[f].queueCount)
            continue;
        out = {};
        out.family = f;
        bool extended = false;
        for (uint32_t i = 0; i < info.queueCreateInfoCount; ++i)
        {
            auto ci = info.pQueueCreateInfos[i];
            std::vector<float> priorities(ci.pQueuePriorities, ci.pQueuePriorities + ci.queueCount);
            if (ci.queueFamilyIndex == f && ci.flags == 0 && !extended)
            {
                out.index = ci.queueCount;
                priorities.push_back(1.0f);
                ++ci.queueCount;
                extended = true;
            }
            out.infos.push_back(ci);
            out.priorities.push_back(std::move(priorities));
        }
        if (!extended)
        {
            VkDeviceQueueCreateInfo ci { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
            ci.queueFamilyIndex = f;
            ci.queueCount = 1;
            out.index = 0;
            out.infos.push_back(ci);
            out.priorities.push_back({ 1.0f });
        }
        for (size_t i = 0; i < out.infos.size(); ++i)
            out.infos[i].pQueuePriorities = out.priorities[i].data();
        return true;
    }
    return false;
}
void FinishedVkDeviceCreated(VkDevice device, VkPhysicalDevice physical, const FinishedVkQueueRequest& request)
{
    std::lock_guard lock(finishedMutex);
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, request.family, request.index, &queue);
    if (!queue)
        return;
    privateQueue = { device, physical, queue, request.family };
    queueFamilies[queue] = request.family;
    LOG_INFO("DLSS-NR Vulkan finished picture: OptiScaler compute queue {} (family {}, index {})", (void*) queue,
             request.family, request.index);
}
std::vector<uint32_t> FinishedVkSwapchainFamilies(VkDevice device)
{
    std::lock_guard lock(finishedMutex);
    if (privateQueue.device != device || !privateQueue.queue)
        return {};
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(privateQueue.physical, &count, nullptr);
    std::vector<uint32_t> families;
    for (uint32_t i = 0; i < count; ++i)
        families.push_back(i);
    return families.size() > 1 ? families : std::vector<uint32_t> {};
}
void FinishedVkSubmitted(VkQueue queue, VkCommandBuffer cmd)
{
    if (State::Instance().isShuttingDown)
        return;
    std::lock_guard lock(finishedMutex);
    if (!queueFamilies.contains(queue))
    {
        if (auto family = Vulkan_wDx12::cmdBufferStateTracker.GetCommandBufferQueueFamily(cmd))
        {
            queueFamilies[queue] = *family;
            Diag("submit: learned a queue family", std::format("queue {} family {}", (void*) queue, *family));
        }
    }
    for (auto* owner : owners)
        owner->Submitted(queue, cmd);
}
void FinishedVkReset(VkCommandBuffer cmd)
{
    if (State::Instance().isShuttingDown)
        return;
    std::lock_guard lock(finishedMutex);
    for (auto* owner : owners)
        owner->Reset(cmd);
}
void FinishedVkResetPool(VkCommandPool pool)
{
    if (State::Instance().isShuttingDown)
        return;
    std::lock_guard lock(finishedMutex);
    for (auto* owner : owners)
        owner->ResetPool(pool);
}
void FinishedVkPresent(VkQueue queue, VkPresentInfoKHR* present)
{
    if (State::Instance().isShuttingDown)
        return;
    const auto start = std::chrono::steady_clock::now();
    std::lock_guard lock(finishedMutex);
    const auto locked = std::chrono::steady_clock::now();
    struct Timer
    {
        std::chrono::steady_clock::time_point start, locked;
        ~Timer()
        {
            if (DiagOn())
                RecordTime(hookTimes, start, locked);
        }
    } timer { start, locked };
    if (owners.empty())
        Diag("present: no NR finished-picture owner exists yet");
    for (auto* owner : owners)
        if (owner->Present(queue, present))
            break;
    ++presentEpoch;
    if (presentEpoch % 600 == 0)
        DiagSummary();
}
void FinishedVkDiagnostic(const char* key)
{
    std::lock_guard lock(finishedMutex);
    Diag(key);
}
std::string FinishedVkStatus()
{
    std::lock_guard lock(finishedMutex);
    return status;
}
} // namespace DlssNr
