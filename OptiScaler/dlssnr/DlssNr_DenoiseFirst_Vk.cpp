#include "pch.h"
#include "DlssNr_DenoiseFirst_Vk.h"
#include "DlssNrPipeline_Vk.h"

#include <Config.h>
#include <proxies/NVNGX_Proxy.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace DlssNr
{
namespace
{
// NGX RR guide keys (nvsdk_ngx_defs_dlssd.h), the same set the D3D12 private RR reads.
constexpr const char* kRrGuides[] = { "DLSS.Input.DiffuseAlbedo", "DLSS.Input.SpecularAlbedo", "GBuffer.Normals",
                                      "GBuffer.Roughness",        "MotionVectorsReflection",   "DLSSD.SpecularHitDistance",
                                      "DLSSD.DiffuseHitDistance" };
constexpr const char* kRrOffsets[] = { "DLSS.Input.DiffuseAlbedo",  "DLSS.Input.SpecularAlbedo", "DLSS.Input.Normals",
                                       "DLSS.Input.Roughness",      nullptr,
                                       "DLSSD.SpecularHitDistance", "DLSSD.DiffuseHitDistance" };
constexpr VkImageLayout kReadable = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

std::string Hex(NVSDK_NGX_Result result)
{
    char text[16];
    std::snprintf(text, sizeof(text), "%08X", (unsigned) result);
    return text;
}

void* Pointer(NVSDK_NGX_Parameter* p, const char* key)
{
    void* value = nullptr;
    p->Get(key, &value);
    return value;
}

unsigned UInt(NVSDK_NGX_Parameter* p, const char* key, unsigned fallback = 0)
{
    unsigned value = fallback;
    p->Get(key, &value);
    return value;
}

float Float(NVSDK_NGX_Parameter* p, const char* key, float fallback)
{
    float value = fallback;
    p->Get(key, &value);
    return std::isfinite(value) ? value : fallback;
}

// The guides a private RR pass needs: albedos, normals and roughness (its own buffer, or packed in normals when
// the roughness mode says so). Reflection vectors and hit distances are optional for RR; Indiana Jones sends
// neither and its own RR runs on exactly this set, so the private pass is given whatever the game gives.
bool HasRrGuides(NVSDK_NGX_Parameter* p)
{
    const unsigned roughness = UInt(p, "DLSS.Roughness.Mode");
    return roughness <= 1 && Pointer(p, kRrGuides[0]) && Pointer(p, kRrGuides[1]) && Pointer(p, kRrGuides[2]) &&
           (roughness == 1 || Pointer(p, kRrGuides[3]));
}

void Transition(DlssNr_Vk& nr, VkCommandBuffer cmd, ImageVk& image, VkImageLayout to)
{
    if (image.layout == to)
        return;
    nr.SetImageLayout(cmd, image.info.Image, image.layout, to, image.info.SubresourceRange);
    image.layout = to;
}
} // namespace

DenoiseFirstVk::~DenoiseFirstVk()
{
    if (State::Instance().isShuttingDown)
        return; // the device may already be gone; leak rather than touch it
    Shutdown();
}

void DenoiseFirstVk::Say(const std::string& text)
{
    if (text == _status)
        return;
    _status = text;
    LOG_INFO("DLSS-NR Vulkan denoise first: {}", text);
}

void DenoiseFirstVk::ReleaseFeature()
{
    if (_feature && NVNGXProxy::VULKAN_ReleaseFeature())
        NVNGXProxy::VULKAN_ReleaseFeature()(_feature);
    _feature = nullptr;
    if (_parameters && NVNGXProxy::VULKAN_DestroyParameters())
        NVNGXProxy::VULKAN_DestroyParameters()(_parameters);
    _parameters = nullptr;
}

void DenoiseFirstVk::Shutdown()
{
    if (_device != VK_NULL_HANDLE)
        vkDeviceWaitIdle(_device);
    ReleaseFeature();
    if (_device != VK_NULL_HANDLE)
    {
        _clean.Destroy(_device);
        _composite.Destroy(_device);
    }
    _device = VK_NULL_HANDLE;
    _width = _height = _key = 0;
    _failed = false;
    _reset = true;
}

bool DenoiseFirstVk::CreateFeature(VkCommandBuffer cmd, const DenoiseFirstVkInput& in, bool rr, unsigned width,
                                   unsigned height)
{
    if (!NVNGXProxy::IsVulkanInited() &&
        !NVNGXProxy::InitVulkan(in.instance, in.physicalDevice, in.device, vkGetInstanceProcAddr, vkGetDeviceProcAddr))
    {
        Say("the NVIDIA NGX Vulkan driver could not initialize");
        return false;
    }
    auto allocate = NVNGXProxy::VULKAN_AllocateParameters() ? NVNGXProxy::VULKAN_AllocateParameters()
                                                             : NVNGXProxy::VULKAN_GetCapabilityParameters();
    if (!allocate || !NVNGXProxy::VULKAN_CreateFeature1() || !NVNGXProxy::VULKAN_EvaluateFeature() ||
        !NVNGXProxy::VULKAN_ReleaseFeature() || !NVNGXProxy::VULKAN_DestroyParameters())
    {
        Say("the NVIDIA NGX Vulkan interface is incomplete");
        return false;
    }
    if (allocate(&_parameters) != NVSDK_NGX_Result_Success || !_parameters)
    {
        Say("could not allocate NGX parameters");
        return false;
    }
    const bool exposureTexture = Pointer(in.parameters, NVSDK_NGX_Parameter_ExposureTexture) != nullptr;
    // Without the game's exposure texture the private pass meters the frame itself.
    const bool autoExposure = in.autoExposure || !exposureTexture;
    const unsigned flags = (in.depthInverted ? NVSDK_NGX_DLSS_Feature_Flags_DepthInverted : 0) |
                           (in.jitteredMotion ? NVSDK_NGX_DLSS_Feature_Flags_MVJittered : 0) |
                           NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                           (rr || in.hdr ? NVSDK_NGX_DLSS_Feature_Flags_IsHDR : 0) |
                           (autoExposure ? NVSDK_NGX_DLSS_Feature_Flags_AutoExposure : 0);
    auto* p = _parameters;
    p->Set(NVSDK_NGX_Parameter_Width, width);
    p->Set(NVSDK_NGX_Parameter_Height, height);
    p->Set(NVSDK_NGX_Parameter_OutWidth, width);
    p->Set(NVSDK_NGX_Parameter_OutHeight, height);
    p->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
    p->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
    p->Set(NVSDK_NGX_Parameter_PerfQualityValue, (int) NVSDK_NGX_PerfQuality_Value_DLAA);
    p->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, (int) flags);
    if (rr)
    {
        p->Set(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 0);
        p->Set("DLSS.Denoise.Mode", 1);
        p->Set("DLSS.Roughness.Mode", UInt(in.parameters, "DLSS.Roughness.Mode"));
        p->Set("DLSS.Use.HW.Depth", UInt(in.parameters, "DLSS.Use.HW.Depth", 1));
    }
    const auto created = NVNGXProxy::VULKAN_CreateFeature1()(
        in.device, cmd, rr ? NVSDK_NGX_Feature_RayReconstruction : NVSDK_NGX_Feature_SuperSampling, p, &_feature);
    if (created != NVSDK_NGX_Result_Success || !_feature)
    {
        Say("1:1 " + std::string(rr ? "RR" : "DLSS") + " creation failed 0x" +
            Hex(created));
        ReleaseFeature();
        return false;
    }
    LOG_INFO("DLSS-NR Vulkan denoise first: 1:1 {} {}x{}, inverted {}, jittered MV {}, HDR {}, auto exposure {}",
             rr ? "DLSS RR" : "DLSS SR", width, height, in.depthInverted, in.jitteredMotion, rr || in.hdr,
             autoExposure);
    return true;
}

