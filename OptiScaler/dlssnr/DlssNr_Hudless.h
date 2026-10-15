#pragma once

// The game's HUD-less picture and UI mask, as tagged for Streamline frame generation. Tagged on the render thread
// (slSetTag / slSetTagForFrame) and read in the frame generation present hook, both once per game frame. The tags
// are "valid until present", so the resources are alive and in their tagged state when the present hook runs.

#include <Config.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <atomic>
#include <mutex>
#include <sl.h>

namespace DlssNr::Hudless
{
// Streamline 2.14 numbering. Older SDK headers (including the one bundled here) number UIAlpha 68, which 2.14 uses
// for kBufferTypeResponsivityMask, so the mask is identified by its number in the game's runtime, not the header.
inline constexpr uint32_t kHudlessColor = 2;         // sl::kBufferTypeHUDLessColor
inline constexpr uint32_t kUiColorAndAlpha = 23;     // sl::kBufferTypeUIColorAndAlpha: mask in alpha
inline constexpr uint32_t kUiAlpha = 69;             // sl::kBufferTypeUIAlpha (2.14): mask in the first channel

struct Tag
{
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
};

struct Frame
{
    Tag color;
    Tag mask;
    bool maskInAlpha = false; // UIColorAndAlpha: read .a; UIAlpha: read .r
    uint64_t serial = 0;      // increases with every tagged HUD-less picture
};

inline std::mutex mutex;
inline Frame latest;

inline bool Wanted()
{
    const auto& cfg = *Config::Instance();
    return cfg.DlssNrEnabled.value_or_default() && cfg.DlssNrFinishedPicture.value_or_default() &&
           cfg.DlssNrFinishedHudless.value_or_default();
}

// Some games (Alan Wake 2) only make their HUD-less picture while their frame generation setting is on. With this
// option the game keeps it on, but Streamline is told not to generate frames. Everything else the game turns on for
// frame generation (Reflex) stays as the game set it.
inline bool FrameGenerationOffWanted()
{
    const auto& cfg = *Config::Instance();
    return cfg.DlssNrHudlessFrameGenOff.value_or_default() && cfg.DlssNrFinishedPicture.value_or_default() &&
           cfg.DlssNrFinishedHudless.value_or_default();
}

// Whether to pass the game's frame generation request on as off. Switching frame generation off behind the game's
// back while it generates frames can leave the game waiting forever for a generated frame (Alan Wake 2 froze), so a
// change of the option only takes effect while the game has frame generation off itself (its menus and loading
// screens). Frame generation then never changes under the game: it stays off when the game turns it back on, or
// comes back on exactly when the game asks for it.
inline std::atomic<int> frameGenerationOffApplied { -1 }; // -1: the game has not asked for frame generation yet

inline bool FrameGenerationOff(bool gameWantsFrameGeneration)
{
    int off = frameGenerationOffApplied.load();
    if (off < 0 || !gameWantsFrameGeneration)
    {
        off = FrameGenerationOffWanted() ? 1 : 0;
        frameGenerationOffApplied = off;
    }
    return gameWantsFrameGeneration && off == 1;
}

// The option was changed but waits for the game to switch frame generation off itself.
inline bool FrameGenerationOffPending()
{
    const int off = frameGenerationOffApplied.load();
    return off >= 0 && (off == 1) != FrameGenerationOffWanted();
}

inline void Record(const sl::ResourceTag* tags, uint32_t count)
{
    if (tags == nullptr || !Wanted())
        return;

    std::lock_guard lock(mutex);
    for (uint32_t i = 0; i < count; i++)
    {
        const auto type = (uint32_t) tags[i].type;
        if (type != kHudlessColor && type != kUiAlpha && type != kUiColorAndAlpha)
            continue;

        auto* native = tags[i].resource != nullptr ? (ID3D12Resource*) tags[i].resource->native : nullptr;
        if (native == nullptr || tags[i].resource->type != sl::ResourceType::eTex2d)
            continue;

        Tag tag;
        tag.resource = native;
        tag.state = (D3D12_RESOURCE_STATES) tags[i].resource->state;
        if (type == kHudlessColor)
        {
            latest.color = std::move(tag);
            ++latest.serial;
        }
        else
        {
            latest.mask = std::move(tag);
            latest.maskInAlpha = type == kUiColorAndAlpha;
        }
    }
}

// The newest HUD-less picture, once. Empty when none was tagged since the previous call.
inline bool Take(Frame& out, uint64_t& lastSerial)
{
    std::lock_guard lock(mutex);
    if (!latest.color.resource || latest.serial == lastSerial)
        return false;
    lastSerial = latest.serial;
    out = latest;
    return true;
}

inline void Clear()
{
    std::lock_guard lock(mutex);
    latest = {};
}
} // namespace DlssNr::Hudless
