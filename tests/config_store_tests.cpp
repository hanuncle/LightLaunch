#include "config_store.h"
#include "reorder.h"

#include <windows.h>

#include <iostream>
#include <string>

namespace {

bool Check(bool condition, const wchar_t* message) {
    if (!condition) {
        std::wcerr << L"FAILED: " << message << L'\n';
    }
    return condition;
}

bool WriteUnicodeText(const std::wstring& path, const std::wstring& text) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    constexpr wchar_t bom = 0xFEFF;
    DWORD written = 0;
    const bool bomWritten = WriteFile(file, &bom, sizeof(bom), &written, nullptr) != FALSE &&
                            written == sizeof(bom);
    const DWORD textBytes = static_cast<DWORD>(text.size() * sizeof(wchar_t));
    const bool textWritten = WriteFile(file, text.data(), textBytes, &written, nullptr) != FALSE &&
                             written == textBytes;
    const bool flushed = FlushFileBuffers(file) != FALSE;
    CloseHandle(file);
    return bomWritten && textWritten && flushed;
}

std::wstring TemporaryConfigPath() {
    wchar_t temporaryDirectory[MAX_PATH]{};
    const DWORD directoryLength = GetTempPathW(MAX_PATH, temporaryDirectory);
    if (directoryLength == 0 || directoryLength >= MAX_PATH) {
        return {};
    }

    wchar_t temporaryFile[MAX_PATH]{};
    if (GetTempFileNameW(temporaryDirectory, L"LLT", 0, temporaryFile) == 0 ||
        !DeleteFileW(temporaryFile)) {
        return {};
    }
    return temporaryFile;
}

