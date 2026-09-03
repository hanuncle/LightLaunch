#include "background_dialog.h"

#include "background_image.h"

#include <commctrl.h>
#include <commdlg.h>
#include <windowsx.h>

#include <algorithm>
#include <memory>
#include <string>

namespace lightlaunch {
namespace {

constexpr wchar_t kDialogClass[] = L"LightLaunch.FenceBackgroundDialog";
constexpr int kPathEditId = 6101;
constexpr int kBrowseButtonId = 6102;
constexpr int kClearButtonId = 6103;
constexpr int kPreviewId = 6104;
constexpr int kModeComboId = 6105;
constexpr int kOpacitySliderId = 6106;
constexpr int kOpacityValueId = 6107;
constexpr int kCropXSliderId = 6108;
constexpr int kCropXValueId = 6109;
constexpr int kCropYSliderId = 6110;
constexpr int kCropYValueId = 6111;
constexpr int kColorSwatchId = 6112;
constexpr int kChooseColorButtonId = 6113;
constexpr int kTransparencySliderId = 6114;
constexpr int kTransparencyValueId = 6115;
constexpr int kBorderSwatchId = 6116;
constexpr int kChooseBorderButtonId = 6117;
constexpr UINT_PTR kPreviewSubclassId = 1;

struct DialogState {
    HWND owner = nullptr;
    HINSTANCE instance = nullptr;
    HWND window = nullptr;
    HWND pathEdit = nullptr;
    HWND preview = nullptr;
    HWND colorSwatch = nullptr;
    HWND borderSwatch = nullptr;
    HWND modeCombo = nullptr;
    HWND opacitySlider = nullptr;
    HWND opacityValue = nullptr;
    HWND transparencySlider = nullptr;
    HWND transparencyValue = nullptr;
    HWND cropXSlider = nullptr;
    HWND cropXValue = nullptr;
    HWND cropYSlider = nullptr;
    HWND cropYValue = nullptr;
    HFONT font = nullptr;
    UINT dpi = 96;
    FenceBackground working;
    std::unique_ptr<BackgroundImage> previewImage;
    std::optional<FenceBackground> result;
    bool finished = false;
};

int Scale(int logical, UINT dpi) {
    return MulDiv(logical, static_cast<int>(dpi), 96);
}

COLORREF BackgroundColor(const FenceBackground& background) {
    return static_cast<COLORREF>(background.backgroundColor & 0x00FFFFFFU);
}

COLORREF BorderColor(const FenceBackground& background) {
    return static_cast<COLORREF>(background.borderColor & 0x00FFFFFFU);
}

bool IsDarkColor(COLORREF color) {
    const unsigned int luminance =
        299U * GetRValue(color) + 587U * GetGValue(color) +
        114U * GetBValue(color);
    return luminance < 145000U;
}

HWND AddControl(DialogState& state, const wchar_t* className,
                const wchar_t* text, DWORD style, int id, int x, int y,
                int width, int height, DWORD extendedStyle = 0) {
    HWND control = CreateWindowExW(
        extendedStyle, className, text, WS_CHILD | WS_VISIBLE | style,
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

void SetPercentLabel(HWND label, unsigned int value) {
    const std::wstring text = std::to_wstring(std::min(value, 100U)) + L"%";
    SetWindowTextW(label, text.c_str());
}

void RefreshDialog(DialogState& state) {
    const LRESULT mode = SendMessageW(state.modeCombo, CB_GETCURSEL, 0, 0);
    if (mode >= 0 && mode <= static_cast<LRESULT>(FenceBackgroundMode::Stretch)) {
        state.working.mode = static_cast<FenceBackgroundMode>(mode);
    }
    const unsigned int opacity = static_cast<unsigned int>(
        SendMessageW(state.opacitySlider, TBM_GETPOS, 0, 0));
    const unsigned int transparency = static_cast<unsigned int>(
        SendMessageW(state.transparencySlider, TBM_GETPOS, 0, 0));
    const unsigned int cropX = static_cast<unsigned int>(
        SendMessageW(state.cropXSlider, TBM_GETPOS, 0, 0));
    const unsigned int cropY = static_cast<unsigned int>(
        SendMessageW(state.cropYSlider, TBM_GETPOS, 0, 0));
    state.working.opacityPercent =
        static_cast<std::uint8_t>(std::min(opacity, 100U));
    state.working.transparencyPercent =
        static_cast<std::uint8_t>(std::min(transparency, 85U));
    state.working.cropX =
        static_cast<std::uint16_t>(std::min(cropX, 10000U));
    state.working.cropY =
        static_cast<std::uint16_t>(std::min(cropY, 10000U));
    SetPercentLabel(state.opacityValue, opacity);
    SetPercentLabel(state.transparencyValue, transparency);
    SetPercentLabel(state.cropXValue, (cropX + 50U) / 100U);
    SetPercentLabel(state.cropYValue, (cropY + 50U) / 100U);
    const BOOL cropEnabled =
        state.working.mode == FenceBackgroundMode::Cover &&
                state.previewImage != nullptr
            ? TRUE
            : FALSE;
    EnableWindow(state.cropXSlider, cropEnabled);
    EnableWindow(state.cropYSlider, cropEnabled);
    InvalidateRect(state.preview, nullptr, TRUE);
    if (state.colorSwatch != nullptr) {
        InvalidateRect(state.colorSwatch, nullptr, TRUE);
    }
    if (state.borderSwatch != nullptr) {
        InvalidateRect(state.borderSwatch, nullptr, TRUE);
    }
}

bool LoadPreview(DialogState& state, const std::wstring& path,
                 bool showError) {
    if (path.empty()) {
        state.previewImage.reset();
        SetWindowTextW(state.pathEdit, L"");
        RefreshDialog(state);
        return true;
    }

    auto image = std::make_unique<BackgroundImage>();
    if (!image->Load(path)) {
        if (showError) {
            MessageBoxW(state.window,
                        L"无法读取这张图片，请选择 PNG、JPEG、BMP、GIF 或 TIFF 文件。",
                        L"围栏背景", MB_OK | MB_ICONWARNING);
        }
        return false;
    }
    state.previewImage = std::move(image);
    state.working.imagePath = path;
    SetWindowTextW(state.pathEdit, path.c_str());
    RefreshDialog(state);
    return true;
}

void ChooseImage(DialogState& state) {
    std::wstring path(32768, L'\0');
    if (!state.working.imagePath.empty()) {
        const std::size_t count =
            std::min(path.size() - 1, state.working.imagePath.size());
        std::copy_n(state.working.imagePath.data(), count, path.data());
    }
    constexpr wchar_t filter[] =
        L"图片文件 (*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff)\0"
        L"*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff\0"
        L"所有文件 (*.*)\0*.*\0\0";
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = state.window;
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = path.data();
    dialog.nMaxFile = static_cast<DWORD>(path.size());
    dialog.lpstrTitle = L"选择围栏背景图片";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
                   OFN_EXPLORER | OFN_DONTADDTORECENT;
    if (GetOpenFileNameW(&dialog)) {
        LoadPreview(state, path.c_str(), true);
    }
}

void ChooseBackgroundColor(DialogState& state) {
    static COLORREF customColors[16] = {
        RGB(230, 234, 231), RGB(244, 246, 244), RGB(216, 221, 218),
        RGB(34, 42, 37),    RGB(242, 232, 222), RGB(224, 234, 244),
        RGB(232, 226, 244), RGB(224, 239, 230), RGB(245, 233, 198),
        RGB(231, 218, 218), RGB(207, 224, 226), RGB(226, 226, 226),
        RGB(255, 255, 255), RGB(180, 188, 183), RGB(94, 105, 98),
        RGB(17, 22, 33),
    };

    CHOOSECOLORW chooser{};
    chooser.lStructSize = sizeof(chooser);
    chooser.hwndOwner = state.window;
    chooser.rgbResult = BackgroundColor(state.working);
    chooser.lpCustColors = customColors;
    chooser.Flags = CC_RGBINIT | CC_FULLOPEN;
    if (ChooseColorW(&chooser)) {
        state.working.backgroundColor = static_cast<std::uint32_t>(
            chooser.rgbResult & 0x00FFFFFFUL);
        RefreshDialog(state);
    }
}

void ChooseBorderColor(DialogState& state) {
    static COLORREF customColors[16] = {
        RGB(255, 255, 255), RGB(230, 234, 231), RGB(177, 185, 180),
        RGB(105, 119, 111), RGB(34, 42, 37), RGB(214, 225, 236),
        RGB(191, 210, 232), RGB(226, 214, 237), RGB(221, 229, 214),
        RGB(238, 222, 205), RGB(235, 207, 207), RGB(207, 224, 226),
        RGB(128, 136, 147), RGB(94, 105, 98), RGB(61, 68, 84),
        RGB(17, 22, 33),
    };

    CHOOSECOLORW chooser{};
    chooser.lStructSize = sizeof(chooser);
    chooser.hwndOwner = state.window;
    chooser.rgbResult = BorderColor(state.working);
    chooser.lpCustColors = customColors;
    chooser.Flags = CC_RGBINIT | CC_FULLOPEN;
    if (ChooseColorW(&chooser)) {
        state.working.borderColor = static_cast<std::uint32_t>(
            chooser.rgbResult & 0x00FFFFFFUL);
        RefreshDialog(state);
    }
}

void DrawPreview(DialogState& state, const DRAWITEMSTRUCT& item) {
    RECT client = item.rcItem;
    const COLORREF surfaceColor = BackgroundColor(state.working);
    HBRUSH base = CreateSolidBrush(surfaceColor);
    FillRect(item.hDC, &client, base != nullptr
                                  ? base
                                  : static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    if (base != nullptr) {
        DeleteObject(base);
    }

    const int width = std::max(0, static_cast<int>(client.right - client.left) - 2);
    const int height = std::max(0, static_cast<int>(client.bottom - client.top) - 2);
    if (state.previewImage != nullptr && width > 0 && height > 0) {
        HBITMAP preview = state.previewImage->Render(
            state.working, width, height, surfaceColor);
        if (preview != nullptr) {
            HDC memoryDc = CreateCompatibleDC(item.hDC);
            if (memoryDc != nullptr) {
                HGDIOBJ oldBitmap = SelectObject(memoryDc, preview);
                BitBlt(item.hDC, client.left + 1, client.top + 1, width, height,
                       memoryDc, 0, 0, SRCCOPY);
                SelectObject(memoryDc, oldBitmap);
                DeleteDC(memoryDc);
            }
            DeleteObject(preview);
        }
    } else {
        const int previousMode = SetBkMode(item.hDC, TRANSPARENT);
        const COLORREF previousColor =
            SetTextColor(item.hDC, IsDarkColor(surfaceColor)
                                       ? RGB(220, 225, 232)
                                       : RGB(74, 84, 78));
        DrawTextW(item.hDC, L"纯色背景", -1, &client,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SetTextColor(item.hDC, previousColor);
        SetBkMode(item.hDC, previousMode);
    }

    HPEN border = CreatePen(PS_SOLID, 1, BorderColor(state.working));
    HGDIOBJ oldPen = border != nullptr ? SelectObject(item.hDC, border) : nullptr;
    HGDIOBJ oldBrush = SelectObject(item.hDC, GetStockObject(NULL_BRUSH));
    Rectangle(item.hDC, client.left, client.top, client.right, client.bottom);
    SelectObject(item.hDC, oldBrush);
    if (oldPen != nullptr) {
        SelectObject(item.hDC, oldPen);
    }
    if (border != nullptr) {
        DeleteObject(border);
    }
}

void DrawColorSwatch(COLORREF color, const DRAWITEMSTRUCT& item) {
    HBRUSH fill = CreateSolidBrush(color);
    FillRect(item.hDC, &item.rcItem,
             fill != nullptr
                 ? fill
                 : static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    if (fill != nullptr) {
        DeleteObject(fill);
    }

    HPEN border = CreatePen(PS_SOLID, 1,
                            IsDarkColor(color) ? RGB(128, 136, 147)
                                               : RGB(153, 162, 156));
    HGDIOBJ oldPen = border != nullptr ? SelectObject(item.hDC, border) : nullptr;
    HGDIOBJ oldBrush = SelectObject(item.hDC, GetStockObject(NULL_BRUSH));
    Rectangle(item.hDC, item.rcItem.left, item.rcItem.top,
              item.rcItem.right, item.rcItem.bottom);
    SelectObject(item.hDC, oldBrush);
    if (oldPen != nullptr) {
        SelectObject(item.hDC, oldPen);
    }
    if (border != nullptr) {
        DeleteObject(border);
    }
    if ((item.itemState & ODS_FOCUS) != 0) {
        RECT focus = item.rcItem;
        InflateRect(&focus, -3, -3);
        DrawFocusRect(item.hDC, &focus);
    }
}

void UpdateCropFromPreview(DialogState& state, HWND preview, LPARAM lParam) {
    if (state.previewImage == nullptr ||
        state.working.mode != FenceBackgroundMode::Cover) {
        return;
    }
    RECT client{};
    GetClientRect(preview, &client);
    const int width = std::max(1, static_cast<int>(client.right - client.left));
    const int height = std::max(1, static_cast<int>(client.bottom - client.top));
    const int x = std::clamp(GET_X_LPARAM(lParam), 0, width);
    const int y = std::clamp(GET_Y_LPARAM(lParam), 0, height);
    const int cropX = std::clamp(MulDiv(x, 10000, width), 0, 10000);
    const int cropY = std::clamp(MulDiv(y, 10000, height), 0, 10000);
    SendMessageW(state.cropXSlider, TBM_SETPOS, TRUE, cropX);
    SendMessageW(state.cropYSlider, TBM_SETPOS, TRUE, cropY);
    RefreshDialog(state);
}

LRESULT CALLBACK PreviewSubclassProc(HWND window, UINT message, WPARAM wParam,
                                    LPARAM lParam, UINT_PTR,
                                    DWORD_PTR referenceData) {
    auto* state = reinterpret_cast<DialogState*>(referenceData);
    if (state == nullptr) {
        return DefSubclassProc(window, message, wParam, lParam);
    }
    switch (message) {
        case WM_LBUTTONDOWN:
            SetCapture(window);
            UpdateCropFromPreview(*state, window, lParam);
            return 0;
        case WM_MOUSEMOVE:
            if ((wParam & MK_LBUTTON) != 0 && GetCapture() == window) {
                UpdateCropFromPreview(*state, window, lParam);
                return 0;
            }
            break;
        case WM_LBUTTONUP:
            if (GetCapture() == window) {
                UpdateCropFromPreview(*state, window, lParam);
                ReleaseCapture();
            }
            return 0;
        case WM_NCDESTROY:
            RemoveWindowSubclass(window, PreviewSubclassProc,
                                 kPreviewSubclassId);
            break;
        default:
            break;
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

bool CreateControls(DialogState& state) {
    state.pathEdit = AddControl(
        state, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL | ES_READONLY,
        kPathEditId, 18, 38, 384, 28, WS_EX_CLIENTEDGE);
    HWND browse = AddControl(state, L"BUTTON", L"选择", WS_TABSTOP,
                             kBrowseButtonId, 410, 38, 62, 28);
    HWND clear = AddControl(state, L"BUTTON", L"清除", WS_TABSTOP,
                            kClearButtonId, 480, 38, 62, 28);
    AddControl(state, L"STATIC", L"背景图片", 0, -1, 18, 15, 100, 20);

    state.preview = AddControl(state, L"STATIC", L"", SS_OWNERDRAW | SS_NOTIFY,
                               kPreviewId, 18, 80, 524, 220);
    AddControl(state, L"STATIC", L"背景颜色", 0, -1, 18, 320, 78, 24);
    state.colorSwatch = AddControl(
        state, L"STATIC", L"", WS_TABSTOP | SS_OWNERDRAW | SS_NOTIFY,
        kColorSwatchId, 104, 316, 36, 28);
    HWND chooseColor = AddControl(state, L"BUTTON", L"选择颜色...", WS_TABSTOP,
                                  kChooseColorButtonId, 148, 316, 102, 28);
    AddControl(state, L"STATIC", L"填充模式", 0, -1, 268, 320, 78, 24);
    state.modeCombo = AddControl(
        state, WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST,
        kModeComboId, 350, 316, 192, 180);
    SendMessageW(state.modeCombo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"填充（按焦点裁切）"));
    SendMessageW(state.modeCombo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"适应（完整显示）"));
    SendMessageW(state.modeCombo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"拉伸"));

    AddControl(state, L"STATIC", L"图片不透明度", 0, -1, 18, 360, 118, 24);
    state.opacitySlider = AddControl(
        state, TRACKBAR_CLASSW, L"", WS_TABSTOP | TBS_HORZ | TBS_NOTICKS,
        kOpacitySliderId, 146, 354, 300, 32);
    state.opacityValue = AddControl(state, L"STATIC", L"", SS_RIGHT,
                                    kOpacityValueId, 458, 360, 60, 24);

    AddControl(state, L"STATIC", L"背景透明度", 0, -1, 18, 400, 118, 24);
    state.transparencySlider = AddControl(
        state, TRACKBAR_CLASSW, L"", WS_TABSTOP | TBS_HORZ | TBS_NOTICKS,
        kTransparencySliderId, 146, 394, 300, 32);
    state.transparencyValue = AddControl(
        state, L"STATIC", L"", SS_RIGHT, kTransparencyValueId,
        458, 400, 60, 24);

    AddControl(state, L"STATIC", L"边框线颜色", 0, -1, 18, 440, 78, 24);
    state.borderSwatch = AddControl(
        state, L"STATIC", L"", WS_TABSTOP | SS_OWNERDRAW | SS_NOTIFY,
        kBorderSwatchId, 104, 436, 36, 28);
    HWND chooseBorder = AddControl(
        state, L"BUTTON", L"选择颜色...", WS_TABSTOP,
        kChooseBorderButtonId, 148, 436, 102, 28);

    AddControl(state, L"STATIC", L"横向裁切焦点", 0, -1, 18, 480, 118, 24);
    state.cropXSlider = AddControl(
        state, TRACKBAR_CLASSW, L"", WS_TABSTOP | TBS_HORZ | TBS_NOTICKS,
        kCropXSliderId, 146, 474, 300, 32);
    state.cropXValue = AddControl(state, L"STATIC", L"", SS_RIGHT,
                                  kCropXValueId, 458, 480, 60, 24);

    AddControl(state, L"STATIC", L"纵向裁切焦点", 0, -1, 18, 520, 118, 24);
    state.cropYSlider = AddControl(
        state, TRACKBAR_CLASSW, L"", WS_TABSTOP | TBS_HORZ | TBS_NOTICKS,
        kCropYSliderId, 146, 514, 300, 32);
    state.cropYValue = AddControl(state, L"STATIC", L"", SS_RIGHT,
                                  kCropYValueId, 458, 520, 60, 24);

    HWND ok = AddControl(state, L"BUTTON", L"确定",
                         WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK,
                         374, 562, 80, 30);
    HWND cancel = AddControl(state, L"BUTTON", L"取消", WS_TABSTOP,
                             IDCANCEL, 462, 562, 80, 30);

    if (state.pathEdit == nullptr || browse == nullptr || clear == nullptr ||
        state.preview == nullptr || state.colorSwatch == nullptr ||
        chooseColor == nullptr || state.borderSwatch == nullptr ||
        chooseBorder == nullptr || state.modeCombo == nullptr ||
        state.opacitySlider == nullptr || state.opacityValue == nullptr ||
        state.transparencySlider == nullptr ||
        state.transparencyValue == nullptr ||
        state.cropXSlider == nullptr || state.cropXValue == nullptr ||
        state.cropYSlider == nullptr || state.cropYValue == nullptr ||
        ok == nullptr || cancel == nullptr) {
        return false;
    }

    SetWindowSubclass(state.preview, PreviewSubclassProc,
                      kPreviewSubclassId,
                      reinterpret_cast<DWORD_PTR>(&state));
    SendMessageW(state.opacitySlider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendMessageW(state.transparencySlider, TBM_SETRANGE, TRUE,
                 MAKELPARAM(0, 85));
    SendMessageW(state.cropXSlider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 10000));
    SendMessageW(state.cropYSlider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 10000));
    SendMessageW(state.opacitySlider, TBM_SETLINESIZE, 0, 1);
    SendMessageW(state.opacitySlider, TBM_SETPAGESIZE, 0, 5);
    SendMessageW(state.transparencySlider, TBM_SETLINESIZE, 0, 1);
    SendMessageW(state.transparencySlider, TBM_SETPAGESIZE, 0, 5);
    SendMessageW(state.cropXSlider, TBM_SETLINESIZE, 0, 100);
    SendMessageW(state.cropXSlider, TBM_SETPAGESIZE, 0, 500);
    SendMessageW(state.cropYSlider, TBM_SETLINESIZE, 0, 100);
    SendMessageW(state.cropYSlider, TBM_SETPAGESIZE, 0, 500);
    SendMessageW(state.opacitySlider, TBM_SETPOS, TRUE,
                 state.working.opacityPercent);
    SendMessageW(state.transparencySlider, TBM_SETPOS, TRUE,
                 state.working.transparencyPercent);
    SendMessageW(state.cropXSlider, TBM_SETPOS, TRUE,
                 state.working.cropX);
    SendMessageW(state.cropYSlider, TBM_SETPOS, TRUE,
                 state.working.cropY);
    SendMessageW(state.modeCombo, CB_SETCURSEL,
                 static_cast<WPARAM>(state.working.mode), 0);
    SetWindowTextW(state.pathEdit, state.working.imagePath.c_str());
    if (!state.working.imagePath.empty()) {
        LoadPreview(state, state.working.imagePath, false);
    }
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
                case kBrowseButtonId:
                    ChooseImage(*state);
                    return 0;
                case kClearButtonId:
                    state->working.imagePath.clear();
                    LoadPreview(*state, L"", false);
                    return 0;
                case kColorSwatchId:
                    if (HIWORD(wParam) == STN_CLICKED) {
                        ChooseBackgroundColor(*state);
                    }
                    return 0;
                case kChooseColorButtonId:
                    ChooseBackgroundColor(*state);
                    return 0;
                case kBorderSwatchId:
                    if (HIWORD(wParam) == STN_CLICKED) {
                        ChooseBorderColor(*state);
                    }
                    return 0;
                case kChooseBorderButtonId:
                    ChooseBorderColor(*state);
                    return 0;
                case kModeComboId:
                    if (HIWORD(wParam) == CBN_SELCHANGE) {
                        RefreshDialog(*state);
                    }
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
            if (reinterpret_cast<HWND>(lParam) == state->opacitySlider ||
                reinterpret_cast<HWND>(lParam) ==
                    state->transparencySlider ||
                reinterpret_cast<HWND>(lParam) == state->cropXSlider ||
                reinterpret_cast<HWND>(lParam) == state->cropYSlider) {
                RefreshDialog(*state);
                return 0;
            }
            break;
        case WM_DRAWITEM: {
            const auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (item != nullptr) {
                if (item->CtlID == kPreviewId) {
                    DrawPreview(*state, *item);
                    return TRUE;
                }
                if (item->CtlID == kColorSwatchId) {
                    DrawColorSwatch(BackgroundColor(state->working), *item);
                    return TRUE;
                }
                if (item->CtlID == kBorderSwatchId) {
                    DrawColorSwatch(BorderColor(state->working), *item);
                    return TRUE;
                }
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

std::optional<FenceBackground> PromptForFenceBackground(
    HWND owner, HINSTANCE instance, const FenceBackground& initial) {
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

    RECT bounds{0, 0, Scale(560, state.dpi), Scale(604, state.dpi)};
    AdjustWindowRectExForDpi(&bounds, WS_CAPTION | WS_SYSMENU | WS_POPUP,
                             FALSE, WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
                             state.dpi);
    int x = 0;
    int y = 0;
    RECT ownerBounds{};
    if (owner != nullptr && GetWindowRect(owner, &ownerBounds)) {
        x = ownerBounds.left +
            ((ownerBounds.right - ownerBounds.left) -
             (bounds.right - bounds.left)) /
                2;
        y = ownerBounds.top +
            ((ownerBounds.bottom - ownerBounds.top) -
             (bounds.bottom - bounds.top)) /
                2;
    }
    HMONITOR monitor = MonitorFromWindow(
        owner != nullptr ? owner : GetDesktopWindow(), MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (monitor != nullptr && GetMonitorInfoW(monitor, &monitorInfo)) {
        const int workLeft = static_cast<int>(monitorInfo.rcWork.left);
        const int workTop = static_cast<int>(monitorInfo.rcWork.top);
        const int maximumX = std::max(
            workLeft,
            static_cast<int>(monitorInfo.rcWork.right -
                             (bounds.right - bounds.left)));
        const int maximumY = std::max(
            workTop,
            static_cast<int>(monitorInfo.rcWork.bottom -
                             (bounds.bottom - bounds.top)));
        x = std::clamp(x, workLeft, maximumX);
        y = std::clamp(y, workTop, maximumY);
    }

    state.window = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, kDialogClass,
        L"围栏背景设置", WS_CAPTION | WS_SYSMENU | WS_POPUP,
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
