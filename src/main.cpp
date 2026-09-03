#include "application.h"

#include <windows.h>
#include <commctrl.h>
#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <cstdint>
#include <cwctype>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr wchar_t kSingleInstanceMutexPrefix[] = L"Local\\LightLaunch.SingleInstance.Config.";
constexpr wchar_t kMainWindowClass[] = L"LightLaunch.MainWindow";
constexpr wchar_t kConfigIdentityProperty[] = L"LightLaunch.ConfigIdentity";
constexpr UINT kActivateWindowMessage = WM_APP + 3;

struct StartupOptions {
    std::wstring configPath;
    bool valid = true;
};

StartupOptions ParseStartupOptions() {
    StartupOptions options;
    int argumentCount = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (arguments == nullptr) {
        options.valid = false;
        return options;
    }

    if (argumentCount == 1) {
        LocalFree(arguments);
        return options;
    }

    if (argumentCount == 3 && std::wstring(arguments[1]) == L"--config" &&
        arguments[2][0] != L'\0') {
        options.configPath = arguments[2];
    } else {
        options.valid = false;
    }
    LocalFree(arguments);
    return options;
}

std::wstring NormalizeConfigPath(const std::wstring& configPath) {
    const DWORD required = GetFullPathNameW(configPath.c_str(), 0, nullptr, nullptr);
    if (required == 0) {
        return {};
    }

    std::vector<wchar_t> buffer(required, L'\0');
    const DWORD length = GetFullPathNameW(configPath.c_str(), required, buffer.data(), nullptr);
    if (length == 0 || length >= required) {
        return {};
    }
    return std::wstring(buffer.data(), length);
}

std::uint64_t ConfigIdentity(const std::wstring& configPath) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const wchar_t character : configPath) {
        hash ^= static_cast<std::uint64_t>(std::towlower(character));
        hash *= 1099511628211ULL;
    }
    return hash == 0 ? 1 : hash;
}

std::wstring MutexNameForConfig(std::uint64_t identity) {
    return std::wstring(kSingleInstanceMutexPrefix) + std::to_wstring(identity);
}

bool EnsureConfigDirectory(const std::wstring& configPath) {
    std::vector<wchar_t> parent(configPath.begin(), configPath.end());
    parent.push_back(L'\0');
    if (!PathRemoveFileSpecW(parent.data())) {
        return false;
    }
    const int result = SHCreateDirectoryExW(nullptr, parent.data(), nullptr);
    const DWORD attributes = GetFileAttributesW(parent.data());
    return result == ERROR_SUCCESS || result == ERROR_ALREADY_EXISTS ||
           (attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
}

HANDLE AcquireConfigLock(const std::wstring& configPath) {
    if (!EnsureConfigDirectory(configPath)) {
        return INVALID_HANDLE_VALUE;
    }
    const std::wstring lockPath = configPath + L".lock";
    return CreateFileW(lockPath.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, 0, nullptr,
                       OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_DELETE_ON_CLOSE |
                                        FILE_FLAG_OPEN_REPARSE_POINT,
                       nullptr);
}

struct WindowSearch {
    std::uintptr_t identity = 0;
    HWND window = nullptr;
};

BOOL CALLBACK FindMatchingWindow(HWND window, LPARAM parameter) {
    auto* search = reinterpret_cast<WindowSearch*>(parameter);
    wchar_t className[64]{};
    if (GetClassNameW(window, className, static_cast<int>(std::size(className))) == 0 ||
        std::wstring_view(className) != kMainWindowClass) {
        return TRUE;
    }
    const auto identity = reinterpret_cast<std::uintptr_t>(
        GetPropW(window, kConfigIdentityProperty));
    if (identity == search->identity) {
        search->window = window;
        return FALSE;
    }
    return TRUE;
}

void ActivateExistingInstance(std::uintptr_t identity) {
    constexpr ULONGLONG kWindowDiscoveryTimeoutMilliseconds = 750;
    const ULONGLONG deadline = GetTickCount64() + kWindowDiscoveryTimeoutMilliseconds;
    HWND existing = nullptr;
    do {
        WindowSearch search{identity, nullptr};
        EnumWindows(FindMatchingWindow, reinterpret_cast<LPARAM>(&search));
        existing = search.window;
        if (existing != nullptr) {
            break;
        }
        Sleep(25);
    } while (GetTickCount64() < deadline);

    if (existing == nullptr) {
        return;
    }
    DWORD_PTR ignored = 0;
    SendMessageTimeoutW(existing, kActivateWindowMessage, 0, 0,
                        SMTO_ABORTIFHUNG | SMTO_NORMAL, 1000, &ignored);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    const StartupOptions options = ParseStartupOptions();
    if (!options.valid) {
        MessageBoxW(nullptr, L"启动参数无效。", L"LightLaunch", MB_OK | MB_ICONERROR);
        return 2;
    }

    const std::wstring requestedConfigPath =
        options.configPath.empty() ? lightlaunch::ConfigStore::DefaultPath() : options.configPath;
    const std::wstring configPath = NormalizeConfigPath(requestedConfigPath);
    if (configPath.empty()) {
        MessageBoxW(nullptr, L"无法解析配置路径。", L"LightLaunch", MB_OK | MB_ICONERROR);
        return 2;
    }

    const std::uint64_t configIdentity = ConfigIdentity(configPath);
    static_assert(sizeof(std::uintptr_t) >= sizeof(configIdentity),
                  "LightLaunch is built for 64-bit Windows.");
    const std::wstring mutexName = MutexNameForConfig(configIdentity);
    HANDLE singleInstance = CreateMutexW(nullptr, FALSE, mutexName.c_str());
    if (singleInstance == nullptr) {
        MessageBoxW(nullptr, L"无法创建单实例锁。", L"LightLaunch", MB_OK | MB_ICONERROR);
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        ActivateExistingInstance(static_cast<std::uintptr_t>(configIdentity));
        CloseHandle(singleInstance);
        return 0;
    }

    HANDLE configLock = AcquireConfigLock(configPath);
    if (configLock == INVALID_HANDLE_VALUE) {
        MessageBoxW(nullptr, L"配置文件正在被其他实例使用，或配置目录不可写。",
                    L"LightLaunch", MB_OK | MB_ICONERROR);
        CloseHandle(singleInstance);
        return 1;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES |
                     ICC_BAR_CLASSES;
    if (!InitCommonControlsEx(&controls)) {
        CloseHandle(configLock);
        CloseHandle(singleInstance);
        return 1;
    }

    const HRESULT oleResult = OleInitialize(nullptr);
    lightlaunch::Application application(instance, configPath,
                                         static_cast<std::uintptr_t>(configIdentity));
    const int exitCode = application.Run(showCommand);
    if (SUCCEEDED(oleResult)) {
        OleUninitialize();
    }
    CloseHandle(configLock);
    CloseHandle(singleInstance);
    return exitCode;
}
