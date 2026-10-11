#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

// HUD-less finished picture: motion-adaptive NR strength and temporal smoothing of NR's change
// (precompile/dlssnr_hudless_temporal.hlsl). The constants overlay the shared 256-byte NR constant buffer.
namespace DlssNr::HudlessTemporal
{
struct Constants
{
    // Mode and Width/Height sit where the shared dispatch reads them (offsets 0, 8, 12) to size the grid.
    uint32_t mode = 0;
    uint32_t unused = 0;
    uint32_t width = 0, height = 0;
    uint32_t guideX = 0, guideY = 0, guideWidth = 0, guideHeight = 0;
    float mvToOutX = 0, mvToOutY = 0;
    uint32_t historyValid = 0;
    uint32_t smoothing = 0;
    float currentWeight = 1;
    float tolerance = 1.25f;
    uint32_t adaptive = 0;
    float stillStrength = 1, motionStrength = 1, motionStart = 1, motionFull = 8;
    uint32_t debugView = 0;
    float reject = 0;       // picture-change threshold (luma), 0 = off
    uint32_t bandSplit = 0; // smooth the coarse change only
    uint32_t padding[42] {};
};
static_assert(sizeof(Constants) == 256);

struct Settings
{
    bool adaptive = false, smoothing = false;
    float stillStrength = 1, motionStrength = 0.9f, motionStart = 4, motionFull = 16;
    float frames = 12, tolerance = 4.0f;
    float reject = 0.03f;
    bool bandSplit = true;
    int debugView = 0;
    bool Active() const { return adaptive || smoothing || debugView != 0; }
};

template <class ConfigLike> Settings ReadSettings(const ConfigLike& cfg)
{
    const auto finite = [](float v, float fallback) { return std::isfinite(v) ? v : fallback; };
    Settings s;
    s.adaptive = cfg.DlssNrHudlessMotionAdaptive.value_or_default();
    s.smoothing = cfg.DlssNrHudlessSmoothing.value_or_default();
    s.stillStrength = std::clamp(finite(cfg.DlssNrHudlessStillStrength.value_or_default(), 1.0f), 0.0f, 2.0f);
    s.motionStrength = std::clamp(finite(cfg.DlssNrHudlessMotionStrength.value_or_default(), 0.9f), 0.0f, 2.0f);
    s.motionStart = std::clamp(finite(cfg.DlssNrHudlessMotionStart.value_or_default(), 4.0f), 0.0f, 512.0f);
    s.motionFull = std::clamp(finite(cfg.DlssNrHudlessMotionFull.value_or_default(), 16.0f), s.motionStart, 512.0f);
    s.frames = std::clamp(finite(cfg.DlssNrHudlessSmoothingFrames.value_or_default(), 12.0f), 1.0f, 32.0f);
    s.tolerance = std::clamp(finite(cfg.DlssNrHudlessSmoothingTolerance.value_or_default(), 4.0f), 0.0f, 16.0f);
    s.reject = std::clamp(finite(cfg.DlssNrHudlessSmoothingReject.value_or_default(), 0.03f), 0.0f, 1.0f);
    s.bandSplit = cfg.DlssNrHudlessSmoothingDetail.value_or_default();
    s.debugView = std::clamp(cfg.DlssNrHudlessDebugView.value_or_default(), 0, 3);
    return s;
}
} // namespace DlssNr::HudlessTemporal
