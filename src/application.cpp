#include "application.h"

#include "background_dialog.h"
#include "background_image.h"
#include "input_dialog.h"
#include "reorder.h"

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace lightlaunch {

struct LoadedIcon {
    std::size_t itemIndex = 0;
    HICON icon = nullptr;
};

struct IconDispatchContext {
    std::mutex mutex;
    HWND window = nullptr;
    bool accepting = true;
};

struct IconLoadBatch {
    std::atomic_bool cancelled = false;
    std::mutex mutex;
    std::vector<LoadedIcon> ready;
    bool notificationPending = false;

    ~IconLoadBatch() {
        for (const LoadedIcon& loaded : ready) {
            if (loaded.icon != nullptr) {
                DestroyIcon(loaded.icon);
            }
        }
    }
};

struct PreviewIconTarget {
    std::size_t categoryIndex = 0;
    std::size_t slot = 0;
    std::wstring target;
};

struct LoadedPreviewIcon {
    std::size_t categoryIndex = 0;
    std::size_t slot = 0;
    HICON icon = nullptr;
};

struct PreviewIconLoadBatch {
    std::atomic_bool cancelled = false;
    std::mutex mutex;
    std::vector<LoadedPreviewIcon> ready;
    bool notificationPending = false;

    ~PreviewIconLoadBatch() {
        for (const LoadedPreviewIcon& loaded : ready) {
            if (loaded.icon != nullptr) {
                DestroyIcon(loaded.icon);
            }
        }
    }
};

struct FenceWindow {
    Application* owner = nullptr;
    std::size_t categoryIndex = 0;
    HWND window = nullptr;
    HWND pinButton = nullptr;
    HWND closeButton = nullptr;
    HWND itemList = nullptr;
    HIMAGELIST imageList = nullptr;
    HFONT bodyFont = nullptr;
    HFONT headingFont = nullptr;
    UINT dpi = 96;
    bool pinned = false;
    bool movingOrSizing = false;
    int itemDragSource = -1;
    int itemDropTarget = -1;
    POINT itemDragOrigin{};
    bool itemDragging = false;
    ULONGLONG pointerOutsideSince = 0;
    ULONGLONG keepVisibleUntil = 0;
    std::shared_ptr<IconLoadBatch> iconLoadBatch;
    std::unique_ptr<BackgroundImage> backgroundImage;
    std::wstring loadedBackgroundPath;
    FenceBackground renderedBackground;
    SIZE renderedBackgroundSize{};
    bool hasRenderedBackground = false;
};

namespace {

constexpr wchar_t kMainWindowClass[] = L"LightLaunch.MainWindow";
constexpr wchar_t kContentWindowClass[] = L"LightLaunch.ContentWindow";
constexpr wchar_t kRailInputWindowClass[] = L"LightLaunch.RailInputWindow";
constexpr wchar_t kWindowTitle[] = L"LightLaunch - 轻量应用启动器";
constexpr wchar_t kContentWindowTitlePrefix[] = L"LightLaunch - ";
constexpr wchar_t kConfigIdentityProperty[] = L"LightLaunch.ConfigIdentity";
constexpr COLORREF kRailTransparencyColor = RGB(1, 2, 3);

constexpr int kCategoryListId = 1001;
constexpr int kAddCategoryId = 1002;
constexpr int kRenameCategoryId = 1003;
constexpr int kDeleteCategoryId = 1004;
constexpr int kItemListId = 2001;
constexpr int kAddApplicationId = 2002;
constexpr int kAddFolderId = 2003;
constexpr int kRemoveItemId = 2004;
constexpr int kPinButtonId = 3001;
constexpr int kFencePinButtonId = 3002;
constexpr int kCloseContentButtonId = 3003;

constexpr UINT kMenuLaunch = 4001;
constexpr UINT kMenuRename = 4002;
constexpr UINT kMenuRemove = 4003;
constexpr UINT kMenuOpenLocation = 4004;
constexpr UINT kMenuMoveBase = 4100;
constexpr UINT kMenuCategoryNew = 4201;
constexpr UINT kMenuCategoryRename = 4202;
constexpr UINT kMenuCategoryDelete = 4203;
constexpr UINT kMenuContentAddTarget = 4301;
constexpr UINT kMenuContentAddFolder = 4302;
constexpr UINT kMenuFenceBackgroundSettings = 4401;
constexpr UINT kMenuFenceBackgroundClear = 4402;
constexpr UINT kIconLoadedMessage = WM_APP + 1;
constexpr UINT kTrayCallbackMessage = WM_APP + 2;
constexpr UINT kActivateWindowMessage = WM_APP + 3;
constexpr UINT kPreviewIconLoadedMessage = WM_APP + 4;
constexpr UINT kTrayIconId = 1;
constexpr UINT kTrayShow = 5001;
constexpr UINT kTrayExit = 5002;
constexpr UINT_PTR kEdgePollTimerId = 1;
constexpr UINT_PTR kCategoryListSubclassId = 1;
constexpr UINT_PTR kItemListSubclassId = 2;
constexpr UINT_PTR kPinButtonSubclassId = 3;
constexpr UINT_PTR kAddCategoryButtonSubclassId = 4;
constexpr UINT kHiddenEdgePollIntervalMilliseconds = 30;
constexpr UINT kVisibleEdgePollIntervalMilliseconds = 16;
constexpr UINT kPinnedPollIntervalMilliseconds = 250;
constexpr ULONGLONG kAutoHideDelayMilliseconds = 40;
constexpr ULONGLONG kInteractionKeepVisibleMilliseconds = 220;
constexpr ULONGLONG kActivatedKeepVisibleMilliseconds = 650;
constexpr ULONGLONG kFenceAutoHideDelayMilliseconds = 500;
constexpr ULONGLONG kFenceTransitMilliseconds = 900;
constexpr int kCollapsedPanelLogicalWidth = 100;
constexpr int kFenceWindowLogicalWidth = 468;
constexpr int kFenceWindowLogicalHeight = 438;
constexpr int kFenceMinimumLogicalWidth = 280;
constexpr int kFenceMinimumLogicalHeight = 180;
constexpr int kFenceHeaderLogicalHeight = 50;
constexpr int kFenceLogicalRadius = 16;
constexpr int kFenceResizeLogicalBorder = 7;
constexpr int kFenceControlLogicalSize = 32;
constexpr int kFenceContentLogicalPadding = 14;
constexpr int kFenceStatusLogicalHeight = 24;
// Default colour-only fences use a lighter glass treatment. Image-backed
// fences retain the previous opacity so artwork does not become washed out.
constexpr BYTE kFenceGlassOpacity = 176;
constexpr BYTE kFenceImageOpacity = 232;
constexpr COLORREF kDefaultFenceSurfaceColor = RGB(230, 234, 231);
constexpr int kRailPanelLogicalWidth = 84;
constexpr int kRailPanelLogicalMinimumHeight = 144;
constexpr int kRailPanelLogicalMaximumHeight = 620;
constexpr int kRailPanelLogicalMargin = 8;
constexpr int kRailPanelLogicalRadius = 16;
constexpr int kRailHeaderLogicalHeight = 34;
constexpr int kRailFooterLogicalHeight = 42;
constexpr int kRailBottomLogicalPadding = 6;
constexpr int kCategoryColumns = 1;
constexpr int kCategoryCardLogicalHeight = 62;
constexpr int kCategoryPreviewFrameLogicalSize = 38;
constexpr int kCategoryPreviewLogicalSize = 12;
constexpr int kCategoryPreviewLogicalGap = 3;
constexpr BYTE kRailPanelOpacity = 232;
constexpr int kRailPanelTopRed = 244;
constexpr int kRailPanelTopGreen = 246;
constexpr int kRailPanelTopBlue = 244;
constexpr int kRailPanelBottomRed = 216;
constexpr int kRailPanelBottomGreen = 221;
constexpr int kRailPanelBottomBlue = 218;
constexpr COLORREF kRailPanelBorderColor = RGB(250, 251, 249);

bool HasExceededDragThreshold(POINT origin, POINT current) {
    return std::abs(current.x - origin.x) >= GetSystemMetrics(SM_CXDRAG) ||
           std::abs(current.y - origin.y) >= GetSystemMetrics(SM_CYDRAG);
}

COLORREF FenceSurfaceColor(const FenceBackground& settings) {
    return static_cast<COLORREF>(settings.backgroundColor & 0x00FFFFFFU);
}

BYTE FenceOpacity(const FenceBackground& settings) {
    return settings.imagePath.empty() ? kFenceGlassOpacity
                                      : kFenceImageOpacity;
}

bool IsDarkFenceSurface(COLORREF color) {
    const unsigned int luminance =
        299U * GetRValue(color) + 587U * GetGValue(color) +
        114U * GetBValue(color);
    return luminance < 145000U;
}

COLORREF FenceTextColor(COLORREF surface) {
    return IsDarkFenceSurface(surface) ? RGB(237, 240, 247)
                                       : RGB(34, 42, 37);
}

COLORREF FenceMutedTextColor(COLORREF surface) {
    return IsDarkFenceSurface(surface) ? RGB(165, 172, 188)
                                       : RGB(91, 101, 95);
}

COLORREF FenceBorderColor(COLORREF surface) {
    return IsDarkFenceSurface(surface) ? RGB(61, 68, 84)
                                       : RGB(177, 185, 180);
}

COLORREF FenceSeparatorColor(COLORREF surface) {
    return IsDarkFenceSurface(surface) ? RGB(37, 41, 54)
                                       : RGB(190, 198, 193);
}

COLORREF BlendColor(COLORREF from, COLORREF to, unsigned int amountPercent) {
    const unsigned int amount = std::min(amountPercent, 100U);
    const unsigned int inverse = 100U - amount;
    const auto channel = [&](BYTE fromValue, BYTE toValue) {
        return static_cast<BYTE>((fromValue * inverse + toValue * amount +
                                  50U) /
                                 100U);
    };
    return RGB(channel(GetRValue(from), GetRValue(to)),
               channel(GetGValue(from), GetGValue(to)),
               channel(GetBValue(from), GetBValue(to)));
}

bool IsSlowLocation(const std::wstring& target) {
    if (PathIsUNCW(target.c_str())) {
        return true;
    }

    const int driveNumber = PathGetDriveNumberW(target.c_str());
    if (driveNumber < 0 || driveNumber > 25) {
        return false;
    }
    const wchar_t root[] = {static_cast<wchar_t>(L'A' + driveNumber), L':', L'\\', L'\0'};
    const UINT driveType = GetDriveTypeW(root);
    return driveType == DRIVE_REMOTE || driveType == DRIVE_REMOVABLE ||
           driveType == DRIVE_CDROM;
}

void DestroyLoadedIcons(std::vector<LoadedIcon>& icons) {
    for (const LoadedIcon& loaded : icons) {
        if (loaded.icon != nullptr) {
            DestroyIcon(loaded.icon);
        }
    }
    icons.clear();
}

void CancelIconBatch(std::shared_ptr<IconLoadBatch>& batch) {
    if (batch == nullptr) {
        return;
    }

    batch->cancelled.store(true, std::memory_order_relaxed);
    std::vector<LoadedIcon> discarded;
    {
        std::lock_guard lock(batch->mutex);
        discarded.swap(batch->ready);
        batch->notificationPending = false;
    }
    DestroyLoadedIcons(discarded);
    batch.reset();
}

void DestroyLoadedPreviewIcons(std::vector<LoadedPreviewIcon>& icons) {
    for (const LoadedPreviewIcon& loaded : icons) {
        if (loaded.icon != nullptr) {
            DestroyIcon(loaded.icon);
        }
    }
    icons.clear();
}

void CancelPreviewIconBatch(std::shared_ptr<PreviewIconLoadBatch>& batch) {
    if (batch == nullptr) {
        return;
    }

    batch->cancelled.store(true, std::memory_order_relaxed);
    std::vector<LoadedPreviewIcon> discarded;
    {
        std::lock_guard lock(batch->mutex);
        discarded.swap(batch->ready);
        batch->notificationPending = false;
    }
    DestroyLoadedPreviewIcons(discarded);
    batch.reset();
}

void LoadPreviewIconsInBackground(std::vector<PreviewIconTarget> targets,
                                  std::shared_ptr<PreviewIconLoadBatch> batch,
                                  std::shared_ptr<IconDispatchContext> dispatch) {
    std::thread([targets = std::move(targets), batch = std::move(batch),
                 dispatch = std::move(dispatch)]() mutable {
        const HRESULT oleResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        for (const PreviewIconTarget& target : targets) {
            if (batch->cancelled.load(std::memory_order_relaxed)) {
                break;
            }
            if (IsSlowLocation(target.target)) {
                continue;
            }

            SHFILEINFOW fileInfo{};
            if (SHGetFileInfoW(target.target.c_str(), 0, &fileInfo, sizeof(fileInfo),
                               SHGFI_ICON | SHGFI_LARGEICON) == 0 ||
                fileInfo.hIcon == nullptr) {
                continue;
            }
            if (batch->cancelled.load(std::memory_order_relaxed)) {
                DestroyIcon(fileInfo.hIcon);
                break;
            }

            bool shouldNotify = false;
            bool accepted = false;
            {
                std::lock_guard lock(batch->mutex);
                if (!batch->cancelled.load(std::memory_order_relaxed)) {
                    batch->ready.push_back(LoadedPreviewIcon{
                        target.categoryIndex, target.slot, fileInfo.hIcon});
                    accepted = true;
                    if (!batch->notificationPending) {
                        batch->notificationPending = true;
                        shouldNotify = true;
                    }
                }
            }
            if (!accepted) {
                DestroyIcon(fileInfo.hIcon);
                break;
            }

            if (shouldNotify) {
                bool posted = false;
                {
                    std::lock_guard lock(dispatch->mutex);
                    if (dispatch->accepting && dispatch->window != nullptr) {
                        posted = PostMessageW(dispatch->window, kPreviewIconLoadedMessage,
                                              0, 0) != FALSE;
                    }
                }
                if (!posted) {
                    std::vector<LoadedPreviewIcon> discarded;
                    {
                        std::lock_guard lock(batch->mutex);
                        discarded.swap(batch->ready);
                        batch->notificationPending = false;
                    }
                    DestroyLoadedPreviewIcons(discarded);
                }
            }
        }
        if (SUCCEEDED(oleResult)) {
            CoUninitialize();
        }
    }).detach();
}

void LoadIconsInBackground(std::vector<std::wstring> targets,
                           std::shared_ptr<IconLoadBatch> batch,
                           std::shared_ptr<IconDispatchContext> dispatch) {
    std::thread([targets = std::move(targets), batch = std::move(batch),
                 dispatch = std::move(dispatch)]() mutable {
        const HRESULT oleResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        for (std::size_t index = 0; index < targets.size(); ++index) {
            if (batch->cancelled.load(std::memory_order_relaxed)) {
                break;
            }
            if (IsSlowLocation(targets[index])) {
                continue;
            }

            SHFILEINFOW fileInfo{};
            if (SHGetFileInfoW(targets[index].c_str(), 0, &fileInfo, sizeof(fileInfo),
                               SHGFI_ICON | SHGFI_LARGEICON) == 0 ||
                fileInfo.hIcon == nullptr) {
                continue;
            }
            if (batch->cancelled.load(std::memory_order_relaxed)) {
                DestroyIcon(fileInfo.hIcon);
                break;
            }

            bool shouldNotify = false;
            bool accepted = false;
            {
                std::lock_guard lock(batch->mutex);
                if (!batch->cancelled.load(std::memory_order_relaxed)) {
                    batch->ready.push_back(LoadedIcon{index, fileInfo.hIcon});
                    accepted = true;
                    if (!batch->notificationPending) {
                        batch->notificationPending = true;
                        shouldNotify = true;
                    }
                }
            }
            if (!accepted) {
                DestroyIcon(fileInfo.hIcon);
                break;
            }

            if (shouldNotify) {
                bool posted = false;
                {
                    std::lock_guard lock(dispatch->mutex);
                    if (dispatch->accepting && dispatch->window != nullptr) {
                        posted = PostMessageW(dispatch->window, kIconLoadedMessage, 0, 0) != FALSE;
                    }
                }
                if (!posted) {
                    std::vector<LoadedIcon> discarded;
                    {
                        std::lock_guard lock(batch->mutex);
                        discarded.swap(batch->ready);
                        batch->notificationPending = false;
                    }
                    DestroyLoadedIcons(discarded);
                }
            }
        }
        if (SUCCEEDED(oleResult)) {
            CoUninitialize();
        }
    }).detach();
}

int ScaleForDpi(int value, UINT dpi) {
    return MulDiv(value, static_cast<int>(dpi), 96);
}

void SetControlFont(HWND control, HFONT font) {
    if (control != nullptr && font != nullptr) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
}

std::wstring FileNameWithoutExtension(const std::wstring& path) {
    std::wstring normalized = path;
    while (normalized.size() > 3 &&
           (normalized.back() == L'\\' || normalized.back() == L'/')) {
        normalized.pop_back();
    }

    const wchar_t* fileName = PathFindFileNameW(normalized.c_str());
    std::vector<wchar_t> buffer(fileName, fileName + std::wcslen(fileName) + 1);
    if (GetFileAttributesW(normalized.c_str()) != INVALID_FILE_ATTRIBUTES &&
        (GetFileAttributesW(normalized.c_str()) & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        PathRemoveExtensionW(buffer.data());
    }
    return buffer.data();
}

class DropHandleGuard {
public:
    explicit DropHandleGuard(HDROP drop) : drop_(drop) {}
    ~DropHandleGuard() {
        if (drop_ != nullptr) {
            DragFinish(drop_);
        }
    }

    DropHandleGuard(const DropHandleGuard&) = delete;
    DropHandleGuard& operator=(const DropHandleGuard&) = delete;

private:
    HDROP drop_ = nullptr;
};

}  // namespace

Application::Application(HINSTANCE instance, std::wstring configPath,
                         std::uintptr_t configIdentity)
    : instance_(instance),
      configIdentity_(configIdentity),
      taskbarCreatedMessage_(RegisterWindowMessageW(L"TaskbarCreated")),
      iconDispatchContext_(std::make_shared<IconDispatchContext>()),
      config_(configPath.empty() ? ConfigStore::DefaultPath() : std::move(configPath)) {}

Application::~Application() = default;

int Application::Run(int showCommand) {
    if (!config_.Load(state_)) {
        state_.categories.push_back(Category{L"常用", {}});
        state_.selectedCategory = 0;
        SaveState(false);
    }

    if (!RegisterWindowClass()) {
        MessageBoxW(nullptr, L"无法注册主窗口。", L"LightLaunch", MB_OK | MB_ICONERROR);
        return 1;
    }

    dpi_ = GetDpiForSystem();
    window_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED, kMainWindowClass, kWindowTitle,
        WS_POPUP | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, ScaleForDpi(kCollapsedPanelLogicalWidth, dpi_),
        ScaleForDpi(720, dpi_),
        nullptr, nullptr, instance_, this);
    if (window_ == nullptr) {
        MessageBoxW(nullptr, L"无法创建主窗口。", L"LightLaunch", MB_OK | MB_ICONERROR);
        return 1;
    }

    if (!AddTrayIcon()) {
        MessageBoxW(window_, L"无法创建系统托盘图标。关闭窗口时程序将直接退出。",
                    L"LightLaunch", MB_OK | MB_ICONWARNING);
    }

    static_cast<void>(showCommand);
    ShowWindow(window_, SW_HIDE);
    UpdateWindow(window_);

    MSG message{};
    int messageResult = 0;
    while ((messageResult = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (messageResult == -1) {
        return 1;
    }
    return static_cast<int>(message.wParam);
}

bool Application::RegisterWindowClass() const {
    const auto registerClass = [&](const wchar_t* className, WNDPROC procedure,
                                   HBRUSH background, HICON icon) {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        windowClass.lpfnWndProc = procedure;
        windowClass.hInstance = instance_;
        windowClass.hIcon = icon;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = background;
        windowClass.lpszClassName = className;
        windowClass.hIconSm = icon;

        if (RegisterClassExW(&windowClass) != 0) {
            return true;
        }
        return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    };

    const HICON applicationIcon = LoadIconW(nullptr, IDI_APPLICATION);
    return registerClass(kMainWindowClass, WindowProc, nullptr, applicationIcon) &&
           registerClass(kContentWindowClass, ContentWindowProc, nullptr,
                         applicationIcon) &&
           registerClass(kRailInputWindowClass, RailInputWindowProc, nullptr, nullptr);
}

LRESULT CALLBACK Application::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    Application* app = reinterpret_cast<Application*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        app = static_cast<Application*>(create->lpCreateParams);
        app->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        if (app->configIdentity_ != 0) {
            SetPropW(window, kConfigIdentityProperty,
                     reinterpret_cast<HANDLE>(app->configIdentity_));
        }
        if (app->iconDispatchContext_ != nullptr) {
            std::lock_guard lock(app->iconDispatchContext_->mutex);
            app->iconDispatchContext_->window = window;
            app->iconDispatchContext_->accepting = true;
        }
    } else if (message == WM_NCDESTROY && app != nullptr &&
               app->iconDispatchContext_ != nullptr) {
        RemovePropW(window, kConfigIdentityProperty);
        std::lock_guard lock(app->iconDispatchContext_->mutex);
        app->iconDispatchContext_->accepting = false;
        app->iconDispatchContext_->window = nullptr;
    }

    if (app != nullptr) {
        return app->HandleMessage(message, wParam, lParam);
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK Application::ContentWindowProc(HWND window, UINT message, WPARAM wParam,
                                                LPARAM lParam) {
    FenceWindow* fence = reinterpret_cast<FenceWindow*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        fence = static_cast<FenceWindow*>(create->lpCreateParams);
        if (fence != nullptr) {
            fence->window = window;
            SetWindowLongPtrW(window, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(fence));
        }
    }

    if (fence == nullptr || fence->owner == nullptr) {
        return DefWindowProcW(window, message, wParam, lParam);
    }

    const LRESULT result = fence->owner->HandleFenceMessage(
        *fence, window, message, wParam, lParam);
    if (message == WM_NCDESTROY && fence->window == window) {
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        fence->window = nullptr;
    }
    return result;
}

LRESULT CALLBACK Application::RailInputWindowProc(HWND window, UINT message, WPARAM wParam,
                                                  LPARAM lParam) {
    Application* app = reinterpret_cast<Application*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        app = static_cast<Application*>(create->lpCreateParams);
        app->railInputWindow_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }

    if (app == nullptr) {
        return DefWindowProcW(window, message, wParam, lParam);
    }

    const LRESULT result = app->HandleRailInputMessage(message, wParam, lParam);
    if (message == WM_NCDESTROY && app->railInputWindow_ == window) {
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        app->railInputWindow_ = nullptr;
    }
    return result;
}

LRESULT CALLBACK Application::CategoryListSubclassProc(HWND window, UINT message, WPARAM wParam,
                                                       LPARAM lParam, UINT_PTR,
                                                       DWORD_PTR referenceData) {
    auto* app = reinterpret_cast<Application*>(referenceData);
    if (app != nullptr && (app->interactionDepth_ > 0 || app->IsInteractionBlocked()) &&
        (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP ||
         message == WM_LBUTTONDBLCLK || message == WM_RBUTTONDOWN ||
         message == WM_RBUTTONUP || message == WM_CONTEXTMENU ||
         message == WM_MOUSEWHEEL || message == WM_KEYDOWN)) {
        return 0;
    }
    if (app != nullptr && message == WM_LBUTTONDOWN) {
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const std::optional<std::size_t> categoryIndex =
            app->CategoryIndexFromListPoint(point);
        if (categoryIndex.has_value()) {
            SetFocus(window);
            app->categoryDragSource_ = categoryIndex;
            app->categoryDropTarget_ = categoryIndex;
            app->categoryDragOrigin_ = point;
            app->categoryDragging_ = false;
            SetCapture(window);
        } else {
            app->categoryDragSource_.reset();
            app->categoryDropTarget_.reset();
            app->categoryDragging_ = false;
            SendMessageW(window, LB_SETCURSEL, static_cast<WPARAM>(-1), 0);
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    }

    if (app != nullptr && message == WM_LBUTTONDBLCLK) {
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const std::optional<std::size_t> categoryIndex =
            app->CategoryIndexFromListPoint(point);
        if (categoryIndex.has_value()) {
            app->ActivateCategory(*categoryIndex);
        }
        return 0;
    }

    if (app != nullptr && message == WM_MOUSEMOVE &&
        app->categoryDragSource_.has_value() && (wParam & MK_LBUTTON) != 0) {
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (!app->categoryDragging_ &&
            HasExceededDragThreshold(app->categoryDragOrigin_, point)) {
            app->categoryDragging_ = true;
        }
        if (app->categoryDragging_) {
            const std::optional<std::size_t> target =
                app->CategoryIndexFromListPoint(point);
            if (target != app->categoryDropTarget_) {
                app->categoryDropTarget_ = target;
                InvalidateRect(window, nullptr, FALSE);
            }
            SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
        }
        return 0;
    }

    if (app != nullptr && message == WM_LBUTTONUP) {
        const std::optional<std::size_t> source = app->categoryDragSource_;
        const std::optional<std::size_t> destination = app->categoryDropTarget_;
        const bool wasDragging = app->categoryDragging_;
        if (GetCapture() == window) {
            ReleaseCapture();
        }
        app->categoryDragSource_.reset();
        app->categoryDropTarget_.reset();
        app->categoryDragging_ = false;
        InvalidateRect(window, nullptr, FALSE);

        if (source.has_value()) {
            if (wasDragging && destination.has_value() &&
                *source != *destination) {
                app->ReorderCategory(*source, *destination);
            } else if (!wasDragging) {
                app->ActivateCategory(*source);
            }
        }
        return 0;
    }

    if (app != nullptr &&
        (message == WM_CANCELMODE || message == WM_CAPTURECHANGED)) {
        app->categoryDragSource_.reset();
        app->categoryDropTarget_.reset();
        app->categoryDragging_ = false;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    if (app != nullptr && message == WM_CONTEXTMENU) {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        app->ShowCategoryContextMenu(point);
        return 0;
    }
    if (app != nullptr && message == WM_KEYDOWN && !app->state_.categories.empty()) {
        std::size_t categoryIndex = std::min(app->state_.selectedCategory,
                                             app->state_.categories.size() - 1);
        bool handled = true;
        switch (wParam) {
            case VK_LEFT:
                if (categoryIndex > 0) {
                    --categoryIndex;
                }
                break;
            case VK_RIGHT:
                categoryIndex = std::min(categoryIndex + 1,
                                         app->state_.categories.size() - 1);
                break;
            case VK_UP:
                if (categoryIndex >= static_cast<std::size_t>(kCategoryColumns)) {
                    categoryIndex -= kCategoryColumns;
                }
                break;
            case VK_DOWN:
                categoryIndex = std::min(
                    categoryIndex + static_cast<std::size_t>(kCategoryColumns),
                    app->state_.categories.size() - 1);
                break;
            case VK_HOME:
                categoryIndex = 0;
                break;
            case VK_END:
                categoryIndex = app->state_.categories.size() - 1;
                break;
            case VK_RETURN:
            case VK_SPACE:
                break;
            default:
                handled = false;
                break;
        }
        if (handled) {
            app->ActivateCategory(categoryIndex);
            app->QueueVisibleCategoryPreviewLoad();
            return 0;
        }
    }
    if (app != nullptr &&
        (message == WM_VSCROLL || message == WM_MOUSEWHEEL || message == WM_KEYDOWN)) {
        const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
        app->QueueVisibleCategoryPreviewLoad();
        return result;
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

LRESULT CALLBACK Application::PinButtonSubclassProc(HWND window, UINT message, WPARAM wParam,
                                                    LPARAM lParam, UINT_PTR,
                                                    DWORD_PTR referenceData) {
    auto* app = reinterpret_cast<Application*>(referenceData);
    if (app == nullptr) {
        return DefSubclassProc(window, message, wParam, lParam);
    }

    switch (message) {
        case WM_ERASEBKGND:
            return TRUE;

        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC deviceContext = BeginPaint(window, &paint);
            RECT client{};
            GetClientRect(window, &client);
            app->DrawPinButton(deviceContext, client);
            EndPaint(window, &paint);
            return 0;
        }

        case BM_SETCHECK: {
            const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
            InvalidateRect(window, nullptr, FALSE);
            return result;
        }

        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP: {
            const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
            InvalidateRect(window, nullptr, FALSE);
            return result;
        }

        default:
            break;
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

LRESULT CALLBACK Application::AddCategoryButtonSubclassProc(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR,
    DWORD_PTR referenceData) {
    auto* app = reinterpret_cast<Application*>(referenceData);
    if (app == nullptr) {
        return DefSubclassProc(window, message, wParam, lParam);
    }

    switch (message) {
        case WM_ERASEBKGND:
            return TRUE;

        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC deviceContext = BeginPaint(window, &paint);
            RECT client{};
            GetClientRect(window, &client);
            app->DrawAddCategoryButton(deviceContext, client);
            EndPaint(window, &paint);
            return 0;
        }

        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP: {
            const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
            InvalidateRect(window, nullptr, FALSE);
            return result;
        }

        default:
            break;
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

LRESULT CALLBACK Application::ItemListSubclassProc(HWND window, UINT message, WPARAM wParam,
                                                   LPARAM lParam, UINT_PTR,
                                                   DWORD_PTR referenceData) {
    auto* fence = reinterpret_cast<FenceWindow*>(referenceData);
    Application* app = fence != nullptr ? fence->owner : nullptr;
    const auto clearDragState = [window, fence]() {
        if (fence == nullptr) {
            return;
        }
        if (fence->itemDropTarget >= 0) {
            ListView_SetItemState(window, fence->itemDropTarget, 0,
                                  LVIS_DROPHILITED);
        }
        fence->itemDragSource = -1;
        fence->itemDropTarget = -1;
        fence->itemDragging = false;
    };
    if (app != nullptr && (app->interactionDepth_ > 0 || app->IsInteractionBlocked()) &&
        (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP ||
         message == WM_LBUTTONDBLCLK || message == WM_RBUTTONDOWN ||
         message == WM_RBUTTONUP || message == WM_CONTEXTMENU ||
         message == WM_MOUSEWHEEL || message == WM_KEYDOWN ||
         message == WM_DROPFILES)) {
        if (message == WM_DROPFILES) {
            DragFinish(reinterpret_cast<HDROP>(wParam));
        }
        return 0;
    }
    if (app != nullptr && fence != nullptr && message == WM_LBUTTONDOWN) {
        LVHITTESTINFO hit{};
        hit.pt = POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const int itemIndex = ListView_HitTest(window, &hit);
        const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
        clearDragState();
        if (itemIndex >= 0) {
            fence->itemDragSource = itemIndex;
            fence->itemDropTarget = itemIndex;
            fence->itemDragOrigin = hit.pt;
            SetCapture(window);
        }
        return result;
    }
    if (app != nullptr && fence != nullptr && message == WM_MOUSEMOVE &&
        fence->itemDragSource >= 0 && (wParam & MK_LBUTTON) != 0) {
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (!fence->itemDragging &&
            HasExceededDragThreshold(fence->itemDragOrigin, point)) {
            fence->itemDragging = true;
        }
        if (!fence->itemDragging) {
            return DefSubclassProc(window, message, wParam, lParam);
        }

        LVHITTESTINFO hit{};
        hit.pt = point;
        int target = ListView_HitTest(window, &hit);
        RECT client{};
        GetClientRect(window, &client);
        if (target < 0 && PtInRect(&client, point) != FALSE &&
            fence->categoryIndex < app->state_.categories.size() &&
            !app->state_.categories[fence->categoryIndex].items.empty()) {
            target = static_cast<int>(
                app->state_.categories[fence->categoryIndex].items.size() - 1);
        }
        if (target != fence->itemDropTarget) {
            if (fence->itemDropTarget >= 0) {
                ListView_SetItemState(window, fence->itemDropTarget, 0,
                                      LVIS_DROPHILITED);
            }
            fence->itemDropTarget = target;
            if (target >= 0) {
                ListView_SetItemState(window, target, LVIS_DROPHILITED,
                                      LVIS_DROPHILITED);
            }
        }
        SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
        return 0;
    }
    if (app != nullptr && fence != nullptr && message == WM_LBUTTONUP) {
        const int source = fence->itemDragSource;
        const int destination = fence->itemDropTarget;
        const bool wasDragging = fence->itemDragging;
        const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
        if (GetCapture() == window) {
            ReleaseCapture();
        }
        clearDragState();
        if (wasDragging && source >= 0 && destination >= 0 &&
            source != destination) {
            app->ReorderItem(*fence, static_cast<std::size_t>(source),
                             static_cast<std::size_t>(destination));
        }
        return result;
    }
    if (fence != nullptr &&
        (message == WM_CANCELMODE || message == WM_CAPTURECHANGED)) {
        clearDragState();
    }
    if (app != nullptr && message == WM_DROPFILES) {
        FenceWindow* previousFence = app->activeFence_;
        app->activeFence_ = fence;
        app->HandleDroppedFiles(reinterpret_cast<HDROP>(wParam));
        app->activeFence_ = previousFence;
        return 0;
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

LRESULT Application::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    if (taskbarCreatedMessage_ != 0 && message == taskbarCreatedMessage_) {
        trayIconAdded_ = false;
        if (!AddTrayIcon()) {
            ShowFromTray();
        }
        return 0;
    }

    switch (message) {
        case WM_CREATE:
            return CreateControls() ? 0 : -1;

        case WM_ERASEBKGND: {
            RECT client{};
            GetClientRect(window_, &client);
            FillRect(reinterpret_cast<HDC>(wParam), &client,
                     railBackgroundBrush_ != nullptr
                         ? railBackgroundBrush_
                         : static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            return TRUE;
        }

        case WM_CTLCOLORLISTBOX:
            if (reinterpret_cast<HWND>(lParam) == categoryList_) {
                HDC deviceContext = reinterpret_cast<HDC>(wParam);
                SetBkMode(deviceContext, OPAQUE);
                SetBkColor(deviceContext, kRailTransparencyColor);
                return reinterpret_cast<LRESULT>(railBackgroundBrush_);
            }
            break;

        case WM_TIMER:
            if (wParam == kEdgePollTimerId) {
                PollScreenEdge();
                return 0;
            }
            break;

        case WM_ACTIVATE:
            if (LOWORD(wParam) != WA_INACTIVE) {
                PositionRailInputWindow();
            }
            break;

        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED && trayIconAdded_) {
                HideToTray();
                return 0;
            }
            LayoutControls(LOWORD(lParam), HIWORD(lParam));
            return 0;

        case WM_DPICHANGED: {
            dpi_ = HIWORD(wParam);
            const auto* suggested = reinterpret_cast<RECT*>(lParam);
            RecreateFonts();
            ApplyFonts();
            if (panelMonitor_ != nullptr) {
                PositionEdgePanel(panelMonitor_);
            } else {
                SetWindowPos(window_, nullptr, suggested->left, suggested->top,
                             suggested->right - suggested->left,
                             suggested->bottom - suggested->top,
                             SWP_NOACTIVATE | SWP_NOZORDER);
            }
            return 0;
        }

        case WM_DISPLAYCHANGE:
            if (IsInteractionBlocked()) {
                return 0;
            }
            HideToTray();
            for (const std::unique_ptr<FenceWindow>& fence : fences_) {
                if (fence != nullptr) {
                    ClampFenceToWorkArea(*fence);
                }
            }
            return 0;

        case WM_SETTINGCHANGE:
            if (wParam == SPI_SETWORKAREA) {
                if (IsInteractionBlocked()) {
                    return 0;
                }
                HideToTray();
                for (const std::unique_ptr<FenceWindow>& fence : fences_) {
                    if (fence != nullptr) {
                        ClampFenceToWorkArea(*fence);
                    }
                }
                return 0;
            }
            break;

        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize.x = ScaleForDpi(kCollapsedPanelLogicalWidth, dpi_);
            info->ptMinTrackSize.y = ScaleForDpi(kRailPanelLogicalMinimumHeight, dpi_);
            return 0;
        }

        case WM_COMMAND:
            if (IsInteractionBlocked()) {
                if (LOWORD(wParam) == kPinButtonId && pinButton_ != nullptr) {
                    SendMessageW(pinButton_, BM_SETCHECK,
                                 pinned_ ? BST_CHECKED : BST_UNCHECKED, 0);
                }
                return 0;
            }
            switch (LOWORD(wParam)) {
                case kCategoryListId:
                    if (HIWORD(wParam) == LBN_SELCHANGE) {
                        const LRESULT selected = SendMessageW(categoryList_, LB_GETCURSEL, 0, 0);
                        if (selected != LB_ERR) {
                            const std::size_t firstInRow =
                                static_cast<std::size_t>(selected) * kCategoryColumns;
                            const std::size_t rowEnd = std::min(
                                firstInRow + static_cast<std::size_t>(kCategoryColumns),
                                state_.categories.size());
                            const std::size_t categoryIndex =
                                state_.selectedCategory >= firstInRow &&
                                        state_.selectedCategory < rowEnd
                                    ? state_.selectedCategory
                                    : firstInRow;
                            if (categoryIndex < state_.categories.size()) {
                                ActivateCategory(categoryIndex);
                            }
                        }
                    }
                    return 0;
                case kAddCategoryId:
                    AddCategory();
                    return 0;
                case kRenameCategoryId:
                    RenameCategory(state_.selectedCategory);
                    return 0;
                case kDeleteCategoryId:
                    DeleteCategory(state_.selectedCategory);
                    return 0;
                case kPinButtonId:
                    SetPinned(SendMessageW(pinButton_, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    return 0;
                case kTrayShow:
                    ShowFromTray();
                    return 0;
                case kTrayExit:
                    ExitApplication();
                    return 0;
                default:
                    break;
            }
            break;

        case WM_DRAWITEM: {
            const auto* drawItem = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (drawItem != nullptr && drawItem->CtlID == kCategoryListId) {
                DrawCategoryItem(*drawItem);
                return TRUE;
            }
            break;
        }

        case kTrayCallbackMessage:
            if (IsInteractionBlocked()) {
                switch (LOWORD(lParam)) {
                    case NIN_SELECT:
                    case NIN_KEYSELECT:
                    case WM_LBUTTONUP:
                    case WM_LBUTTONDBLCLK:
                        ShowFromTray();
                        break;
                    default:
                        break;
                }
                return 0;
            }
            switch (LOWORD(lParam)) {
                case NIN_SELECT:
                case NIN_KEYSELECT:
                case WM_LBUTTONUP:
                case WM_LBUTTONDBLCLK:
                    ShowFromTray();
                    break;
                case WM_CONTEXTMENU:
                case WM_RBUTTONUP:
                    ShowTrayMenu();
                    break;
                default:
                    break;
            }
            return 0;

        case kActivateWindowMessage:
            ShowFromTray();
            return 0;

        case WM_ENTERMENULOOP:
            ++interactionDepth_;
            return 0;

        case WM_EXITMENULOOP:
            interactionDepth_ = std::max(0, interactionDepth_ - 1);
            keepVisibleUntil_ = GetTickCount64() + kInteractionKeepVisibleMilliseconds;
            pointerOutsideSince_ = 0;
            return 0;

        case kIconLoadedMessage: {
            DrainFenceIconLoads();
            return 0;
        }

        case kPreviewIconLoadedMessage: {
            std::vector<LoadedPreviewIcon> ready;
            const std::shared_ptr<PreviewIconLoadBatch> batch = previewIconLoadBatch_;
            if (batch != nullptr) {
                std::lock_guard lock(batch->mutex);
                ready.swap(batch->ready);
                batch->notificationPending = false;
            }
            for (const LoadedPreviewIcon& loaded : ready) {
                if (loaded.icon != nullptr &&
                    loaded.categoryIndex < categoryPreviewIcons_.size() &&
                    loaded.slot < categoryPreviewIcons_[loaded.categoryIndex].size()) {
                    HICON& cached = categoryPreviewIcons_[loaded.categoryIndex][loaded.slot];
                    if (cached != nullptr) {
                        DestroyIcon(cached);
                    }
                    cached = loaded.icon;
                } else if (loaded.icon != nullptr) {
                    DestroyIcon(loaded.icon);
                }
            }
            if (categoryList_ != nullptr) {
                InvalidateRect(categoryList_, nullptr, FALSE);
            }
            return 0;
        }

        case WM_CLOSE:
            if (IsInteractionBlocked()) {
                ShowFromTray();
                return 0;
            }
            if (trayIconAdded_) {
                SaveState(false);
                HideToTray();
            } else {
                ExitApplication();
            }
            return 0;

        case WM_QUERYENDSESSION:
            return TRUE;

        case WM_ENDSESSION:
            if (wParam != FALSE) {
                SaveState(false);
                DestroyWindow(window_);
            }
            return 0;

        case WM_DESTROY:
            RemoveTrayIcon();
            DestroyResources();
            if (railInputWindow_ != nullptr && IsWindow(railInputWindow_)) {
                DestroyWindow(railInputWindow_);
            }
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }

    return DefWindowProcW(window_, message, wParam, lParam);
}

LRESULT Application::HandleFenceMessage(FenceWindow& fence, HWND fenceWindow,
                                        UINT message, WPARAM wParam, LPARAM lParam) {
    FenceWindow* previousFence = activeFence_;
    activeFence_ = &fence;
    struct ActiveFenceRestore {
        FenceWindow*& slot;
        FenceWindow* previous;
        ~ActiveFenceRestore() { slot = previous; }
    } restore{activeFence_, previousFence};

    switch (message) {
        case WM_ACTIVATE:
            if (LOWORD(wParam) != WA_INACTIVE) {
                state_.selectedCategory = fence.categoryIndex;
                fence.pointerOutsideSince = 0;
                fence.keepVisibleUntil = GetTickCount64() +
                                         kInteractionKeepVisibleMilliseconds;
                if (categoryList_ != nullptr) {
                    InvalidateRect(categoryList_, nullptr, FALSE);
                }
            }
            break;

        case WM_NCCALCSIZE:
            return 0;

        case WM_NCHITTEST: {
            RECT bounds{};
            if (!GetWindowRect(fenceWindow, &bounds)) {
                break;
            }
            const int x = GET_X_LPARAM(lParam);
            const int y = GET_Y_LPARAM(lParam);
            const int border = ScaleForDpi(kFenceResizeLogicalBorder, fence.dpi);
            const bool left = x < bounds.left + border;
            const bool right = x >= bounds.right - border;
            const bool top = y < bounds.top + border;
            const bool bottom = y >= bounds.bottom - border;
            if (top && left) return HTTOPLEFT;
            if (top && right) return HTTOPRIGHT;
            if (bottom && left) return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (left) return HTLEFT;
            if (right) return HTRIGHT;
            if (top) return HTTOP;
            if (bottom) return HTBOTTOM;
            RECT pinBounds{};
            RECT closeBounds{};
            const POINT screenPoint{x, y};
            const bool overPin = fence.pinButton != nullptr &&
                                 GetWindowRect(fence.pinButton, &pinBounds) != FALSE &&
                                 PtInRect(&pinBounds, screenPoint) != FALSE;
            const bool overClose = fence.closeButton != nullptr &&
                                   GetWindowRect(fence.closeButton, &closeBounds) != FALSE &&
                                   PtInRect(&closeBounds, screenPoint) != FALSE;
            if (overPin || overClose) {
                return HTCLIENT;
            }
            if (y < bounds.top + ScaleForDpi(kFenceHeaderLogicalHeight, fence.dpi)) {
                return HTCAPTION;
            }
            return HTCLIENT;
        }

        case WM_NCLBUTTONDBLCLK:
            if (wParam == HTCAPTION) {
                return 0;
            }
            break;

        case WM_CONTEXTMENU:
            if (IsInteractionBlocked()) {
                return 0;
            }
            if (reinterpret_cast<HWND>(wParam) == fenceWindow) {
                ShowContentContextMenu();
                return 0;
            }
            break;

        case WM_ERASEBKGND:
            return TRUE;

        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC deviceContext = BeginPaint(fenceWindow, &paint);
            RECT client{};
            GetClientRect(fenceWindow, &client);
            DrawFence(fence, deviceContext, client);
            EndPaint(fenceWindow, &paint);
            return 0;
        }

        case WM_SIZE:
            LayoutFenceControls(fence, LOWORD(lParam), HIWORD(lParam));
            ApplyFenceRoundedRegion(fence);
            return 0;

        case WM_DPICHANGED: {
            fence.dpi = HIWORD(wParam);
            RecreateFenceFonts(fence);
            const auto* suggested = reinterpret_cast<RECT*>(lParam);
            if (suggested != nullptr) {
                SetWindowPos(fenceWindow, nullptr, suggested->left, suggested->top,
                             suggested->right - suggested->left,
                             suggested->bottom - suggested->top,
                             SWP_NOACTIVATE | SWP_NOZORDER);
            }
            RefreshFence(fence);
            return 0;
        }

        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize.x = ScaleForDpi(kFenceMinimumLogicalWidth, fence.dpi);
            info->ptMinTrackSize.y = ScaleForDpi(kFenceMinimumLogicalHeight, fence.dpi);
            return 0;
        }

        case WM_ENTERSIZEMOVE:
            fence.movingOrSizing = true;
            fence.pointerOutsideSince = 0;
            return 0;

        case WM_MOVING:
        case WM_SIZING:
            fence.pointerOutsideSince = 0;
            return TRUE;

        case WM_EXITSIZEMOVE:
            fence.movingOrSizing = false;
            fence.pointerOutsideSince = 0;
            fence.keepVisibleUntil = GetTickCount64() + kFenceTransitMilliseconds;
            ClampFenceToWorkArea(fence);
            UpdateFenceListBackground(fence);
            UpdatePollTimerInterval();
            return 0;

        case WM_COMMAND:
            if (IsInteractionBlocked()) {
                return 0;
            }
            switch (LOWORD(wParam)) {
                case kCloseContentButtonId:
                    HideFence(fence);
                    return 0;
                case kFencePinButtonId:
                    SetFencePinned(fence, !fence.pinned);
                    return 0;
                case kAddApplicationId:
                    AddApplication();
                    return 0;
                case kAddFolderId:
                    AddFolder();
                    return 0;
                case kRemoveItemId:
                    RemoveSelectedItem();
                    return 0;
                default:
                    break;
            }
            break;

        case WM_NOTIFY: {
            if (IsInteractionBlocked()) {
                return 0;
            }
            const auto* header = reinterpret_cast<NMHDR*>(lParam);
            if (header == nullptr || header->idFrom != kItemListId ||
                header->hwndFrom != fence.itemList) {
                break;
            }

            if (header->code == NM_DBLCLK) {
                const auto* activation = reinterpret_cast<NMITEMACTIVATE*>(lParam);
                if (activation->iItem >= 0) {
                    LaunchItemAt(activation->iItem);
                }
                return 0;
            }
            if (header->code == NM_RCLICK) {
                const auto* activation = reinterpret_cast<NMITEMACTIVATE*>(lParam);
                if (activation->iItem >= 0) {
                    ListView_SetItemState(fence.itemList, activation->iItem,
                                          LVIS_SELECTED | LVIS_FOCUSED,
                                          LVIS_SELECTED | LVIS_FOCUSED);
                    ShowItemContextMenu(activation->iItem);
                } else {
                    ShowContentContextMenu();
                }
                return 0;
            }
            if (header->code == LVN_KEYDOWN) {
                const auto* key = reinterpret_cast<NMLVKEYDOWN*>(lParam);
                if (key->wVKey == VK_RETURN) {
                    LaunchSelectedItem();
                } else if (key->wVKey == VK_DELETE) {
                    RemoveSelectedItem();
                }
                return 0;
            }
            if (header->code == LVN_ITEMCHANGED) {
                return 0;
            }
            break;
        }

        case WM_DRAWITEM: {
            const auto* drawItem = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (drawItem != nullptr &&
                (drawItem->CtlID == kFencePinButtonId ||
                 drawItem->CtlID == kCloseContentButtonId)) {
                DrawFenceButton(fence, *drawItem);
                return TRUE;
            }
            break;
        }

        case WM_ENTERMENULOOP:
            ++interactionDepth_;
            fence.pointerOutsideSince = 0;
            return 0;

        case WM_EXITMENULOOP:
            interactionDepth_ = std::max(0, interactionDepth_ - 1);
            keepVisibleUntil_ = GetTickCount64() + kInteractionKeepVisibleMilliseconds;
            pointerOutsideSince_ = 0;
            fence.keepVisibleUntil = GetTickCount64() +
                                     kInteractionKeepVisibleMilliseconds;
            fence.pointerOutsideSince = 0;
            return 0;

        case WM_CLOSE:
            if (IsInteractionBlocked()) {
                return 0;
            }
            HideFence(fence);
            return 0;

        default:
            break;
    }

    return DefWindowProcW(fenceWindow, message, wParam, lParam);
}

LRESULT Application::HandleRailInputMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    const bool blockPointerInput = interactionDepth_ > 0 || IsInteractionBlocked();

    switch (message) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;

        case WM_ERASEBKGND: {
            RECT client{};
            GetClientRect(railInputWindow_, &client);
            FillRect(reinterpret_cast<HDC>(wParam), &client,
                     railBackgroundBrush_ != nullptr
                         ? railBackgroundBrush_
                         : static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            return TRUE;
        }

        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC deviceContext = BeginPaint(railInputWindow_, &paint);
            RECT client{};
            GetClientRect(railInputWindow_, &client);
            DrawRailPanel(deviceContext, client);
            EndPaint(railInputWindow_, &paint);
            return 0;
        }

        case WM_CONTEXTMENU: {
            if (blockPointerInput) {
                return 0;
            }
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ShowCategoryContextMenu(point);
            return 0;
        }

        case WM_RBUTTONUP: {
            if (blockPointerInput) {
                return 0;
            }
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ClientToScreen(railInputWindow_, &point);
            ShowCategoryContextMenu(point);
            return 0;
        }

        case WM_MOUSEWHEEL:
            if (blockPointerInput) {
                return 0;
            }
            if (categoryList_ != nullptr) {
                return SendMessageW(categoryList_, message, wParam, lParam);
            }
            return 0;

        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
        case WM_MOUSEMOVE:
            if (blockPointerInput) {
                return 0;
            }
            if (categoryList_ != nullptr) {
                POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                ClientToScreen(railInputWindow_, &point);
                RECT listRect{};
                if (!GetWindowRect(categoryList_, &listRect) ||
                    PtInRect(&listRect, point) == FALSE) {
                    return 0;
                }
                ScreenToClient(categoryList_, &point);
                if (message == WM_LBUTTONDOWN) {
                    SetForegroundWindow(window_);
                }
                return SendMessageW(categoryList_, message, wParam,
                                    MAKELPARAM(point.x, point.y));
            }
            return 0;

        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;

        default:
            break;
    }

    return DefWindowProcW(railInputWindow_, message, wParam, lParam);
}

bool Application::CreateControls() {
    dpi_ = GetDpiForWindow(window_);
    RecreateFonts();

    railBackgroundBrush_ = CreateSolidBrush(kRailTransparencyColor);
    if (railBackgroundBrush_ == nullptr ||
        !SetLayeredWindowAttributes(window_, kRailTransparencyColor, 255,
                                    LWA_COLORKEY)) {
        return false;
    }

    railInputWindow_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_NOACTIVATE,
        kRailInputWindowClass, L"", WS_POPUP,
        0, 0, 0, 0, nullptr, nullptr, instance_, this);
    // Color-keyed pixels do not receive mouse input. This layered window sits
    // behind the controls, paints the rounded translucent panel, and keeps its
    // otherwise transparent surface available for blank-area context menus.
    if (railInputWindow_ == nullptr ||
        !SetLayeredWindowAttributes(railInputWindow_, kRailTransparencyColor,
                                    kRailPanelOpacity,
                                    LWA_COLORKEY | LWA_ALPHA)) {
        return false;
    }

    pinButton_ = CreateWindowExW(0, L"BUTTON", L"固定",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                      BS_AUTOCHECKBOX | BS_PUSHLIKE | BS_FLAT,
                                  0, 0, 0, 0, window_,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPinButtonId)),
                                  instance_, nullptr);
    addCategoryButton_ = CreateWindowExW(
        0, L"BUTTON", L"新建分组",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_FLAT,
        0, 0, 0, 0, window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddCategoryId)),
        instance_, nullptr);
    categoryList_ = CreateWindowExW(0, L"LISTBOX", nullptr,
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                                         LBS_NOTIFY | LBS_NOINTEGRALHEIGHT |
                                         LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
                                     0, 0, 0, 0, window_,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCategoryListId)), instance_, nullptr);

    if (pinButton_ == nullptr || addCategoryButton_ == nullptr ||
        categoryList_ == nullptr) {
        return false;
    }

    if (!SetWindowSubclass(categoryList_, CategoryListSubclassProc,
                           kCategoryListSubclassId,
                           reinterpret_cast<DWORD_PTR>(this)) ||
        !SetWindowSubclass(pinButton_, PinButtonSubclassProc,
                           kPinButtonSubclassId,
                           reinterpret_cast<DWORD_PTR>(this)) ||
        !SetWindowSubclass(addCategoryButton_, AddCategoryButtonSubclassProc,
                           kAddCategoryButtonSubclassId,
                           reinterpret_cast<DWORD_PTR>(this))) {
        return false;
    }
    if (SetTimer(window_, kEdgePollTimerId, kHiddenEdgePollIntervalMilliseconds,
                 nullptr) == 0) {
        return false;
    }

    SetWindowTheme(categoryList_, L"Explorer", nullptr);
    SetWindowTheme(pinButton_, L"Explorer", nullptr);
    SetWindowTheme(addCategoryButton_, L"Explorer", nullptr);

    ApplyFonts();
    RefreshCategories();

    RECT client{};
    GetClientRect(window_, &client);
    LayoutControls(client.right, client.bottom);
    return true;
}

