#include "pch.h"

#include "DlssNr.h"
#include "DlssNrFeature_Vk.h"
#include "DlssNrFinished_Vk.h"
#include "DlssNr_PipelineUi.h"
#include "DlssNr_Upscaler.h"
#include "DlssNr_MenuSections.h"
#include "DlssNr_Placement.h"
#include "DlssNr_Hudless.h"
#include <Config.h>
#include <menu/menu_common.h>
#include <algorithm>
#include <cmath>

namespace DlssNr::MenuSections
{

static void RenderPlacement(Config* config)
{
    const bool enabled = config->DlssNrEnabled.value_or_default();
    const bool finishedPicture = config->DlssNrFinishedPicture.value_or_default();
    if (finishedPicture && enabled)
    {
        const auto feature = State::Instance().currentFeature;
        if (State::Instance().swapchainApi == API::Vulkan)
            ImGui::TextWrapped("%s", DlssNr::FinishedVkStatus().c_str());
        else if (feature && (feature->Api() != API::DX12 ||
                             (feature->IsWithDx12() && State::Instance().swapchainApi != API::DX11 &&
                              State::Instance().swapchainInteropApi != SwapchainInteropApi::Dx11wDx12)))
            ImGui::TextWrapped("This option needs DirectX 12 or a DirectX 11 upscaler marked w/Dx12.");
        else
            ImGui::TextWrapped("%s", DlssNr::FinishedPictureStatus().c_str());
    }

    const auto placement =
        ResolvePlacement(config->DlssNrRunBeforeSr.value_or_default(), config->DlssNrDeferredDlss.value_or_default(),
                         config->DlssNrResidualAcrossRr.value_or_default(), finishedPicture);
    if (placement.deferred)
    {
        ImGui::TextWrapped("Private upscale: %s", DlssNr::DeferredDlssStatus().c_str());
        ImGui::TextWrapped(finishedPicture ? "The game processes clean input through SR/RR and its effects. The "
                                             "separately upscaled NR edit is applied to the finished picture."
                                           : "The game processes clean input through SR/RR. The separately upscaled NR "
                                             "edit is applied after upscale.");
    }
}

static void RenderStatus(Config* config)
{
    const bool enabled = config->DlssNrEnabled.value_or_default();
    const bool finishedPicture = config->DlssNrFinishedPicture.value_or_default();
    const auto dx12 = ReadStatus(Backend::Dx12);
    const auto vk = ReadStatus(Backend::Vulkan);
    const bool vulkan = vk.running;

    // An existing model handle does not mean NR is enabled this frame.
    if (!enabled)
    {
        ImGui::TextDisabled("NR off.");
    }
    else if (!dx12.running && !vulkan)
    {
        const auto feature = State::Instance().currentFeature;
        const bool nativeVk = feature && feature->Api() == API::Vulkan && !feature->IsWithDx12();
        const auto& reason = nativeVk ? vk.failureReason : dx12.failureReason;

        if (!reason.empty())
        {
            ImGui::TextWrapped("%s", reason.c_str());
            ImGui::SameLine();

            if (nativeVk)
                ImGui::TextUnformatted("Restart the game to retry native Vulkan NR.");
            else if (ImGui::SmallButton("Retry"))
                DlssNr::RetryAfterFailure();
        }
        else if (feature && feature->Api() == API::DX11 && !feature->IsWithDx12())
        {
            ImGui::TextWrapped("NR needs the D3D12 bridge on D3D11. Choose an upscaler marked w/Dx12 and restart.");
        }
        else if (nativeVk && ResolvePlacement(config->DlssNrRunBeforeSr.value_or_default(),
                                              config->DlssNrDeferredDlss.value_or_default(),
                                              config->DlssNrResidualAcrossRr.value_or_default(), finishedPicture)
                                 .deferred)
        {
            ImGui::TextWrapped("The private edit-upscale path requires DirectX 12 or its bridge. Disable separate edit "
                               "upscaling to use native Vulkan NR.");
        }
        else
            ImGui::TextUnformatted("Waiting for the upscaler to run.");
    }
    else
    {
        const auto ms = vulkan ? vk.gpuTime : dx12.gpuTime;

        // Hiding the edit keeps model evaluation running.
        const char* runSuffix = !config->DlssNrApplyModel.value_or_default() ? "  (model running, edit hidden)" : "";

        // Keep the running indicator green, using the theme's HDR-adjusted text brightness.
        const auto textColor = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImVec4(textColor.x * 0.55f, textColor.y * 0.80f, textColor.z * 0.55f, textColor.w));
        if (ms.has_value())
            ImGui::Text("Running%s - %.2f ms elapsed%s", vulkan ? " natively on Vulkan" : "", ms.value(), runSuffix);
        else if (vulkan)
            // Measured but not yet read: the first few frames are still in the query ring.
            ImGui::Text("Running natively on Vulkan - %llu frames%s", vk.frames, runSuffix);
        else
            ImGui::Text("Running.%s", runSuffix);
        ImGui::PopStyleColor();

        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Time between the start and end of NR on the GPU, including delays while other work "
                              "runs.\nCompare FPS to check the effect on game performance.");

