#pragma once

// Denoise first on Vulkan: the game's own reconstruction runs once more at 1:1 on the raw render (DLSS RR
// when the game uses RR, otherwise DLSS SR), NR edits that clean image, and the edit is carried back onto
// the raw jittered render before the game's real upscale. Same arrangement as the D3D12 pass
// (DlssNr_Dx12_DenoiseFirst.cpp, second step "edit onto the raw render"); only that step exists here.

#include <shaders/dlssnr/DlssNr_Vk.h>
#include <dlssnr/DlssNr_Image_Vk.h>
#include <nvsdk_ngx_vk.h>
#include <string>

namespace DlssNr
{
struct DenoiseFirstVkInput
{
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    NVSDK_NGX_Parameter* parameters = nullptr; // the game's evaluate table
    DlssNrFrameInfo_Vk frame {};               // FrameInfo(parameters, true)
    VkImageInfo depth {}, motion {};
    bool rayReconstruction = false;
    bool depthInverted = false, jitteredMotion = false, lowResolutionMotion = true, hdr = false, autoExposure = false;
};

class DenoiseFirstVk
{
  public:
    ~DenoiseFirstVk();

    // Returns the composite (layout GENERAL) to hand to the game's upscaler as its colour, or an empty
    // image when this frame keeps the raw input (warm-up, unsupported input, failure).
    VkImageInfo Before(DlssNr_Vk& nr, VkCommandBuffer cmd, const DenoiseFirstVkInput& in);
    void Shutdown();
    const std::string& Status() const { return _status; }

  private:
    void Say(const std::string& text);
    void ReleaseFeature();
    bool CreateFeature(VkCommandBuffer cmd, const DenoiseFirstVkInput& in, bool rr, unsigned width, unsigned height);

    VkDevice _device = VK_NULL_HANDLE;
    NVSDK_NGX_Parameter* _parameters = nullptr;
    NVSDK_NGX_Handle* _feature = nullptr;
    unsigned _width = 0, _height = 0, _key = 0;
    bool _rr = false, _failed = false, _reset = true;
    int _warmup = 0;

    ImageVk _clean, _composite;
    NVSDK_NGX_Resource_VK _cleanResource {};
    std::string _status;
};
} // namespace DlssNr
