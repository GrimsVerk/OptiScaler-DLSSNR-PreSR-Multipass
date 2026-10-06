#pragma once

#include "SysUtils.h"
#include <Config.h>

#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>

class ScopedIndent
{
  public:
    explicit ScopedIndent(float indent = 16.0f) : m_indent(indent) { ImGui::Indent(m_indent); }

    ~ScopedIndent() { ImGui::Unindent(m_indent); }

  private:
    float m_indent;
};

class ScopedCollapsingHeader
{
  public:
    explicit ScopedCollapsingHeader(const char* label, ImGuiTreeNodeFlags flags = 0)
    {
        // Inside a closed MenuSection (or any window that is skipping its items) nothing may be drawn: a child
        // window would still appear, because children only follow their parent's collapsed state.
        if (ImGui::GetCurrentWindow()->SkipItems)
            return;

        ImGui::PushID(label);

        ImGui::BeginChild("##CollapsingHeaderChild", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        _headerOpen = ImGui::CollapsingHeader(label, flags);
        _active = true;
    }

    bool IsHeaderOpen() const { return _headerOpen; }

    ~ScopedCollapsingHeader()
    {
        if (_active)
        {
            ImGui::EndChild();
            ImGui::PopID();
        }
    }

  private:
    bool _active = false;
    bool _headerOpen = false;
};

// A settings section drawn as a collapsible header, closed by default, whose body is simply the code that
// follows until the object goes out of scope. Unlike ScopedCollapsingHeader the body does not sit inside an if.
// While the header is closed the section's window has SkipItems raised, which turns every widget call in the body
// into a no-op, the same way ImGui hides clipped table cells. The body's code still runs, so values that later
// sections read are still computed and no control flow in the long section functions had to change.
// Collapsible headers inside the body become indented sub-menus; a section declared inside a closed section
// draws nothing.
class MenuSection
{
  public:
    explicit MenuSection(const char* label, const char* help = nullptr, ImGuiTreeNodeFlags flags = 0)
    {
        if (ImGui::GetCurrentWindow()->SkipItems)
            return;

        ImGui::Spacing();
        ImGui::PushID(label);
        ImGui::BeginChild("##MenuSection", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        _active = true;

        ImGuiWindow* window = ImGui::GetCurrentWindow();
        _skipBefore = window->SkipItems; // true when the child itself is scrolled out of view
        _open = ImGui::CollapsingHeader(label, flags);

        if (help != nullptr && !window->SkipItems)
        {
            // The marker sits on the header, right after its label; the header keeps the click.
            const ImGuiStyle& style = ImGui::GetStyle();
            ImGui::SameLine(ImGui::GetTreeNodeToLabelSpacing() + ImGui::CalcTextSize(label, nullptr, true).x +
                            style.ItemSpacing.x * 2.0f);
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlappedByItem | ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", help);
        }

        if (_open)
        {
            ImGui::Indent(_indent);
            ImGui::Spacing();
            return;
        }

        // Closed: remember the layout cursor, then make the body a no-op. Clearing the last-item record stops
        // the body's tooltips from firing for the header they would otherwise believe they follow.
        _cursor = window->DC.CursorPos;
        _cursorMax = window->DC.CursorMaxPos;
        _cursorPrevLine = window->DC.CursorPosPrevLine;
        _currLineSize = window->DC.CurrLineSize;
        _prevLineSize = window->DC.PrevLineSize;
        _currLineTextBase = window->DC.CurrLineTextBaseOffset;
        _prevLineTextBase = window->DC.PrevLineTextBaseOffset;
        window->SkipItems = true;
        ImGuiContext& g = *ImGui::GetCurrentContext();
        g.LastItemData.ID = 0;
        g.LastItemData.StatusFlags = ImGuiItemStatusFlags_None;
        g.LastItemData.Rect = ImRect();
    }

    MenuSection(const MenuSection&) = delete;
    MenuSection& operator=(const MenuSection&) = delete;

    bool IsOpen() const { return _open; }

    ~MenuSection()
    {
        if (!_active)
            return;

        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (_open)
        {
            ImGui::Spacing();
            ImGui::Unindent(_indent);
        }
        else
        {
            window->SkipItems = _skipBefore;
            window->DC.CursorPos = _cursor;
            window->DC.CursorMaxPos = _cursorMax;
            window->DC.CursorPosPrevLine = _cursorPrevLine;
            window->DC.CurrLineSize = _currLineSize;
            window->DC.PrevLineSize = _prevLineSize;
            window->DC.CurrLineTextBaseOffset = _currLineTextBase;
            window->DC.PrevLineTextBaseOffset = _prevLineTextBase;
            window->DC.IsSameLine = false;
            window->DC.IsSetPos = false;
        }

        ImGui::EndChild();
        ImGui::PopID();
    }

  private:
    static constexpr float _indent = 16.0f;
    bool _active = false;
    bool _open = false;
    bool _skipBefore = false;
    ImVec2 _cursor {}, _cursorMax {}, _cursorPrevLine {}, _currLineSize {}, _prevLineSize {};
    float _currLineTextBase = 0.0f, _prevLineTextBase = 0.0f;
};

template <typename T> struct MenuOption
{
    T value;
    std::string label;
    std::string tooltip;
    bool disabled = false;
    bool hidden = false;

    MenuOption& set_disabled(bool condition, const std::string& reason = "")
    {
        if (condition)
        {
            disabled = true;
            if (!reason.empty())
                tooltip = reason;
        }
        return *this;
    }

    MenuOption& set_hidden(bool condition)
    {
        if (condition)
        {
            hidden = true;
        }
        return *this;
    }
};

class MenuCommon
{
  private:
    // internal values
    inline static HWND _handle = nullptr;
    // inline static WNDPROC _oWndProc = nullptr;
    inline static bool _isVisible = false;
    inline static bool _isInited = false;
    inline static bool _isUWP = false;

