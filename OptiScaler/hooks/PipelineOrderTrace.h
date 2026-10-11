#pragma once

// Diagnostic ([Log] PipelineOrderTrace, off by default): logs, for a few short windows of frames, the order in
// which the game calls Streamline (DLSS evaluate, resource tags for frame generation) and presents, with the
// format and size of every tagged resource. Answers "where in the frame does the game run DLSS, which image does it
// hand to frame generation, and how many presents follow each game frame".

#include <Config.h>
#include <d3d12.h>
#include <atomic>
#include <chrono>

namespace PipelineOrderTrace
{
// Presents counted from the first Streamline evaluate (-1 until then); windows start this many presents later.
inline std::atomic<int64_t> presentCount { -1 };
inline constexpr int64_t windowStarts[] = { 300, 3000 };
inline constexpr int64_t windowLength = 6;

inline bool Enabled() { return Config::Instance()->PipelineOrderTrace.value_or_default(); }

inline bool Active()
{
    if (!Enabled())
        return false;

    const auto p = presentCount.load();
    for (auto start : windowStarts)
    {
        if (p >= start && p < start + windowLength)
            return true;
    }

    return false;
}

inline void OnEvaluate()
{
    int64_t expected = -1;
    presentCount.compare_exchange_strong(expected, 0);
}

inline void OnPresent(const char* which, UINT flags)
{
    // Present rate every 5 s (all presents through OptiScaler's swapchain, generated frames included).
    if (Enabled())
    {
        static auto windowStart = std::chrono::steady_clock::now();
        static uint64_t windowPresents = 0;
        ++windowPresents;
        const auto now = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(now - windowStart).count();
        if (seconds >= 5.0)
        {
            LOG_INFO("PipelineOrder: present rate {:.1f}/s over {:.1f} s", windowPresents / seconds, seconds);
            windowStart = now;
            windowPresents = 0;
        }
    }

    if (presentCount.load() < 0)
        return;

    if (Active())
        LOG_INFO("PipelineOrder: present #{} via {} flags 0x{:X} (thread {})", presentCount.load(), which, flags,
                 GetCurrentThreadId());

    presentCount++;
}

inline std::string Describe(void* native)
{
    if (native == nullptr)
        return "null";

    auto desc = ((ID3D12Resource*) native)->GetDesc();
    return std::format("{}x{} format {} ({} mips, dim {})", desc.Width, desc.Height, (int) desc.Format,
                       desc.MipLevels, (int) desc.Dimension);
}
} // namespace PipelineOrderTrace
