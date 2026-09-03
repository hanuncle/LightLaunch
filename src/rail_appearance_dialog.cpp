#include "rail_appearance_dialog.h"

#include <commctrl.h>
#include <commdlg.h>

#include <algorithm>
#include <string>

namespace lightlaunch {
namespace {

constexpr wchar_t kDialogClass[] = L"LightLaunch.RailAppearanceDialog";
constexpr int kBackgroundSwatchId = 6201;
constexpr int kChooseBackgroundId = 6202;
constexpr int kTransparencySliderId = 6203;
constexpr int kTransparencyValueId = 6204;
constexpr int kBorderSwatchId = 6205;
constexpr int kChooseBorderId = 6206;

struct DialogState {
    HWND owner = nullptr;
    HINSTANCE instance = nullptr;
    HWND window = nullptr;
    HWND backgroundSwatch = nullptr;
    HWND transparencySlider = nullptr;
    HWND transparencyValue = nullptr;
    HWND borderSwatch = nullptr;
    HFONT font = nullptr;
    UINT dpi = 96;
    RailAppearance working;
    std::optional<RailAppearance> result;
    bool finished = false;
};

int Scale(int logical, UINT dpi) {
    return MulDiv(logical, static_cast<int>(dpi), 96);
}

COLORREF AppearanceColor(std::uint32_t color) {
    return static_cast<COLORREF>(color & 0x00FFFFFFU);
}

HWND AddControl(DialogState& state, const wchar_t* className,
                const wchar_t* text, DWORD style, int id, int x, int y,
                int width, int height) {
    HWND control = CreateWindowExW(
        0, className, text, WS_CHILD | WS_VISIBLE | style,
        Scale(x, state.dpi), Scale(y, state.dpi), Scale(width, state.dpi),
        Scale(height, state.dpi), state.window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), state.instance,
        nullptr);
    if (control != nullptr && state.font != nullptr) {
        SendMessageW(control, WM_SETFONT,
                     reinterpret_cast<WPARAM>(state.font), TRUE);
    }
    return control;
}

void RefreshDialog(DialogState& state) {
    const unsigned int transparency = static_cast<unsigned int>(
        SendMessageW(state.transparencySlider, TBM_GETPOS, 0, 0));
    state.working.transparencyPercent =
        static_cast<std::uint8_t>(std::min(transparency, 85U));
    const std::wstring value = std::to_wstring(transparency) + L"%";
    SetWindowTextW(state.transparencyValue, value.c_str());
    InvalidateRect(state.backgroundSwatch, nullptr, TRUE);
    InvalidateRect(state.borderSwatch, nullptr, TRUE);
}

void ChooseAppearanceColor(DialogState& state, bool border) {
    static COLORREF customColors[16] = {
        RGB(230, 234, 231), RGB(244, 246, 244), RGB(216, 221, 218),
        RGB(255, 255, 255), RGB(177, 185, 180), RGB(105, 119, 111),
        RGB(34, 42, 37), RGB(224, 234, 244), RGB(232, 226, 244),
        RGB(224, 239, 230), RGB(245, 233, 198), RGB(231, 218, 218),
        RGB(207, 224, 226), RGB(128, 136, 147), RGB(61, 68, 84),
        RGB(17, 22, 33),
    };

    CHOOSECOLORW chooser{};
    chooser.lStructSize = sizeof(chooser);
    chooser.hwndOwner = state.window;
    chooser.rgbResult = AppearanceColor(
        border ? state.working.borderColor : state.working.backgroundColor);
    chooser.lpCustColors = customColors;
    chooser.Flags = CC_RGBINIT | CC_FULLOPEN;
    if (ChooseColorW(&chooser)) {
        const std::uint32_t selected = static_cast<std::uint32_t>(
            chooser.rgbResult & 0x00FFFFFFUL);
        if (border) {
            state.working.borderColor = selected;
        } else {
            state.working.backgroundColor = selected;
        }
        RefreshDialog(state);
    }
}

void DrawSwatch(COLORREF color, const DRAWITEMSTRUCT& item) {
    HBRUSH fill = CreateSolidBrush(color);
    FillRect(item.hDC, &item.rcItem,
             fill != nullptr
                 ? fill
                 : static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    if (fill != nullptr) {
        DeleteObject(fill);
    }
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(130, 139, 133));
    HGDIOBJ oldPen = pen != nullptr ? SelectObject(item.hDC, pen) : nullptr;
    HGDIOBJ oldBrush = SelectObject(item.hDC, GetStockObject(NULL_BRUSH));
    Rectangle(item.hDC, item.rcItem.left, item.rcItem.top,
              item.rcItem.right, item.rcItem.bottom);
    SelectObject(item.hDC, oldBrush);
    if (oldPen != nullptr) {
        SelectObject(item.hDC, oldPen);
    }
    if (pen != nullptr) {
        DeleteObject(pen);
    }
    if ((item.itemState & ODS_FOCUS) != 0) {
        RECT focus = item.rcItem;
        InflateRect(&focus, -3, -3);
        DrawFocusRect(item.hDC, &focus);
    }
}

bool CreateControls(DialogState& state) {
    AddControl(state, L"STATIC", L"背景颜色", 0, -1, 20, 25, 90, 24);
    state.backgroundSwatch = AddControl(
        state, L"STATIC", L"", WS_TABSTOP | SS_OWNERDRAW | SS_NOTIFY,
        kBackgroundSwatchId, 120, 21, 42, 30);
    HWND chooseBackground = AddControl(
        state, L"BUTTON", L"选择颜色...", WS_TABSTOP,
        kChooseBackgroundId, 174, 21, 110, 30);

    AddControl(state, L"STATIC", L"背景透明度", 0, -1, 20, 77, 90, 24);
    state.transparencySlider = AddControl(
        state, TRACKBAR_CLASSW, L"", WS_TABSTOP | TBS_HORZ | TBS_NOTICKS,
        kTransparencySliderId, 116, 70, 236, 34);
    state.transparencyValue = AddControl(
        state, L"STATIC", L"", SS_RIGHT, kTransparencyValueId,
        354, 77, 52, 24);

    AddControl(state, L"STATIC", L"边框线颜色", 0, -1, 20, 129, 90, 24);
    state.borderSwatch = AddControl(
        state, L"STATIC", L"", WS_TABSTOP | SS_OWNERDRAW | SS_NOTIFY,
        kBorderSwatchId, 120, 125, 42, 30);
    HWND chooseBorder = AddControl(
        state, L"BUTTON", L"选择颜色...", WS_TABSTOP,
        kChooseBorderId, 174, 125, 110, 30);

    HWND ok = AddControl(state, L"BUTTON", L"确定",
                         WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK,
                         246, 177, 78, 30);
    HWND cancel = AddControl(state, L"BUTTON", L"取消", WS_TABSTOP,
                             IDCANCEL, 332, 177, 78, 30);
    if (state.backgroundSwatch == nullptr || chooseBackground == nullptr ||
        state.transparencySlider == nullptr ||
        state.transparencyValue == nullptr || state.borderSwatch == nullptr ||
        chooseBorder == nullptr || ok == nullptr || cancel == nullptr) {
        return false;
    }

    SendMessageW(state.transparencySlider, TBM_SETRANGE, TRUE,
                 MAKELPARAM(0, 85));
    SendMessageW(state.transparencySlider, TBM_SETLINESIZE, 0, 1);
    SendMessageW(state.transparencySlider, TBM_SETPAGESIZE, 0, 5);
    SendMessageW(state.transparencySlider, TBM_SETPOS, TRUE,
                 state.working.transparencyPercent);
    RefreshDialog(state);
    return true;
}

LRESULT CALLBACK DialogWindowProc(HWND window, UINT message, WPARAM wParam,
                                  LPARAM lParam) {
    auto* state = reinterpret_cast<DialogState*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = static_cast<DialogState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(state));
        if (state != nullptr) {
            state->window = window;
        }
    }
    if (state == nullptr) {
        return DefWindowProcW(window, message, wParam, lParam);
    }

    switch (message) {
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case kBackgroundSwatchId:
                    if (HIWORD(wParam) == STN_CLICKED) {
                        ChooseAppearanceColor(*state, false);
                    }
                    return 0;
                case kChooseBackgroundId:
                    ChooseAppearanceColor(*state, false);
                    return 0;
                case kBorderSwatchId:
                    if (HIWORD(wParam) == STN_CLICKED) {
                        ChooseAppearanceColor(*state, true);
                    }
                    return 0;
                case kChooseBorderId:
                    ChooseAppearanceColor(*state, true);
                    return 0;
                case IDOK:
                    RefreshDialog(*state);
                    state->result = state->working;
                    state->finished = true;
                    DestroyWindow(window);
                    return 0;
                case IDCANCEL:
                    state->finished = true;
                    DestroyWindow(window);
                    return 0;
                default:
                    break;
            }
            break;
        case WM_HSCROLL:
            if (reinterpret_cast<HWND>(lParam) == state->transparencySlider) {
                RefreshDialog(*state);
                return 0;
            }
            break;
        case WM_DRAWITEM: {
            const auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (item != nullptr && item->CtlID == kBackgroundSwatchId) {
                DrawSwatch(AppearanceColor(state->working.backgroundColor),
                           *item);
                return TRUE;
            }
            if (item != nullptr && item->CtlID == kBorderSwatchId) {
                DrawSwatch(AppearanceColor(state->working.borderColor), *item);
                return TRUE;
            }
            break;
        }
        case WM_CLOSE:
            state->finished = true;
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            state->window = nullptr;
            state->finished = true;
            return 0;
        default:
            break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool RegisterDialogClass(HINSTANCE instance) {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = DialogWindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground =
        reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszClassName = kDialogClass;
    if (RegisterClassExW(&windowClass) != 0) {
        return true;
    }
    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

}  // namespace

