#include <pch.h>

#include "IFeature_Vk.h"
#include "State.h"
#include "nvsdk_ngx_vk.h"
#include <dlssnr/DlssNrPipeline_Vk.h>
#include <dlssnr/DlssNrFinished_Vk.h>

IFeature_Vk::~IFeature_Vk()
{
    if (State::Instance().isShuttingDown)
    {
        NeuralRendering.release();
        OutputScaler.release();
        RCAS.release();
        Magnifier.release();
    }
}

bool IFeature_Vk::Init(VkInstance InInstance, VkPhysicalDevice InPD, VkDevice InDevice, VkCommandBuffer InCmdBuffer,
                       PFN_vkGetInstanceProcAddr InGIPA, PFN_vkGetDeviceProcAddr InGDPA,
                       NVSDK_NGX_Parameter* InParameters)
{
    Instance = InInstance;
    PhysicalDevice = InPD;
    Device = InDevice;
    GIPA = InGIPA;
    GDPA = InGDPA;

    auto result = InitInternal(InCmdBuffer, InParameters);

    if (result)
    {

        OutputScaler = std::make_unique<OS_Vk>("Output Scaling", InDevice, InPD, (TargetWidth() < DisplayWidth()));
        RCAS = std::make_unique<RCAS_Vk>("RCAS", InDevice, InPD);
        Magnifier = std::make_unique<Magnifier_Vk>("Magnifier", InDevice, InPD);

        // UpscalerTime = std::make_unique<GpuTime_Vk>(InDevice);
    }

    return result;
}