    // mipmap calculations
    inline static bool _showMipmapCalcWindow = false;
    inline static bool _showHudlessWindow = false;
    inline static float _mipBias = 0.0f;
    inline static float _mipBiasCalculated = 0.0f;
    inline static uint32_t _mipmapUpscalerQuality = 0;
    inline static float _mipmapUpscalerRatio = 0;
    inline static uint32_t _displayWidth = 0;
    inline static uint32_t _renderWidth = 0;

    inline static UINT64 _frameCount = 0;

    // reflex
    inline static float _limitFps = std::numeric_limits<float>::infinity();

    // ffx
    inline static int _ffxUpscalerIndex = -1;
    inline static int _ffxFGIndex = -1;

    // output scaling
    inline static float _ssRatio = 0.0f;
    inline static bool _ssEnabled = false;
    inline static Scaler _ssDownsampler = Scaler::FSR1;

    // ui scale
    inline static int _selectedScale = 0;

    // overlay states
    inline static bool _dx11Ready = false;
    inline static bool _dx12Ready = false;
    inline static bool _vulkanReady = false;

    inline static void ShowTooltip(const char* tip);

    inline static void ShowHelpMarker(const char* tip);
    inline static void ShowResetButton(CustomOptional<bool, NoDefault>* initFlag, std::string buttonName);
    inline static void ReInitUpscaler();

    inline static void SeparatorWithHelpMarker(const char* label, const char* tip);

    static Upscaler GetBackendCode(const API api);
    static void GetCurrentBackendInfo(const API api, Upscaler& upscaler, std::string* name);
    static void RenderUpscalerCombo(const API api, Upscaler currentUpscaler, const std::vector<Upscaler>& options);
    static void AddDx11Backends(Upscaler upscaler);
    static void AddDx12Backends(Upscaler upscaler);
    static void AddVulkanBackends(Upscaler upscaler);
    template <HasDefaultValue B> static void AddResourceBarrier(std::string name, CustomOptional<int32_t, B>* value);
    template <HasDefaultValue B> static void AddDLSSRenderPreset(std::string name, CustomOptional<uint32_t, B>* value);
    template <HasDefaultValue B> static void AddDLSSDRenderPreset(std::string name, CustomOptional<uint32_t, B>* value);
    template <typename TStorage, typename T>
    static void PopulateCombo(const std::string& name, TStorage& currentValue,
                              const std::vector<MenuOption<T>>& options);