void Application::RecreateFonts() {
    HFONT newBodyFont = CreateFontW(-MulDiv(10, static_cast<int>(dpi_), 72), 0, 0, 0,
                                    FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                                    DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HFONT newHeadingFont = CreateFontW(-MulDiv(14, static_cast<int>(dpi_), 72), 0, 0, 0,
                                       FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                                       DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    if (newBodyFont == nullptr || newHeadingFont == nullptr) {
        if (newBodyFont != nullptr) {
            DeleteObject(newBodyFont);
        }
        if (newHeadingFont != nullptr) {
            DeleteObject(newHeadingFont);
        }
        return;
    }

    HFONT oldBodyFont = bodyFont_;
    HFONT oldHeadingFont = headingFont_;
    bodyFont_ = newBodyFont;
    headingFont_ = newHeadingFont;
    ApplyFonts();
    if (oldBodyFont != nullptr) {
        DeleteObject(oldBodyFont);
    }
    if (oldHeadingFont != nullptr) {
        DeleteObject(oldHeadingFont);
    }
}

void Application::ApplyFonts() const {
    SetControlFont(pinButton_, bodyFont_);
    SetControlFont(addCategoryButton_, bodyFont_);
    SetControlFont(categoryList_, bodyFont_);
    if (categoryList_ != nullptr) {
        const UINT categoryDpi = GetDpiForWindow(categoryList_);
        const int itemHeight = std::min(255,
                                        ScaleForDpi(kCategoryCardLogicalHeight,
                                                    categoryDpi != 0 ? categoryDpi : dpi_));
        SendMessageW(categoryList_, LB_SETITEMHEIGHT, 0,
                     static_cast<LPARAM>(itemHeight));
    }
}

RECT Application::RailPanelRect(int width, int height) const {
    const UINT windowDpi = window_ != nullptr ? GetDpiForWindow(window_) : 0;
    const UINT railDpi = windowDpi != 0 ? windowDpi : dpi_;
    const int outerMargin = ScaleForDpi(kRailPanelLogicalMargin, railDpi);
    const int availableWidth = std::max(1, width - outerMargin * 2);
    const int availableHeight = std::max(1, height - outerMargin * 2);
    const int panelWidth = std::min(availableWidth,
                                    ScaleForDpi(kRailPanelLogicalWidth, railDpi));
    const std::size_t categoryRows = std::max<std::size_t>(
        1, (state_.categories.size() + kCategoryColumns - 1) / kCategoryColumns);
    int rowHeight = ScaleForDpi(kCategoryCardLogicalHeight, railDpi);
    if (categoryList_ != nullptr) {
        const LRESULT currentHeight = SendMessageW(categoryList_, LB_GETITEMHEIGHT, 0, 0);
        if (currentHeight != LB_ERR && currentHeight > 0) {
            rowHeight = static_cast<int>(currentHeight);
        }
    }
    const int desiredHeight = ScaleForDpi(kRailHeaderLogicalHeight, railDpi) +
                              ScaleForDpi(kRailFooterLogicalHeight, railDpi) +
                              ScaleForDpi(kRailBottomLogicalPadding, railDpi) +
                              static_cast<int>(categoryRows) * rowHeight;
    const int boundedHeight = std::clamp(
        desiredHeight,
        ScaleForDpi(kRailPanelLogicalMinimumHeight, railDpi),
        ScaleForDpi(kRailPanelLogicalMaximumHeight, railDpi));
    const int panelHeight = std::min(availableHeight, boundedHeight);
    const int left = std::max(0, width - outerMargin - panelWidth);
    const int top = std::max(0, (height - panelHeight) / 2);
    return RECT{left, top, left + panelWidth, top + panelHeight};
}

void Application::DrawRailPanel(HDC deviceContext, const RECT& clientRect) const {
    if (deviceContext == nullptr) {
        return;
    }

    FillRect(deviceContext, &clientRect,
             railBackgroundBrush_ != nullptr
                 ? railBackgroundBrush_
                 : static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

    const int width = std::max(
        0, static_cast<int>(clientRect.right - clientRect.left));
    const int height = std::max(
        0, static_cast<int>(clientRect.bottom - clientRect.top));
    if (width == 0 || height == 0) {
        return;
    }

    const int radius = ScaleForDpi(kRailPanelLogicalRadius, dpi_);
    HRGN panelRegion = CreateRoundRectRgn(clientRect.left, clientRect.top,
                                          clientRect.right + 1, clientRect.bottom + 1,
                                          radius, radius);
    if (panelRegion != nullptr) {
        SelectClipRgn(deviceContext, panelRegion);
    }

    constexpr int kGradientBands = 40;
    for (int band = 0; band < kGradientBands; ++band) {
        const int top = clientRect.top + height * band / kGradientBands;
        const int bottom = clientRect.top + height * (band + 1) / kGradientBands;
        const int denominator = std::max(1, kGradientBands - 1);
        const int red = kRailPanelTopRed +
                        (kRailPanelBottomRed - kRailPanelTopRed) * band /
                            denominator;
        const int green = kRailPanelTopGreen +
                          (kRailPanelBottomGreen - kRailPanelTopGreen) * band /
                              denominator;
        const int blue = kRailPanelTopBlue +
                         (kRailPanelBottomBlue - kRailPanelTopBlue) * band /
                             denominator;
        const COLORREF bandColor =
            static_cast<COLORREF>(static_cast<BYTE>(red)) |
            (static_cast<COLORREF>(static_cast<BYTE>(green)) << 8) |
            (static_cast<COLORREF>(static_cast<BYTE>(blue)) << 16);
        HBRUSH bandBrush = CreateSolidBrush(bandColor);
        if (bandBrush != nullptr) {
            RECT bandRect{clientRect.left, top, clientRect.right, bottom};
            FillRect(deviceContext, &bandRect, bandBrush);
            DeleteObject(bandBrush);
        }
    }

    SelectClipRgn(deviceContext, nullptr);
    if (panelRegion != nullptr) {
        DeleteObject(panelRegion);
    }

    HPEN borderPen = CreatePen(PS_SOLID, std::max(1, ScaleForDpi(1, dpi_)),
                               kRailPanelBorderColor);
    HGDIOBJ oldPen = borderPen != nullptr ? SelectObject(deviceContext, borderPen) : nullptr;
    HGDIOBJ oldBrush = SelectObject(deviceContext, GetStockObject(HOLLOW_BRUSH));
    RoundRect(deviceContext, clientRect.left, clientRect.top,
              clientRect.right - 1, clientRect.bottom - 1, radius, radius);
    SelectObject(deviceContext, oldBrush);
    if (oldPen != nullptr) {
        SelectObject(deviceContext, oldPen);
    }
    if (borderPen != nullptr) {
        DeleteObject(borderPen);
    }

    RECT innerRect = clientRect;
    InflateRect(&innerRect, -ScaleForDpi(1, dpi_), -ScaleForDpi(1, dpi_));
    HPEN innerPen = CreatePen(PS_SOLID, std::max(1, ScaleForDpi(1, dpi_)),
                              RGB(202, 208, 204));
    oldPen = innerPen != nullptr ? SelectObject(deviceContext, innerPen) : nullptr;
    oldBrush = SelectObject(deviceContext, GetStockObject(HOLLOW_BRUSH));
    RoundRect(deviceContext, innerRect.left, innerRect.top,
              innerRect.right - 1, innerRect.bottom - 1,
              std::max(1, radius - ScaleForDpi(2, dpi_)),
              std::max(1, radius - ScaleForDpi(2, dpi_)));
    SelectObject(deviceContext, oldBrush);
    if (oldPen != nullptr) {
        SelectObject(deviceContext, oldPen);
    }
    if (innerPen != nullptr) {
        DeleteObject(innerPen);
    }

    const int footerTop =
        clientRect.bottom -
        ScaleForDpi(kRailFooterLogicalHeight + kRailBottomLogicalPadding,
                    dpi_);
    HPEN dividerPen = CreatePen(PS_SOLID, std::max(1, ScaleForDpi(1, dpi_)),
                                RGB(177, 185, 180));
    oldPen = dividerPen != nullptr
                 ? SelectObject(deviceContext, dividerPen)
                 : nullptr;
    MoveToEx(deviceContext,
             clientRect.left + ScaleForDpi(14, dpi_), footerTop, nullptr);
    LineTo(deviceContext,
           clientRect.right - ScaleForDpi(14, dpi_), footerTop);
    if (oldPen != nullptr) {
        SelectObject(deviceContext, oldPen);
    }
    if (dividerPen != nullptr) {
        DeleteObject(dividerPen);
    }
}

void Application::DrawPinButton(HDC deviceContext, const RECT& clientRect) const {
    if (deviceContext == nullptr) {
        return;
    }

    FillRect(deviceContext, &clientRect,
             railBackgroundBrush_ != nullptr
                 ? railBackgroundBrush_
                 : static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

    const LRESULT state = pinButton_ != nullptr
                              ? SendMessageW(pinButton_, BM_GETSTATE, 0, 0)
                              : 0;
    const bool pressed = (state & BST_PUSHED) != 0;
    const bool hot = (state & BST_HOT) != 0;
    const COLORREF background = pinned_ ? RGB(249, 250, 248)
                                        : hot || pressed ? RGB(238, 241, 238)
                                                         : RGB(225, 230, 226);
    const COLORREF glyph = pinned_ ? RGB(20, 25, 22) : RGB(43, 49, 45);

    RECT buttonRect = clientRect;
    InflateRect(&buttonRect, -ScaleForDpi(1, dpi_), -ScaleForDpi(1, dpi_));
    HBRUSH buttonBrush = CreateSolidBrush(background);
    HPEN buttonPen = CreatePen(PS_SOLID, std::max(1, ScaleForDpi(1, dpi_)),
                               pinned_ ? RGB(179, 187, 182) : RGB(213, 219, 215));
    HGDIOBJ oldBrush = buttonBrush != nullptr
                           ? SelectObject(deviceContext, buttonBrush)
                           : nullptr;
    HGDIOBJ oldPen = buttonPen != nullptr ? SelectObject(deviceContext, buttonPen) : nullptr;
    const int buttonRadius = ScaleForDpi(6, dpi_);
    RoundRect(deviceContext, buttonRect.left, buttonRect.top,
              buttonRect.right, buttonRect.bottom, buttonRadius, buttonRadius);
    if (oldPen != nullptr) {
        SelectObject(deviceContext, oldPen);
    }
    if (oldBrush != nullptr) {
        SelectObject(deviceContext, oldBrush);
    }
    if (buttonPen != nullptr) {
        DeleteObject(buttonPen);
    }
    if (buttonBrush != nullptr) {
        DeleteObject(buttonBrush);
    }

    const int centerX = (clientRect.left + clientRect.right) / 2;
    const int centerY = (clientRect.top + clientRect.bottom) / 2;
    const int unit = std::max(1, ScaleForDpi(1, dpi_));
    POINT pinShape[] = {
        {centerX - 4 * unit, centerY - 7 * unit},
        {centerX + 4 * unit, centerY - 7 * unit},
        {centerX + 3 * unit, centerY - 3 * unit},
        {centerX + 5 * unit, centerY},
        {centerX + 5 * unit, centerY + 2 * unit},
        {centerX + unit, centerY + 2 * unit},
        {centerX, centerY + 8 * unit},
        {centerX - unit, centerY + 2 * unit},
        {centerX - 5 * unit, centerY + 2 * unit},
        {centerX - 5 * unit, centerY},
        {centerX - 3 * unit, centerY - 3 * unit}};
    HBRUSH glyphBrush = CreateSolidBrush(glyph);
    HPEN glyphPen = CreatePen(PS_SOLID, unit, glyph);
    oldBrush = glyphBrush != nullptr ? SelectObject(deviceContext, glyphBrush) : nullptr;
    oldPen = glyphPen != nullptr ? SelectObject(deviceContext, glyphPen) : nullptr;
    Polygon(deviceContext, pinShape,
            static_cast<int>(sizeof(pinShape) / sizeof(pinShape[0])));
    if (oldPen != nullptr) {
        SelectObject(deviceContext, oldPen);
    }
    if (oldBrush != nullptr) {
        SelectObject(deviceContext, oldBrush);
    }
    if (glyphPen != nullptr) {
        DeleteObject(glyphPen);
    }
    if (glyphBrush != nullptr) {
        DeleteObject(glyphBrush);
    }
}

void Application::DrawAddCategoryButton(HDC deviceContext,
                                        const RECT& clientRect) const {
    if (deviceContext == nullptr) {
        return;
    }

    FillRect(deviceContext, &clientRect,
             railBackgroundBrush_ != nullptr
                 ? railBackgroundBrush_
                 : static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

    const LRESULT state =
        addCategoryButton_ != nullptr
            ? SendMessageW(addCategoryButton_, BM_GETSTATE, 0, 0)
            : 0;
    const bool pressed = (state & BST_PUSHED) != 0;
    const bool hot = (state & BST_HOT) != 0;
    const COLORREF background =
        pressed ? RGB(220, 226, 222)
                : hot ? RGB(239, 242, 239) : RGB(229, 234, 230);
    const COLORREF border = hot ? RGB(177, 186, 180)
                                : RGB(199, 206, 202);
    const COLORREF glyph = RGB(48, 57, 52);

    RECT buttonRect = clientRect;
    InflateRect(&buttonRect, -ScaleForDpi(1, dpi_),
                -ScaleForDpi(1, dpi_));
    HBRUSH buttonBrush = CreateSolidBrush(background);
    HPEN buttonPen = CreatePen(PS_SOLID,
                               std::max(1, ScaleForDpi(1, dpi_)),
                               border);
    HGDIOBJ oldBrush =
        buttonBrush != nullptr ? SelectObject(deviceContext, buttonBrush)
                               : nullptr;
    HGDIOBJ oldPen =
        buttonPen != nullptr ? SelectObject(deviceContext, buttonPen)
                             : nullptr;
    const int radius = ScaleForDpi(8, dpi_) * 2;
    RoundRect(deviceContext, buttonRect.left, buttonRect.top,
              buttonRect.right, buttonRect.bottom, radius, radius);
    if (oldPen != nullptr) {
        SelectObject(deviceContext, oldPen);
    }
    if (oldBrush != nullptr) {
        SelectObject(deviceContext, oldBrush);
    }
    if (buttonPen != nullptr) {
        DeleteObject(buttonPen);
    }
    if (buttonBrush != nullptr) {
        DeleteObject(buttonBrush);
    }

    const int centerX = (clientRect.left + clientRect.right) / 2;
    const int centerY = (clientRect.top + clientRect.bottom) / 2;
    const int unit = std::max(1, ScaleForDpi(1, dpi_));
    HPEN plusPen = CreatePen(PS_SOLID, std::max(2, unit * 2), glyph);
    oldPen = plusPen != nullptr ? SelectObject(deviceContext, plusPen)
                                : nullptr;
    MoveToEx(deviceContext, centerX - 6 * unit, centerY, nullptr);
    LineTo(deviceContext, centerX + 6 * unit, centerY);
    MoveToEx(deviceContext, centerX, centerY - 6 * unit, nullptr);
    LineTo(deviceContext, centerX, centerY + 6 * unit);
    if (oldPen != nullptr) {
        SelectObject(deviceContext, oldPen);
    }
    if (plusPen != nullptr) {
        DeleteObject(plusPen);
    }
}

void Application::LayoutControls(int width, int height) {
    if (categoryList_ == nullptr || addCategoryButton_ == nullptr) {
        return;
    }

    const UINT windowDpi = window_ != nullptr ? GetDpiForWindow(window_) : 0;
    const UINT railDpi = windowDpi != 0 ? windowDpi : dpi_;
    const RECT panel = RailPanelRect(width, height);
    const int innerMargin = ScaleForDpi(6, railDpi);
    const int pinSize = ScaleForDpi(24, railDpi);
    const int addSize = ScaleForDpi(30, railDpi);
    const int footerHeight = ScaleForDpi(kRailFooterLogicalHeight, railDpi);
    const int listTop = panel.top + ScaleForDpi(kRailHeaderLogicalHeight, railDpi);
    const int itemHeight = std::min(255,
                                    ScaleForDpi(kCategoryCardLogicalHeight, railDpi));
    SendMessageW(categoryList_, LB_SETITEMHEIGHT, 0,
                 static_cast<LPARAM>(itemHeight));

    MoveWindow(pinButton_, panel.right - innerMargin - pinSize,
               panel.top + ScaleForDpi(7, railDpi), pinSize, pinSize, TRUE);
    MoveWindow(categoryList_, panel.left + innerMargin, listTop,
               std::max(72, static_cast<int>(panel.right - panel.left) -
                                innerMargin * 2),
               std::max(50, static_cast<int>(panel.bottom) - listTop -
                                footerHeight -
                                ScaleForDpi(kRailBottomLogicalPadding, railDpi)),
               TRUE);
    MoveWindow(addCategoryButton_,
               panel.left + (panel.right - panel.left - addSize) / 2,
               panel.bottom - ScaleForDpi(kRailBottomLogicalPadding, railDpi) -
                   addSize,
               addSize, addSize, TRUE);
    ShowScrollBar(categoryList_, SB_VERT, FALSE);
    PositionRailInputWindow();
    QueueVisibleCategoryPreviewLoad();
}

FenceWindow* Application::CreateFenceWindow(std::size_t categoryIndex) {
    if (categoryIndex >= state_.categories.size()) {
        return nullptr;
    }

    auto fence = std::make_unique<FenceWindow>();
    fence->owner = this;
    fence->categoryIndex = categoryIndex;
    fence->dpi = window_ != nullptr ? GetDpiForWindow(window_) : GetDpiForSystem();
    if (fence->dpi == 0) {
        fence->dpi = 96;
    }

    HMONITOR monitor = panelMonitor_;
    if (monitor == nullptr) {
        monitor = MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST);
    }
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (monitor == nullptr || !GetMonitorInfoW(monitor, &monitorInfo)) {
        monitorInfo.rcWork = RECT{0, 0, GetSystemMetrics(SM_CXSCREEN),
                                  GetSystemMetrics(SM_CYSCREEN)};
    }

    const int workWidth = monitorInfo.rcWork.right - monitorInfo.rcWork.left;
    const int workHeight = monitorInfo.rcWork.bottom - monitorInfo.rcWork.top;
    const int outerMargin = ScaleForDpi(24, fence->dpi);
    const int width = std::max(
        ScaleForDpi(kFenceMinimumLogicalWidth, fence->dpi),
        std::min(ScaleForDpi(kFenceWindowLogicalWidth, fence->dpi),
                 std::max(1, workWidth - outerMargin * 2)));
    const int height = std::max(
        ScaleForDpi(kFenceMinimumLogicalHeight, fence->dpi),
        std::min(ScaleForDpi(kFenceWindowLogicalHeight, fence->dpi),
                 std::max(1, workHeight - outerMargin * 2)));
    const int cascade = ScaleForDpi(
        static_cast<int>((fences_.size() % 6) * 26), fence->dpi);
    const int x = std::clamp(
        monitorInfo.rcWork.left + (workWidth - width) / 2 + cascade,
        monitorInfo.rcWork.left, std::max(monitorInfo.rcWork.left,
                                         monitorInfo.rcWork.right - width));
    const int y = std::clamp(
        monitorInfo.rcWork.top + (workHeight - height) / 2 + cascade,
        monitorInfo.rcWork.top, std::max(monitorInfo.rcWork.top,
                                        monitorInfo.rcWork.bottom - height));
    const std::wstring title = std::wstring(kContentWindowTitlePrefix) +
                               state_.categories[categoryIndex].name;

    fence->window = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_LAYERED, kContentWindowClass, title.c_str(),
        WS_POPUP | WS_THICKFRAME | WS_CLIPCHILDREN,
        x, y, width, height, nullptr, nullptr, instance_, fence.get());
    if (fence->window == nullptr ||
        !SetLayeredWindowAttributes(
            fence->window, 0,
            FenceOpacity(state_.categories[categoryIndex].background),
            LWA_ALPHA)) {
        if (fence->window != nullptr) {
            DestroyWindow(fence->window);
        }
        return nullptr;
    }

    fence->pinButton = CreateWindowExW(
        0, L"BUTTON", L"固定围栏", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 0, 0, fence->window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kFencePinButtonId)),
        instance_, nullptr);
    fence->closeButton = CreateWindowExW(
        0, L"BUTTON", L"关闭围栏", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 0, 0, fence->window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCloseContentButtonId)),
        instance_, nullptr);
    fence->itemList = CreateWindowExW(
        0, WC_LISTVIEWW, nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_ICON | LVS_SINGLESEL |
            LVS_SHOWSELALWAYS | LVS_AUTOARRANGE,
        0, 0, 0, 0, fence->window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kItemListId)), instance_, nullptr);
    if (fence->pinButton == nullptr || fence->closeButton == nullptr ||
        fence->itemList == nullptr ||
        !SetWindowSubclass(fence->itemList, ItemListSubclassProc,
                           kItemListSubclassId,
                           reinterpret_cast<DWORD_PTR>(fence.get()))) {
        DestroyFenceResources(*fence);
        DestroyWindow(fence->window);
        return nullptr;
    }

    DragAcceptFiles(fence->itemList, TRUE);
    SetWindowTheme(fence->itemList, L"", L"");
    SetWindowTheme(fence->pinButton, L"", L"");
    SetWindowTheme(fence->closeButton, L"", L"");
    ListView_SetExtendedListViewStyle(
        fence->itemList,
        LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP | LVS_EX_BORDERSELECT);
    const COLORREF surfaceColor =
        FenceSurfaceColor(state_.categories[categoryIndex].background);
    ListView_SetBkColor(fence->itemList, surfaceColor);
    ListView_SetTextBkColor(fence->itemList, surfaceColor);
    ListView_SetTextColor(fence->itemList, FenceTextColor(surfaceColor));
    RecreateFenceFonts(*fence);
    SendMessageW(fence->pinButton, BM_SETCHECK, BST_UNCHECKED, 0);

    RECT client{};
    GetClientRect(fence->window, &client);
    LayoutFenceControls(*fence, client.right, client.bottom);
    ApplyFenceRoundedRegion(*fence);

    FenceWindow* created = fence.get();
    fences_.push_back(std::move(fence));
    return created;
}