bool IFeature_Vk::Evaluate(VkCommandBuffer InCmdBuffer, NVSDK_NGX_Parameter* InParameters)
{
    if (!IsInited())
    {
        LOG_ERROR("Not inited!");
        return false;
    }

    if (!NeuralRendering && Config::Instance()->DlssNrEnabled.value_or_default() && !IsWithDx12())
        NeuralRendering = std::make_unique<DlssNr_Vk>("Neural Rendering", Device, PhysicalDevice);

    if (Config::Instance()->OverrideSharpness.value_or_default())
        _sharpness = Config::Instance()->Sharpness.value_or_default();
    else
        _sharpness = GetSharpness(InParameters);

    if (_sharpness > 1.0f)
        _sharpness = 1.0f;

    auto upscaler = GetUpscalerType();
    bool useRcas = upscaler == Upscaler::XeSS ||
                   (upscaler == Upscaler::DLSS && Version() >= feature_version(2, 5, 1)) || upscaler == Upscaler::DLSSD;

    if (!useRcas)
        useRcas = Config::Instance()->RcasEnabled.value_or_default();

    if (_sharpness == 0.0f)
        useRcas = false;

    if (!useRcas && (Config::Instance()->MotionSharpnessEnabled.value_or_default() &&
                     Config::Instance()->MotionSharpness.value_or_default() > 0.0f))
    {
        useRcas = true;
    }

    if (!RCAS->IsInit())
        useRcas = false;

    bool useOutputScaling =
        Config::Instance()->OutputScalingEnabled.value_or_default() && (LowResMV() || RenderWidth() == DisplayWidth());

    if (!OutputScaler->IsInit())
        useOutputScaling = false;

    NVSDK_NGX_Resource_VK* paramOutput = nullptr;
    NVSDK_NGX_Resource_VK* paramMotion = nullptr;
    NVSDK_NGX_Resource_VK* paramDepth = nullptr;

    InParameters->Get(NVSDK_NGX_Parameter_Output, (void**) &paramOutput);
    InParameters->Get(NVSDK_NGX_Parameter_MotionVectors, (void**) &paramMotion);
    InParameters->Get(NVSDK_NGX_Parameter_Depth, (void**) &paramDepth);

    DlssNr::ScopedVkParameters scopedParameters(InParameters);

    // Save the original output so we can restore it later
    VkImageInfo originalOutput {};
    if (paramOutput)
    {
        originalOutput.Image = paramOutput->Resource.ImageViewInfo.Image;
        originalOutput.ImageView = paramOutput->Resource.ImageViewInfo.ImageView;
        originalOutput.SubresourceRange = paramOutput->Resource.ImageViewInfo.SubresourceRange;
        originalOutput.Format = paramOutput->Resource.ImageViewInfo.Format;
        originalOutput.Width = paramOutput->Resource.ImageViewInfo.Width;
        originalOutput.Height = paramOutput->Resource.ImageViewInfo.Height;
    }

    const VkImageUsageFlags intermediateUsage =
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    // Order is important as that's the order of shader dispatch
    ShaderPipeline_Vk pipeline;
    const bool finishedNr = Config::Instance()->DlssNrFinishedPicture.value_or_default();
    const bool useNr = !finishedNr && NeuralRendering && NeuralRendering->CanRender() && !IsWithDx12() &&
                       Config::Instance()->DlssNrEnabled.value_or_default() &&
                       DlssNr::HasSupportedSubrects(InParameters, false);
    // Denoise first takes over the placement when it is on: NR runs on a private 1:1 reconstruction and
    // its edit reaches the game's upscale through the raw colour input.
    const bool denoiseFirst = useNr && Config::Instance()->DlssNrDenoiseFirst.value_or_default() &&
                              DlssNr::HasSupportedSubrects(InParameters, true);
    const bool nrBeforeUpscale = useNr && !denoiseFirst && Config::Instance()->DlssNrRunBeforeSr.value_or_default() &&
                                 DlssNr::HasSupportedSubrects(InParameters, true);
    const auto nrDepth = DlssNr::ImageInfo(paramDepth);
    const auto nrMotion = DlssNr::ImageInfo(paramMotion);
    auto nrFrame = DlssNr::FrameInfo(InParameters, nrBeforeUpscale);
    nrFrame.DepthInverted = DepthInverted();
    nrFrame.ColourIsLinearHdr = IsHdr();
    nrFrame.RayReconstruction = upscaler == Upscaler::DLSSD;
    nrFrame.MotionVectorsLowResolution = LowResMV();
    if (finishedNr && NeuralRendering && !IsWithDx12())
        NeuralRendering->CaptureFinished(InCmdBuffer, nrDepth, nrMotion, nrFrame, Instance);
    else if (finishedNr)
        DlssNr::FinishedVkDiagnostic(NeuralRendering ? "evaluate: game upscaler runs through the DX12 bridge"
                                                     : "evaluate: no NR instance");
    // Keep the current per-evaluate subrect. The feature's cached render size may be last frame's.

    if (useNr && !nrBeforeUpscale && !denoiseFirst)
        pipeline.push_back(DlssNr::MakePass(*NeuralRendering, InCmdBuffer, Instance, nrDepth, nrMotion, nrFrame));

    if (useOutputScaling)
    {
        pipeline.push_back(
            { // Setup
              [&](const VkImageInfo& nextOutput) -> VkImageInfo
              {
                  if (OutputScaler->CreateImageResource(Device, PhysicalDevice, TargetWidth(), TargetHeight(),
                                                        nextOutput.Format, intermediateUsage))
                  {
                      OutputScaler->SetImageLayout(InCmdBuffer, OutputScaler->GetImage(), VK_IMAGE_LAYOUT_UNDEFINED,
                                                   VK_IMAGE_LAYOUT_GENERAL, nextOutput.SubresourceRange);

                      // TODO: improve query of the output info
                      VkImageInfo info = nextOutput;
                      info.Image = OutputScaler->GetImage();
                      info.ImageView = OutputScaler->GetImageView();
                      info.Width = TargetWidth();
                      info.Height = TargetHeight();
                      return info;
                  }
                  return VkImageInfo {}; // Returns null handle
              },

              // Dispatch
              [&](const VkImageInfo& input, const VkImageInfo& output) -> bool
              {
                  LOG_DEBUG("Scaling output...");
                  OutputScaler->SetImageLayout(InCmdBuffer, input.Image, VK_IMAGE_LAYOUT_GENERAL,
                                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, input.SubresourceRange);

                  if (!OutputScaler->Dispatch(InCmdBuffer, input, output))
                  {
                      Config::Instance()->OutputScalingEnabled.set_volatile_value(false);
                      State::Instance().changeBackend[Handle()->Id] = true;
                      return false;
                  }
                  return true;
              } });
    }

    _actualSharpness = _sharpness;
    if (useRcas)
    {
        pipeline.push_back(
            { // Setup
              [&](const VkImageInfo& nextOutput) -> VkImageInfo
              {
                  InParameters->Set(NVSDK_NGX_Parameter_Sharpness, 0.0f);
                  _sharpness = 0.0f;

                  if (RCAS->CreateImageResource(Device, PhysicalDevice, nextOutput.Width, nextOutput.Height,
                                                nextOutput.Format, intermediateUsage))
                  {
                      RCAS->SetImageLayout(InCmdBuffer, RCAS->GetImage(), VK_IMAGE_LAYOUT_UNDEFINED,
                                           VK_IMAGE_LAYOUT_GENERAL, nextOutput.SubresourceRange);

                      VkImageInfo info = nextOutput;
                      info.Image = RCAS->GetImage();
                      info.ImageView = RCAS->GetImageView();
                      return info;
                  }
                  return VkImageInfo {};
              },

              // Dispatch
              [&](const VkImageInfo& input, const VkImageInfo& output) -> bool
              {
                  if (!RCAS->CanRender() || !paramMotion || !paramDepth || !paramOutput)
                      return true;

                  RCAS->SetImageLayout(InCmdBuffer, input.Image, VK_IMAGE_LAYOUT_GENERAL,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, input.SubresourceRange);

                  RcasConstants rcasConstants {};
                  rcasConstants.Sharpness = _actualSharpness.value_or(_sharpness);
                  rcasConstants.DepthIsLinear = DepthLinear();
                  rcasConstants.DepthIsReversed = DepthInverted();
                  rcasConstants.IsHdr = IsHdr();

                  _sharpness = _actualSharpness.value_or(_sharpness);
                  _actualSharpness.reset();

                  InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_X, &rcasConstants.MvScaleX);
                  InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &rcasConstants.MvScaleY);

                  float nearPlane = 0.0f;
                  float farPlane = 0.0f;

                  if (InParameters->Get("DLSSG.CameraNear", &nearPlane) == NVSDK_NGX_Result_Success &&
                      InParameters->Get("DLSSG.CameraFar", &farPlane) == NVSDK_NGX_Result_Success)
                  {
                      rcasConstants.CameraNear = nearPlane;
                      rcasConstants.CameraFar = farPlane;
                  }
                  else
                  {
                      rcasConstants.CameraNear = Config::Instance()->FsrCameraNear.value_or_default();
                      rcasConstants.CameraFar = Config::Instance()->FsrCameraFar.value_or_default();
                  }

                  // In Vulkan we pass the Info structs instead of raw resources
                  VkImageInfo mvInfo = *(VkImageInfo*) &paramMotion->Resource.ImageViewInfo;
                  VkImageInfo depthInfo = *(VkImageInfo*) &paramDepth->Resource.ImageViewInfo;

                  if (!RCAS->Dispatch(Device, InCmdBuffer, rcasConstants, input, mvInfo, output, &depthInfo))
                  {
                      Config::Instance()->RcasEnabled.set_volatile_value(false);
                      return false;
                  }
                  return true;
              } });
    }

    if (Magnifier->ShouldRun())
    {
        pipeline.push_back(
            { // Setup
              [&](const VkImageInfo& nextOutput) -> VkImageInfo
              {
                  if (Magnifier->CreateImageResource(Device, PhysicalDevice, nextOutput.Width, nextOutput.Height,
                                                     nextOutput.Format, intermediateUsage))
                  {
                      Magnifier->SetImageLayout(InCmdBuffer, Magnifier->GetImage(), VK_IMAGE_LAYOUT_UNDEFINED,
                                                VK_IMAGE_LAYOUT_GENERAL, nextOutput.SubresourceRange);

                      VkImageInfo info = nextOutput;
                      info.Image = Magnifier->GetImage();
                      info.ImageView = Magnifier->GetImageView();
                      return info;
                  }
                  return VkImageInfo {};
              },

              // Dispatch
              [&](const VkImageInfo& input, const VkImageInfo& output) -> bool
              {
                  if (!Magnifier->CanRender())
                      return true;

                  Magnifier->SetImageLayout(InCmdBuffer, input.Image, VK_IMAGE_LAYOUT_GENERAL,
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, input.SubresourceRange);

                  // In Vulkan we pass the Info structs instead of raw resources
                  VkImageInfo mvInfo = *(VkImageInfo*) &paramMotion->Resource.ImageViewInfo;
                  VkImageInfo depthInfo = *(VkImageInfo*) &paramDepth->Resource.ImageViewInfo;

                  return Magnifier->Dispatch(InCmdBuffer, input, output);
              } });
    }

    const auto currentTarget = SetupShaderPipeline(pipeline, originalOutput);
    if (paramOutput)
        scopedParameters.SetOutput(currentTarget);

    if (denoiseFirst)
    {
        if (!DenoiseFirst)
            DenoiseFirst = std::make_unique<DlssNr::DenoiseFirstVk>();
        DlssNr::DenoiseFirstVkInput input {};
        input.instance = Instance;
        input.physicalDevice = PhysicalDevice;
        input.device = Device;
        input.parameters = InParameters;
        input.frame = DlssNr::FrameInfo(InParameters, true);
        input.frame.DepthInverted = DepthInverted();
        input.frame.ColourIsLinearHdr = IsHdr();
        input.frame.RayReconstruction = upscaler == Upscaler::DLSSD;
        input.frame.MotionVectorsLowResolution = LowResMV();
        input.depth = nrDepth;
        input.motion = nrMotion;
        input.rayReconstruction = upscaler == Upscaler::DLSSD;
        input.depthInverted = DepthInverted();
        input.jitteredMotion = JitteredMV();
        input.lowResolutionMotion = LowResMV();
        input.hdr = IsHdr();
        input.autoExposure = AutoExposure();
        const auto composite = DenoiseFirst->Before(*NeuralRendering, InCmdBuffer, input);
        if (composite.Image)
            scopedParameters.SetColour(*NeuralRendering, InCmdBuffer, composite);
    }
    else if (DenoiseFirst)
        DenoiseFirst.reset();

    if (nrBeforeUpscale)
    {
        const auto originalColour = DlssNr::ParameterImage(InParameters, NVSDK_NGX_Parameter_Color);
        const auto enhancedColour =
            DlssNr::PrepareInput(*NeuralRendering, InCmdBuffer, Instance, originalColour, nrDepth, nrMotion, nrFrame);
        if (enhancedColour.Image && enhancedColour.Image != originalColour.Image)
            scopedParameters.SetColour(*NeuralRendering, InCmdBuffer, enhancedColour);
    }

    // UpscalerTime->Start(InCmdBuffer);

    auto evalResult = EvaluateInternal(InCmdBuffer, InParameters);

    // UpscalerTime->End(InCmdBuffer);

    if (!evalResult)
        return false;

    if (!DispatchShaderPipeline(pipeline))
        return false;

    _frameCount++;

    return evalResult;
}
