#pragma once
#include <shaders/dlssnr/DlssNr_Vk.h>
#include <memory>
#include <vector>

namespace DlssNr
{
// Per-upscaler presentation state. Global entry points only dispatch to live owners.
class FinishedVk
{
    struct Impl;
    std::unique_ptr<Impl> impl;

  public:
    FinishedVk(DlssNr_Vk& shader, VkDevice device, VkPhysicalDevice physicalDevice);
    ~FinishedVk();
    void Capture(VkCommandBuffer cmd, const VkImageInfo& depth, const VkImageInfo& motion,
                 const DlssNrFrameInfo_Vk& frame, VkInstance instance);
    void Submitted(VkQueue queue, VkCommandBuffer cmd);
    void Reset(VkCommandBuffer cmd);
    void ResetPool(VkCommandPool pool);
    bool Present(VkQueue queue, VkPresentInfoKHR* present);
};
void FinishedVkSwapchain(VkDevice device, VkSwapchainKHR swapchain, const VkSwapchainCreateInfoKHR& info);
// A compute queue of OptiScaler's own, added at device creation, so the present-time work overlaps the game's next
// frame on the graphics queue instead of sitting between frames, the way NR after the upscale overlaps on the game's
// compute queue. Without a spare queue the work stays on the present queue.
struct FinishedVkQueueRequest
{
    std::vector<VkDeviceQueueCreateInfo> infos;
    std::vector<std::vector<float>> priorities;
    uint32_t family = UINT32_MAX, index = 0;
};
bool FinishedVkRequestQueue(VkPhysicalDevice physical, const VkDeviceCreateInfo& info, FinishedVkQueueRequest& out);
void FinishedVkDeviceCreated(VkDevice device, VkPhysicalDevice physical, const FinishedVkQueueRequest& request);
// Queue families the swapchain must be shared with (concurrent sharing) for the compute route; empty when unused.
std::vector<uint32_t> FinishedVkSwapchainFamilies(VkDevice device);
void FinishedVkSubmitted(VkQueue queue, VkCommandBuffer cmd);
void FinishedVkReset(VkCommandBuffer cmd);
void FinishedVkResetPool(VkCommandPool pool);
void FinishedVkPresent(VkQueue queue, VkPresentInfoKHR* present);
std::string FinishedVkStatus();
// Records a finished-picture diagnostic event when [DlssNr] FinishedDiagnostics is on.
void FinishedVkDiagnostic(const char* key);
} // namespace DlssNr
