#include "pch.h"
#include "DlssNr_StreamlinePictureVk.h"
#include "DlssNr_HudlessVk.h"
#include "DlssNrFinished_Vk.h"
#include "DlssNr_HudlessTest.h"
#include <Config.h>
#include <State.h>
#include "detours/detours.h"

// Vulkan counterpart of DlssNr_StreamlinePicture, for the finished picture without the HUD. Streamline's DLSS-G
// hands the game's present to its own pacer thread, which runs frame generation on its own queue after waiting for
// the game's queue. A detour on the interposer's present runs before all of that: NR's work is submitted on the
// game's queue ahead of Streamline's signal, so frame generation reads the NR picture.
namespace DlssNr::StreamlinePictureVk
{
namespace
{
uint64_t lastTagSerial = 0;

// The interposer's vkQueuePresentKHR: the game's present, before Streamline and any of its plugins. Frame generation
// takes its copies of the tagged pictures before its own plugin hook runs, so NR has to run here.
using InterposerPresent = VkResult(VKAPI_PTR*)(VkQueue, const VkPresentInfoKHR*);
InterposerPresent originalInterposerPresent = nullptr;
HMODULE hookedInterposer = nullptr;
PFN_vkGetSwapchainImagesKHR interposerSwapchainImages = nullptr;

VkResult VKAPI_CALL PresentBeforeStreamline(VkQueue queue, const VkPresentInfoKHR* present)
{
    HudlessTest::Harness::Get().Poll(); // test harness only (HudlessTestControl); live settings
    HudlessVk::Frame frame;
    const bool fresh = HudlessVk::Take(frame, lastTagSerial);
    VkPresentInfoKHR local = present ? *present : VkPresentInfoKHR {};
    const bool own =
        present && FinishedVkPresentHudless(queue, &local, fresh ? &frame : nullptr, interposerSwapchainImages);
    return originalInterposerPresent(queue, own ? &local : present);
}
} // namespace

void HookInterposer(HMODULE interposer)
{
    if (!interposer || interposer == hookedInterposer)
        return;
    const auto& cfg = *Config::Instance();
    // NR on at launch is enough (the finished picture needs that anyway); the mode itself can be switched later.
    if (!cfg.DlssNrEnabled.value_or_default())
        return;
    auto* present = reinterpret_cast<InterposerPresent>(GetProcAddress(interposer, "vkQueuePresentKHR"));
    if (!present)
        return;
    interposerSwapchainImages =
        reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(GetProcAddress(interposer, "vkGetSwapchainImagesKHR"));
    originalInterposerPresent = present;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&) originalInterposerPresent, PresentBeforeStreamline);
    if (DetourTransactionCommit() == NO_ERROR)
    {
        hookedInterposer = interposer;
        LOG_INFO("DLSS-NR: hooked Streamline's Vulkan present (sl.interposer vkQueuePresentKHR) for the HUD-less "
                 "finished picture");
    }
    else
    {
        originalInterposerPresent = nullptr;
        LOG_WARN("DLSS-NR: could not hook Streamline's Vulkan present");
    }
}
} // namespace DlssNr::StreamlinePictureVk
