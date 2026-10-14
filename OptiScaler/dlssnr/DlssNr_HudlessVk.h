#pragma once

// Vulkan: the game's frame generation inputs for the finished picture without the HUD. Streamline tags (slSetTag /
// slSetTagForFrame) carry the HUD-less picture, the UI layer (colour and alpha) and the backbuffer the game is about
// to present; the latest of each is kept here for the DLSS-G plugin's present hook, which runs before frame
// generation on the game's present.

#include <sl.h>
#include <vulkan/vulkan.h>
#include <atomic>
#include <cstdint>
#include <mutex>

namespace DlssNr::HudlessVk
{
inline constexpr uint32_t kHudlessColor = 2;
inline constexpr uint32_t kUiColorAndAlpha = 23;
inline constexpr uint32_t kBackbuffer = 53;

struct Image
{
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED; // as tagged
    VkImageUsageFlags usage = 0;
    bool Valid() const { return image != VK_NULL_HANDLE && width && height; }
};

struct Frame
{
    Image hudless, ui, backbuffer;
    uint64_t serial = 0;
};

inline std::mutex mutex;
inline Frame latest;
inline uint64_t serial = 0;

inline Image FromTag(const sl::ResourceTag& tag)
{
    Image out;
    if (!tag.resource)
        return out;
    out.image = static_cast<VkImage>(tag.resource->native);
    out.view = static_cast<VkImageView>(tag.resource->view);
    out.format = static_cast<VkFormat>(tag.resource->nativeFormat);
    out.width = tag.resource->width;
    out.height = tag.resource->height;
    out.layout = static_cast<VkImageLayout>(tag.resource->state);
    out.usage = tag.resource->usage;
    return out;
}

inline void Record(const sl::ResourceTag* tags, uint32_t count)
{
    if (!tags)
        return;
    std::lock_guard lock(mutex);
    bool any = false;
    for (uint32_t i = 0; i < count; ++i)
    {
        const auto type = static_cast<uint32_t>(tags[i].type);
        if (type == kHudlessColor)
            latest.hudless = FromTag(tags[i]), any = true;
        else if (type == kUiColorAndAlpha)
            latest.ui = FromTag(tags[i]), any = true;
        else if (type == kBackbuffer)
            latest.backbuffer = FromTag(tags[i]), any = true;
    }
    if (any)
        latest.serial = ++serial;
}

// The frame tagged since the last call, if any.
inline bool Take(Frame& out, uint64_t& lastSerial)
{
    std::lock_guard lock(mutex);
    if (latest.serial == 0 || latest.serial == lastSerial)
        return false;
    out = latest;
    lastSerial = latest.serial;
    return true;
}
} // namespace DlssNr::HudlessVk