    struct RenderMenuContext;

    // RenderMenu orchestration helpers. These keep the public RenderMenu() flow short
    // while preserving the original ImGui layout and draw order.
    static void UpdateRenderTiming(RenderMenuContext& ctx);
    static void UpdateMenuInputMode(RenderMenuContext& ctx);
    static void HandleMenuShortcuts(RenderMenuContext& ctx);
    static void UpdateVersionAndStartupNotifications(RenderMenuContext& ctx);
    static void BeginMenuFrameIfNeeded(RenderMenuContext& ctx);
    static void RenderSplashWindow(RenderMenuContext& ctx);
    static void RenderNotifications(RenderMenuContext& ctx);
    static void UpdateFrameTimeAverages(RenderMenuContext& ctx);
    static void RenderPerformanceOverlay(RenderMenuContext& ctx);

    // Labels for the Neural Rendering comparison views. Drawn every frame, into the frame plane,
    // so a screenshot keeps them and the wipe reveals and hides them like the images.
    static void RenderMainMenuWindow(RenderMenuContext& ctx);

    // RenderMainMenuWindow section helpers. These keep the main window flow readable
    // without changing the existing ImGui layout, labels, or setting side effects.
    static void RenderMainMenuHeaderMessages(RenderMenuContext& ctx);
    static void RenderMainMenuTable(RenderMenuContext& ctx);
    static void RenderActiveUpscalerSettings(RenderMenuContext& ctx);
    static void RenderFrameGenerationSelection(RenderMenuContext& ctx);
    static void RenderFrameGenerationRuntimeSettings(RenderMenuContext& ctx);
    static void RenderFsrCommonSettings(RenderMenuContext& ctx);
    static void RenderFramerateSettings(RenderMenuContext& ctx);
    static void RenderFakenvapiSettings(RenderMenuContext& ctx);
    static void RenderLowLatencySettings(RenderMenuContext& ctx);
    static void RenderActiveImageSettings(RenderMenuContext& ctx);
    static void RenderMagnifierSettings(RenderMenuContext& ctx);
    static void RenderQuirksSettings(RenderMenuContext& ctx);
    static void RenderAdvancedSettings(RenderMenuContext& ctx);
    static void RenderLoggingSettings(RenderMenuContext& ctx);
    static void RenderProfilesSettings(RenderMenuContext& ctx);
    static void RenderThemeSettings(RenderMenuContext& ctx);
    static void RenderFpsOverlaySettings(RenderMenuContext& ctx);
    static void RenderUpscalerInputsSettings(RenderMenuContext& ctx);
    static void RenderApiAndTextureSettings(RenderMenuContext& ctx);
    static void RenderKeybindSettings(RenderMenuContext& ctx);
    static void RenderMainMenuGraphs(RenderMenuContext& ctx);
    static void RenderMainMenuBottomBar(RenderMenuContext& ctx);
    static void RenderMipmapBiasWindow(RenderMenuContext& ctx, ImGuiWindowFlags flags);
    static void RenderHudlessResourcesWindow(RenderMenuContext& ctx, ImGuiWindowFlags flags);

    static void UpdateManualInput(HWND targetHwnd);

  public:
    static void Dx11Inited() { _dx11Ready = true; }
    static void Dx12Inited() { _dx12Ready = true; }
    static void VulkanInited() { _vulkanReady = true; }
    static bool IsInited() { return _isInited; }
    static bool IsVisible() { return _isVisible; }
    static HWND Handle() { return _handle; }

    static bool RenderMenu();
    static void Init(HWND InHwnd, bool isUWP);
    static void Shutdown();
    static void HideMenu();
    static void Present();
    static void ApplyThemeStyle();
};