        if (ms.has_value())
        {
            const auto& state = State::Instance();
            // The FG swapchain interval is between real game frames, not interpolated presents.
            // Native Vulkan has no DXGI timing, so use the existing overlay frame interval there.
            const double frameMs = state.swapchainApi == API::Vulkan
                                       ? (state.frameTimes.empty() ? 0.0 : state.frameTimes.back())
                                   : state.currentFG ? state.lastFGFrameTime
                                                     : state.presentFrameTime;
            PipelineUi::DrawTimingBar(ms.value(), frameMs);
        }
    }
}

} // namespace DlssNr::MenuSections

namespace DlssNr
{

void RenderMenu(Config* config, float menuResScale)
{
    using namespace MenuSections;
    ImGui::Spacing();
    if (auto header = ScopedCollapsingHeader("DLSS Neural Rendering"); header.IsHeaderOpen())
    {
        ScopedIndent indent {};
        const float toggleGap = ImGui::GetStyle().ItemSpacing.x;
        const float toggleWidth = (ImGui::GetContentRegionAvail().x - toggleGap) * 0.5f;
        const float toggleRight = ImGui::GetCursorPosX() + toggleWidth + toggleGap;
        bool enabled = config->DlssNrEnabled.value_or_default();
        if (PipelineUi::CheckboxWrapped("Enable Neural Rendering", &enabled, toggleWidth))
            config->DlssNrEnabled = enabled;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Enable NR processing.");

        bool applyModel = config->DlssNrApplyModel.value_or_default();
        if (PipelineUi::CheckboxWrapped("Apply model", &applyModel, toggleWidth))
            config->DlssNrApplyModel = applyModel;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Show or hide the NR effect. The model still runs when hidden.\nDisable Enable Neural "
                              "Rendering to stop its GPU cost.");

        const auto feature = State::Instance().currentFeature;
        const bool rayReconstruction = feature && feature->GetUpscalerType() == Upscaler::DLSSD;
        bool finished = config->DlssNrFinishedPicture.value_or_default();
        auto placement = ResolvePlacement(config->DlssNrRunBeforeSr.value_or_default(),
                                          config->DlssNrDeferredDlss.value_or_default(),
                                          config->DlssNrResidualAcrossRr.value_or_default(), finished);
        bool generateBefore = placement.beforeUpscale;
        // Denoise first only takes over the placement where it actually runs (native D3D12).
        const bool denoiseFirstOn =
            config->DlssNrDenoiseFirst.value_or_default() &&
            (!feature || feature->Api() == API::DX12 || (feature->Api() == API::Vulkan && !feature->IsWithDx12()));
        ImGui::SameLine(toggleRight);
        ImGui::BeginDisabled(placement.deferred || denoiseFirstOn);
        if (PipelineUi::CheckboxWrapped("Generate model before upscale", &generateBefore, toggleWidth))
        {
            config->DlssNrRunBeforeSr = generateBefore;
            config->DlssNrResidualAcrossRr = false;
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(placement.deferred ? "The separate-edit path always generates before upscale."
                              : denoiseFirstOn   ? "Denoise first decides the placement while it is on."
                                                 : "Run NR before the game's upscaler, including RR.");

        if (PipelineUi::CheckboxWrapped("Apply NR to the finished picture", &finished, toggleWidth))
        {
            config->DlssNrFinishedPicture = finished;
            DlssNr::RetryAfterFailure();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Apply NR after game effects and HUD. Early generation carries the edit through a separate upscaler.");

        placement = ResolvePlacement(config->DlssNrRunBeforeSr.value_or_default(),
                                     config->DlssNrDeferredDlss.value_or_default(),
                                     config->DlssNrResidualAcrossRr.value_or_default(), finished);
        ImGui::SameLine(toggleRight);
        bool deferred = placement.deferred;
        if (PipelineUi::CheckboxWrapped("Generate before upscale, apply after upscale", &deferred, toggleWidth))
        {
            config->DlssNrDeferredDlss = deferred;
            config->DlssNrResidualAcrossRr = false; // Clear the legacy alias when the unified option changes.
            if (deferred || finished)
                config->DlssNrRunBeforeSr = deferred;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Keep the game's SR/RR input clean and upscale only the NR edit with a separate non-RR backend."
                "\nApply after upscale, or at presentation when finished-picture mode is enabled.");

        bool hudless = finished && config->DlssNrFinishedHudless.value_or_default();
        if (PipelineUi::CheckboxWrapped("Apply NR to the finished picture without the HUD", &hudless, toggleWidth))
        {
            config->DlssNrFinishedHudless = hudless;
            if (hudless)
            {
                // Runs NR itself on the HUD-less picture: the finished picture on, the separate-edit route off.
                config->DlssNrFinishedPicture = true;
                config->DlssNrRunBeforeSr = false;
                config->DlssNrDeferredDlss = false;
                config->DlssNrResidualAcrossRr = false;
                finished = true;
            }
            DlssNr::RetryAfterFailure();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(
                "For games with DLSS frame generation through Streamline that give it a picture without the HUD\n"
                "(tone mapped, before the HUD is drawn) and a UI mask. NR runs on that picture just before frame\n"
                "generation; its change goes into the shown picture only where there is no HUD, and frame\n"
                "generation gets the NR picture too, so generated frames match. Adds only NR's own GPU time: no\n"
                "CPU wait, nothing extra for Reflex to account for.\n"
                "Ticking it also turns on the finished picture and turns off generating before upscale.\n"
                "Needs SDR output. Games without such a picture use the ordinary finished picture.");

        if (finished && config->DlssNrFinishedHudless.value_or_default())
        {
            ImGui::Indent();
            bool fgOff = config->DlssNrHudlessFrameGenOff.value_or_default();
            if (ImGui::Checkbox("Without frame generation", &fgOff))
                config->DlssNrHudlessFrameGenOff = fgOff;
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Some games (Alan Wake 2) only make the picture without the HUD while their own frame generation\n"
                    "setting is on. Leave that setting on in the game and tick this: no frames are generated while NR\n"
                    "is on, but the game keeps the picture without the HUD coming. Reflex stays as the game set it.\n"
                    "Frame generation stays off while this is ticked, also with NR toggled off.\n"
                    "A change takes effect the next time the game switches frame generation off itself: open and\n"
                    "close the game's own menu. Switching it while frames are generated can freeze the game.");
            if (DlssNr::Hudless::FrameGenerationOffPending())
                ImGui::TextDisabled("Open and close the game's menu to apply.");
            int applyMode = config->DlssNrHudlessApplyMode.value_or_default() == 1 ? 1 : 0;
            if (ImGui::Combo("NR apply mode", &applyMode, "Ratio (multiplied)\0Add (difference)\0"))
                config->DlssNrHudlessApplyMode = applyMode;
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "How NR's change reaches the picture.\n"
                    "Ratio: each pixel is multiplied by NR / input, so the change is relative to the pixel; smoothing\n"
                    "and motion-adaptive strength work on that relative change. Bounded by the ratio limit below.\n"
                    "Add: NR - input is added as an absolute amount (the behaviour before this option).\n"
                    "Switching restarts the smoothing history.");
            if (applyMode == 0)
            {
                float limit = config->DlssNrHudlessRatioLimit.value_or_default();
                if (ImGui::SliderFloat("Ratio limit", &limit, 1.5f, 16.0f, "%.1fx", ImGuiSliderFlags_AlwaysClamp))
                    config->DlssNrHudlessRatioLimit = limit;
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("The most NR may brighten (or 1 / it, darken) a pixel in ratio mode.");
            }
            bool adaptive = config->DlssNrHudlessMotionAdaptive.value_or_default();
            if (PipelineUi::CheckboxWrapped("Motion-adaptive NR strength", &adaptive, toggleWidth))
                config->DlssNrHudlessMotionAdaptive = adaptive;
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Scales NR's change by how fast each pixel moves on screen, from the game's motion vectors:\n"
                    "full 'still' strength below the start of the motion range, 'in motion' strength above its end,\n"
                    "a smooth blend in between. Per pixel, so a moving character does not weaken NR on a still\n"
                    "background. Costs one light pass (shared with smoothing).");
            if (adaptive)
            {
                float motion = config->DlssNrHudlessMotionStrength.value_or_default() * 100.0f;
                float still = config->DlssNrHudlessStillStrength.value_or_default() * 100.0f;
                if (ImGui::DragFloatRange2("NR strength", &motion, &still, 0.5f, 0.0f, 100.0f, "In motion %.0f%%",
                                           "Still %.0f%%", ImGuiSliderFlags_AlwaysClamp))
                {
                    config->DlssNrHudlessMotionStrength = motion / 100.0f;
                    config->DlssNrHudlessStillStrength = still / 100.0f;
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Left: strength while a pixel moves. Right: strength while it is still.\n"
                                      "100%% is NR as set above.");
                float start = config->DlssNrHudlessMotionStart.value_or_default();
                float full = config->DlssNrHudlessMotionFull.value_or_default();
                if (ImGui::DragFloatRange2("Motion range", &start, &full, 0.05f, 0.0f, 64.0f, "From %.2f px",
                                           "To %.2f px/frame", ImGuiSliderFlags_AlwaysClamp))
                {
                    config->DlssNrHudlessMotionStart = start;
                    config->DlssNrHudlessMotionFull = full;
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("On-screen movement in output pixels per rendered frame. Below 'From' NR is at\n"
                                      "still strength, above 'To' at motion strength.");
            }

            bool smoothing = config->DlssNrHudlessSmoothing.value_or_default();
            if (PipelineUi::CheckboxWrapped("Temporal smoothing of NR's change", &smoothing, toggleWidth))
                config->DlssNrHudlessSmoothing = smoothing;
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Averages NR's change (not the picture) over recent frames, following the game's motion\n"
                    "vectors, to calm NR flicker and shimmer in motion. Where the previous change does not fit the\n"
                    "current one (revealed areas, lighting changes) it is limited to the current range, so it does\n"
                    "not smear. The game's own picture is never averaged.");
            if (smoothing)
            {
                float frames = config->DlssNrHudlessSmoothingFrames.value_or_default();
                if (ImGui::SliderFloat("Smoothing frames", &frames, 1.0f, 16.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp))
                    config->DlssNrHudlessSmoothingFrames = frames;
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("How many frames NR's change is averaged over. 1 = off. More = calmer, but\n"
                                      "NR's detail follows motion more slowly.");
                float tolerance = config->DlssNrHudlessSmoothingTolerance.value_or_default();
                if (ImGui::SliderFloat("History tolerance", &tolerance, 0.0f, 8.0f, "%.2f",
                                       ImGuiSliderFlags_AlwaysClamp))
                    config->DlssNrHudlessSmoothingTolerance = tolerance;
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("How far the previous change may differ from the current one, in local standard\n"
                                      "deviations of the current change. Lower = less smearing, higher = calmer.");
                bool detail = config->DlssNrHudlessSmoothingDetail.value_or_default();
                if (ImGui::Checkbox("Keep fine detail sharp", &detail))
                    config->DlssNrHudlessSmoothingDetail = detail;
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Smooth only the coarse part of NR's change (tone and brightness shimmer, where\n"
                                      "most of NR's flicker in motion is); fine detail always comes from the current frame.");
                float reject = config->DlssNrHudlessSmoothingReject.value_or_default();
                if (ImGui::SliderFloat("Reset where the picture changes", &reject, 0.0f, 0.2f, "%.3f",
                                       ImGuiSliderFlags_AlwaysClamp))
                    config->DlssNrHudlessSmoothingReject = reject;
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Drops the history where the game's own picture changed under the motion (revealed\n"
                                      "areas, moving lights). Lower = resets more often, 0 = off.");
            }
            ImGui::Unindent();
        }

        if (finished)
        {
            bool diagnostics = config->DlssNrFinishedDiagnostics.value_or_default();
            if (ImGui::Checkbox("Finished-picture diagnostics (log)", &diagnostics))
                config->DlssNrFinishedDiagnostics = diagnostics;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Native Vulkan only. Logs, once each, why a frame was not processed, and a count of "
                                  "outcomes every 600 frames.\nChanges no behaviour; leave it off when not "
                                  "troubleshooting.");
        }
        ImGui::Spacing();

        // Denoise first. Written for people who did not design it: what it does, what it costs, what each knob is.
        placement = ResolvePlacement(config->DlssNrRunBeforeSr.value_or_default(),
                                     config->DlssNrDeferredDlss.value_or_default(),
                                     config->DlssNrResidualAcrossRr.value_or_default(), finished);
        // Denoise first runs in the D3D12 pipeline and natively on Vulkan (edit-onto-raw step only). On a native
        // DX11 upscaler the toggle would be read by nothing, so say so instead of letting it look switched on.
        const bool denoiseVulkan = feature && feature->Api() == API::Vulkan && !feature->IsWithDx12();
        const bool denoiseUnsupportedApi = feature && feature->Api() != API::DX12 && !denoiseVulkan;
        const bool denoiseBlocked = placement.deferred || finished || denoiseUnsupportedApi;
        bool denoiseFirst = config->DlssNrDenoiseFirst.value_or_default();
        ImGui::BeginDisabled(denoiseBlocked);
        if (PipelineUi::CheckboxWrapped("Denoise at native, then NR, then upscale", &denoiseFirst, toggleWidth))
            config->DlssNrDenoiseFirst = denoiseFirst;
        ImGui::EndDisabled();
        if (denoiseUnsupportedApi && denoiseFirst)
            ImGui::TextWrapped("Denoise first needs DirectX 12 or Vulkan; NR runs %s the upscaler in this game.",
                               config->DlssNrRunBeforeSr.value_or_default() ? "before" : "after");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(
                denoiseUnsupportedApi ? "Only implemented for DirectX 12 and native Vulkan games so far."
                : denoiseBlocked
                    ? "Turn off the separate-edit and finished-picture options first."
                    : "Runs the game's own upscaler once more at the render resolution (Ray Reconstruction if the game "
                      "uses it, otherwise DLSS) so NR is shown a clean, denoised, steady image instead of the noisy "
                      "jittered one.\nNR edits that image; the second step below decides how the edit reaches the "
                      "screen.\nDirectX 12 and native Vulkan (Vulkan: edit onto the raw render only). Costs one extra "
                      "upscaler pass per frame.");
        if (denoiseFirst && !denoiseBlocked)
        {
            ScopedIndent denoiseIndent {};
            if (denoiseVulkan)
                ImGui::TextWrapped("Vulkan: the second step is always \"Edit onto the raw render\".");
            int step = std::clamp(config->DlssNrDenoiseFirstStep.value_or_default(), 0, 2);
            ImGui::BeginDisabled(denoiseVulkan);
            if (ImGui::Combo("Second step", &step,
                             "Game upscaler again, jitter zeroed\0Private DLSS SR, jitter zeroed\0Edit onto the raw "
                             "render, then the game upscaler\0"))
                config->DlssNrDenoiseFirstStep = step;
            ImGui::EndDisabled();
            if (denoiseVulkan)
                step = 2; // the Vulkan pass only implements this step; show its options
            HelpMarker(
                "Game upscaler again: the game's upscaler receives the NR'd clean image. Simple, but the second pass "
                "gets no real sub-pixel samples, so it can look softer than native.\n\n"
                "Private DLSS SR: the same, but a separate plain DLSS pass does the upscale and replaces the output. "
                "Avoids running Ray Reconstruction twice.\n\n"
                "Edit onto the raw render: only NR's change is taken from the clean image, lined up with the raw "
                "render's jitter and added onto it. The game's upscaler then sees real samples carrying the edit. "
                "Sharpest in theory; Ray Reconstruction may soften part of the edit.");
            if (step == 2)
            {
                int edit = config->DlssNrDenoiseFirstEdit.value_or_default() == 1 ? 1 : 0;
                if (ImGui::Combo("Edit as", &edit, "Difference (added)\0Ratio (multiplied)\0"))
                    config->DlssNrDenoiseFirstEdit = edit;
                HelpMarker("Ratio multiplies each raw pixel by NR / clean, so every noisy sample keeps its own hue "
                           "and only its brightness and tint move. Recommended.\nDifference adds NR - clean as an "
                           "absolute amount. On samples that came back dark that amount outweighs the sample, which "
                           "recolours them and roughly doubles the colour noise the upscaler receives.");
                if (edit == 1)
                {
                    bool fireflies = config->DlssNrDenoiseFirstFireflyGuard.value_or_default();
                    if (ImGui::Checkbox("Do not multiply fireflies", &fireflies))
                        config->DlssNrDenoiseFirstFireflyGuard = fireflies;
                    HelpMarker("A raw sample far brighter than the clean value is a path-tracing firefly, not a lit "
                               "pixel. Multiplying it by the ratio makes it brighter, and when the camera is still "
                               "the game's Ray Reconstruction can keep such sparkles in shadows. With this on, those "
                               "samples get the absolute change their pixel would have had instead. Turn off to "
                               "compare.");
                }

                bool shift = config->DlssNrDenoiseFirstShift.value_or_default();
                if (ImGui::Checkbox("Shift the edit by the frame's jitter", &shift))
                    config->DlssNrDenoiseFirstShift = shift;
                HelpMarker("The clean image sits on a steady pixel grid; the raw render is offset by a sub-pixel "
                           "jitter that changes every frame. Shifting the edit by that jitter lines the two up. "
                           "Turn off only to compare.");
                if (shift)
                {
                    bool flip = config->DlssNrDenoiseFirstFlipJitter.value_or_default();
                    if (ImGui::Checkbox("Flip the jitter direction", &flip))
                        config->DlssNrDenoiseFirstFlipJitter = flip;
                    HelpMarker("Engines disagree on which way the jitter value points. The wrong direction doubles "
                               "the misalignment instead of removing it. If edges look worse with the shift on than "
                               "off, flip this.");
                    int kernel = std::clamp(config->DlssNrDenoiseFirstKernel.value_or_default(), 0, 2);
                    if (ImGui::Combo("Edit resampling", &kernel, "Bilinear\0Catmull-Rom\0Lanczos 2\0"))
                        config->DlssNrDenoiseFirstKernel = kernel;
                    HelpMarker("How the edit is interpolated at the shifted position. Bilinear is soft and never "
                               "overshoots. Catmull-Rom is sharp with little overshoot. Lanczos 2 is the sharpest "
                               "and can ring around hard edges.");
                    bool clampEdit = config->DlssNrDenoiseFirstNeighbourhoodClamp.value_or_default();
                    if (ImGui::Checkbox("Clamp the edit to its neighbours", &clampEdit))
                        config->DlssNrDenoiseFirstNeighbourhoodClamp = clampEdit;
                    HelpMarker("Limits the interpolated edit to the range of the four nearest source pixels. Removes "
                               "ringing from the sharper kernels at the cost of a little sharpness.");
                }
            }
            ImGui::TextWrapped("Denoise first: %s", DlssNr::DenoiseFirstStatus().c_str());
            ImGui::Spacing();
        }

        // "Generate model before upscale" and denoise first both drop back to after-upscale NR when the game's
        // colour input is one they cannot read. Without this line the only sign is an unchanged frame time.
        if (const auto blocker = DlssNr::BeforeUpscaleBlocker(); enabled && !blocker.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.2f, 1.0f));
            ImGui::TextWrapped("NR before the upscale cannot run in this game: %s. NR is running after the upscale "
                               "instead.",
                               blocker.c_str());
            ImGui::PopStyleColor();
            ImGui::Spacing();
        }

        // placement is current: nothing it depends on changes in the denoise-first block above.
        const bool nativePrivateVk = feature && feature->Api() == API::Vulkan && !feature->IsWithDx12();
        if (placement.deferred && !nativePrivateVk)
        {
            int backend = (int) GetPrivateUpscaler(config->DlssNrPrivateUpscaler.value_or_default());
            if (ImGui::Combo("Private NR upscaler", &backend, "DLSS\0FSR 2.2\0FSR (FidelityFX)\0XeSS\0"))
                config->DlssNrPrivateUpscaler = backend;
            HelpMarker(
                "Upscales only the NR edit, with or without game RR. FSR (FidelityFX) and XeSS need their runtimes.");
        }

        PipelineUi::View view;
        view.privateUpscaler =
            PrivateUpscalerName(GetPrivateUpscaler(config->DlssNrPrivateUpscaler.value_or_default()));
        view.enabled = enabled;
        view.applyModel = config->DlssNrApplyModel.value_or_default();
        view.passes = config->DlssNrPasses.value_or_default();
        view.scalePercent = (int) lroundf(config->DlssNrWorkingScale.value_or_default() * 100.0f);
        view.rayReconstruction = rayReconstruction;
        if (finished)
        {
            view.route = placement.deferred ? PipelineUi::Route::FinishedBefore : PipelineUi::Route::Finished;
            view.hudless = !placement.deferred && config->DlssNrFinishedHudless.value_or_default();
        }
        else if (placement.deferred)
            view.route = PipelineUi::Route::Deferred;
        else
            view.route = placement.beforeUpscale ? PipelineUi::Route::Before : PipelineUi::Route::After;

        static PipelineUi::Section selected = PipelineUi::Section::Placement;
        ImGui::Separator();
        PipelineUi::Draw(view, selected);
        ImGui::Separator();
        ImGui::Spacing();
        RenderStatus(config);
        ImGui::SeparatorText(PipelineUi::SectionName(selected));
        ImGui::PushItemWidth(std::min(220.0f * menuResScale, ImGui::GetContentRegionAvail().x * 0.42f));
        static constexpr void (*sections[])(Config*) = { RenderPlacement, RenderInput, RenderModel, RenderBlend };
        sections[(int) selected](config);
        ImGui::PopItemWidth();
        if (ImGui::CollapsingHeader("Inspect NR"))
        {
            ImGui::PushItemWidth(std::min(220.0f * menuResScale, ImGui::GetContentRegionAvail().x * 0.42f));
            RenderInspect(config);
            ImGui::PopItemWidth();
        }
    }
}

} // namespace DlssNr