VkImageInfo DenoiseFirstVk::Before(DlssNr_Vk& nr, VkCommandBuffer cmd, const DenoiseFirstVkInput& in)
{
    const auto& cfg = *Config::Instance();
    auto* source = in.parameters;
    if (!source || !nr.CanRender() || !nr.DenoiseFirstReady())
    {
        Say("inactive: the NR pass or its edit shader is unavailable");
        return {};
    }
    if (cfg.DlssNrDebugView.value_or_default() != 0 || cfg.DlssNrCompare.value_or_default() != 0 ||
        cfg.DlssNrShowSkinMask.value_or_default())
    {
        Say("Disable the Debug view, Compare and Show skin mask first.");
        return {};
    }
    const auto colour = ParameterImage(source, NVSDK_NGX_Parameter_Color);
    if (!colour.Image || !in.depth.Image || !in.motion.Image)
    {
        Say("inactive: colour, depth and motion are required");
        return {};
    }
    if (!in.lowResolutionMotion)
    {
        Say("inactive: this game supplies display-resolution motion vectors; the 1:1 pass needs render-resolution "
            "ones");
        return {};
    }
    const unsigned width = in.frame.RenderSubrectWidth ? in.frame.RenderSubrectWidth : colour.Width;
    const unsigned height = in.frame.RenderSubrectHeight ? in.frame.RenderSubrectHeight : colour.Height;
    const bool rr = in.rayReconstruction && HasRrGuides(source);
    const unsigned key = (in.depthInverted ? 1u : 0u) | (in.jitteredMotion ? 2u : 0u) | (in.hdr ? 4u : 0u) |
                         (in.autoExposure ? 8u : 0u) | (rr ? 16u : 0u);

    if (_device != in.device || _width != width || _height != height || _key != key)
    {
        Shutdown();
        _device = in.device;
        _width = width;
        _height = height;
        _key = key;
        _rr = rr;
        if (in.rayReconstruction && !rr)
        {
            std::string present;
            for (const char* guide :
                 { "DLSS.Input.DiffuseAlbedo", "DLSS.Input.SpecularAlbedo", "GBuffer.Normals", "GBuffer.Roughness",
                   "MotionVectorsReflection", "DLSSD.SpecularHitDistance", "DLSSD.DiffuseHitDistance",
                   "WorldToViewMatrix", "ViewToClipMatrix", "GBuffer.Albedo", "GBuffer.DiffuseAlbedo",
                   "GBuffer.SpecularAlbedo", "DLSS.Input.Normals", "DLSS.Input.Roughness" })
                if (Pointer(source, guide))
                    present += std::string(present.empty() ? "" : ", ") + guide;
            LOG_WARN("DLSS-NR Vulkan denoise first: the game runs RR but its guides are not readable here (roughness "
                     "mode {}, present: {}); the 1:1 pass falls back to DLSS SR, which does not denoise path tracing",
                     UInt(source, "DLSS.Roughness.Mode"), present.empty() ? "none" : present);
        }
    }
    if (_failed)
        return {};

    if (!_clean.Ensure(in.device, in.physicalDevice, width, height, VK_FORMAT_R16G16B16A16_SFLOAT) ||
        !_composite.Ensure(in.device, in.physicalDevice, width, height, VK_FORMAT_R16G16B16A16_SFLOAT))
    {
        _failed = true;
        Say("allocation failed");
        return {};
    }
    if (!_feature)
    {
        if (!CreateFeature(cmd, in, rr, width, height))
        {
            _failed = true;
            return {};
        }
        _warmup = 2; // creation records GPU uploads; evaluate only after that work has been submitted
        _reset = true;
        Say(std::string("1:1 ") + (rr ? "RR" : "DLSS") + " created; warming up");
        return {};
    }
    if (_warmup > 0)
    {
        --_warmup;
        return {};
    }

    // Step one: the game's reconstruction at 1:1. Jittered noisy samples in, clean steady image out.
    Transition(nr, cmd, _clean, VK_IMAGE_LAYOUT_GENERAL);
    _cleanResource = WrapImage(_clean.info, true);
    auto* p = _parameters;
    p->Set(NVSDK_NGX_Parameter_Color, Pointer(source, NVSDK_NGX_Parameter_Color));
    p->Set(NVSDK_NGX_Parameter_Depth, Pointer(source, NVSDK_NGX_Parameter_Depth));
    p->Set(NVSDK_NGX_Parameter_MotionVectors, Pointer(source, NVSDK_NGX_Parameter_MotionVectors));
    p->Set(NVSDK_NGX_Parameter_ExposureTexture, Pointer(source, NVSDK_NGX_Parameter_ExposureTexture));
    p->Set(NVSDK_NGX_Parameter_Output, (void*) &_cleanResource);
    p->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, width);
    p->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, height);
    p->Set(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, in.frame.DepthSubrectBaseX);
    p->Set(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, in.frame.DepthSubrectBaseY);
    p->Set(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, in.frame.MotionSubrectBaseX);
    p->Set(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, in.frame.MotionSubrectBaseY);
    const float jitterX = Float(source, NVSDK_NGX_Parameter_Jitter_Offset_X, 0.0f);
    const float jitterY = Float(source, NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.0f);
    p->Set(NVSDK_NGX_Parameter_Reset, (unsigned) (in.frame.Reset || _reset));
    p->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, jitterX);
    p->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, jitterY);
    p->Set(NVSDK_NGX_Parameter_MV_Scale_X, in.frame.MvScaleX);
    p->Set(NVSDK_NGX_Parameter_MV_Scale_Y, in.frame.MvScaleY);
    p->Set(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, Float(source, NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, 16.67f));
    p->Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, Float(source, NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1.0f));
    p->Set(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, Float(source, NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1.0f));
    p->Set(NVSDK_NGX_Parameter_Sharpness, 0.0f);
    if (_rr)
    {
        for (unsigned i = 0; i < std::size(kRrGuides); ++i)
        {
            p->Set(kRrGuides[i], Pointer(source, kRrGuides[i]));
            if (kRrOffsets[i])
            {
                const std::string base = kRrOffsets[i];
                p->Set((base + ".Subrect.Base.X").c_str(), UInt(source, (base + ".Subrect.Base.X").c_str()));
                p->Set((base + ".Subrect.Base.Y").c_str(), UInt(source, (base + ".Subrect.Base.Y").c_str()));
            }
        }
        p->Set("WorldToViewMatrix", Pointer(source, "WorldToViewMatrix"));
        p->Set("ViewToClipMatrix", Pointer(source, "ViewToClipMatrix"));
    }
    const auto evaluated = NVNGXProxy::VULKAN_EvaluateFeature()(cmd, _feature, p, nullptr);
    if (evaluated != NVSDK_NGX_Result_Success)
    {
        _failed = true;
        Say(std::string("1:1 ") + (_rr ? "RR" : "DLSS") + " evaluation failed 0x" +
            Hex(evaluated));
        return {};
    }
    Transition(nr, cmd, _clean, kReadable);

    // Step two: NR on the clean image. PrepareInput writes NR's answer to its own scratch image.
    const auto edited = PrepareInput(nr, cmd, in.instance, _clean.info, in.depth, in.motion, in.frame);
    if (!edited.Image)
    {
        _reset = true;
        Say("waiting for NR evaluation; the game's upscale keeps its raw input");
        return {};
    }

    // Step three: edit onto the raw render. Measured on The Witcher 3 (D3D12): the raw render lines up
    // with the clean image shifted by MINUS the NGX jitter offset, so negative is the default.
    const bool shift = cfg.DlssNrDenoiseFirstShift.value_or_default();
    const float sign = cfg.DlssNrDenoiseFirstFlipJitter.value_or_default() ? 1.0f : -1.0f;
    DlssNrConstants c {};
    c.Mode = 0;
    c.Width = width;
    c.Height = height;
    c.TransferStrength = 1.0f;
    c.DebugView = (uint32_t) std::clamp(cfg.DlssNrDenoiseFirstKernel.value_or_default(), 0, 2);
    c.MaxRatio = std::clamp(cfg.DlssNrMaxRatio.value_or_default(), 1.0f, 8.0f);
    c.MvScaleX = shift ? sign * jitterX : 0.0f;
    c.MvScaleY = shift ? sign * jitterY : 0.0f;
    c.CompareMode = cfg.DlssNrDenoiseFirstEdit.value_or_default() == 1 ? 1u : 0u;
    c.CompareSwap = cfg.DlssNrDenoiseFirstNeighbourhoodClamp.value_or_default() ? 1u : 0u;
    c.Passthrough = cfg.DlssNrDenoiseFirstFireflyGuard.value_or_default() ? 1u : 0u;

    nr.SetImageLayout(cmd, edited.Image, VK_IMAGE_LAYOUT_GENERAL, kReadable, edited.SubresourceRange);
    Transition(nr, cmd, _composite, VK_IMAGE_LAYOUT_GENERAL);
    const bool ok = nr.DispatchDenoiseFirst(cmd, c, colour.ImageView, kReadable, edited.ImageView,
                                            _clean.info.ImageView, _composite.info.ImageView);
    nr.SetImageLayout(cmd, edited.Image, kReadable, VK_IMAGE_LAYOUT_GENERAL, edited.SubresourceRange);
    if (!ok)
    {
        _reset = true;
        Say("edit composition failed; the game's upscale keeps its raw input");
        return {};
    }
    _reset = false;
    Say(std::string("running: ") + (_rr ? "RR" : "DLSS") + " 1:1 -> NR -> edit onto raw render -> game upscaler (" +
        (c.CompareMode ? "ratio" : "difference") + ", jitter shift " + (shift ? (sign > 0 ? "on, flipped" : "on") : "off") +
        ")");
    return _composite.info;
}
} // namespace DlssNr
