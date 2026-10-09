#include "pch.h"
#include "DlssNr_Dx12_State.h"
#include <thread>

auto DlssNr_Dx12::State::ArmScreenCapture() -> void
{
    if (screenCapture.remaining)
        return;
    if (screenCapture.runs >= 8)
    {
        LOG_WARN("NR screen capture: eight-run limit reached; restart to capture again");
        return;
    }
    ++screenCapture.runs;
    SYSTEMTIME time {};
    GetLocalTime(&time);
    char folder[100];
    std::snprintf(folder, sizeof(folder), "%04u%02u%02u-%02u%02u%02u-%03u-screen-%u", time.wYear, time.wMonth,
                  time.wDay, time.wHour, time.wMinute, time.wSecond, time.wMilliseconds, screenCapture.runs);
    screenCapture.directory = Util::DllPath().parent_path() / "nr-pipeline-captures" / folder;
    screenCapture.remaining = 4;
    screenCapture.taken = 0;
    dlssNrScreenCaptureArmed = true;
    LOG_INFO("NR screen capture armed: {} (four presents)", screenCapture.directory.string());
}

auto DlssNr_Dx12::State::BeginScreenCapture(ID3D12GraphicsCommandList* cmd, ID3D12Device* device,
                                            ID3D12Resource* color, const char* mode, DXGI_COLOR_SPACE_TYPE space)
    -> DlssNr::PipelineCaptureFrame*
{
    if (!screenCapture.remaining || !cmd || !device || !color)
        return nullptr;
    auto job = std::make_unique<DlssNr::PipelineCaptureFrame>();
    if (!job->Init(device))
    {
        screenCapture.remaining = 0;
        dlssNrScreenCaptureArmed = false;
        LOG_ERROR("NR screen capture allocation failed");
        return nullptr;
    }
    job->directory = screenCapture.directory / std::to_string(screenCapture.taken);
    const auto& cfg = *Config::Instance();
    job->metadata << "screen_capture mode " << mode << " game_frame " << ::State::Instance().frameCount
                  << " colour_space " << unsigned(space) << '\n'
                  << "stage_semantics screen_before_nr=presented_picture_before_finished_NR "
                     "screen_after_nr=presented_picture_after_finished_NR screen=presented_picture\n"
                  << "white_point_source " << cfg.DlssNrWhitePointSource.value_or_default() << " white_point_scale "
                  << cfg.DlssNrWhitePointScale.value_or_default() << " apply_model "
                  << cfg.DlssNrApplyModel.value_or_default() << " nr_passes " << cfg.DlssNrPasses.value_or_default()
                  << " working_scale " << cfg.DlssNrWorkingScale.value_or_default() << " run_before_sr "
                  << cfg.DlssNrRunBeforeSr.value_or_default() << '\n';
    job->Copy(cmd, device, std::strcmp(mode, "finished") == 0 ? "screen_before_nr" : "screen", color,
              D3D12_RESOURCE_STATE_PRESENT);
    return job.release();
}

auto DlssNr_Dx12::State::CommitScreenCapture(DlssNr::PipelineCaptureFrame* job, ID3D12Fence* fence, UINT64 value,
                                             ID3D12CommandAllocator* allocator, ID3D12GraphicsCommandList* list)
    -> void
{
    screenCapture.pending.push_back({ job, fence, value, allocator, list });
    ++screenCapture.taken;
    if (screenCapture.remaining && --screenCapture.remaining == 0)
        dlssNrScreenCaptureArmed = false;
}

auto DlssNr_Dx12::State::CaptureFinalScreen(ID3D12Resource* color, ID3D12CommandQueue* queue,
                                            DXGI_COLOR_SPACE_TYPE space) -> void
{
    if (!screenCapture.remaining || !color || !queue)
        return;
    const auto type = queue->GetDesc().Type;
    if (type != D3D12_COMMAND_LIST_TYPE_DIRECT && type != D3D12_COMMAND_LIST_TYPE_COMPUTE)
        return;
    LateContext::ComPtr<ID3D12Device> device;
    LateContext::ComPtr<ID3D12CommandAllocator> allocator;
    LateContext::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))) ||
        (!screenCapture.fence && FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                                            IID_PPV_ARGS(&screenCapture.fence)))) ||
        FAILED(device->CreateCommandAllocator(type, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, type, allocator.Get(), nullptr, IID_PPV_ARGS(&list))))
        return;
    auto* job = BeginScreenCapture(list.Get(), device.Get(), color, "final", space);
    if (!job)
        return;
    job->End(list.Get());
    if (FAILED(list->Close()))
    {
        delete job;
        return;
    }
    ID3D12CommandList* lists[] = { list.Get() };
    queue->ExecuteCommandLists(1, lists);
    if (FAILED(queue->Signal(screenCapture.fence.Get(), ++screenCapture.fenceValue)))
        return; // the readbacks may still be in flight: keep the job rather than free them
    CommitScreenCapture(job, screenCapture.fence.Get(), screenCapture.fenceValue, allocator.Get(), list.Get());
}