std::optional<RailAppearance> PromptForRailAppearance(
    HWND owner, HINSTANCE instance, const RailAppearance& initial) {
    if (instance == nullptr || !RegisterDialogClass(instance)) {
        return std::nullopt;
    }

    DialogState state{};
    state.owner = owner;
    state.instance = instance;
    state.working = initial;
    state.dpi = owner != nullptr ? GetDpiForWindow(owner) : GetDpiForSystem();
    if (state.dpi == 0) {
        state.dpi = 96;
    }
    state.font = CreateFontW(
        -MulDiv(10, static_cast<int>(state.dpi), 72), 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    RECT bounds{0, 0, Scale(430, state.dpi), Scale(222, state.dpi)};
    AdjustWindowRectExForDpi(&bounds, WS_CAPTION | WS_SYSMENU | WS_POPUP,
                             FALSE, WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
                             state.dpi);
    int x = 0;
    int y = 0;
    RECT ownerBounds{};
    if (owner != nullptr && GetWindowRect(owner, &ownerBounds)) {
        x = ownerBounds.left +
            ((ownerBounds.right - ownerBounds.left) -
             (bounds.right - bounds.left)) / 2;
        y = ownerBounds.top +
            ((ownerBounds.bottom - ownerBounds.top) -
             (bounds.bottom - bounds.top)) / 2;
    }
    HMONITOR monitor = MonitorFromWindow(
        owner != nullptr ? owner : GetDesktopWindow(), MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (monitor != nullptr && GetMonitorInfoW(monitor, &monitorInfo)) {
        const int workLeft = static_cast<int>(monitorInfo.rcWork.left);
        const int workTop = static_cast<int>(monitorInfo.rcWork.top);
        const int maximumX = std::max(
            workLeft, static_cast<int>(monitorInfo.rcWork.right) -
                          static_cast<int>(bounds.right - bounds.left));
        const int maximumY = std::max(
            workTop, static_cast<int>(monitorInfo.rcWork.bottom) -
                         static_cast<int>(bounds.bottom - bounds.top));
        x = std::clamp(x, workLeft, maximumX);
        y = std::clamp(y, workTop, maximumY);
    }

    state.window = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, kDialogClass,
        L"Dock 外观设置", WS_CAPTION | WS_SYSMENU | WS_POPUP,
        x, y, bounds.right - bounds.left, bounds.bottom - bounds.top,
        owner, nullptr, instance, &state);
    if (state.window == nullptr || !CreateControls(state)) {
        if (state.window != nullptr) {
            DestroyWindow(state.window);
        }
        if (state.font != nullptr) {
            DeleteObject(state.font);
        }
        return std::nullopt;
    }

    if (owner != nullptr) {
        EnableWindow(owner, FALSE);
    }
    ShowWindow(state.window, SW_SHOW);
    UpdateWindow(state.window);
    MSG message{};
    while (!state.finished) {
        const BOOL status = GetMessageW(&message, nullptr, 0, 0);
        if (status <= 0) {
            state.finished = true;
            if (status == 0) {
                PostQuitMessage(static_cast<int>(message.wParam));
            }
            break;
        }
        if (state.window == nullptr ||
            !IsDialogMessageW(state.window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (state.window != nullptr) {
        DestroyWindow(state.window);
    }
    if (owner != nullptr) {
        EnableWindow(owner, TRUE);
        SetActiveWindow(owner);
    }
    if (state.font != nullptr) {
        DeleteObject(state.font);
    }
    return state.result;
}

}  // namespace lightlaunch