bool StatesEqual(const lightlaunch::AppState& left, const lightlaunch::AppState& right) {
    if (left.selectedCategory != right.selectedCategory ||
        left.railAppearance != right.railAppearance ||
        left.categories.size() != right.categories.size()) {
        return false;
    }

    for (std::size_t categoryIndex = 0; categoryIndex < left.categories.size(); ++categoryIndex) {
        const auto& leftCategory = left.categories[categoryIndex];
        const auto& rightCategory = right.categories[categoryIndex];
        if (leftCategory.name != rightCategory.name ||
            leftCategory.background != rightCategory.background ||
            leftCategory.items.size() != rightCategory.items.size()) {
            return false;
        }

        for (std::size_t itemIndex = 0; itemIndex < leftCategory.items.size(); ++itemIndex) {
            const auto& leftItem = leftCategory.items[itemIndex];
            const auto& rightItem = rightCategory.items[itemIndex];
            if (leftItem.name != rightItem.name || leftItem.target != rightItem.target ||
                leftItem.arguments != rightItem.arguments ||
                leftItem.workingDirectory != rightItem.workingDirectory) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace

int wmain() {
    const std::wstring path = TemporaryConfigPath();
    if (path.empty()) {
        std::wcerr << L"FAILED: temporary config path could not be created.\n";
        return 1;
    }
    lightlaunch::ConfigStore store(path);

    lightlaunch::AppState expected;
    expected.selectedCategory = 1;
    expected.railAppearance.backgroundColor = 0x00D9E6F0U;
    expected.railAppearance.transparencyPercent = 42;
    expected.railAppearance.borderColor = 0x00B8A690U;
    expected.categories = {
        {L"常用", {{L"记事本", L"C:\\Windows\\System32\\notepad.exe", L"\"完整参数\"", L"C:\\Windows\\System32"}}},
        {L"游戏开发", {
            {L"Unity Hub", L"D:\\开发工具\\Unity Hub.exe", L"--project \\\"测试项目\\\"", L"D:\\开发工具"},
            {L"素材目录", L"D:\\游戏素材", L"", L"D:\\游戏素材"},
        }},
        {L" \"特殊分组\" = ; 🚀 ", {
            {L" \"特殊项目\" ", L"C:\\带空格\\工具.exe", L"\"完整参数\" ", L"C:\\带空格"},
        }},
    };
    expected.categories[1].background.imagePath =
        L"D:\\游戏素材\\背景图 \"夜景\".png";
    expected.categories[1].background.mode =
        lightlaunch::FenceBackgroundMode::Contain;
    expected.categories[1].background.opacityPercent = 68;
    expected.categories[1].background.cropX = 2375;
    expected.categories[1].background.cropY = 8125;
    expected.categories[1].background.backgroundColor = 0x007E5C2AU;
    expected.categories[1].background.transparencyPercent = 54;
    expected.categories[1].background.borderColor = 0x00D8C5A4U;

    bool success = true;
    const lightlaunch::RailAppearance defaultRail;
    success &= Check(defaultRail.backgroundColor == 0x00F4F6F4U &&
                         defaultRail.transparencyPercent == 53 &&
                         defaultRail.borderColor == 0x00DADDD8U,
                     L"default Dock appearance should match the approved palette");
    const lightlaunch::FenceBackground defaultFence;
    success &= Check(defaultFence.backgroundColor == 0x00E7EAE6U &&
                         defaultFence.transparencyPercent == 31 &&
                         defaultFence.borderColor == 0x00FFFFFFU,
                     L"new fences should match the approved group 3 appearance");
    std::vector<int> reordered{0, 1, 2, 3};
    success &= Check(lightlaunch::MoveVectorElement(reordered, 0, 2),
                     L"forward drag reorder should succeed");
    success &= Check(reordered == std::vector<int>({1, 2, 0, 3}),
                     L"forward drag reorder should place the source at the destination");
    success &= Check(lightlaunch::MoveVectorElement(reordered, 3, 1),
                     L"backward drag reorder should succeed");
    success &= Check(reordered == std::vector<int>({1, 3, 2, 0}),
                     L"backward drag reorder should place the source at the destination");
    success &= Check(!lightlaunch::MoveVectorElement(reordered, 1, 1),
                     L"dropping at the current position should be a no-op");
    success &= Check(lightlaunch::RemapIndexAfterMove(0, 0, 2) == 2 &&
                         lightlaunch::RemapIndexAfterMove(1, 0, 2) == 0 &&
                         lightlaunch::RemapIndexAfterMove(2, 0, 2) == 1,
                     L"open fence indices should follow a forward category reorder");
    success &= Check(store.Save(expected), L"save should succeed");

    lightlaunch::AppState loaded;
    success &= Check(store.Load(loaded), L"load should succeed");
    success &= Check(StatesEqual(expected, loaded), L"round-trip should preserve Unicode and fields");

    lightlaunch::AppState newer = expected;
    newer.categories[0].name = L"新的常用";
    success &= Check(store.Save(newer), L"second save should succeed and create backup");

    HANDLE corrupt = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (corrupt != INVALID_HANDLE_VALUE) {
        constexpr char invalid[] = "not an ini";
        DWORD written = 0;
        WriteFile(corrupt, invalid, sizeof(invalid), &written, nullptr);
        CloseHandle(corrupt);
    }

    lightlaunch::AppState recovered;
    success &= Check(store.Load(recovered), L"load should recover from backup");
    success &= Check(StatesEqual(expected, recovered), L"backup should contain previous valid state");

    recovered.categories[0].name = L"从备份恢复后继续保存";
    success &= Check(store.Save(recovered), L"save after backup recovery should succeed");
    lightlaunch::AppState afterRecoverySave;
    success &= Check(store.Load(afterRecoverySave), L"new main config should load after recovery save");
    success &= Check(StatesEqual(recovered, afterRecoverySave),
                     L"saving recovered state must not preserve corrupt main data");

    success &= Check(store.Save(newer), L"save should prepare a valid backup for truncation test");
    success &= Check(WriteUnicodeText(path, L"[General]\r\nSchemaVersion=1\r\n"),
                     L"general-only config should be written");
    lightlaunch::AppState truncatedRecovery;
    success &= Check(store.Load(truncatedRecovery),
                     L"general-only config should recover from backup");
    success &= Check(StatesEqual(recovered, truncatedRecovery),
                     L"general-only config must not be accepted as an empty state");

    success &= Check(store.Save(newer), L"save should replace the truncated main config");
    success &= Check(WritePrivateProfileStringW(L"Category.0", L"Name", L"b64:@@==",
                                                path.c_str()) != FALSE,
                     L"encoded field corruption should be written");
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
    lightlaunch::AppState fieldRecovery;
    success &= Check(store.Load(fieldRecovery),
                     L"malformed encoded field should recover from backup");
    success &= Check(StatesEqual(recovered, fieldRecovery),
                     L"malformed encoded field must invalidate the complete main config");

    success &= Check(store.Save(newer), L"save should replace the malformed main config");
    success &= Check(WritePrivateProfileStringW(L"Category.0.Item.0", L"WorkingDirectory",
                                                nullptr, path.c_str()) != FALSE,
                     L"required empty-capable field should be removed for truncation test");
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
    lightlaunch::AppState missingFieldRecovery;
    success &= Check(store.Load(missingFieldRecovery),
                     L"missing empty-capable field should recover from backup");
    success &= Check(StatesEqual(recovered, missingFieldRecovery),
                     L"missing empty-capable field must invalidate the complete main config");

    success &= Check(store.Save(newer),
                     L"save should replace the missing-field main config");
    success &= Check(WritePrivateProfileStringW(
                         L"Category.0", L"BackgroundMode", L"9", path.c_str()) !=
                         FALSE,
                     L"out-of-range background mode should be written");
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
    lightlaunch::AppState invalidBackgroundRecovery;
    success &= Check(store.Load(invalidBackgroundRecovery),
                     L"invalid background mode should recover from backup");
    success &= Check(StatesEqual(recovered, invalidBackgroundRecovery),
                     L"invalid background mode must invalidate the complete main config");

    success &= Check(store.Save(expected), L"save should prepare the backup failure test");
    const std::wstring backupPath = path + L".bak";
    const bool backupMadeReadOnly =
        SetFileAttributesW(backupPath.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE;
    success &= Check(backupMadeReadOnly, L"backup should be made read-only for failure injection");
    if (backupMadeReadOnly) {
        success &= Check(!store.Save(newer),
                         L"save must fail when the previous valid config cannot be backed up");
        SetFileAttributesW(backupPath.c_str(), FILE_ATTRIBUTE_NORMAL);
        lightlaunch::AppState afterBackupFailure;
        success &= Check(store.Load(afterBackupFailure),
                         L"main config should remain readable after backup failure");
        success &= Check(StatesEqual(expected, afterBackupFailure),
                         L"backup failure must leave the previous main config untouched");
    }

    const std::wstring legacyPath = TemporaryConfigPath();
    success &= Check(!legacyPath.empty(), L"legacy test path should be created");
    if (!legacyPath.empty()) {
        const std::wstring legacyText =
            L"[General]\r\nSchemaVersion=1\r\nCategoryCount=1\r\nSelectedCategory=0\r\n\r\n"
            L"[Category.0]\r\nName=Legacy Plain Text\r\nItemCount=0\r\n";
        success &= Check(WriteUnicodeText(legacyPath, legacyText),
                         L"legacy plain-text config should be written");
        lightlaunch::ConfigStore legacyStore(legacyPath);
        lightlaunch::AppState legacyState;
        success &= Check(legacyStore.Load(legacyState),
                         L"schema 1 plain-text config should remain readable");
        success &= Check(legacyState.categories.size() == 1 &&
                             legacyState.categories[0].name == L"Legacy Plain Text",
                         L"legacy category should preserve its plain-text name");
        success &= Check(
            legacyState.categories.size() == 1 &&
                legacyState.categories[0].background ==
                    lightlaunch::FenceBackground{},
            L"legacy category should receive default fence background settings");
        DeleteFileW(legacyPath.c_str());
        DeleteFileW((legacyPath + L".bak").c_str());
        DeleteFileW((legacyPath + L".tmp").c_str());
    }

    DeleteFileW(path.c_str());
    SetFileAttributesW(backupPath.c_str(), FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(backupPath.c_str());
    DeleteFileW((path + L".tmp").c_str());

    if (success) {
        std::wcout << L"All config store tests passed.\n";
        return 0;
    }
    return 1;
}
