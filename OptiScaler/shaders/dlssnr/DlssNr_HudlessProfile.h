#pragma once

// GPU time of each step of the DX12 HUD-less finished picture, for performance work ([DlssNr] HudlessProfile=true).
// Timestamps go on the compose command list itself (it is executed directly by the compose step, so the queue
// hooks used by the other NR timers do not see it). Each frame uses one ring slot with its own fence; a slot is
// read once its fence completes. Every 600 frames the mean of every step is logged. Off by default: no queries,
// no extra GPU work.

#include <Config.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>

namespace DlssNr::HudlessProfile
{
class Profiler
{
    static constexpr unsigned Frames = 8, Marks = 16;
    struct Slot
    {
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        UINT64 value = 0;
        UINT64 frequency = 0;
        unsigned used = 0;
        const char* names[Marks] {};
        bool pending = false, submitted = false;
    };
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> heap;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    std::array<Slot, Frames> slots;
    int active = -1;
    ID3D12GraphicsCommandList* activeList = nullptr;
    // Running sums per step name, in first-seen order.
    const char* stepNames[Marks] {};
    double stepSums[Marks] {};
    double totalSum = 0.0;
    unsigned steps = 0, frames = 0;

    bool Init(ID3D12Device* d)
    {
        if (device.Get() == d && heap && readback)
            return true;
        device = d;
        heap.Reset();
        readback.Reset();
        for (auto& s : slots)
            s = Slot {};
        D3D12_QUERY_HEAP_DESC qd { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, Frames * Marks, 0 };
        if (FAILED(d->CreateQueryHeap(&qd, IID_PPV_ARGS(&heap))))
            return false;
        const auto hp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
        const auto rd = CD3DX12_RESOURCE_DESC::Buffer(Frames * Marks * sizeof(UINT64));
        if (FAILED(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              IID_PPV_ARGS(&readback))))
        {
            heap.Reset();
            return false;
        }
        for (auto& s : slots)
            if (FAILED(d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s.fence))))
            {
                heap.Reset();
                readback.Reset();
                return false;
            }
        return true;
    }

    unsigned StepIndex(const char* name)
    {
        for (unsigned i = 0; i < steps; ++i)
            if (std::strcmp(stepNames[i], name) == 0)
                return i;
        if (steps < Marks)
        {
            stepNames[steps] = name;
            return steps++;
        }
        return Marks - 1;
    }

    void Collect()
    {
        for (unsigned i = 0; i < Frames; ++i)
        {
            auto& s = slots[i];
            if (!s.pending || !s.submitted)
                continue;
            const auto done = s.fence->GetCompletedValue();
            if (done == UINT64_MAX)
            {
                s.pending = false; // device removed; abandon
                continue;
            }
            if (done < s.value)
                continue;
            UINT64* data = nullptr;
            D3D12_RANGE range { i * Marks * sizeof(UINT64), (i * Marks + s.used) * sizeof(UINT64) };
            if (s.used >= 2 && s.frequency && SUCCEEDED(readback->Map(0, &range, (void**) &data)))
            {
                const UINT64* t = data + i * Marks;
                bool ok = true;
                for (unsigned m = 1; m < s.used; ++m)
                    ok &= t[m] >= t[m - 1] && t[0] != 0;
                if (ok)
                {
                    for (unsigned m = 1; m < s.used; ++m)
                        stepSums[StepIndex(s.names[m])] += double(t[m] - t[m - 1]) * 1000.0 / double(s.frequency);
                    totalSum += double(t[s.used - 1] - t[0]) * 1000.0 / double(s.frequency);
                    ++frames;
                }
                D3D12_RANGE none { 0, 0 };
                readback->Unmap(0, &none);
            }
            s.pending = false;
        }
        if (frames >= 600)
        {
            std::string line;
            char part[96];
            for (unsigned i = 0; i < steps; ++i)
            {
                std::snprintf(part, sizeof(part), "%s %.3f; ", stepNames[i], stepSums[i] / frames);
                line += part;
            }
            LOG_INFO("DLSS-NR HUD-less GPU profile ({} frames, ms per frame): {}total {:.3f}", frames, line,
                     totalSum / frames);
            for (auto& v : stepSums)
                v = 0.0;
            totalSum = 0.0;
            frames = 0;
        }
    }

  public:
    static Profiler& Get()
    {
        static Profiler instance;
        return instance;
    }

    static bool Enabled() { return Config::Instance()->DlssNrHudlessProfile.value_or_default(); }

    void Begin(ID3D12GraphicsCommandList* cmd, ID3D12Device* d, ID3D12CommandQueue* queue)
    {
        if (active >= 0)
        {
            slots[active].pending = false; // the previous recording was never submitted
            active = -1;
        }
        activeList = nullptr;
        if (!Enabled() || !cmd || !d || !queue || !Init(d))
            return;
        Collect();
        for (unsigned i = 0; i < Frames; ++i)
        {
            auto& s = slots[i];
            if (s.pending)
                continue;
            UINT64 frequency = 0;
            if (FAILED(queue->GetTimestampFrequency(&frequency)) || frequency == 0)
                return;
            s.frequency = frequency;
            s.used = 0;
            s.pending = true;
            s.submitted = false;
            active = (int) i;
            activeList = cmd;
            Mark(cmd, "start");
            return;
        }
    }

    void Mark(ID3D12GraphicsCommandList* cmd, const char* name)
    {
        if (active < 0 || cmd != activeList)
            return;
        auto& s = slots[active];
        if (s.used >= Marks)
            return;
        s.names[s.used] = name;
        cmd->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, active * Marks + s.used);
        ++s.used;
    }

    void End(ID3D12GraphicsCommandList* cmd)
    {
        if (active < 0 || cmd != activeList)
            return;
        auto& s = slots[active];
        if (s.used)
            cmd->ResolveQueryData(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, active * Marks, s.used, readback.Get(),
                                  active * Marks * sizeof(UINT64));
    }

    // After the compose list was executed on the queue.
    void Submitted(ID3D12CommandQueue* queue)
    {
        if (active < 0)
            return;
        auto& s = slots[active];
        if (queue && SUCCEEDED(queue->Signal(s.fence.Get(), ++s.value)))
            s.submitted = true;
        else
            s.pending = false;
        active = -1;
        activeList = nullptr;
    }

    // Recording failed before execution.
    void Abandon()
    {
        if (active >= 0)
            slots[active].pending = false;
        active = -1;
        activeList = nullptr;
    }
};

inline void Mark(ID3D12GraphicsCommandList* cmd, const char* name) { Profiler::Get().Mark(cmd, name); }
} // namespace DlssNr::HudlessProfile
