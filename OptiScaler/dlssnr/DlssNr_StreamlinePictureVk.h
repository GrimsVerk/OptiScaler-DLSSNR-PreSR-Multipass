#pragma once

#include <windows.h>

namespace DlssNr::StreamlinePictureVk
{
// Detours Streamline's own vkQueuePresentKHR (sl.interposer), so the HUD-less work runs before Streamline.
void HookInterposer(HMODULE interposer);
} // namespace DlssNr::StreamlinePictureVk