auto DlssNr_Dx12::State::CollectScreenCaptures() -> void
{
    for (auto it = screenCapture.pending.begin(); it != screenCapture.pending.end();)
    {
        const auto completed = it->fence->GetCompletedValue();
        if (completed == UINT64_MAX)
        {
            LOG_WARN("NR screen capture discarded: the device was removed");
            it = screenCapture.pending.erase(it); // the readbacks are abandoned, not freed under the GPU
            continue;
        }
        if (completed < it->value)
        {
            ++it;
            continue;
        }
        // 4K readbacks take a while to write; keep that off the presentation thread.
        std::thread(
            [job = it->job]
            {
                if (job->Write())
                    LOG_INFO("NR screen capture saved: {}", job->directory.string());
                else
                    LOG_WARN("NR screen capture discarded or write failed: {}", job->directory.string());
                delete job;
            })
            .detach();
        it = screenCapture.pending.erase(it);
    }
}

auto DlssNr_Dx12::State::ApplyFinishedColor(ID3D12Resource* color, ID3D12CommandQueue* queue,
                                            DXGI_COLOR_SPACE_TYPE colorSpace, bool gameFrameHandoff) -> bool
{
    if (!Config::Instance()->DlssNrFinishedPicture.value_or_default() ||
        !Config::Instance()->DlssNrEnabled.value_or_default())
    {
        late.Cancel();
        return false;
    }
    LateContext::ComPtr<ID3D12Device> currentDevice;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&currentDevice))) || currentDevice != late.device)
        return false;
    ID3D12CommandQueue* realQueue = nullptr;
    if (!Util::CheckForRealObject(__FUNCTION__, queue, (IUnknown**) &realQueue))
        realQueue = queue;
    const auto desc = color->GetDesc();
    const bool pq = colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    const bool scrgb = colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
    const bool sdr = colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    if ((!sdr && !pq && !scrgb) || (scrgb && desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT) ||
        (!scrgb && desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM && desc.Format != DXGI_FORMAT_R10G10B10A2_UNORM))
    {
        late.Cancel();
        late.Say("This screen colour format is not supported.");
        return false;
    }
    LateContext::Slot* latest = nullptr;
    const auto epoch = ::State::Instance().frameCount;
    const auto& cfg = *Config::Instance();
    const bool residualOnly =
        DlssNr::ResolvePlacement(cfg.DlssNrRunBeforeSr.value_or_default(), cfg.DlssNrDeferredDlss.value_or_default(),
                                 cfg.DlssNrResidualAcrossRr.value_or_default(), true)
            .deferred;
    // Without frame generation the producer's signal was queued before this presentation and cannot depend on it,
    // so the presentation queue waits for it on the GPU instead of skipping the frame. Skipping left most frames
    // without NR whenever DLSS ran on another queue (Expedition 33: NR on about one present in five, a history reset
    // on every skip, and uneven frame times).
    const bool gpuWait = !gameFrameHandoff && !(::State::Instance().currentFG &&
                                                ::State::Instance().currentFG->IsActive() &&
                                                !::State::Instance().currentFG->IsPaused());
    for (auto& slot : late.slots)
    {
        if (!slot.pending || !slot.submitted)
            continue;
        // Native FG's internal Present counter includes generated frames and is not an app-frame identity.
        if (!gameFrameHandoff && (epoch < slot.frame.SubmissionEpoch || epoch - slot.frame.SubmissionEpoch > 1))
        {
            slot.pending = false;
            late.reset = true;
            continue;
        }
        // With frame generation a queued producer signal can depend on this presentation.
        const auto completed = slot.fence->GetCompletedValue();
        if (!DlssNr::FinishedInputReady(slot.producerQueue.Get() == realQueue || (gpuWait && completed != UINT64_MAX),
                                        completed, slot.ready))
            continue;
        if (slot.residualOnly == residualOnly && slot.frame.OutputWidth == desc.Width &&
            slot.frame.OutputHeight == desc.Height && (!latest || slot.serial > latest->serial))
            latest = &slot;
    }
    // A tuning change may need a few model warm-up frames. Keep displaying the last
    // held edit in that gap rather than exposing live/rotating game backbuffers.
    if (!latest && residualOnly && Config::Instance()->DlssNrHoldFrame.value_or_default() && inputHold.active &&
        late.heldValid && late.heldGeneration == inputHold.generation && late.heldSlot &&
        late.heldSlot->serial == late.heldSlotSerial && !late.heldSlot->pending &&
        late.heldSlot->frame.OutputWidth == desc.Width && late.heldSlot->frame.OutputHeight == desc.Height)
    {
        auto& held = *late.heldSlot;
        if (late.Finished(held))
            latest = &held;
    }
    if (!latest)
        return false; // loading screen, another swapchain, or this real frame was already consumed
    auto& slot = *latest;
    const bool holdFinished =
        slot.residualOnly && Config::Instance()->DlssNrHoldFrame.value_or_default() && inputHold.active;
    if (!holdFinished)
        late.heldValid = false;
    if (holdFinished)
    {
        if (late.heldFailed)
            return false;
        if (late.heldFinished && !SameHoldShape(late.heldFinished->GetDesc(), desc))
        {
            // Defer replacement rather than blocking a presentation needed by the old work.
            if (late.heldFence &&
                !DlssNr::FinishedInputReady(false, late.heldFence->GetCompletedValue(), late.heldReady))
                return false;
            late.heldFinished.Reset();
            late.heldValid = false;
        }
        if (!late.heldFinished)
        {
            late.heldFinished.Attach(CreateScratch(late.device.Get(), desc.Format, (unsigned) desc.Width, desc.Height));
            late.heldState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }
        if (late.heldFence && !DlssNr::FinishedInputReady(late.heldQueue.Get() == realQueue,
                                                          late.heldFence->GetCompletedValue(), late.heldReady))
            return false;
        if (!late.heldFinished)
        {
            late.Say("Could not hold the finished frame.");
            return false;
        }
    }
    // Drop other submitted evaluates from this picture, not their in-flight resources.
    for (auto& other : late.slots)
        if (other.submitted && other.serial <= slot.serial)
            other.pending = false;
    // The selected input is complete or ordered before composition on this queue.
    if (FAILED(slot.allocator->Reset()) || FAILED(slot.commands->Reset(slot.allocator.Get(), nullptr)))
    {
        late.reset = true;
        late.Say("Could not prepare the finished picture.");
        return false;
    }
    auto* cmd = slot.commands.Get();
    auto* screenJob = BeginScreenCapture(cmd, late.device.Get(), color, "finished", colorSpace);
    if (holdFinished)
    {
        const bool capture =
            !late.heldValid || late.heldGeneration != inputHold.generation || late.heldSpace != colorSpace;
        if (capture)
        {
            Barrier(cmd, color, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Barrier(cmd, late.heldFinished.Get(), late.heldState, D3D12_RESOURCE_STATE_COPY_DEST);
            cmd->CopyResource(late.heldFinished.Get(), color);
            Barrier(cmd, late.heldFinished.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Barrier(cmd, color, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
            late.heldState = D3D12_RESOURCE_STATE_COPY_SOURCE;
            late.heldGeneration = inputHold.generation;
            late.heldSpace = colorSpace;
            late.heldValid = true;
        }
        else
        {
            Barrier(cmd, color, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
            cmd->CopyResource(color, late.heldFinished.Get());
            Barrier(cmd, color, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
        }
    }
    const auto before = nr.successfulDispatches;
    bool appliedResidual = false;
    bool matchedResponse = false;
    if (slot.residualOnly)
    {
        if (slot.encoded &&
            (slot.encoded->GetDesc().Width != desc.Width || slot.encoded->GetDesc().Height != desc.Height ||
             slot.encoded->GetDesc().Format != desc.Format))
            slot.encoded.Reset();
        if (!slot.encoded)
            slot.encoded.Attach(CreateScratch(late.device.Get(), desc.Format, (unsigned) desc.Width, desc.Height));
        if (slot.encoded && Config::Instance()->DlssNrApplyModel.value_or_default())
        {
            Barrier(cmd, slot.residual.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Barrier(cmd, color, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            DlssNrConstants apply {};
            apply.Mode = pq ? 4 : scrgb ? 3 : 2;
            apply.Width = (unsigned) desc.Width;
            apply.Height = desc.Height;
            apply.WhitePoint = slot.frame.PreExposure;
            apply.TransferStrength = slot.sceneLinear ? 1.0f : 0.0f;
            apply.MaxRatio = std::clamp(Config::Instance()->DlssNrMaxRatio.value_or_default(), 1.0f, 8.0f);
            const bool measure = Config::Instance()->DlssNrHdrTransfer.value_or_default() && slot.cleanSceneValid &&
                                 slot.sceneLinear && (pq || scrgb);
            if (measure)
            {
                Barrier(cmd, slot.cleanScene.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                for (auto& curve : slot.response)
                {
                    if (!curve)
                    {
                        curve.Attach(
                            CreateScratch(late.device.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT, kDlssNrHdrCurveBins, 1));
                        if (curve)
                            Barrier(cmd, curve.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                        slot.responseValid = false;
                    }
                }
                if (slot.response[0] && slot.response[1])
                {
                    const unsigned next = slot.responseIndex ^ 1u;
                    DlssNrConstants fit {};
                    fit.Mode = pq ? 7 : 6;
                    fit.Width = kDlssNrHdrCurveBins;
                    fit.Height = 1;
                    fit.WhitePoint = slot.frame.PreExposure;
                    fit.ColourStrength = 0.35f; // new weight, only for fits that agree with their history
                    fit.DebugView = slot.responseValid && !slot.frame.Reset && !late.reset &&
                                    slot.responseWidth == desc.Width && slot.responseHeight == desc.Height &&
                                    slot.responseSpace == colorSpace && epoch >= slot.responseEpoch &&
                                    epoch - slot.responseEpoch <= 8 &&
                                    slot.frame.PreExposure >= slot.responseExposure * 0.8f &&
                                    slot.frame.PreExposure <= slot.responseExposure * 1.25f;
                    Barrier(cmd, slot.response[next].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    matchedResponse = shader.DispatchResidualPass(cmd, fit, color, slot.cleanScene.Get(),
                                                                  slot.response[slot.responseIndex].Get(), nullptr,
                                                                  slot.response[next].Get(), true);
                    Barrier(cmd, slot.response[next].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    slot.responseValid = matchedResponse;
                    if (matchedResponse)
                    {
                        slot.responseIndex = next;
                        slot.responseWidth = (unsigned) desc.Width;
                        slot.responseHeight = desc.Height;
                        slot.responseSpace = colorSpace;
                        slot.responseExposure = slot.frame.PreExposure;
                        slot.responseEpoch = epoch;
                        apply.Mode = pq ? 9 : 8;
                    }
                }
            }
            else
                slot.responseValid = false;
            appliedResidual = shader.DispatchResidualPass(
                cmd, apply, color, matchedResponse ? slot.cleanScene.Get() : nullptr, slot.residual.Get(),
                matchedResponse ? slot.response[slot.responseIndex].Get() : nullptr, slot.encoded.Get(), true);
            if (measure)
                Barrier(cmd, slot.cleanScene.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_COPY_DEST);
            if (!appliedResidual)
                slot.responseValid = false;
            if (appliedResidual)
            {
                Barrier(cmd, slot.encoded.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_COPY_SOURCE);
                Barrier(cmd, color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
                cmd->CopyResource(color, slot.encoded.Get());
                Barrier(cmd, color, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Barrier(cmd, slot.encoded.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
            Barrier(cmd, color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PRESENT);
            Barrier(cmd, slot.residual.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_COPY_DEST);
        }
    }
    else
    {
        auto frame = slot.frame;
        frame.ColourIsLinearHdr = pq || scrgb;
        // Absolute display encodings: 203-nit reference white, in 80-nit scRGB units.
        frame.WhitePointOverride = (pq || scrgb) ? 203.0f / 80.0f : 0.0f;
        frame.Reset |= late.reset;
        frame.SubmissionEpoch = epoch;
        Barrier(cmd, slot.depth.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(cmd, slot.motion.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        ID3D12Resource* nrColor = color;
        bool colorReady = true;
        DlssNrConstants conversion {};
        conversion.Width = (unsigned) desc.Width;
        conversion.Height = desc.Height;
        if (pq)
        {
            auto ensure = [&](LateContext::ComPtr<ID3D12Resource>& resource, DXGI_FORMAT format)
            {
                if (resource && (resource->GetDesc().Width != desc.Width || resource->GetDesc().Height != desc.Height ||
                                 resource->GetDesc().Format != format))
                    resource.Reset();
                if (!resource)
                    resource.Attach(CreateScratch(late.device.Get(), format, (unsigned) desc.Width, desc.Height));
                return resource != nullptr;
            };
            colorReady = ensure(slot.linear, DXGI_FORMAT_R16G16B16A16_FLOAT) && ensure(slot.encoded, desc.Format);
            if (colorReady)
            {
                Barrier(cmd, color, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                colorReady = shader.DispatchResidualPass(cmd, conversion, color, nullptr, nullptr, nullptr,
                                                         slot.linear.Get(), true);
                Barrier(cmd, color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PRESENT);
                // Dispatch reads the converted colour; its transition out of UAV orders the conversion.
                nrColor = slot.linear.Get();
                frame.OutputArrivalState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            }
        }
        if (colorReady)
            Run(cmd, nrColor, slot.depth.Get(), slot.motion.Get(), nrColor, frame, queue);
        if (pq && colorReady && nr.successfulDispatches > before &&
            Config::Instance()->DlssNrApplyModel.value_or_default())
        {
            conversion.Mode = 1;
            Barrier(cmd, slot.linear.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Barrier(cmd, color, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            if (shader.DispatchResidualPass(cmd, conversion, slot.linear.Get(), nullptr, color, nullptr,
                                            slot.encoded.Get(), true))
            {
                Barrier(cmd, slot.encoded.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_COPY_SOURCE);
                Barrier(cmd, color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
                cmd->CopyResource(color, slot.encoded.Get());
                Barrier(cmd, color, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Barrier(cmd, slot.encoded.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
            Barrier(cmd, color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PRESENT);
            Barrier(cmd, slot.linear.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        Barrier(cmd, slot.motion.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        Barrier(cmd, slot.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    if (screenJob)
    {
        screenJob->metadata << "nr_ran " << (slot.residualOnly ? appliedResidual : nr.successfulDispatches > before)
                            << " history_reset " << (slot.frame.Reset || late.reset) << '\n';
        screenJob->Copy(cmd, late.device.Get(), "screen_after_nr", color, D3D12_RESOURCE_STATE_PRESENT);
        screenJob->End(cmd);
    }
    if (FAILED(cmd->Close()))
    {
        delete screenJob;
        late.heldFailed |= holdFinished;
        late.Say("Could not finish the picture. Restart the game to retry.");
        slot.pending = true; // quarantine the slot; do not reuse possibly recorded NR resources
        slot.submitted = false;
        return false;
    }
    // The producer's copy may still be running on another queue; order this work after it on the GPU.
    if (slot.producerQueue.Get() != realQueue && slot.fence->GetCompletedValue() < slot.ready &&
        FAILED(queue->Wait(slot.fence.Get(), slot.ready)))
    {
        delete screenJob;
        late.Say("The graphics queue stopped. Restart the game to retry.");
        return false;
    }
    ID3D12CommandList* lists[] = { cmd };
    queue->ExecuteCommandLists(1, lists);
    slot.done = std::max(slot.done, slot.ready) + 1; // held replays also need a fresh completion value
    if (FAILED(queue->Signal(slot.fence.Get(), slot.done)))
    {
        late.heldFailed |= holdFinished;
        late.Say("The graphics queue stopped. Restart the game to retry.");
        return false;
    }
    if (screenJob)
        CommitScreenCapture(screenJob, slot.fence.Get(), slot.done);
    if (holdFinished)
    {
        late.heldFence = slot.fence;
        late.heldQueue = realQueue;
        late.heldReady = slot.done;
        late.heldSlot = &slot;
        late.heldSlotSerial = slot.serial;
    }
    const bool ran = slot.residualOnly ? appliedResidual : nr.successfulDispatches > before;
    late.reset = !ran;
    late.Say(!Config::Instance()->DlssNrApplyModel.value_or_default() ? "NR changes are hidden."
             : ran                                                    ? (slot.residualOnly
                                                                             ? (matchedResponse
                                                                                    ? "Applying pre-SR changes with HDR brightness matching (local fallback enabled)."
                                                                                    : "Applying the pre-SR changes to the finished picture.")
                                                                             : "Applying NR to the finished picture.")
                                                                      : "Preparing NR for the finished picture.");
    if (ran && (++late.successes == 1 || late.successes % 300 == 0))
        LOG_INFO("DLSS-NR finished picture: {} frames, {}x{}, OptiScaler FG {}, same producer queue {}, game-frame "
                 "handoff {}",
                 late.successes, desc.Width, desc.Height,
                 ::State::Instance().currentFG && ::State::Instance().currentFG->IsActive() &&
                     !::State::Instance().currentFG->IsPaused(),
                 slot.producerQueue.Get() == realQueue, gameFrameHandoff);
    return ran;
}