FenceWindow* Application::FindFence(std::size_t categoryIndex) const {
    const auto found = std::find_if(
        fences_.begin(), fences_.end(),
        [categoryIndex](const std::unique_ptr<FenceWindow>& fence) {
            return fence != nullptr && fence->categoryIndex == categoryIndex;
        });
    return found == fences_.end() ? nullptr : found->get();
}

bool Application::IsFenceVisible(std::size_t categoryIndex) const {
    FenceWindow* fence = FindFence(categoryIndex);
    return fence != nullptr && fence->window != nullptr &&
           IsWindowVisible(fence->window) != FALSE;
}

void Application::OpenOrShowFence(std::size_t categoryIndex) {
    if (categoryIndex >= state_.categories.size()) {
        return;
    }
    FenceWindow* fence = FindFence(categoryIndex);
    if (fence == nullptr) {
        fence = CreateFenceWindow(categoryIndex);
    }
    if (fence == nullptr || fence->window == nullptr) {
        MessageBoxW(window_, L"无法创建分组围栏。", L"LightLaunch",
                    MB_OK | MB_ICONERROR);
        return;
    }

    state_.selectedCategory = categoryIndex;
    FenceWindow* previousFence = activeFence_;
    activeFence_ = fence;
    RefreshFence(*fence);
    activeFence_ = previousFence;
    fence->pointerOutsideSince = 0;
    fence->keepVisibleUntil = GetTickCount64() + kFenceTransitMilliseconds;
    ShowWindow(fence->window, SW_SHOW);
    SetWindowPos(fence->window, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(fence->window);
    if (categoryList_ != nullptr) {
        SendMessageW(categoryList_, LB_SETCURSEL,
                     static_cast<WPARAM>(categoryIndex / kCategoryColumns), 0);
        InvalidateRect(categoryList_, nullptr, FALSE);
    }
    UpdatePollTimerInterval();
}

void Application::HideFence(FenceWindow& fence) {
    const COLORREF surfaceColor =
        fence.categoryIndex < state_.categories.size()
            ? FenceSurfaceColor(
                  state_.categories[fence.categoryIndex].background)
            : kDefaultFenceSurfaceColor;
    if (fence.window != nullptr && IsWindow(fence.window)) {
        ShowWindow(fence.window, SW_HIDE);
    }
    if (fence.itemList != nullptr && IsWindow(fence.itemList)) {
        LVBKIMAGEW clearBackground{};
        clearBackground.ulFlags = LVBKIF_SOURCE_NONE;
        SendMessageW(fence.itemList, LVM_SETBKIMAGEW, 0,
                     reinterpret_cast<LPARAM>(&clearBackground));
        ListView_SetBkColor(fence.itemList, surfaceColor);
        ListView_SetTextBkColor(fence.itemList, surfaceColor);
        ListView_SetTextColor(fence.itemList,
                              FenceTextColor(surfaceColor));
    }
    fence.backgroundImage.reset();
    fence.loadedBackgroundPath.clear();
    fence.hasRenderedBackground = false;
    fence.pointerOutsideSince = 0;
    fence.keepVisibleUntil = 0;
    if (categoryList_ != nullptr) {
        InvalidateRect(categoryList_, nullptr, FALSE);
    }
    UpdatePollTimerInterval();
}

void Application::SetFencePinned(FenceWindow& fence, bool pinned) {
    fence.pinned = pinned;
    fence.pointerOutsideSince = 0;
    fence.keepVisibleUntil = GetTickCount64() +
                             kInteractionKeepVisibleMilliseconds;
    if (fence.pinButton != nullptr) {
        SendMessageW(fence.pinButton, BM_SETCHECK,
                     pinned ? BST_CHECKED : BST_UNCHECKED, 0);
        InvalidateRect(fence.pinButton, nullptr, FALSE);
    }
    InvalidateRect(fence.window, nullptr, FALSE);
    UpdatePollTimerInterval();
}

void Application::LayoutFenceControls(FenceWindow& fence, int width, int height) {
    if (fence.itemList == nullptr) {
        return;
    }
    const int headerHeight = ScaleForDpi(kFenceHeaderLogicalHeight, fence.dpi);
    const int controlSize = ScaleForDpi(kFenceControlLogicalSize, fence.dpi);
    const int controlTop = ScaleForDpi(9, fence.dpi);
    const int controlRight = ScaleForDpi(8, fence.dpi);
    const int controlGap = ScaleForDpi(2, fence.dpi);
    const int padding = ScaleForDpi(kFenceContentLogicalPadding, fence.dpi);
    const int statusHeight = ScaleForDpi(kFenceStatusLogicalHeight, fence.dpi);

    MoveWindow(fence.closeButton, width - controlRight - controlSize,
               controlTop, controlSize, controlSize, TRUE);
    MoveWindow(fence.pinButton,
               width - controlRight - controlSize * 2 - controlGap,
               controlTop, controlSize, controlSize, TRUE);
    const int itemTop = headerHeight + ScaleForDpi(8, fence.dpi);
    const int itemBottom = std::max(itemTop + ScaleForDpi(48, fence.dpi),
                                    height - padding - statusHeight);
    MoveWindow(fence.itemList, padding, itemTop,
               std::max(80, width - padding * 2),
               std::max(48, itemBottom - itemTop), TRUE);
    ListView_SetIconSpacing(fence.itemList, ScaleForDpi(104, fence.dpi),
                            ScaleForDpi(100, fence.dpi));
    if (!fence.movingOrSizing) {
        UpdateFenceListBackground(fence);
    }
    InvalidateRect(fence.window, nullptr, FALSE);
}

void Application::ApplyFenceRoundedRegion(FenceWindow& fence) const {
    if (fence.window == nullptr) {
        return;
    }
    RECT bounds{};
    if (!GetWindowRect(fence.window, &bounds)) {
        return;
    }
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    const int radius = ScaleForDpi(kFenceLogicalRadius, fence.dpi) * 2;
    HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, radius, radius);
    if (region != nullptr && SetWindowRgn(fence.window, region, TRUE) == 0) {
        DeleteObject(region);
    }
}

