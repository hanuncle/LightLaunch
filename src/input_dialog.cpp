#include "input_dialog.h"

#include <algorithm>
#include <cwctype>
#include <string>

namespace lightlaunch {
namespace {

constexpr wchar_t kInputDialogClass[] = L"LightLaunch.InputDialog";
constexpr int kEditId = 100;
constexpr int kOkId = IDOK;
constexpr int kCancelId = IDCANCEL;

struct DialogState {
    std::wstring prompt;
    std::wstring initialValue;
    std::wstring value;
    HWND edit = nullptr;
    HFONT font = nullptr;
    UINT dpi = 96;
    bool accepted = false;
};

int ScaleForDpi(int value, UINT dpi) {
    return MulDiv(value, static_cast<int>(dpi), 96);
}

std::wstring Trim(std::wstring value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](wchar_t character) {
        return std::iswspace(character) != 0;
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](wchar_t character) {
        return std::iswspace(character) != 0;
    }).base();

    if (first >= last) {
        return {};
    }
    value = std::wstring(first, last);
    std::replace(value.begin(), value.end(), L'\r', L' ');
    std::replace(value.begin(), value.end(), L'\n', L' ');
    return value;
}

void ApplyFont(HWND window, HFONT font) {
    SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

LRESULT CALLBACK InputDialogProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<DialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));

    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = static_cast<DialogState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }

    switch (message) {
        case WM_CREATE: {
            if (state == nullptr) {
                return -1;
            }

            HWND prompt = CreateWindowExW(0, L"STATIC", state->prompt.c_str(),
                                           WS_CHILD | WS_VISIBLE,
                                           ScaleForDpi(18, state->dpi), ScaleForDpi(16, state->dpi),
                                           ScaleForDpi(384, state->dpi), ScaleForDpi(22, state->dpi),
                                           window, nullptr, nullptr, nullptr);
            state->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
                                           state->initialValue.c_str(),
                                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                           ScaleForDpi(18, state->dpi), ScaleForDpi(43, state->dpi),
                                           ScaleForDpi(384, state->dpi), ScaleForDpi(27, state->dpi), window,
                                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditId)), nullptr, nullptr);
            HWND ok = CreateWindowExW(0, L"BUTTON", L"确定",
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                       ScaleForDpi(238, state->dpi), ScaleForDpi(86, state->dpi),
                                       ScaleForDpi(78, state->dpi), ScaleForDpi(28, state->dpi), window,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(kOkId)), nullptr, nullptr);
            HWND cancel = CreateWindowExW(0, L"BUTTON", L"取消",
                                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                           ScaleForDpi(324, state->dpi), ScaleForDpi(86, state->dpi),
                                           ScaleForDpi(78, state->dpi), ScaleForDpi(28, state->dpi), window,
                                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCancelId)), nullptr, nullptr);

            ApplyFont(prompt, state->font);
            ApplyFont(state->edit, state->font);
            ApplyFont(ok, state->font);
            ApplyFont(cancel, state->font);
            SendMessageW(state->edit, EM_SETSEL, 0, -1);
            SetFocus(state->edit);
            return 0;
        }

        case WM_COMMAND:
            if (state == nullptr) {
                break;
            }
            switch (LOWORD(wParam)) {
                case kOkId: {
                    const int length = GetWindowTextLengthW(state->edit);
                    std::wstring value(static_cast<std::size_t>(length) + 1, L'\0');
                    GetWindowTextW(state->edit, value.data(), length + 1);
                    value.resize(static_cast<std::size_t>(length));
                    value = Trim(std::move(value));
                    if (value.empty()) {
                        MessageBoxW(window, L"名称不能为空。", L"LightLaunch",
                                    MB_OK | MB_ICONINFORMATION);
                        SetFocus(state->edit);
                        return 0;
                    }
                    state->value = std::move(value);
                    state->accepted = true;
                    DestroyWindow(window);
                    return 0;
                }
                case kCancelId:
                    DestroyWindow(window);
                    return 0;
                default:
                    break;
            }
            break;

        case WM_CLOSE:
            DestroyWindow(window);
            return 0;

        default:
            break;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

bool EnsureInputDialogClass(HINSTANCE instance) {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = InputDialogProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    windowClass.lpszClassName = kInputDialogClass;

    if (RegisterClassExW(&windowClass) != 0) {
        return true;
    }
    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

HFONT CreateDialogFont() {
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) {
        return CreateFontIndirectW(&metrics.lfMessageFont);
    }
    return static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
}

}  // namespace

std::optional<std::wstring> PromptForText(HWND owner, HINSTANCE instance,
                                          const std::wstring& title,
                                          const std::wstring& prompt,
                                          const std::wstring& initialValue) {
    if (!EnsureInputDialogClass(instance)) {
        return std::nullopt;
    }

    DialogState state;
    state.prompt = prompt;
    state.initialValue = initialValue;
    state.font = CreateDialogFont();
    state.dpi = GetDpiForWindow(owner);

    RECT ownerRect{};
    GetWindowRect(owner, &ownerRect);
    const int width = ScaleForDpi(436, state.dpi);
    const int height = ScaleForDpi(166, state.dpi);
    const int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
    const int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2;

    EnableWindow(owner, FALSE);
    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
                                  kInputDialogClass, title.c_str(),
                                  WS_POPUP | WS_CAPTION | WS_SYSMENU,
                                  x, y, width, height, owner, nullptr, instance, &state);
    if (dialog == nullptr) {
        EnableWindow(owner, TRUE);
        if (state.font != GetStockObject(DEFAULT_GUI_FONT)) {
            DeleteObject(state.font);
        }
        return std::nullopt;
    }

    ShowWindow(dialog, SW_SHOW);
    UpdateWindow(dialog);

    bool sawQuit = false;
    int quitCode = 0;
    MSG message{};
    while (IsWindow(dialog)) {
        const BOOL result = GetMessageW(&message, nullptr, 0, 0);
        if (result <= 0) {
            if (result == 0) {
                sawQuit = true;
                quitCode = static_cast<int>(message.wParam);
            }
            break;
        }

        if (message.message == WM_KEYDOWN && message.wParam == VK_RETURN) {
            SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(kOkId, BN_CLICKED), 0);
            continue;
        }
        if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
            SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(kCancelId, BN_CLICKED), 0);
            continue;
        }
        if (!IsDialogMessageW(dialog, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    if (state.font != GetStockObject(DEFAULT_GUI_FONT)) {
        DeleteObject(state.font);
    }
    if (sawQuit) {
        PostQuitMessage(quitCode);
    }

    if (!state.accepted) {
        return std::nullopt;
    }
    return state.value;
}

}  // namespace lightlaunch
