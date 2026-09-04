#pragma once

#include "config_store.h"
#include "model.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lightlaunch {

struct IconDispatchContext;
struct IconLoadBatch;
struct PreviewIconLoadBatch;
struct FenceWindow;

class Application {
public:
    explicit Application(HINSTANCE instance, std::wstring configPath = {},
                         std::uintptr_t configIdentity = 0);
    ~Application();
    int Run(int showCommand);

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK ContentWindowProc(HWND window, UINT message, WPARAM wParam,
                                              LPARAM lParam);
    static LRESULT CALLBACK RailInputWindowProc(HWND window, UINT message, WPARAM wParam,
                                                LPARAM lParam);
    static LRESULT CALLBACK CategoryListSubclassProc(HWND window, UINT message, WPARAM wParam,
                                                     LPARAM lParam, UINT_PTR subclassId,
                                                     DWORD_PTR referenceData);
    static LRESULT CALLBACK PinButtonSubclassProc(HWND window, UINT message, WPARAM wParam,
                                                  LPARAM lParam, UINT_PTR subclassId,
                                                  DWORD_PTR referenceData);
    static LRESULT CALLBACK AddCategoryButtonSubclassProc(
        HWND window, UINT message, WPARAM wParam, LPARAM lParam,
        UINT_PTR subclassId, DWORD_PTR referenceData);
    static LRESULT CALLBACK ItemListSubclassProc(HWND window, UINT message, WPARAM wParam,
                                                 LPARAM lParam, UINT_PTR subclassId,
                                                 DWORD_PTR referenceData);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleFenceMessage(FenceWindow& fence, HWND fenceWindow,
                               UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleRailInputMessage(UINT message, WPARAM wParam, LPARAM lParam);

    bool RegisterWindowClass() const;
    bool CreateControls();
    void RecreateFonts();
    void ApplyFonts() const;
    void LayoutControls(int width, int height);
    void LayoutFenceControls(FenceWindow& fence, int width, int height);
    void DestroyResources();
    bool AddTrayIcon();
    void RemoveTrayIcon();
    void HideToTray();
    void ShowFromTray();
    void ShowTrayMenu();
    void ExitApplication();
    void SetPinned(bool pinned);
    void PollScreenEdge();
    void ShowEdgePanel(bool activate);
    void PositionEdgePanel(HMONITOR monitor);
    void PositionRailInputWindow();
    void ResetEdgeTracking();
    HWND DialogOwner() const;
    bool IsInteractionBlocked() const;

    void RefreshCategories();
    void RefreshItems();
    void RefreshFence(FenceWindow& fence);
    void RefreshAllFences();
    void StartCategoryPreviewLoad();
    void QueueVisibleCategoryPreviewLoad();
    void ClearCategoryPreviewIcons();
    void DrawCategoryItem(const DRAWITEMSTRUCT& drawItem) const;
    void DrawPinButton(HDC deviceContext, const RECT& clientRect) const;
    void DrawRailPanel(HDC deviceContext, const RECT& clientRect) const;
    RECT RailPanelRect(int width, int height) const;
    RECT CategoryCellRect(const RECT& rowRect, int column) const;
    std::optional<std::size_t> CategoryIndexFromListPoint(POINT clientPoint) const;
    void DrawAddCategoryButton(HDC deviceContext, const RECT& clientRect) const;
    void ActivateCategory(std::size_t categoryIndex);
    FenceWindow* CreateFenceWindow(std::size_t categoryIndex);
    FenceWindow* FindFence(std::size_t categoryIndex) const;
    bool IsFenceVisible(std::size_t categoryIndex) const;
    void OpenOrShowFence(std::size_t categoryIndex);
    void HideFence(FenceWindow& fence);
    void SetFencePinned(FenceWindow& fence, bool pinned);
    void ApplyFenceRoundedRegion(FenceWindow& fence) const;
    void RecreateFenceFonts(FenceWindow& fence);
    void DrawFence(const FenceWindow& fence, HDC deviceContext,
                   const RECT& clientRect) const;
    void RefreshFenceBackground(FenceWindow& fence, bool forceReload = false);
    void UpdateFenceListBackground(FenceWindow& fence);
    void InvalidateFenceSurface(FenceWindow& fence) const;
    void ShowFenceBackgroundSettings();
    void ClearFenceBackground();
    void DrawFenceButton(const FenceWindow& fence,
                         const DRAWITEMSTRUCT& drawItem) const;
    void DestroyFenceResources(FenceWindow& fence);
    void DestroyAllFences();
    void RemoveFenceForCategory(std::size_t categoryIndex);
    void DrainFenceIconLoads();
    void ClampFenceToWorkArea(FenceWindow& fence) const;
    void PollFences(POINT cursor, ULONGLONG now, bool insideDock);
    void UpdatePollTimerInterval();

    Category* CurrentCategory();
    const Category* CurrentCategory() const;
    int SelectedItemIndex() const;

    void AddCategory();
    void RenameCategory(std::size_t categoryIndex);
    void DeleteCategory(std::size_t categoryIndex);
    void ReorderCategory(std::size_t sourceIndex, std::size_t destinationIndex);
    void AddApplication();
    void AddFolder();
    void AddTarget(const std::wstring& target);
    void AddTargets(const std::vector<std::wstring>& targets);
    void HandleDroppedFiles(HDROP drop);
    void RenameSelectedItem();
    void RemoveSelectedItem();
    void MoveSelectedItem(std::size_t destinationCategory);
    void ReorderItem(FenceWindow& fence, std::size_t sourceIndex,
                     std::size_t destinationIndex);
    void LaunchSelectedItem();
    void LaunchItemAt(int itemIndex);
    void OpenSelectedItemLocation();
    void ShowItemContextMenu(int itemIndex);
    void ShowContentContextMenu();
    void ShowCategoryContextMenu(POINT screenPoint);
    void ShowRailAppearanceSettings();

    std::optional<std::wstring> PickApplication() const;
    std::optional<std::wstring> PickFolder() const;
    bool SaveState();

    static std::wstring DisplayNameForTarget(const std::wstring& target);
    static std::wstring DirectoryForTarget(const std::wstring& target);
    static std::wstring WindowsErrorMessage(DWORD errorCode);
    static bool EqualsIgnoreCase(const std::wstring& left, const std::wstring& right);

    HINSTANCE instance_ = nullptr;
    std::uintptr_t configIdentity_ = 0;
    HWND window_ = nullptr;
    HWND railInputWindow_ = nullptr;
    HWND pinButton_ = nullptr;
    HWND addCategoryButton_ = nullptr;
    HWND categoryList_ = nullptr;
    HFONT bodyFont_ = nullptr;
    HFONT headingFont_ = nullptr;
    HBRUSH railBackgroundBrush_ = nullptr;
    UINT dpi_ = 96;
    UINT taskbarCreatedMessage_ = 0;
    bool trayIconAdded_ = false;
    bool pinned_ = false;
    bool loadedFromBackup_ = false;
    HMONITOR panelMonitor_ = nullptr;
    ULONGLONG pointerOutsideSince_ = 0;
    ULONGLONG keepVisibleUntil_ = 0;
    int interactionDepth_ = 0;
    ULONGLONG lastLaunchTick_ = 0;
    std::wstring lastLaunchTarget_;
    std::wstring lastLaunchArguments_;
    std::wstring lastLaunchWorkingDirectory_;
    std::shared_ptr<IconDispatchContext> iconDispatchContext_;
    std::shared_ptr<PreviewIconLoadBatch> previewIconLoadBatch_;
    std::vector<std::array<HICON, 4>> categoryPreviewIcons_;
    std::vector<std::array<bool, 4>> categoryPreviewRequested_;
    std::optional<std::size_t> hoveredCategory_;
    std::optional<std::size_t> categoryDragSource_;
    std::optional<std::size_t> categoryDropTarget_;
    POINT categoryDragOrigin_{};
    bool categoryDragging_ = false;
    std::vector<std::unique_ptr<FenceWindow>> fences_;
    FenceWindow* activeFence_ = nullptr;

    ConfigStore config_;
    AppState state_;
};

}  // namespace lightlaunch