void Application::RecreateFenceFonts(FenceWindow& fence) {
    HFONT newBodyFont = CreateFontW(
        -MulDiv(10, static_cast<int>(fence.dpi), 72), 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HFONT newHeadingFont = CreateFontW(
        -MulDiv(12, static_cast<int>(fence.dpi), 72), 0, 0, 0, FW_SEMIBOLD,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    if (newBodyFont == nullptr || newHeadingFont == nullptr) {
        if (newBodyFont != nullptr) DeleteObject(newBodyFont);
        if (newHeadingFont != nullptr) DeleteObject(newHeadingFont);
        return;
    }
    if (fence.bodyFont != nullptr) DeleteObject(fence.bodyFont);
    if (fence.headingFont != nullptr) DeleteObject(fence.headingFont);
    fence.bodyFont = newBodyFont;
    fence.headingFont = newHeadingFont;
    SetControlFont(fence.itemList, fence.bodyFont);
}

void Application::DrawFence(const FenceWindow& fence, HDC deviceContext,
                            const RECT& clientRect) const {
    FenceBackground fallbackBackground;
    const FenceBackground& settings =
        fence.categoryIndex < state_.categories.size()
            ? state_.categories[fence.categoryIndex].background
            : fallbackBackground;
    const COLORREF surfaceColor = FenceSurfaceColor(settings);
    const bool darkSurface = IsDarkFenceSurface(surfaceColor);
    const COLORREF headerColor = BlendColor(
        surfaceColor, RGB(255, 255, 255), darkSurface ? 8U : 18U);
    HBRUSH bodyBrush = CreateSolidBrush(surfaceColor);
    HBRUSH headerBrush = CreateSolidBrush(headerColor);
    FillRect(deviceContext, &clientRect,
             bodyBrush != nullptr ? bodyBrush
                                  : static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    RECT header = clientRect;
    header.bottom = std::min(static_cast<int>(header.bottom),
                             ScaleForDpi(kFenceHeaderLogicalHeight, fence.dpi));
    FillRect(deviceContext, &header,
             headerBrush != nullptr ? headerBrush
                                    : static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    if (bodyBrush != nullptr) DeleteObject(bodyBrush);
    if (headerBrush != nullptr) DeleteObject(headerBrush);

    HPEN separator = CreatePen(
        PS_SOLID, 1,
        darkSurface ? FenceSeparatorColor(surfaceColor)
                    : BlendColor(surfaceColor, RGB(255, 255, 255), 62U));
    HGDIOBJ previousPen = separator != nullptr
                              ? SelectObject(deviceContext, separator)
                              : nullptr;
    MoveToEx(deviceContext, clientRect.left, header.bottom - 1, nullptr);
    LineTo(deviceContext, clientRect.right, header.bottom - 1);

    HPEN border = CreatePen(
        PS_SOLID, 1,
        darkSurface ? FenceBorderColor(surfaceColor)
                    : BlendColor(surfaceColor, RGB(255, 255, 255), 82U));
    if (border != nullptr) {
        SelectObject(deviceContext, border);
    }
    HGDIOBJ previousBrush = SelectObject(deviceContext, GetStockObject(NULL_BRUSH));
    const int radius = ScaleForDpi(kFenceLogicalRadius, fence.dpi) * 2;
    RoundRect(deviceContext, clientRect.left, clientRect.top,
              clientRect.right - 1, clientRect.bottom - 1, radius, radius);
    HPEN innerBorder = CreatePen(
        PS_SOLID, 1,
        darkSurface ? BlendColor(surfaceColor, RGB(255, 255, 255), 14U)
                    : BlendColor(surfaceColor, RGB(105, 119, 111), 18U));
    if (innerBorder != nullptr) {
        SelectObject(deviceContext, innerBorder);
        RoundRect(deviceContext, clientRect.left + 1, clientRect.top + 1,
                  clientRect.right - 2, clientRect.bottom - 2,
                  std::max(2, radius - 2), std::max(2, radius - 2));
    }
    SelectObject(deviceContext, previousBrush);
    if (previousPen != nullptr) SelectObject(deviceContext, previousPen);
    if (separator != nullptr) DeleteObject(separator);
    if (border != nullptr) DeleteObject(border);
    if (innerBorder != nullptr) DeleteObject(innerBorder);

    const int previousMode = SetBkMode(deviceContext, TRANSPARENT);
    SetTextColor(deviceContext, FenceTextColor(surfaceColor));
    HGDIOBJ previousFont = SelectObject(
        deviceContext, fence.headingFont != nullptr ? fence.headingFont : headingFont_);
    RECT titleRect{ScaleForDpi(14, fence.dpi), 0,
                   std::max(ScaleForDpi(40, fence.dpi),
                            static_cast<int>(clientRect.right) -
                                ScaleForDpi(84, fence.dpi)),
                   header.bottom};
    const wchar_t* title = fence.categoryIndex < state_.categories.size()
                               ? state_.categories[fence.categoryIndex].name.c_str()
                               : L"分组";
    DrawTextW(deviceContext, title, -1, &titleRect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    SelectObject(deviceContext, fence.bodyFont != nullptr ? fence.bodyFont : bodyFont_);
    SetTextColor(deviceContext, FenceMutedTextColor(surfaceColor));
    RECT statusRect{ScaleForDpi(kFenceContentLogicalPadding, fence.dpi),
                    clientRect.bottom - ScaleForDpi(kFenceStatusLogicalHeight + 4,
                                                    fence.dpi),
                    clientRect.right - ScaleForDpi(kFenceContentLogicalPadding,
                                                   fence.dpi),
                    clientRect.bottom - ScaleForDpi(4, fence.dpi)};
    std::wstring status = L"右键空白处或拖入文件即可添加";
    if (fence.categoryIndex < state_.categories.size() &&
        !state_.categories[fence.categoryIndex].items.empty()) {
        status = std::to_wstring(
                     state_.categories[fence.categoryIndex].items.size()) +
                 L" 个项目 · 双击启动";
    }
    DrawTextW(deviceContext, status.c_str(), -1, &statusRect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(deviceContext, previousFont);
    SetBkMode(deviceContext, previousMode);
}

void Application::RefreshFenceBackground(FenceWindow& fence,
                                         bool forceReload) {
    if (fence.itemList == nullptr ||
        fence.categoryIndex >= state_.categories.size()) {
        return;
    }

    const FenceBackground& settings =
        state_.categories[fence.categoryIndex].background;
    if (fence.window != nullptr && IsWindow(fence.window)) {
        SetLayeredWindowAttributes(fence.window, 0, FenceOpacity(settings),
                                   LWA_ALPHA);
    }
    const bool sourceChanged = fence.loadedBackgroundPath != settings.imagePath;
    if (forceReload || sourceChanged) {
        fence.backgroundImage.reset();
        fence.loadedBackgroundPath = settings.imagePath;
        if (!settings.imagePath.empty()) {
            auto image = std::make_unique<BackgroundImage>();
            if (image->Load(settings.imagePath)) {
                fence.backgroundImage = std::move(image);
            }
        }
        fence.hasRenderedBackground = false;
    }
    if (fence.renderedBackground != settings) {
        fence.hasRenderedBackground = false;
    }
    UpdateFenceListBackground(fence);
}

void Application::UpdateFenceListBackground(FenceWindow& fence) {
    if (fence.itemList == nullptr || !IsWindow(fence.itemList) ||
        fence.categoryIndex >= state_.categories.size()) {
        return;
    }

    RECT client{};
    if (!GetClientRect(fence.itemList, &client)) {
        return;
    }
    const int width = std::max(0, static_cast<int>(client.right - client.left));
    const int height = std::max(0, static_cast<int>(client.bottom - client.top));
    if (width == 0 || height == 0) {
        return;
    }

    const FenceBackground& settings =
        state_.categories[fence.categoryIndex].background;
    const COLORREF surfaceColor = FenceSurfaceColor(settings);
    ListView_SetTextColor(fence.itemList, FenceTextColor(surfaceColor));
    if (fence.backgroundImage == nullptr ||
        !fence.backgroundImage->IsLoaded() || settings.imagePath.empty()) {
        LVBKIMAGEW clearBackground{};
        clearBackground.ulFlags = LVBKIF_SOURCE_NONE;
        SendMessageW(fence.itemList, LVM_SETBKIMAGEW, 0,
                     reinterpret_cast<LPARAM>(&clearBackground));
        ListView_SetBkColor(fence.itemList, surfaceColor);
        ListView_SetTextBkColor(fence.itemList, surfaceColor);
        fence.renderedBackground = settings;
        fence.renderedBackgroundSize = SIZE{width, height};
        fence.hasRenderedBackground = false;
        return;
    }

    if (fence.hasRenderedBackground &&
        fence.renderedBackground == settings &&
        fence.renderedBackgroundSize.cx == width &&
        fence.renderedBackgroundSize.cy == height) {
        return;
    }

    HBITMAP rendered = fence.backgroundImage->Render(
        settings, width, height, surfaceColor);
    if (rendered == nullptr) {
        LVBKIMAGEW clearBackground{};
        clearBackground.ulFlags = LVBKIF_SOURCE_NONE;
        SendMessageW(fence.itemList, LVM_SETBKIMAGEW, 0,
                     reinterpret_cast<LPARAM>(&clearBackground));
        ListView_SetBkColor(fence.itemList, surfaceColor);
        ListView_SetTextBkColor(fence.itemList, surfaceColor);
        fence.hasRenderedBackground = false;
        return;
    }

    LVBKIMAGEW clearBackground{};
    clearBackground.ulFlags = LVBKIF_SOURCE_NONE;
    SendMessageW(fence.itemList, LVM_SETBKIMAGEW, 0,
                 reinterpret_cast<LPARAM>(&clearBackground));

    LVBKIMAGEW background{};
    background.ulFlags = LVBKIF_SOURCE_HBITMAP | LVBKIF_STYLE_NORMAL;
    background.hbm = rendered;
    if (SendMessageW(fence.itemList, LVM_SETBKIMAGEW, 0,
                     reinterpret_cast<LPARAM>(&background)) == FALSE) {
        DeleteObject(rendered);
        ListView_SetBkColor(fence.itemList, surfaceColor);
        ListView_SetTextBkColor(fence.itemList, surfaceColor);
        fence.hasRenderedBackground = false;
        return;
    }

    ListView_SetBkColor(fence.itemList, surfaceColor);
    ListView_SetTextBkColor(fence.itemList, CLR_NONE);
    fence.renderedBackground = settings;
    fence.renderedBackgroundSize = SIZE{width, height};
    fence.hasRenderedBackground = true;
    InvalidateRect(fence.itemList, nullptr, TRUE);
}

void Application::InvalidateFenceSurface(FenceWindow& fence) const {
    if (fence.itemList != nullptr && IsWindow(fence.itemList)) {
        InvalidateRect(fence.itemList, nullptr, TRUE);
    }
    if (fence.window != nullptr && IsWindow(fence.window)) {
        InvalidateRect(fence.window, nullptr, FALSE);
    }
    if (fence.pinButton != nullptr && IsWindow(fence.pinButton)) {
        InvalidateRect(fence.pinButton, nullptr, TRUE);
    }
    if (fence.closeButton != nullptr && IsWindow(fence.closeButton)) {
        InvalidateRect(fence.closeButton, nullptr, TRUE);
    }
}

void Application::ShowFenceBackgroundSettings() {
    FenceWindow* fence = activeFence_;
    Category* category = CurrentCategory();
    if (fence == nullptr || category == nullptr || fence->window == nullptr) {
        return;
    }

    ++interactionDepth_;
    const std::optional<FenceBackground> updated =
        PromptForFenceBackground(fence->window, instance_, category->background);
    interactionDepth_ = std::max(0, interactionDepth_ - 1);
    fence->pointerOutsideSince = 0;
    fence->keepVisibleUntil = GetTickCount64() +
                              kInteractionKeepVisibleMilliseconds;
    if (!updated.has_value()) {
        return;
    }
    if (*updated == category->background) {
        RefreshFenceBackground(*fence, true);
        InvalidateFenceSurface(*fence);
        return;
    }

    const FenceBackground previous = category->background;
    category->background = *updated;
    if (!SaveState()) {
        category->background = previous;
        return;
    }
    RefreshFenceBackground(*fence, true);
    InvalidateFenceSurface(*fence);
}

void Application::ClearFenceBackground() {
    FenceWindow* fence = activeFence_;
    Category* category = CurrentCategory();
    if (fence == nullptr || category == nullptr ||
        category->background.imagePath.empty()) {
        return;
    }

    const FenceBackground previous = category->background;
    category->background.imagePath.clear();
    if (!SaveState()) {
        category->background = previous;
        return;
    }
    fence->pointerOutsideSince = 0;
    fence->keepVisibleUntil = GetTickCount64() +
                              kInteractionKeepVisibleMilliseconds;
    RefreshFenceBackground(*fence, true);
    InvalidateFenceSurface(*fence);
}

void Application::DrawFenceButton(const FenceWindow& fence,
                                  const DRAWITEMSTRUCT& drawItem) const {
    RECT bounds = drawItem.rcItem;
    FenceBackground fallbackBackground;
    const FenceBackground& settings =
        fence.categoryIndex < state_.categories.size()
            ? state_.categories[fence.categoryIndex].background
            : fallbackBackground;
    const COLORREF surfaceColor = FenceSurfaceColor(settings);
    const COLORREF controlSurface =
        settings.imagePath.empty()
            ? BlendColor(surfaceColor, RGB(255, 255, 255),
                         IsDarkFenceSurface(surfaceColor) ? 8U : 18U)
            : surfaceColor;
    const bool darkSurface = IsDarkFenceSurface(surfaceColor);
    HBRUSH base = CreateSolidBrush(controlSurface);
    FillRect(drawItem.hDC, &bounds, base);
    if (base != nullptr) DeleteObject(base);
    const bool pressed = (drawItem.itemState & ODS_SELECTED) != 0;
    const bool hot = (drawItem.itemState & ODS_HOTLIGHT) != 0;
    if (pressed || hot ||
        (drawItem.CtlID == kFencePinButtonId && fence.pinned)) {
        const COLORREF contrastTarget =
            darkSurface ? RGB(255, 255, 255) : RGB(0, 0, 0);
        HBRUSH hover = CreateSolidBrush(
            drawItem.CtlID == kFencePinButtonId && fence.pinned
                ? RGB(70, 118, 176)
                : BlendColor(surfaceColor, contrastTarget,
                             pressed ? 18U : 10U));
        HPEN pen = CreatePen(PS_SOLID, 1,
                             drawItem.CtlID == kFencePinButtonId && fence.pinned
                                 ? RGB(85, 149, 226)
                                 : BlendColor(surfaceColor, contrastTarget,
                                              24U));
        HGDIOBJ oldBrush = SelectObject(drawItem.hDC, hover);
        HGDIOBJ oldPen = SelectObject(drawItem.hDC, pen);
        const int radius = ScaleForDpi(6, fence.dpi) * 2;
        RoundRect(drawItem.hDC, bounds.left + 1, bounds.top + 1,
                  bounds.right - 1, bounds.bottom - 1, radius, radius);
        SelectObject(drawItem.hDC, oldPen);
        SelectObject(drawItem.hDC, oldBrush);
        DeleteObject(pen);
        DeleteObject(hover);
    }

    const int centerX = (bounds.left + bounds.right) / 2;
    const int centerY = (bounds.top + bounds.bottom) / 2;
    const int unit = std::max(1, ScaleForDpi(1, fence.dpi));
    const COLORREF glyphColor =
        drawItem.CtlID == kFencePinButtonId && fence.pinned
            ? RGB(248, 251, 255)
            : FenceTextColor(surfaceColor);
    HPEN glyphPen = CreatePen(PS_SOLID, std::max(1, unit * 2), glyphColor);
    HGDIOBJ oldPen = SelectObject(drawItem.hDC, glyphPen);
    if (drawItem.CtlID == kCloseContentButtonId) {
        MoveToEx(drawItem.hDC, centerX - 5 * unit, centerY - 5 * unit, nullptr);
        LineTo(drawItem.hDC, centerX + 5 * unit, centerY + 5 * unit);
        MoveToEx(drawItem.hDC, centerX + 5 * unit, centerY - 5 * unit, nullptr);
        LineTo(drawItem.hDC, centerX - 5 * unit, centerY + 5 * unit);
    } else {
        HBRUSH glyphBrush = CreateSolidBrush(glyphColor);
        HGDIOBJ oldBrush = SelectObject(drawItem.hDC, glyphBrush);
        POINT pinShape[] = {
            {centerX - 4 * unit, centerY - 7 * unit},
            {centerX + 4 * unit, centerY - 7 * unit},
            {centerX + 3 * unit, centerY - 3 * unit},
            {centerX + 5 * unit, centerY},
            {centerX + 5 * unit, centerY + 2 * unit},
            {centerX + unit, centerY + 2 * unit},
            {centerX, centerY + 8 * unit},
            {centerX - unit, centerY + 2 * unit},
            {centerX - 5 * unit, centerY + 2 * unit},
            {centerX - 5 * unit, centerY},
            {centerX - 3 * unit, centerY - 3 * unit}};
        Polygon(drawItem.hDC, pinShape,
                static_cast<int>(std::size(pinShape)));
        SelectObject(drawItem.hDC, oldBrush);
        DeleteObject(glyphBrush);
    }
    SelectObject(drawItem.hDC, oldPen);
    DeleteObject(glyphPen);
}

void Application::DestroyFenceResources(FenceWindow& fence) {
    CancelIconBatch(fence.iconLoadBatch);
    if (IsWindow(fence.itemList)) {
        LVBKIMAGEW clearBackground{};
        clearBackground.ulFlags = LVBKIF_SOURCE_NONE;
        SendMessageW(fence.itemList, LVM_SETBKIMAGEW, 0,
                     reinterpret_cast<LPARAM>(&clearBackground));
        DragAcceptFiles(fence.itemList, FALSE);
        RemoveWindowSubclass(fence.itemList, ItemListSubclassProc,
                             kItemListSubclassId);
        ListView_SetImageList(fence.itemList, nullptr, LVSIL_NORMAL);
    }
    if (fence.imageList != nullptr) {
        ImageList_Destroy(fence.imageList);
        fence.imageList = nullptr;
    }
    if (fence.bodyFont != nullptr) {
        DeleteObject(fence.bodyFont);
        fence.bodyFont = nullptr;
    }
    if (fence.headingFont != nullptr) {
        DeleteObject(fence.headingFont);
        fence.headingFont = nullptr;
    }
    fence.itemList = nullptr;
    fence.pinButton = nullptr;
    fence.closeButton = nullptr;
    fence.backgroundImage.reset();
    fence.loadedBackgroundPath.clear();
    fence.hasRenderedBackground = false;
}

void Application::DestroyAllFences() {
    activeFence_ = nullptr;
    for (const std::unique_ptr<FenceWindow>& fence : fences_) {
        if (fence == nullptr) {
            continue;
        }
        DestroyFenceResources(*fence);
        if (fence->window != nullptr && IsWindow(fence->window)) {
            DestroyWindow(fence->window);
        }
    }
    fences_.clear();
}

void Application::RemoveFenceForCategory(std::size_t categoryIndex) {
    for (auto iterator = fences_.begin(); iterator != fences_.end();) {
        FenceWindow* fence = iterator->get();
        if (fence == nullptr) {
            iterator = fences_.erase(iterator);
            continue;
        }
        if (fence->categoryIndex == categoryIndex) {
            if (activeFence_ == fence) {
                activeFence_ = nullptr;
            }
            DestroyFenceResources(*fence);
            if (fence->window != nullptr && IsWindow(fence->window)) {
                DestroyWindow(fence->window);
            }
            iterator = fences_.erase(iterator);
            continue;
        }
        if (fence->categoryIndex > categoryIndex) {
            --fence->categoryIndex;
        }
        ++iterator;
    }
    UpdatePollTimerInterval();
}

void Application::DrainFenceIconLoads() {
    for (const std::unique_ptr<FenceWindow>& fence : fences_) {
        if (fence == nullptr || fence->iconLoadBatch == nullptr) {
            continue;
        }
        std::vector<LoadedIcon> ready;
        {
            std::lock_guard lock(fence->iconLoadBatch->mutex);
            ready.swap(fence->iconLoadBatch->ready);
            fence->iconLoadBatch->notificationPending = false;
        }
        for (const LoadedIcon& loaded : ready) {
            if (loaded.icon != nullptr && fence->imageList != nullptr &&
                fence->itemList != nullptr &&
                loaded.itemIndex < static_cast<std::size_t>(
                                       ListView_GetItemCount(fence->itemList))) {
                const int imageIndex = ImageList_AddIcon(fence->imageList, loaded.icon);
                if (imageIndex >= 0) {
                    LVITEMW item{};
                    item.mask = LVIF_IMAGE;
                    item.iItem = static_cast<int>(loaded.itemIndex);
                    item.iImage = imageIndex;
                    ListView_SetItem(fence->itemList, &item);
                }
            }
            if (loaded.icon != nullptr) {
                DestroyIcon(loaded.icon);
            }
        }
    }
}

void Application::ClampFenceToWorkArea(FenceWindow& fence) const {
    if (fence.window == nullptr || !IsWindow(fence.window)) {
        return;
    }
    RECT bounds{};
    if (!GetWindowRect(fence.window, &bounds)) {
        return;
    }
    const HMONITOR monitor = MonitorFromRect(&bounds, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (monitor == nullptr || !GetMonitorInfoW(monitor, &monitorInfo)) {
        return;
    }
    int width = std::min(bounds.right - bounds.left,
                         monitorInfo.rcWork.right - monitorInfo.rcWork.left);
    int height = std::min(bounds.bottom - bounds.top,
                          monitorInfo.rcWork.bottom - monitorInfo.rcWork.top);
    int x = std::clamp(bounds.left, monitorInfo.rcWork.left,
                       std::max(monitorInfo.rcWork.left,
                                monitorInfo.rcWork.right - width));
    int y = std::clamp(bounds.top, monitorInfo.rcWork.top,
                       std::max(monitorInfo.rcWork.top,
                                monitorInfo.rcWork.bottom - height));
    SetWindowPos(fence.window, nullptr, x, y, width, height,
                 SWP_NOACTIVATE | SWP_NOZORDER);
}

void Application::PollFences(POINT cursor, ULONGLONG now, bool insideDock) {
    const HWND capture = GetCapture();
    for (const std::unique_ptr<FenceWindow>& fencePointer : fences_) {
        if (fencePointer == nullptr) {
            continue;
        }
        FenceWindow& fence = *fencePointer;
        if (fence.window == nullptr || !IsWindowVisible(fence.window)) {
            fence.pointerOutsideSince = 0;
            continue;
        }
        if (fence.pinned || fence.movingOrSizing || interactionDepth_ > 0 ||
            !IsWindowEnabled(fence.window) || capture != nullptr) {
            fence.pointerOutsideSince = 0;
            continue;
        }
        RECT bounds{};
        const bool insideFence = GetWindowRect(fence.window, &bounds) != FALSE &&
                                 PtInRect(&bounds, cursor) != FALSE;
        if (insideFence || insideDock || now < fence.keepVisibleUntil) {
            fence.pointerOutsideSince = 0;
            continue;
        }
        if (fence.pointerOutsideSince == 0) {
            fence.pointerOutsideSince = now;
        } else if (now - fence.pointerOutsideSince >=
                   kFenceAutoHideDelayMilliseconds) {
            HideFence(fence);
        }
    }
}

void Application::UpdatePollTimerInterval() {
    if (window_ == nullptr || !IsWindow(window_)) {
        return;
    }
    const bool hasVisibleUnpinnedFence = std::any_of(
        fences_.begin(), fences_.end(),
        [](const std::unique_ptr<FenceWindow>& fence) {
            return fence != nullptr && !fence->pinned && fence->window != nullptr &&
                   IsWindowVisible(fence->window) != FALSE;
        });
    UINT interval = kHiddenEdgePollIntervalMilliseconds;
    if (hasVisibleUnpinnedFence ||
        (IsWindowVisible(window_) && !pinned_)) {
        interval = kVisibleEdgePollIntervalMilliseconds;
    } else if (IsWindowVisible(window_) && pinned_) {
        interval = kPinnedPollIntervalMilliseconds;
    }
    SetTimer(window_, kEdgePollTimerId, interval, nullptr);
}

void Application::DestroyResources() {
    if (window_ != nullptr) {
        KillTimer(window_, kEdgePollTimerId);
    }
    if (IsWindow(categoryList_)) {
        RemoveWindowSubclass(categoryList_, CategoryListSubclassProc,
                             kCategoryListSubclassId);
    }
    if (IsWindow(pinButton_)) {
        RemoveWindowSubclass(pinButton_, PinButtonSubclassProc,
                             kPinButtonSubclassId);
    }
    if (IsWindow(addCategoryButton_)) {
        RemoveWindowSubclass(addCategoryButton_,
                             AddCategoryButtonSubclassProc,
                             kAddCategoryButtonSubclassId);
    }
    if (iconDispatchContext_ != nullptr) {
        std::lock_guard lock(iconDispatchContext_->mutex);
        iconDispatchContext_->accepting = false;
        iconDispatchContext_->window = nullptr;
    }
    DestroyAllFences();
    CancelPreviewIconBatch(previewIconLoadBatch_);
    ClearCategoryPreviewIcons();

    MSG pending{};
    while (PeekMessageW(&pending, window_, kIconLoadedMessage, kIconLoadedMessage,
                        PM_REMOVE)) {
    }
    while (PeekMessageW(&pending, window_, kPreviewIconLoadedMessage,
                        kPreviewIconLoadedMessage, PM_REMOVE)) {
    }
    if (bodyFont_ != nullptr) {
        DeleteObject(bodyFont_);
        bodyFont_ = nullptr;
    }
    if (headingFont_ != nullptr) {
        DeleteObject(headingFont_);
        headingFont_ = nullptr;
    }
    if (railBackgroundBrush_ != nullptr) {
        DeleteObject(railBackgroundBrush_);
        railBackgroundBrush_ = nullptr;
    }
}

bool Application::AddTrayIcon() {
    if (trayIconAdded_) {
        return true;
    }
    if (window_ == nullptr || !IsWindow(window_)) {
        return false;
    }

    NOTIFYICONDATAW icon{};
    icon.cbSize = sizeof(icon);
    icon.hWnd = window_;
    icon.uID = kTrayIconId;
    icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    icon.uCallbackMessage = kTrayCallbackMessage;
    icon.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    lstrcpynW(icon.szTip, kWindowTitle,
              static_cast<int>(sizeof(icon.szTip) / sizeof(icon.szTip[0])));

    if (!Shell_NotifyIconW(NIM_ADD, &icon)) {
        return false;
    }

    icon.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &icon);
    trayIconAdded_ = true;
    return true;
}

void Application::RemoveTrayIcon() {
    if (!trayIconAdded_) {
        return;
    }

    NOTIFYICONDATAW icon{};
    icon.cbSize = sizeof(icon);
    icon.hWnd = window_;
    icon.uID = kTrayIconId;
    Shell_NotifyIconW(NIM_DELETE, &icon);
    trayIconAdded_ = false;
}

void Application::HideToTray() {
    if (window_ != nullptr && IsWindow(window_)) {
        hoveredCategory_.reset();
        if (railInputWindow_ != nullptr) {
            ShowWindow(railInputWindow_, SW_HIDE);
        }
        ShowWindow(window_, SW_HIDE);
        if (categoryList_ != nullptr) {
            SendMessageW(categoryList_, LB_SETCURSEL, static_cast<WPARAM>(-1), 0);
        }
        panelMonitor_ = nullptr;
        ResetEdgeTracking();
        UpdatePollTimerInterval();
    }
}

void Application::ShowFromTray() {
    if (window_ == nullptr || !IsWindow(window_)) {
        return;
    }

    if (IsInteractionBlocked()) {
        HWND modalOwner = window_;
        for (const std::unique_ptr<FenceWindow>& fence : fences_) {
            if (fence != nullptr && fence->window != nullptr &&
                IsWindowVisible(fence->window) && !IsWindowEnabled(fence->window)) {
                modalOwner = fence->window;
                break;
            }
        }
        HWND popup = GetLastActivePopup(modalOwner);
        if (popup != nullptr && IsWindow(popup)) {
            ShowWindow(popup, SW_SHOW);
            SetForegroundWindow(popup);
        }
        MessageBeep(MB_ICONINFORMATION);
        return;
    }

    ShowEdgePanel(true);
    HWND foreground = window_;
    if (!IsWindowEnabled(window_)) {
        HWND popup = GetLastActivePopup(window_);
        if (popup != nullptr && IsWindow(popup)) {
            ShowWindow(popup, SW_SHOW);
            foreground = popup;
        }
    }
    SetForegroundWindow(foreground);
}

void Application::SetPinned(bool pinned) {
    pinned_ = pinned;
    if (pinButton_ != nullptr) {
        SendMessageW(pinButton_, BM_SETCHECK, pinned_ ? BST_CHECKED : BST_UNCHECKED, 0);
        SetWindowTextW(pinButton_, pinned_ ? L"已固定" : L"固定");
    }
    pointerOutsideSince_ = 0;
    UpdatePollTimerInterval();
}

void Application::PollScreenEdge() {
    if (window_ == nullptr || !IsWindow(window_)) {
        return;
    }

    POINT cursor{};
    if (!GetCursorPos(&cursor)) {
        return;
    }

    const HMONITOR cursorMonitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (cursorMonitor == nullptr || !GetMonitorInfoW(cursorMonitor, &monitorInfo)) {
        return;
    }

    const int triggerWidth = std::max(2, ScaleForDpi(2, dpi_));
    const bool atRightEdge = cursor.x >= monitorInfo.rcWork.right - triggerWidth &&
                             cursor.x < monitorInfo.rcWork.right &&
                             cursor.y >= monitorInfo.rcWork.top &&
                             cursor.y < monitorInfo.rcWork.bottom;
    const ULONGLONG now = GetTickCount64();

    RECT dockRect{};
    const bool hasDockRect = IsWindowVisible(window_) &&
                             railInputWindow_ != nullptr &&
                             GetWindowRect(railInputWindow_, &dockRect) != FALSE;
    const bool insideDock = hasDockRect && PtInRect(&dockRect, cursor) != FALSE;
    PollFences(cursor, now, insideDock);

    if (!IsWindowVisible(window_)) {
        hoveredCategory_.reset();
        pointerOutsideSince_ = 0;
        if (!atRightEdge) {
            return;
        }
        panelMonitor_ = cursorMonitor;
        ShowEdgePanel(false);
        return;
    }

    std::optional<std::size_t> hoveredCategory;
    if (categoryList_ != nullptr) {
        POINT listPoint = cursor;
        ScreenToClient(categoryList_, &listPoint);
        hoveredCategory = CategoryIndexFromListPoint(listPoint);
    }
    if (hoveredCategory != hoveredCategory_) {
        hoveredCategory_ = hoveredCategory;
        if (categoryList_ != nullptr) {
            InvalidateRect(categoryList_, nullptr, FALSE);
        }
    }

    if (pinned_) {
        pointerOutsideSince_ = 0;
        UpdatePollTimerInterval();
        return;
    }
    if (interactionDepth_ > 0 || IsInteractionBlocked() ||
        GetCapture() != nullptr) {
        pointerOutsideSince_ = 0;
        return;
    }

    if (atRightEdge && panelMonitor_ != nullptr && cursorMonitor != panelMonitor_) {
        HideToTray();
        panelMonitor_ = cursorMonitor;
        ShowEdgePanel(false);
        return;
    }

    const bool insidePanel = hasDockRect && PtInRect(&dockRect, cursor) != FALSE;
    const bool atPanelRightEdge = atRightEdge && cursorMonitor == panelMonitor_;
    if (insidePanel || atPanelRightEdge) {
        pointerOutsideSince_ = 0;
        return;
    }

    if (now < keepVisibleUntil_) {
        pointerOutsideSince_ = 0;
        return;
    }

    if (pointerOutsideSince_ == 0) {
        pointerOutsideSince_ = now;
    } else if (now - pointerOutsideSince_ >= kAutoHideDelayMilliseconds) {
        HideToTray();
    }
}

void Application::ShowEdgePanel(bool activate) {
    if (window_ == nullptr || !IsWindow(window_)) {
        return;
    }

    POINT cursor{};
    if (!GetCursorPos(&cursor)) {
        cursor = POINT{0, 0};
    }
    HMONITOR monitor = panelMonitor_;
    if (monitor != nullptr) {
        MONITORINFO existingMonitorInfo{};
        existingMonitorInfo.cbSize = sizeof(existingMonitorInfo);
        if (!GetMonitorInfoW(monitor, &existingMonitorInfo)) {
            monitor = nullptr;
            panelMonitor_ = nullptr;
        }
    }
    if (monitor == nullptr) {
        monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    }
    if (monitor == nullptr) {
        monitor = MonitorFromWindow(window_, MONITOR_DEFAULTTOPRIMARY);
    }
    PositionEdgePanel(monitor);

    if (railInputWindow_ != nullptr) {
        ShowWindow(railInputWindow_, SW_SHOWNOACTIVATE);
        SetWindowPos(railInputWindow_, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    ShowWindow(window_, activate ? SW_SHOW : SW_SHOWNOACTIVATE);
    SetWindowPos(window_, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW |
                     (activate ? 0 : SWP_NOACTIVATE));
    if (activate && IsWindowEnabled(window_)) {
        SetForegroundWindow(window_);
    }
    UpdateWindow(window_);
    pointerOutsideSince_ = 0;
    keepVisibleUntil_ = activate ? GetTickCount64() + kActivatedKeepVisibleMilliseconds : 0;
    UpdatePollTimerInterval();
}

void Application::PositionEdgePanel(HMONITOR monitor) {
    if (monitor == nullptr || window_ == nullptr) {
        return;
    }

    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!GetMonitorInfoW(monitor, &monitorInfo)) {
        return;
    }

    panelMonitor_ = monitor;
    const int workWidth = monitorInfo.rcWork.right - monitorInfo.rcWork.left;
    const int workHeight = monitorInfo.rcWork.bottom - monitorInfo.rcWork.top;
    int panelWidth = std::min(
        workWidth, ScaleForDpi(kCollapsedPanelLogicalWidth, dpi_));
    SetWindowPos(window_, HWND_TOPMOST, monitorInfo.rcWork.right - panelWidth,
                 monitorInfo.rcWork.top, panelWidth, workHeight,
                 SWP_NOACTIVATE);

    const UINT actualDpi = GetDpiForWindow(window_);
    if (actualDpi != 0 && actualDpi != dpi_) {
        dpi_ = actualDpi;
        RecreateFonts();
        ApplyFonts();
        panelWidth = std::min(
            workWidth, ScaleForDpi(kCollapsedPanelLogicalWidth, dpi_));
        SetWindowPos(window_, HWND_TOPMOST, monitorInfo.rcWork.right - panelWidth,
                     monitorInfo.rcWork.top, panelWidth, workHeight,
                     SWP_NOACTIVATE);
    }
    RECT client{};
    GetClientRect(window_, &client);
    LayoutControls(client.right, client.bottom);
}

void Application::PositionRailInputWindow() {
    if (railInputWindow_ == nullptr || window_ == nullptr) {
        return;
    }

    RECT client{};
    GetClientRect(window_, &client);
    const RECT panel = RailPanelRect(client.right, client.bottom);
    POINT origin{panel.left, panel.top};
    ClientToScreen(window_, &origin);
    SetWindowPos(railInputWindow_, window_, origin.x, origin.y,
                 panel.right - panel.left, panel.bottom - panel.top,
                 SWP_NOACTIVATE);
    InvalidateRect(railInputWindow_, nullptr, TRUE);
}

void Application::ResetEdgeTracking() {
    pointerOutsideSince_ = 0;
    keepVisibleUntil_ = 0;
}

HWND Application::DialogOwner() const {
    if (activeFence_ != nullptr && activeFence_->window != nullptr &&
        IsWindowVisible(activeFence_->window)) {
        return activeFence_->window;
    }
    for (const std::unique_ptr<FenceWindow>& fence : fences_) {
        if (fence != nullptr && fence->window != nullptr &&
            IsWindowVisible(fence->window)) {
            return fence->window;
        }
    }
    return window_;
}

bool Application::IsInteractionBlocked() const {
    const bool mainDisabled = window_ != nullptr && IsWindow(window_) &&
                              !IsWindowEnabled(window_);
    const bool fenceDisabled = std::any_of(
        fences_.begin(), fences_.end(),
        [](const std::unique_ptr<FenceWindow>& fence) {
            return fence != nullptr && fence->window != nullptr &&
                   IsWindowVisible(fence->window) && !IsWindowEnabled(fence->window);
        });
    return mainDisabled || fenceDisabled;
}

void Application::ShowTrayMenu() {
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }

    AppendMenuW(menu, MF_STRING, kTrayShow, L"显示 LightLaunch");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kTrayExit, L"退出");
    SetMenuDefaultItem(menu, kTrayShow, FALSE);

    POINT point{};
    GetCursorPos(&point);
    SetForegroundWindow(window_);
    TrackPopupMenuEx(menu, TPM_LEFTALIGN | TPM_RIGHTBUTTON,
                     point.x, point.y, window_, nullptr);
    DestroyMenu(menu);
    PostMessageW(window_, WM_NULL, 0, 0);
}

void Application::ExitApplication() {
    if (window_ == nullptr || !IsWindow(window_)) {
        return;
    }
    if (IsInteractionBlocked()) {
        ShowFromTray();
        MessageBeep(MB_ICONINFORMATION);
        return;
    }

    SaveState(false);
    DestroyWindow(window_);
}

void Application::RefreshCategories() {
    if (categoryList_ == nullptr) {
        return;
    }

    SendMessageW(categoryList_, WM_SETREDRAW, FALSE, 0);
    SendMessageW(categoryList_, LB_RESETCONTENT, 0, 0);
    const std::size_t rowCount =
        (state_.categories.size() + kCategoryColumns - 1) / kCategoryColumns;
    for (std::size_t row = 0; row < rowCount; ++row) {
        SendMessageW(categoryList_, LB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L""));
    }

    if (!state_.categories.empty()) {
        state_.selectedCategory = std::min(state_.selectedCategory, state_.categories.size() - 1);
        SendMessageW(categoryList_, LB_SETCURSEL,
                     IsFenceVisible(state_.selectedCategory)
                         ? static_cast<WPARAM>(state_.selectedCategory /
                                              kCategoryColumns)
                         : static_cast<WPARAM>(-1),
                     0);
    } else {
        state_.selectedCategory = 0;
        DestroyAllFences();
    }
    SendMessageW(categoryList_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(categoryList_, nullptr, TRUE);
    StartCategoryPreviewLoad();
    RefreshAllFences();
    if (window_ != nullptr && IsWindow(window_)) {
        if (IsWindowVisible(window_) && panelMonitor_ != nullptr) {
            PositionEdgePanel(panelMonitor_);
        }
        RECT client{};
        GetClientRect(window_, &client);
        LayoutControls(client.right, client.bottom);
    }
}

void Application::RefreshItems() {
    if (activeFence_ != nullptr) {
        RefreshFence(*activeFence_);
    } else {
        RefreshAllFences();
    }
}

void Application::RefreshAllFences() {
    for (const std::unique_ptr<FenceWindow>& fence : fences_) {
        if (fence != nullptr) {
            RefreshFence(*fence);
        }
    }
}

void Application::RefreshFence(FenceWindow& fence) {
    if (fence.itemList == nullptr ||
        fence.categoryIndex >= state_.categories.size()) {
        return;
    }

    CancelIconBatch(fence.iconLoadBatch);

    SendMessageW(fence.itemList, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(fence.itemList);

    if (fence.imageList != nullptr) {
        ListView_SetImageList(fence.itemList, nullptr, LVSIL_NORMAL);
        ImageList_Destroy(fence.imageList);
        fence.imageList = nullptr;
    }

    fence.iconLoadBatch = std::make_shared<IconLoadBatch>();

    const int iconSize = ScaleForDpi(58, fence.dpi);
    fence.imageList = ImageList_Create(iconSize, iconSize,
                                       ILC_COLOR32 | ILC_MASK, 8, 8);
    ListView_SetImageList(fence.itemList, fence.imageList, LVSIL_NORMAL);
    ListView_SetIconSpacing(fence.itemList, ScaleForDpi(104, fence.dpi),
                            ScaleForDpi(100, fence.dpi));

    int placeholderImage = -1;
    if (fence.imageList != nullptr) {
        placeholderImage = ImageList_AddIcon(
            fence.imageList, LoadIconW(nullptr, IDI_APPLICATION));
    }

    const Category& category = state_.categories[fence.categoryIndex];
    RefreshFenceBackground(fence);
    const std::wstring windowTitle = std::wstring(kContentWindowTitlePrefix) +
                                     category.name;
    SetWindowTextW(fence.window, windowTitle.c_str());
    for (std::size_t index = 0; index < category.items.size(); ++index) {
        const LaunchItem& item = category.items[index];
        LVITEMW listItem{};
        listItem.mask = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM;
        listItem.iItem = static_cast<int>(index);
        listItem.iImage = placeholderImage;
        listItem.lParam = static_cast<LPARAM>(index);
        listItem.pszText = const_cast<wchar_t*>(item.name.c_str());
        ListView_InsertItem(fence.itemList, &listItem);
    }

    SendMessageW(fence.itemList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(fence.itemList, nullptr, TRUE);
    InvalidateRect(fence.window, nullptr, FALSE);

    if (fence.imageList != nullptr && !category.items.empty()) {
        std::vector<std::wstring> targets;
        targets.reserve(category.items.size());
        for (const LaunchItem& item : category.items) {
            targets.push_back(item.target);
        }
        LoadIconsInBackground(std::move(targets), fence.iconLoadBatch,
                              iconDispatchContext_);
    }
}

void Application::StartCategoryPreviewLoad() {
    CancelPreviewIconBatch(previewIconLoadBatch_);
    ClearCategoryPreviewIcons();
    categoryPreviewIcons_.resize(state_.categories.size());
    for (auto& icons : categoryPreviewIcons_) {
        icons.fill(nullptr);
    }

    categoryPreviewRequested_.clear();
    categoryPreviewRequested_.resize(state_.categories.size());
    for (auto& requested : categoryPreviewRequested_) {
        requested.fill(false);
    }
    if (state_.categories.empty()) {
        return;
    }

    previewIconLoadBatch_ = std::make_shared<PreviewIconLoadBatch>();
    QueueVisibleCategoryPreviewLoad();
}

void Application::QueueVisibleCategoryPreviewLoad() {
    if (categoryList_ == nullptr || previewIconLoadBatch_ == nullptr ||
        state_.categories.empty()) {
        return;
    }

    std::vector<PreviewIconTarget> targets;
    const LRESULT topResult = SendMessageW(categoryList_, LB_GETTOPINDEX, 0, 0);
    const std::size_t topIndex = topResult == LB_ERR
                                     ? 0
                                     : static_cast<std::size_t>(topResult);
    const LRESULT heightResult = SendMessageW(categoryList_, LB_GETITEMHEIGHT, 0, 0);
    RECT clientRect{};
    GetClientRect(categoryList_, &clientRect);
    const int itemHeight = heightResult == LB_ERR || heightResult <= 0
                               ? ScaleForDpi(kCategoryCardLogicalHeight, dpi_)
                               : static_cast<int>(heightResult);
    const int clientHeight = static_cast<int>(clientRect.bottom - clientRect.top);
    const std::size_t visibleCount = clientHeight <= 0
                                         ? 8
                                         : static_cast<std::size_t>(clientHeight / itemHeight + 2);
    const std::size_t firstRow = topIndex == 0 ? 0 : topIndex - 1;
    const std::size_t firstIndex = firstRow * kCategoryColumns;
    const std::size_t endIndex = std::min(
        state_.categories.size(),
        (topIndex + visibleCount) * static_cast<std::size_t>(kCategoryColumns));

    for (std::size_t categoryIndex = firstIndex; categoryIndex < endIndex;
         ++categoryIndex) {
        const Category& category = state_.categories[categoryIndex];
        const std::size_t previewCount = std::min<std::size_t>(4, category.items.size());
        for (std::size_t slot = 0; slot < previewCount; ++slot) {
            if (categoryIndex >= categoryPreviewRequested_.size() ||
                categoryPreviewRequested_[categoryIndex][slot]) {
                continue;
            }
            categoryPreviewRequested_[categoryIndex][slot] = true;
            targets.push_back(PreviewIconTarget{
                categoryIndex, slot, category.items[slot].target});
        }
    }
    if (targets.empty()) {
        return;
    }

    LoadPreviewIconsInBackground(std::move(targets), previewIconLoadBatch_,
                                 iconDispatchContext_);
}

void Application::ClearCategoryPreviewIcons() {
    for (auto& icons : categoryPreviewIcons_) {
        for (HICON& icon : icons) {
            if (icon != nullptr) {
                DestroyIcon(icon);
                icon = nullptr;
            }
        }
    }
    categoryPreviewIcons_.clear();
    categoryPreviewRequested_.clear();
}

RECT Application::CategoryCellRect(const RECT& rowRect, int column) const {
    const int inset = ScaleForDpi(1, dpi_);
    const int gap = ScaleForDpi(2, dpi_);
    const int availableWidth = std::max(
        kCategoryColumns,
        static_cast<int>(rowRect.right - rowRect.left) - inset * 2 -
            gap * (kCategoryColumns - 1));
    const int cellWidth = availableWidth / kCategoryColumns;
    const int left = rowRect.left + inset + column * (cellWidth + gap);
    const int right = column == kCategoryColumns - 1
                          ? rowRect.right - inset
                          : left + cellWidth;
    return RECT{left, rowRect.top, right, rowRect.bottom};
}

std::optional<std::size_t> Application::CategoryIndexFromListPoint(
    POINT clientPoint) const {
    if (categoryList_ == nullptr || state_.categories.empty()) {
        return std::nullopt;
    }

    const LRESULT hit = SendMessageW(categoryList_, LB_ITEMFROMPOINT, 0,
                                     MAKELPARAM(clientPoint.x, clientPoint.y));
    if (HIWORD(hit) != 0) {
        return std::nullopt;
    }
    const int row = LOWORD(hit);
    RECT rowRect{};
    if (row < 0 ||
        SendMessageW(categoryList_, LB_GETITEMRECT, row,
                     reinterpret_cast<LPARAM>(&rowRect)) == LB_ERR ||
        PtInRect(&rowRect, clientPoint) == FALSE) {
        return std::nullopt;
    }

    for (int column = 0; column < kCategoryColumns; ++column) {
        const RECT cellRect = CategoryCellRect(rowRect, column);
        if (PtInRect(&cellRect, clientPoint) == FALSE) {
            continue;
        }
        const std::size_t categoryIndex =
            static_cast<std::size_t>(row) * kCategoryColumns + column;
        if (categoryIndex < state_.categories.size()) {
            return categoryIndex;
        }
        break;
    }
    return std::nullopt;
}

void Application::ActivateCategory(std::size_t categoryIndex) {
    if (categoryIndex >= state_.categories.size()) {
        return;
    }
    state_.selectedCategory = categoryIndex;
    if (categoryList_ != nullptr) {
        SendMessageW(categoryList_, LB_SETCURSEL,
                     static_cast<WPARAM>(categoryIndex / kCategoryColumns), 0);
    }
    OpenOrShowFence(categoryIndex);
    if (categoryList_ != nullptr) {
        InvalidateRect(categoryList_, nullptr, FALSE);
    }
}

void Application::DrawCategoryItem(const DRAWITEMSTRUCT& drawItem) const {
    if (drawItem.hDC == nullptr || drawItem.itemID == static_cast<UINT>(-1)) {
        return;
    }

    RECT rowRect = drawItem.rcItem;
    FillRect(drawItem.hDC, &rowRect,
             railBackgroundBrush_ != nullptr
                 ? railBackgroundBrush_
                 : static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

    const std::size_t firstCategory =
        static_cast<std::size_t>(drawItem.itemID) * kCategoryColumns;
    if (firstCategory >= state_.categories.size()) {
        return;
    }

    const bool disabled = (drawItem.itemState & ODS_DISABLED) != 0;
    HGDIOBJ previousFont = SelectObject(drawItem.hDC, bodyFont_);
    const int previousBackgroundMode = SetBkMode(drawItem.hDC, TRANSPARENT);

    for (int column = 0; column < kCategoryColumns; ++column) {
        const std::size_t categoryIndex = firstCategory + column;
        if (categoryIndex >= state_.categories.size()) {
            break;
        }

        const Category& category = state_.categories[categoryIndex];
        RECT cellRect = CategoryCellRect(rowRect, column);
        InflateRect(&cellRect, -ScaleForDpi(1, dpi_), -ScaleForDpi(2, dpi_));
        const bool selected = IsFenceVisible(categoryIndex);
        const bool hovered = hoveredCategory_.has_value() &&
                             *hoveredCategory_ == categoryIndex;

        const int folderLogicalSize = kCategoryPreviewFrameLogicalSize +
                                      (hovered ? 3 : selected ? 1 : 0);
        const int folderSize = std::min(
            ScaleForDpi(folderLogicalSize, dpi_),
            std::max(20, static_cast<int>(cellRect.right - cellRect.left) -
                             ScaleForDpi(10, dpi_)));
        const int folderLeft = (cellRect.left + cellRect.right - folderSize) / 2;
        const int folderTop = cellRect.top +
                              ScaleForDpi(hovered ? 0 : 2, dpi_);
        RECT shadowRect{folderLeft + ScaleForDpi(1, dpi_),
                        folderTop + ScaleForDpi(2, dpi_),
                        folderLeft + folderSize + ScaleForDpi(1, dpi_),
                        folderTop + folderSize + ScaleForDpi(2, dpi_)};
        RECT folderRect{folderLeft, folderTop,
                        folderLeft + folderSize, folderTop + folderSize};

        HBRUSH shadowBrush = CreateSolidBrush(RGB(172, 180, 175));
        HPEN shadowPen = CreatePen(PS_SOLID, 1, RGB(172, 180, 175));
        HGDIOBJ oldBrush = shadowBrush != nullptr
                               ? SelectObject(drawItem.hDC, shadowBrush)
                               : nullptr;
        HGDIOBJ oldPen = shadowPen != nullptr
                             ? SelectObject(drawItem.hDC, shadowPen)
                             : nullptr;
        const int folderRadius = ScaleForDpi(10, dpi_);
        RoundRect(drawItem.hDC, shadowRect.left, shadowRect.top,
                  shadowRect.right, shadowRect.bottom,
                  folderRadius, folderRadius);
        if (oldPen != nullptr) {
            SelectObject(drawItem.hDC, oldPen);
        }
        if (oldBrush != nullptr) {
            SelectObject(drawItem.hDC, oldBrush);
        }
        if (shadowPen != nullptr) {
            DeleteObject(shadowPen);
        }
        if (shadowBrush != nullptr) {
            DeleteObject(shadowBrush);
        }

        const COLORREF folderColor = selected ? RGB(252, 253, 251)
                                              : hovered ? RGB(247, 249, 247)
                                                        : RGB(241, 244, 241);
        const COLORREF folderBorder = selected ? RGB(169, 179, 173)
                                               : hovered ? RGB(190, 198, 193)
                                                         : RGB(209, 215, 211);
        HBRUSH folderBrush = CreateSolidBrush(folderColor);
        HPEN folderPen = CreatePen(PS_SOLID, std::max(1, ScaleForDpi(1, dpi_)),
                                   folderBorder);
        oldBrush = folderBrush != nullptr ? SelectObject(drawItem.hDC, folderBrush)
                                          : nullptr;
        oldPen = folderPen != nullptr ? SelectObject(drawItem.hDC, folderPen)
                                      : nullptr;
        RoundRect(drawItem.hDC, folderRect.left, folderRect.top,
                  folderRect.right, folderRect.bottom,
                  folderRadius, folderRadius);
        if (oldPen != nullptr) {
            SelectObject(drawItem.hDC, oldPen);
        }
        if (oldBrush != nullptr) {
            SelectObject(drawItem.hDC, oldBrush);
        }
        if (folderPen != nullptr) {
            DeleteObject(folderPen);
        }
        if (folderBrush != nullptr) {
            DeleteObject(folderBrush);
        }

        const std::size_t previewCount = std::min<std::size_t>(4, category.items.size());
        const int iconSize = std::min(
            ScaleForDpi(kCategoryPreviewLogicalSize + (hovered ? 1 : 0), dpi_),
            std::max(10, (folderSize - ScaleForDpi(10, dpi_)) / 2));
        const int iconGap = std::min(
            ScaleForDpi(kCategoryPreviewLogicalGap, dpi_),
            std::max(2, folderSize - iconSize * 2 - ScaleForDpi(8, dpi_)));
        const int gridWidth = iconSize * 2 + iconGap;
        const int gridHeight = iconSize * 2 + iconGap;
        const int gridX = folderRect.left + (folderSize - gridWidth) / 2;
        const int gridY = folderRect.top + (folderSize - gridHeight) / 2;
        HICON placeholder = LoadIconW(nullptr, IDI_APPLICATION);
        for (std::size_t slot = 0; slot < previewCount; ++slot) {
            HICON icon = placeholder;
            if (categoryIndex < categoryPreviewIcons_.size() &&
                categoryPreviewIcons_[categoryIndex][slot] != nullptr) {
                icon = categoryPreviewIcons_[categoryIndex][slot];
            }
            const int previewColumn = static_cast<int>(slot % 2);
            const int previewRow = static_cast<int>(slot / 2);
            DrawIconEx(drawItem.hDC,
                       gridX + previewColumn * (iconSize + iconGap),
                       gridY + previewRow * (iconSize + iconGap),
                       icon, iconSize, iconSize, 0, nullptr, DI_NORMAL);
        }

        if (previewCount == 0) {
            const int placeholderRadius = std::max(2, ScaleForDpi(3, dpi_));
            HBRUSH emptyBrush = CreateSolidBrush(RGB(215, 221, 217));
            HPEN emptyPen = CreatePen(PS_SOLID, 1, RGB(190, 199, 193));
            oldBrush = emptyBrush != nullptr ? SelectObject(drawItem.hDC, emptyBrush)
                                             : nullptr;
            oldPen = emptyPen != nullptr ? SelectObject(drawItem.hDC, emptyPen)
                                         : nullptr;
            for (int slot = 0; slot < 4; ++slot) {
                const int previewColumn = slot % 2;
                const int previewRow = slot / 2;
                const int left = gridX + previewColumn * (iconSize + iconGap);
                const int top = gridY + previewRow * (iconSize + iconGap);
                RoundRect(drawItem.hDC, left, top, left + iconSize,
                          top + iconSize, placeholderRadius,
                          placeholderRadius);
            }
            if (oldPen != nullptr) {
                SelectObject(drawItem.hDC, oldPen);
            }
            if (oldBrush != nullptr) {
                SelectObject(drawItem.hDC, oldBrush);
            }
            if (emptyPen != nullptr) {
                DeleteObject(emptyPen);
            }
            if (emptyBrush != nullptr) {
                DeleteObject(emptyBrush);
            }
        }

        RECT labelRect{cellRect.left,
                       folderRect.bottom + ScaleForDpi(3, dpi_),
                       cellRect.right,
                       cellRect.bottom};
        const COLORREF oldTextColor = SetTextColor(
            drawItem.hDC,
            disabled ? GetSysColor(COLOR_GRAYTEXT)
                     : selected ? RGB(22, 27, 24) : RGB(48, 54, 50));
        DrawTextW(drawItem.hDC, category.name.c_str(),
                  static_cast<int>(category.name.size()), &labelRect,
                  DT_CENTER | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS |
                      DT_NOPREFIX);
        SetTextColor(drawItem.hDC, oldTextColor);

        if (selected) {
            const int indicatorSize = std::max(3, ScaleForDpi(4, dpi_));
            const int indicatorX = cellRect.left + ScaleForDpi(2, dpi_);
            const int indicatorY = folderRect.top +
                                   (folderRect.bottom - folderRect.top -
                                    indicatorSize) /
                                       2;
            HBRUSH indicatorBrush = CreateSolidBrush(RGB(67, 75, 70));
            HPEN indicatorPen = CreatePen(PS_SOLID, 1, RGB(67, 75, 70));
            oldBrush = indicatorBrush != nullptr
                           ? SelectObject(drawItem.hDC, indicatorBrush)
                           : nullptr;
            oldPen = indicatorPen != nullptr
                         ? SelectObject(drawItem.hDC, indicatorPen)
                         : nullptr;
            Ellipse(drawItem.hDC, indicatorX, indicatorY,
                    indicatorX + indicatorSize, indicatorY + indicatorSize);
            if (oldPen != nullptr) {
                SelectObject(drawItem.hDC, oldPen);
            }
            if (oldBrush != nullptr) {
                SelectObject(drawItem.hDC, oldBrush);
            }
            if (indicatorPen != nullptr) {
                DeleteObject(indicatorPen);
            }
            if (indicatorBrush != nullptr) {
                DeleteObject(indicatorBrush);
            }
        }

        if (categoryDragging_ && categoryDropTarget_.has_value() &&
            *categoryDropTarget_ == categoryIndex) {
            RECT dropRect = cellRect;
            InflateRect(&dropRect, -ScaleForDpi(2, dpi_),
                        -ScaleForDpi(2, dpi_));
            HPEN dropPen = CreatePen(PS_SOLID,
                                     std::max(2, ScaleForDpi(2, dpi_)),
                                     RGB(74, 104, 88));
            HGDIOBJ dropOldPen = dropPen != nullptr
                                     ? SelectObject(drawItem.hDC, dropPen)
                                     : nullptr;
            HGDIOBJ dropOldBrush =
                SelectObject(drawItem.hDC, GetStockObject(HOLLOW_BRUSH));
            RoundRect(drawItem.hDC, dropRect.left, dropRect.top,
                      dropRect.right, dropRect.bottom,
                      ScaleForDpi(10, dpi_), ScaleForDpi(10, dpi_));
            SelectObject(drawItem.hDC, dropOldBrush);
            if (dropOldPen != nullptr) {
                SelectObject(drawItem.hDC, dropOldPen);
            }
            if (dropPen != nullptr) {
                DeleteObject(dropPen);
            }
        }

        if ((drawItem.itemState & ODS_FOCUS) != 0 && selected) {
            RECT focusRect = cellRect;
            InflateRect(&focusRect, -ScaleForDpi(2, dpi_), -ScaleForDpi(1, dpi_));
            DrawFocusRect(drawItem.hDC, &focusRect);
        }
    }

    SetBkMode(drawItem.hDC, previousBackgroundMode);
    SelectObject(drawItem.hDC, previousFont);
}

Category* Application::CurrentCategory() {
    const std::size_t categoryIndex = activeFence_ != nullptr
                                          ? activeFence_->categoryIndex
                                          : state_.selectedCategory;
    if (state_.categories.empty() || categoryIndex >= state_.categories.size()) {
        return nullptr;
    }
    return &state_.categories[categoryIndex];
}

const Category* Application::CurrentCategory() const {
    const std::size_t categoryIndex = activeFence_ != nullptr
                                          ? activeFence_->categoryIndex
                                          : state_.selectedCategory;
    if (state_.categories.empty() || categoryIndex >= state_.categories.size()) {
        return nullptr;
    }
    return &state_.categories[categoryIndex];
}

int Application::SelectedItemIndex() const {
    if (activeFence_ == nullptr || activeFence_->itemList == nullptr) {
        return -1;
    }
    return ListView_GetNextItem(activeFence_->itemList, -1, LVNI_SELECTED);
}

void Application::AddCategory() {
    if (state_.categories.size() >= kMaximumCategories) {
        MessageBoxW(window_, L"分组数量已达到上限。", L"LightLaunch",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }

    const auto name = PromptForText(window_, instance_, L"新建分组", L"分组名称：");
    if (!name.has_value()) {
        return;
    }

    const bool duplicate = std::any_of(state_.categories.begin(), state_.categories.end(),
                                       [&](const Category& category) {
                                           return EqualsIgnoreCase(category.name, *name);
                                       });
    if (duplicate) {
        MessageBoxW(window_, L"已经存在同名分组。", L"LightLaunch",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }

    const std::size_t previousSelection = state_.selectedCategory;
    state_.categories.push_back(Category{*name, {}});
    state_.selectedCategory = state_.categories.size() - 1;
    if (!SaveState()) {
        state_.categories.pop_back();
        state_.selectedCategory = previousSelection;
        return;
    }
    RefreshCategories();
}

void Application::RenameCategory(std::size_t categoryIndex) {
    if (categoryIndex >= state_.categories.size()) {
        return;
    }
    Category& category = state_.categories[categoryIndex];

    const auto name = PromptForText(window_, instance_, L"重命名分组", L"新的分组名称：",
                                    category.name);
    if (!name.has_value() || *name == category.name) {
        return;
    }

    const bool duplicate = std::any_of(state_.categories.begin(), state_.categories.end(),
                                       [&](const Category& candidate) {
                                           return &candidate != &category &&
                                                  EqualsIgnoreCase(candidate.name, *name);
                                       });
    if (duplicate) {
        MessageBoxW(window_, L"已经存在同名分组。", L"LightLaunch",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }

    const std::wstring previousName = category.name;
    category.name = *name;
    if (!SaveState()) {
        category.name = previousName;
        return;
    }
    RefreshCategories();
}

void Application::DeleteCategory(std::size_t categoryIndex) {
    if (categoryIndex >= state_.categories.size()) {
        return;
    }
    const Category& category = state_.categories[categoryIndex];

    const std::wstring message = L"确定删除分组“" + category.name + L"”及其中的 " +
                                 std::to_wstring(category.items.size()) +
                                 L" 个启动项吗？\n\n原始应用、快捷方式和文件夹不会被删除。";
    if (MessageBoxW(window_, message.c_str(), L"删除分组",
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
        return;
    }

    const std::size_t previousSelection = state_.selectedCategory;
    Category deleted = std::move(state_.categories[categoryIndex]);
    state_.categories.erase(state_.categories.begin() +
                            static_cast<std::ptrdiff_t>(categoryIndex));
    if (state_.categories.empty()) {
        state_.selectedCategory = 0;
    } else if (categoryIndex < previousSelection) {
        state_.selectedCategory = previousSelection - 1;
    } else if (categoryIndex == previousSelection) {
        state_.selectedCategory = std::min(categoryIndex, state_.categories.size() - 1);
    } else {
        state_.selectedCategory = previousSelection;
    }
    if (!SaveState()) {
        state_.categories.insert(state_.categories.begin() +
                                     static_cast<std::ptrdiff_t>(categoryIndex),
                                 std::move(deleted));
        state_.selectedCategory = previousSelection;
        return;
    }
    RemoveFenceForCategory(categoryIndex);
    RefreshCategories();
}

void Application::ReorderCategory(std::size_t sourceIndex,
                                  std::size_t destinationIndex) {
    if (sourceIndex >= state_.categories.size() ||
        destinationIndex >= state_.categories.size() ||
        sourceIndex == destinationIndex) {
        return;
    }

    const AppState previousState = state_;
    MoveVectorElement(state_.categories, sourceIndex, destinationIndex);
    state_.selectedCategory = RemapIndexAfterMove(
        state_.selectedCategory, sourceIndex, destinationIndex);

    if (!SaveState()) {
        state_ = previousState;
        return;
    }

    for (const std::unique_ptr<FenceWindow>& fence : fences_) {
        if (fence != nullptr) {
            fence->categoryIndex = RemapIndexAfterMove(
                fence->categoryIndex, sourceIndex, destinationIndex);
        }
    }
    RefreshCategories();
}

void Application::AddApplication() {
    const auto target = PickApplication();
    if (target.has_value()) {
        AddTarget(*target);
    }
}

void Application::AddFolder() {
    const auto target = PickFolder();
    if (target.has_value()) {
        AddTarget(*target);
    }
}

void Application::AddTarget(const std::wstring& target) {
    Category* category = CurrentCategory();
    if (category == nullptr) {
        MessageBoxW(DialogOwner(), L"请先创建一个分组。", L"LightLaunch",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }

    const bool duplicate = std::any_of(category->items.begin(), category->items.end(),
                                       [&](const LaunchItem& item) {
                                           return EqualsIgnoreCase(item.target, target);
                                       });
    if (duplicate) {
        MessageBoxW(DialogOwner(), L"当前分组中已经存在这个项目。", L"LightLaunch",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }

    if (category->items.size() >= kMaximumItemsPerCategory) {
        MessageBoxW(DialogOwner(), L"当前分组的启动项数量已达到上限。", L"LightLaunch",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }

    LaunchItem item;
    item.name = DisplayNameForTarget(target);
    item.target = target;
    item.workingDirectory = DirectoryForTarget(target);
    category->items.push_back(std::move(item));
    if (!SaveState()) {
        category->items.pop_back();
        return;
    }
    RefreshCategories();
}

void Application::AddTargets(const std::vector<std::wstring>& targets) {
    Category* category = CurrentCategory();
    if (category == nullptr) {
        MessageBoxW(DialogOwner(), L"请先创建一个分组。", L"LightLaunch",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }

    const std::size_t originalSize = category->items.size();
    bool capacityReached = false;
    for (const std::wstring& target : targets) {
        if (target.empty()) {
            continue;
        }
        const bool duplicate = std::any_of(category->items.begin(), category->items.end(),
                                           [&](const LaunchItem& item) {
                                               return EqualsIgnoreCase(item.target, target);
                                           });
        if (duplicate) {
            continue;
        }
        if (category->items.size() >= kMaximumItemsPerCategory) {
            capacityReached = true;
            break;
        }

        LaunchItem item;
        item.name = DisplayNameForTarget(target);
        item.target = target;
        item.workingDirectory = DirectoryForTarget(target);
        category->items.push_back(std::move(item));
    }

    if (category->items.size() == originalSize) {
        if (capacityReached) {
            MessageBoxW(DialogOwner(), L"当前分组的启动项数量已达到上限。", L"LightLaunch",
                        MB_OK | MB_ICONINFORMATION);
        }
        return;
    }

    if (!SaveState()) {
        category->items.resize(originalSize);
        return;
    }
    RefreshCategories();
    if (capacityReached) {
        MessageBoxW(DialogOwner(), L"已添加可容纳的项目，其余项目因达到上限而跳过。",
                    L"LightLaunch", MB_OK | MB_ICONINFORMATION);
    }
}

void Application::HandleDroppedFiles(HDROP drop) {
    if (drop == nullptr) {
        return;
    }

    const DropHandleGuard finishDrop(drop);
    ++interactionDepth_;
    std::vector<std::wstring> targets;
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    targets.reserve(count);
    for (UINT index = 0; index < count; ++index) {
        const UINT length = DragQueryFileW(drop, index, nullptr, 0);
        if (length == 0) {
            continue;
        }
        std::vector<wchar_t> buffer(static_cast<std::size_t>(length) + 1, L'\0');
        if (DragQueryFileW(drop, index, buffer.data(),
                           static_cast<UINT>(buffer.size())) != 0) {
            targets.emplace_back(buffer.data());
        }
    }
    interactionDepth_ = std::max(0, interactionDepth_ - 1);
    keepVisibleUntil_ = GetTickCount64() + kInteractionKeepVisibleMilliseconds;
    pointerOutsideSince_ = 0;
    if (activeFence_ != nullptr) {
        activeFence_->keepVisibleUntil = GetTickCount64() +
                                         kInteractionKeepVisibleMilliseconds;
        activeFence_->pointerOutsideSince = 0;
    }

    if (!targets.empty()) {
        AddTargets(targets);
    }
}

void Application::RenameSelectedItem() {
    Category* category = CurrentCategory();
    const int selected = SelectedItemIndex();
    if (category == nullptr || selected < 0 ||
        static_cast<std::size_t>(selected) >= category->items.size()) {
        return;
    }

    LaunchItem& item = category->items[static_cast<std::size_t>(selected)];
    const auto name = PromptForText(DialogOwner(), instance_, L"重命名启动项", L"显示名称：",
                                    item.name);
    if (!name.has_value()) {
        return;
    }
    const std::wstring previousName = item.name;
    item.name = *name;
    if (!SaveState()) {
        item.name = previousName;
        return;
    }
    RefreshItems();
}

void Application::RemoveSelectedItem() {
    Category* category = CurrentCategory();
    const int selected = SelectedItemIndex();
    if (category == nullptr || selected < 0 ||
        static_cast<std::size_t>(selected) >= category->items.size()) {
        return;
    }

    const LaunchItem& item = category->items[static_cast<std::size_t>(selected)];
    const std::wstring message = L"从当前分组移除“" + item.name +
                                 L"”吗？\n\n原始文件不会被删除。";
    if (MessageBoxW(DialogOwner(), message.c_str(), L"移除启动项",
                    MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
        return;
    }

    LaunchItem removed = std::move(category->items[static_cast<std::size_t>(selected)]);
    category->items.erase(category->items.begin() + selected);
    if (!SaveState()) {
        category->items.insert(category->items.begin() + selected, std::move(removed));
        return;
    }
    RefreshCategories();
}

void Application::MoveSelectedItem(std::size_t destinationCategory) {
    Category* source = CurrentCategory();
    const int selected = SelectedItemIndex();
    const std::size_t sourceCategory = activeFence_ != nullptr
                                           ? activeFence_->categoryIndex
                                           : state_.selectedCategory;
    if (source == nullptr || selected < 0 || destinationCategory >= state_.categories.size() ||
        destinationCategory == sourceCategory ||
        static_cast<std::size_t>(selected) >= source->items.size()) {
        return;
    }

    Category& destination = state_.categories[destinationCategory];
    const LaunchItem& selectedItem = source->items[static_cast<std::size_t>(selected)];
    const bool duplicate = std::any_of(destination.items.begin(), destination.items.end(),
                                       [&](const LaunchItem& item) {
                                           return EqualsIgnoreCase(item.target,
                                                                   selectedItem.target);
                                       });
    if (duplicate) {
        MessageBoxW(DialogOwner(), L"目标分组中已经存在这个项目。", L"LightLaunch",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (destination.items.size() >= kMaximumItemsPerCategory) {
        MessageBoxW(DialogOwner(), L"目标分组的启动项数量已达到上限。", L"LightLaunch",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }

    LaunchItem item = std::move(source->items[static_cast<std::size_t>(selected)]);
    source->items.erase(source->items.begin() + selected);
    destination.items.push_back(std::move(item));
    if (!SaveState()) {
        LaunchItem restored = std::move(destination.items.back());
        destination.items.pop_back();
        source->items.insert(source->items.begin() + selected, std::move(restored));
        return;
    }
    RefreshCategories();
}

void Application::ReorderItem(FenceWindow& fence, std::size_t sourceIndex,
                              std::size_t destinationIndex) {
    if (fence.categoryIndex >= state_.categories.size()) {
        return;
    }
    Category& category = state_.categories[fence.categoryIndex];
    if (sourceIndex >= category.items.size() ||
        destinationIndex >= category.items.size() ||
        sourceIndex == destinationIndex) {
        return;
    }

    const std::vector<LaunchItem> previousItems = category.items;
    MoveVectorElement(category.items, sourceIndex, destinationIndex);
    if (!SaveState()) {
        category.items = previousItems;
        return;
    }

    RefreshFence(fence);
    ListView_SetItemState(fence.itemList, static_cast<int>(destinationIndex),
                          LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(fence.itemList, static_cast<int>(destinationIndex), FALSE);
    StartCategoryPreviewLoad();
    if (categoryList_ != nullptr) {
        InvalidateRect(categoryList_, nullptr, FALSE);
    }
}

void Application::LaunchSelectedItem() {
    LaunchItemAt(SelectedItemIndex());
}

void Application::LaunchItemAt(int itemIndex) {
    const Category* category = CurrentCategory();
    if (category == nullptr || itemIndex < 0 ||
        static_cast<std::size_t>(itemIndex) >= category->items.size()) {
        return;
    }

    const LaunchItem& item = category->items[static_cast<std::size_t>(itemIndex)];
    const ULONGLONG now = GetTickCount64();
    if (EqualsIgnoreCase(lastLaunchTarget_, item.target) &&
        lastLaunchArguments_ == item.arguments &&
        EqualsIgnoreCase(lastLaunchWorkingDirectory_, item.workingDirectory) &&
        now - lastLaunchTick_ < 600) {
        return;
    }

    SHELLEXECUTEINFOW execute{};
    execute.cbSize = sizeof(execute);
    execute.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_ASYNCOK;
    execute.hwnd = DialogOwner();
    execute.lpVerb = L"open";
    execute.lpFile = item.target.c_str();
    execute.lpParameters = item.arguments.empty() ? nullptr : item.arguments.c_str();
    execute.lpDirectory = item.workingDirectory.empty() ? nullptr : item.workingDirectory.c_str();
    execute.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&execute)) {
        const DWORD error = GetLastError();
        const std::wstring message = L"启动失败：\n\n" + item.target + L"\n\n" +
                                     WindowsErrorMessage(error);
        MessageBoxW(DialogOwner(), message.c_str(), L"无法启动", MB_OK | MB_ICONERROR);
        return;
    }
    lastLaunchTarget_ = item.target;
    lastLaunchArguments_ = item.arguments;
    lastLaunchWorkingDirectory_ = item.workingDirectory;
    lastLaunchTick_ = now;
    if (activeFence_ != nullptr && !activeFence_->pinned) {
        HideFence(*activeFence_);
    }
    if (!pinned_) {
        HideToTray();
    }
}

void Application::OpenSelectedItemLocation() {
    const Category* category = CurrentCategory();
    const int selected = SelectedItemIndex();
    if (category == nullptr || selected < 0 ||
        static_cast<std::size_t>(selected) >= category->items.size()) {
        return;
    }

    const std::wstring& target = category->items[static_cast<std::size_t>(selected)].target;
    const DWORD attributes = GetFileAttributesW(target.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(DialogOwner(), L"目标不存在，无法打开所在位置。", L"LightLaunch",
                    MB_OK | MB_ICONWARNING);
        return;
    }

    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        ShellExecuteW(DialogOwner(), L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }

    const std::wstring parameters = L"/select,\"" + target + L"\"";
    ShellExecuteW(DialogOwner(), L"open", L"explorer.exe", parameters.c_str(), nullptr,
                  SW_SHOWNORMAL);
}

void Application::ShowItemContextMenu(int itemIndex) {
    if (itemIndex < 0) {
        return;
    }

    HMENU menu = CreatePopupMenu();
    HMENU moveMenu = CreatePopupMenu();
    if (menu == nullptr || moveMenu == nullptr) {
        if (menu != nullptr) {
            DestroyMenu(menu);
        }
        if (moveMenu != nullptr) {
            DestroyMenu(moveMenu);
        }
        return;
    }

    AppendMenuW(menu, MF_STRING | MF_DEFAULT, kMenuLaunch, L"打开");
    AppendMenuW(menu, MF_STRING, kMenuRename, L"重命名");
    AppendMenuW(menu, MF_STRING, kMenuOpenLocation, L"打开所在位置");

    bool hasMoveDestination = false;
    const std::size_t sourceCategory = activeFence_ != nullptr
                                           ? activeFence_->categoryIndex
                                           : state_.selectedCategory;
    for (std::size_t index = 0; index < state_.categories.size(); ++index) {
        if (index == sourceCategory || index >= 800) {
            continue;
        }
        AppendMenuW(moveMenu, MF_STRING, kMenuMoveBase + static_cast<UINT>(index),
                    state_.categories[index].name.c_str());
        hasMoveDestination = true;
    }
    if (hasMoveDestination) {
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(moveMenu), L"移动到");
    } else {
        DestroyMenu(moveMenu);
        moveMenu = nullptr;
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuRemove, L"从分组中移除");

    POINT point{};
    GetCursorPos(&point);
    const HWND owner = DialogOwner();
    SetForegroundWindow(owner);
    ++interactionDepth_;
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                        point.x, point.y, 0, owner, nullptr);
    interactionDepth_ = std::max(0, interactionDepth_ - 1);
    keepVisibleUntil_ = GetTickCount64() + kInteractionKeepVisibleMilliseconds;
    pointerOutsideSince_ = 0;
    if (activeFence_ != nullptr) {
        activeFence_->keepVisibleUntil = GetTickCount64() +
                                         kInteractionKeepVisibleMilliseconds;
        activeFence_->pointerOutsideSince = 0;
    }
    DestroyMenu(menu);
    PostMessageW(owner, WM_NULL, 0, 0);

    switch (command) {
        case kMenuLaunch:
            LaunchItemAt(itemIndex);
            break;
        case kMenuRename:
            RenameSelectedItem();
            break;
        case kMenuRemove:
            RemoveSelectedItem();
            break;
        case kMenuOpenLocation:
            OpenSelectedItemLocation();
            break;
        default:
            if (command >= kMenuMoveBase && command < kMenuMoveBase + state_.categories.size()) {
                MoveSelectedItem(command - kMenuMoveBase);
            }
            break;
    }
}

void Application::ShowContentContextMenu() {
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }

    if (CurrentCategory() == nullptr) {
        AppendMenuW(menu, MF_STRING, kMenuCategoryNew, L"新建分组...");
    } else {
        AppendMenuW(menu, MF_STRING, kMenuContentAddTarget, L"添加应用或文件...");
        AppendMenuW(menu, MF_STRING, kMenuContentAddFolder, L"添加文件夹...");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kMenuFenceBackgroundSettings,
                    L"围栏背景设置...");
        const UINT clearFlags = CurrentCategory()->background.imagePath.empty()
                                    ? MF_STRING | MF_GRAYED
                                    : MF_STRING;
        AppendMenuW(menu, clearFlags, kMenuFenceBackgroundClear,
                    L"清除背景图片");
    }

    POINT point{};
    GetCursorPos(&point);
    const HWND owner = DialogOwner();
    SetForegroundWindow(owner);
    ++interactionDepth_;
    const UINT command = TrackPopupMenu(menu,
                                        TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                        point.x, point.y, 0, owner, nullptr);
    interactionDepth_ = std::max(0, interactionDepth_ - 1);
    keepVisibleUntil_ = GetTickCount64() + kInteractionKeepVisibleMilliseconds;
    pointerOutsideSince_ = 0;
    if (activeFence_ != nullptr) {
        activeFence_->keepVisibleUntil = GetTickCount64() +
                                         kInteractionKeepVisibleMilliseconds;
        activeFence_->pointerOutsideSince = 0;
    }
    DestroyMenu(menu);
    PostMessageW(owner, WM_NULL, 0, 0);

    switch (command) {
        case kMenuCategoryNew:
            AddCategory();
            break;
        case kMenuContentAddTarget:
            AddApplication();
            break;
        case kMenuContentAddFolder:
            AddFolder();
            break;
        case kMenuFenceBackgroundSettings:
            ShowFenceBackgroundSettings();
            break;
        case kMenuFenceBackgroundClear:
            ClearFenceBackground();
            break;
        default:
            break;
    }
}

void Application::ShowCategoryContextMenu(POINT screenPoint) {
    if (categoryList_ == nullptr || !IsWindow(categoryList_)) {
        return;
    }

    int categoryIndex = -1;
    if (screenPoint.x == -1 && screenPoint.y == -1) {
        if (!state_.categories.empty()) {
            categoryIndex = static_cast<int>(std::min(
                state_.selectedCategory, state_.categories.size() - 1));
            const LRESULT selected = static_cast<LRESULT>(
                static_cast<std::size_t>(categoryIndex) / kCategoryColumns);
            RECT itemRect{};
            if (SendMessageW(categoryList_, LB_GETITEMRECT, selected,
                             reinterpret_cast<LPARAM>(&itemRect)) != LB_ERR) {
                const RECT cellRect = CategoryCellRect(
                    itemRect, categoryIndex % kCategoryColumns);
                screenPoint = POINT{(cellRect.left + cellRect.right) / 2,
                                    (cellRect.top + cellRect.bottom) / 2};
                ClientToScreen(categoryList_, &screenPoint);
            }
        }
    } else {
        POINT clientPoint = screenPoint;
        ScreenToClient(categoryList_, &clientPoint);
        const std::optional<std::size_t> candidate =
            CategoryIndexFromListPoint(clientPoint);
        if (candidate.has_value()) {
            categoryIndex = static_cast<int>(*candidate);
        }
    }

    if (screenPoint.x == -1 && screenPoint.y == -1) {
        RECT listRect{};
        GetWindowRect(categoryList_, &listRect);
        screenPoint = POINT{listRect.left + ScaleForDpi(12, dpi_),
                            listRect.top + ScaleForDpi(12, dpi_)};
    }

    if (categoryIndex >= 0) {
        SendMessageW(categoryList_, LB_SETCURSEL,
                     categoryIndex / kCategoryColumns, 0);
        InvalidateRect(categoryList_, nullptr, FALSE);
    }

    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        SendMessageW(categoryList_, LB_SETCURSEL,
                     IsFenceVisible(state_.selectedCategory)
                         ? static_cast<WPARAM>(state_.selectedCategory /
                                              kCategoryColumns)
                         : static_cast<WPARAM>(-1),
                     0);
        return;
    }
    if (categoryIndex >= 0) {
        AppendMenuW(menu, MF_STRING, kMenuCategoryRename, L"重命名分组...");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kMenuCategoryDelete, L"删除分组");
    } else {
        AppendMenuW(menu, MF_STRING, kMenuCategoryNew, L"新建分组...");
    }

    SetForegroundWindow(window_);
    ++interactionDepth_;
    const UINT command = TrackPopupMenu(menu,
                                        TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                        screenPoint.x, screenPoint.y, 0, window_, nullptr);
    interactionDepth_ = std::max(0, interactionDepth_ - 1);
    keepVisibleUntil_ = GetTickCount64() + kInteractionKeepVisibleMilliseconds;
    pointerOutsideSince_ = 0;
    DestroyMenu(menu);
    PostMessageW(window_, WM_NULL, 0, 0);

    switch (command) {
        case kMenuCategoryNew:
            AddCategory();
            break;
        case kMenuCategoryRename:
            RenameCategory(static_cast<std::size_t>(categoryIndex));
            break;
        case kMenuCategoryDelete:
            DeleteCategory(static_cast<std::size_t>(categoryIndex));
            break;
        default:
            break;
    }
    SendMessageW(categoryList_, LB_SETCURSEL,
                 IsFenceVisible(state_.selectedCategory)
                     ? static_cast<WPARAM>(state_.selectedCategory /
                                          kCategoryColumns)
                     : static_cast<WPARAM>(-1),
                 0);
    InvalidateRect(categoryList_, nullptr, FALSE);
}

std::optional<std::wstring> Application::PickApplication() const {
    std::vector<wchar_t> path(32768, L'\0');
    constexpr wchar_t filter[] =
        L"应用和快捷方式 (*.exe;*.lnk;*.bat;*.cmd;*.com;*.url)\0"
        L"*.exe;*.lnk;*.bat;*.cmd;*.com;*.url\0"
        L"所有文件 (*.*)\0*.*\0\0";

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = DialogOwner();
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = path.data();
    dialog.nMaxFile = static_cast<DWORD>(path.size());
    dialog.lpstrTitle = L"选择要添加的应用或快捷方式";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
                   OFN_EXPLORER | OFN_DONTADDTORECENT | OFN_NODEREFERENCELINKS;

    if (!GetOpenFileNameW(&dialog)) {
        const DWORD dialogError = CommDlgExtendedError();
        if (dialogError != 0) {
            const std::wstring message = L"文件选择窗口打开失败。错误代码：" +
                                         std::to_wstring(dialogError);
            MessageBoxW(DialogOwner(), message.c_str(), L"LightLaunch", MB_OK | MB_ICONERROR);
        }
        return std::nullopt;
    }
    return std::wstring(path.data());
}

std::optional<std::wstring> Application::PickFolder() const {
    BROWSEINFOW browse{};
    browse.hwndOwner = DialogOwner();
    browse.lpszTitle = L"选择要添加的文件夹";
    browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_NONEWFOLDERBUTTON;

    PIDLIST_ABSOLUTE itemId = SHBrowseForFolderW(&browse);
    if (itemId == nullptr) {
        return std::nullopt;
    }

    std::array<wchar_t, 32768> path{};
    const BOOL success = SHGetPathFromIDListW(itemId, path.data());
    CoTaskMemFree(itemId);
    if (!success) {
        return std::nullopt;
    }
    return std::wstring(path.data());
}

bool Application::SaveState(bool showError) const {
    if (config_.Save(state_)) {
        return true;
    }
    if (showError) {
        const std::wstring message = L"无法保存配置：\n\n" + config_.Path();
        MessageBoxW(DialogOwner(), message.c_str(), L"保存失败", MB_OK | MB_ICONERROR);
    }
    return false;
}

std::wstring Application::DisplayNameForTarget(const std::wstring& target) {
    std::wstring name = FileNameWithoutExtension(target);
    return name.empty() ? target : name;
}

std::wstring Application::DirectoryForTarget(const std::wstring& target) {
    const DWORD attributes = GetFileAttributesW(target.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return target;
    }

    const wchar_t* extension = PathFindExtensionW(target.c_str());
    if (extension != nullptr &&
        (_wcsicmp(extension, L".lnk") == 0 || _wcsicmp(extension, L".url") == 0)) {
        return {};
    }

    std::vector<wchar_t> path(target.begin(), target.end());
    path.push_back(L'\0');
    if (!PathRemoveFileSpecW(path.data())) {
        return {};
    }
    return path.data();
}

std::wstring Application::WindowsErrorMessage(DWORD errorCode) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, errorCode, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    if (length == 0 || buffer == nullptr) {
        return L"Windows 错误代码：" + std::to_wstring(errorCode);
    }

    std::wstring message(buffer, length);
    LocalFree(buffer);
    while (!message.empty() &&
           (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' ')) {
        message.pop_back();
    }
    return message;
}

bool Application::EqualsIgnoreCase(const std::wstring& left, const std::wstring& right) {
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}

}  // namespace lightlaunch
