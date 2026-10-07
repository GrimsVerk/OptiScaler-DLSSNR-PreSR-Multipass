#pragma once

#include <nvsdk_ngx_params.h>
#include <vulkan/vulkan.h>
#include <filesystem>
#include <memory>
#include <vector>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;

namespace DlssNr
{
// Folders searched for nvngx_dlssnr.dll, in order.
std::vector<std::filesystem::path> CompatibilityRuntimeFolders();

// Own this alongside the feature until its recorded GPU work has retired.
// The driver still owns parameter allocation; only NR model calls use this backend.
class CompatibilityRuntime
{
  public:
    using Allocate = NVSDK_NGX_Result (*)(NVSDK_NGX_Parameter**);
    using Destroy = NVSDK_NGX_Result (*)(NVSDK_NGX_Parameter*);
    struct Module; // the loaded runtime, shared with the Vulkan owner

    static std::shared_ptr<CompatibilityRuntime> TryOpen(ID3D12Device* device);
    static std::shared_ptr<CompatibilityRuntime> Open(const std::filesystem::path& path, ID3D12Device* device,
                                                      Allocate allocate, Destroy destroy,
                                                      const std::filesystem::path& dataPath = {});
    ~CompatibilityRuntime();
    CompatibilityRuntime(const CompatibilityRuntime&) = delete;
    CompatibilityRuntime& operator=(const CompatibilityRuntime&) = delete;
    NVSDK_NGX_Result Create(ID3D12GraphicsCommandList*, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
    NVSDK_NGX_Result Evaluate(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, NVSDK_NGX_Parameter*);
    NVSDK_NGX_Result Release(NVSDK_NGX_Handle*);

    // Loads (or retains) the one runtime module; null if it is missing or another path is already live.
    static std::shared_ptr<Module> AcquireModule(const std::filesystem::path& candidate);

  private:
    std::shared_ptr<Module> module;
    ID3D12Device* device = nullptr;
    NVSDK_NGX_Parameter* capabilities = nullptr;
    Destroy destroyParameters = nullptr;
    bool initialized = false;
    CompatibilityRuntime() = default;
};

// Vulkan counterpart. The NGX driver refuses feature 18 on Vulkan the same way it does on D3D12,
// so the model is created and evaluated through the runtime's own Vulkan exports.
class VulkanCompatibilityRuntime
{
  public:
    using Allocate = CompatibilityRuntime::Allocate;
    using Destroy = CompatibilityRuntime::Destroy;

    static std::shared_ptr<VulkanCompatibilityRuntime> TryOpen(VkInstance instance, VkPhysicalDevice physicalDevice,
                                                               VkDevice device);
    static std::shared_ptr<VulkanCompatibilityRuntime> Open(const std::filesystem::path& path, VkInstance instance,
                                                            VkPhysicalDevice physicalDevice, VkDevice device,
                                                            Allocate allocate, Destroy destroy,
                                                            const std::filesystem::path& dataPath = {});
    ~VulkanCompatibilityRuntime();
    VulkanCompatibilityRuntime(const VulkanCompatibilityRuntime&) = delete;
    VulkanCompatibilityRuntime& operator=(const VulkanCompatibilityRuntime&) = delete;
    NVSDK_NGX_Result Create(VkCommandBuffer, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
    NVSDK_NGX_Result Evaluate(VkCommandBuffer, const NVSDK_NGX_Handle*, NVSDK_NGX_Parameter*);
    NVSDK_NGX_Result Release(NVSDK_NGX_Handle*);

  private:
    std::shared_ptr<CompatibilityRuntime::Module> module;
    VkDevice device = VK_NULL_HANDLE;
    NVSDK_NGX_Parameter* capabilities = nullptr;
    Destroy destroyParameters = nullptr;
    bool initialized = false;
    VulkanCompatibilityRuntime() = default;
};
} // namespace DlssNr
