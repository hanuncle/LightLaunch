#include "config_store.h"
#include "reorder.h"

#include <windows.h>

#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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

bool FileExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::vector<unsigned char> ReadFileBytes(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return {};
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
        size.QuadPart > static_cast<LONGLONG>(MAXDWORD)) {
        CloseHandle(file);
        return {};
    }

    std::vector<unsigned char> bytes(static_cast<std::size_t>(size.QuadPart));
    const DWORD byteCount = static_cast<DWORD>(bytes.size());
    DWORD totalRead = 0;
    bool success = true;
    while (totalRead < byteCount) {
        DWORD bytesRead = 0;
        const DWORD remaining = byteCount - totalRead;
        if (!ReadFile(file, bytes.data() + totalRead, remaining, &bytesRead,
                      nullptr) ||
            bytesRead == 0) {
            success = false;
            break;
        }
        totalRead += bytesRead;
    }
    CloseHandle(file);
    return success && totalRead == byteCount
               ? bytes
               : std::vector<unsigned char>{};
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
    std::vector<std::wstring> recoverySnapshots;
    const auto saveRecovered = [&](const lightlaunch::ConfigStore& targetStore,
                                   const lightlaunch::AppState& targetState) {
        std::wstring snapshotPath;
        const bool saved = targetStore.SaveRecovered(targetState, &snapshotPath);
        if (!snapshotPath.empty()) {
            recoverySnapshots.push_back(std::move(snapshotPath));
        }
        return saved;
    };
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

    lightlaunch::AppState missingState = expected;
    const lightlaunch::ConfigLoadOutcome missingOutcome =
        store.LoadDetailed(missingState);
    success &= Check(missingOutcome.result == lightlaunch::ConfigLoadResult::NotFound,
                     L"missing main and backup should be reported as NotFound");
    success &= Check(StatesEqual(missingState, expected),
                     L"a missing configuration must not mutate the caller state");
    success &= Check(!FileExists(path) && !FileExists(path + L".bak") &&
                         !FileExists(path + L".tmp"),
                     L"loading a missing configuration must not create files");

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
    const lightlaunch::ConfigLoadOutcome recoveryOutcome =
        store.LoadDetailed(recovered);
    success &= Check(recoveryOutcome.result ==
                         lightlaunch::ConfigLoadResult::LoadedBackup &&
                         recoveryOutcome.win32Error == ERROR_INVALID_DATA,
                     L"load should report recovery from an invalid main config");
    success &= Check(StatesEqual(expected, recovered), L"backup should contain previous valid state");

    const std::vector<unsigned char> corruptMainBeforeRecovery = ReadFileBytes(path);
    const std::vector<unsigned char> goodBackupBeforeRecovery =
        ReadFileBytes(path + L".bak");
    success &= Check(!corruptMainBeforeRecovery.empty() &&
                         !goodBackupBeforeRecovery.empty(),
                     L"recovery fixtures should be readable before preservation");
    recovered.categories[0].name = L"从备份恢复后继续保存";
    std::wstring preservedCorruptPath;
    const bool recoveredSaved =
        store.SaveRecovered(recovered, &preservedCorruptPath);
    if (!preservedCorruptPath.empty()) {
        recoverySnapshots.push_back(preservedCorruptPath);
    }
    success &= Check(recoveredSaved,
                     L"explicit save after backup recovery should succeed");
    success &= Check(!preservedCorruptPath.empty() &&
                         ReadFileBytes(preservedCorruptPath) ==
                             corruptMainBeforeRecovery,
                     L"recovery should preserve the unreadable primary file");
    success &= Check(ReadFileBytes(path + L".bak") == goodBackupBeforeRecovery,
                     L"explicit recovery must preserve the known-good backup");
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

    success &= Check(saveRecovered(store, newer),
                     L"explicit recovery should replace the truncated main config");
    success &= Check(WritePrivateProfileStringW(L"Category.0", L"Name", L"b64:@@==",
                                                path.c_str()) != FALSE,
                     L"encoded field corruption should be written");
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
    lightlaunch::AppState fieldRecovery;
    success &= Check(store.Load(fieldRecovery),
                     L"malformed encoded field should recover from backup");
    success &= Check(StatesEqual(recovered, fieldRecovery),
                     L"malformed encoded field must invalidate the complete main config");

    success &= Check(saveRecovered(store, newer),
                     L"explicit recovery should replace the malformed main config");
    success &= Check(WritePrivateProfileStringW(L"Category.0.Item.0", L"WorkingDirectory",
                                                nullptr, path.c_str()) != FALSE,
                     L"required empty-capable field should be removed for truncation test");
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
    lightlaunch::AppState missingFieldRecovery;
    success &= Check(store.Load(missingFieldRecovery),
                     L"missing empty-capable field should recover from backup");
    success &= Check(StatesEqual(recovered, missingFieldRecovery),
                     L"missing empty-capable field must invalidate the complete main config");

    success &= Check(saveRecovered(store, newer),
                     L"explicit recovery should replace the missing-field main config");
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

    success &= Check(saveRecovered(store, expected),
                     L"explicit recovery should prepare the backup failure test");
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

    const std::wstring orphanBackupPath = TemporaryConfigPath();
    success &= Check(!orphanBackupPath.empty(),
                     L"orphan-backup test path should be created");
    if (!orphanBackupPath.empty()) {
        lightlaunch::ConfigStore backupWriter(orphanBackupPath + L".bak");
        success &= Check(backupWriter.Save(expected),
                         L"orphan backup should be prepared");
        lightlaunch::ConfigStore orphanBackupStore(orphanBackupPath);
        lightlaunch::AppState orphanBackupState;
        const lightlaunch::ConfigLoadOutcome orphanBackupOutcome =
            orphanBackupStore.LoadDetailed(orphanBackupState);
        success &= Check(
            orphanBackupOutcome.result ==
                    lightlaunch::ConfigLoadResult::LoadedBackup &&
                orphanBackupOutcome.win32Error == ERROR_FILE_NOT_FOUND,
            L"a valid backup should load when the main file is missing");
        success &= Check(StatesEqual(orphanBackupState, expected),
                         L"orphan backup recovery should preserve state");
        success &= Check(!FileExists(orphanBackupPath),
                         L"loading an orphan backup must not create a main file");
        const std::vector<unsigned char> orphanBackupBytes =
            ReadFileBytes(orphanBackupPath + L".bak");
        success &= Check(!orphanBackupBytes.empty(),
                         L"orphan backup should be readable before lock tests");
        HANDLE orphanBackupLock =
            CreateFileW((orphanBackupPath + L".bak").c_str(),
                        GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
        success &= Check(orphanBackupLock != INVALID_HANDLE_VALUE,
                         L"orphan backup lock should be acquired");
        if (orphanBackupLock != INVALID_HANDLE_VALUE) {
            lightlaunch::AppState blockedOrphanState = expected;
            const lightlaunch::ConfigLoadOutcome blockedOrphanOutcome =
                orphanBackupStore.LoadDetailed(blockedOrphanState);
            success &= Check(
                blockedOrphanOutcome.result ==
                    lightlaunch::ConfigLoadResult::UnreadableOrInvalid,
                L"a missing main and locked backup must not look like first run");
            success &= Check(StatesEqual(blockedOrphanState, expected) &&
                                 !FileExists(orphanBackupPath),
                             L"locked orphan backup must not create or mutate state");
            CloseHandle(orphanBackupLock);
        }
        success &= Check(ReadFileBytes(orphanBackupPath + L".bak") ==
                             orphanBackupBytes,
                         L"locked orphan backup must remain unchanged");

        const std::wstring invalidMainText =
            L"[General]\r\nSchemaVersion=2\r\nCategoryCount=1\r\n"
            L"SelectedCategory=0\r\n";
        success &= Check(WriteUnicodeText(orphanBackupPath, invalidMainText),
                         L"invalid main should be prepared beside the backup");
        const std::vector<unsigned char> invalidMainBytes =
            ReadFileBytes(orphanBackupPath);
        success &= Check(!invalidMainBytes.empty(),
                         L"invalid main should be readable before lock tests");
        orphanBackupLock =
            CreateFileW((orphanBackupPath + L".bak").c_str(),
                        GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
        success &= Check(orphanBackupLock != INVALID_HANDLE_VALUE,
                         L"backup lock beside invalid main should be acquired");
        if (orphanBackupLock != INVALID_HANDLE_VALUE) {
            lightlaunch::AppState blockedInvalidState = expected;
            const lightlaunch::ConfigLoadOutcome blockedInvalidOutcome =
                orphanBackupStore.LoadDetailed(blockedInvalidState);
            success &= Check(
                blockedInvalidOutcome.result ==
                    lightlaunch::ConfigLoadResult::UnreadableOrInvalid,
                L"an invalid main and locked backup must block startup");
            success &= Check(StatesEqual(blockedInvalidState, expected),
                             L"mixed load failure must preserve caller state");
            CloseHandle(orphanBackupLock);
        }
        success &= Check(ReadFileBytes(orphanBackupPath) == invalidMainBytes &&
                             ReadFileBytes(orphanBackupPath + L".bak") ==
                                 orphanBackupBytes,
                         L"mixed load failure must preserve both config files");
        DeleteFileW(orphanBackupPath.c_str());
        DeleteFileW((orphanBackupPath + L".bak").c_str());
        DeleteFileW((orphanBackupPath + L".bak.bak").c_str());
        DeleteFileW((orphanBackupPath + L".bak.tmp").c_str());
        DeleteFileW((orphanBackupPath + L".tmp").c_str());
    }

    const std::wstring invalidPairPath = TemporaryConfigPath();
    success &= Check(!invalidPairPath.empty(),
                     L"invalid-pair test path should be created");
    if (!invalidPairPath.empty()) {
        const std::wstring invalidText =
            L"[General]\r\nSchemaVersion=2\r\nCategoryCount=1\r\n"
            L"SelectedCategory=0\r\n";
        success &= Check(WriteUnicodeText(invalidPairPath, invalidText) &&
                             WriteUnicodeText(invalidPairPath + L".bak", invalidText),
                         L"invalid main and backup should be prepared");
        const std::vector<unsigned char> mainBefore =
            ReadFileBytes(invalidPairPath);
        const std::vector<unsigned char> backupBefore =
            ReadFileBytes(invalidPairPath + L".bak");
        success &= Check(!mainBefore.empty() && !backupBefore.empty(),
                         L"invalid-pair fixtures should be readable before tests");
        lightlaunch::ConfigStore invalidPairStore(invalidPairPath);
        lightlaunch::AppState invalidPairState = expected;
        const lightlaunch::ConfigLoadOutcome invalidPairOutcome =
            invalidPairStore.LoadDetailed(invalidPairState);
        success &= Check(
            invalidPairOutcome.result ==
                    lightlaunch::ConfigLoadResult::UnreadableOrInvalid &&
                invalidPairOutcome.win32Error == ERROR_INVALID_DATA,
            L"invalid main and backup should block startup");
        success &= Check(StatesEqual(invalidPairState, expected),
                         L"a failed load must not mutate the caller state");
        success &= Check(ReadFileBytes(invalidPairPath) == mainBefore &&
                             ReadFileBytes(invalidPairPath + L".bak") == backupBefore &&
                             !FileExists(invalidPairPath + L".tmp"),
                         L"a failed load must not rewrite configuration files");
        success &= Check(!invalidPairStore.Save(expected),
                         L"normal save must reject an unreadable existing main file");
        success &= Check(ReadFileBytes(invalidPairPath) == mainBefore &&
                             ReadFileBytes(invalidPairPath + L".bak") == backupBefore &&
                             !FileExists(invalidPairPath + L".tmp"),
                         L"rejected normal save must preserve invalid configuration files");
        DeleteFileW(invalidPairPath.c_str());
        DeleteFileW((invalidPairPath + L".bak").c_str());
        DeleteFileW((invalidPairPath + L".tmp").c_str());
    }

    const std::wstring lockedPath = TemporaryConfigPath();
    success &= Check(!lockedPath.empty(), L"locked-file test path should be created");
    if (!lockedPath.empty()) {
        lightlaunch::ConfigStore lockedStore(lockedPath);
        success &= Check(lockedStore.Save(expected) && lockedStore.Save(newer),
                         L"locked-file test state and backup should be prepared");
        const std::vector<unsigned char> mainBefore = ReadFileBytes(lockedPath);
        const std::vector<unsigned char> backupBefore =
            ReadFileBytes(lockedPath + L".bak");
        success &= Check(!mainBefore.empty() && !backupBefore.empty(),
                         L"locked-file fixtures should be readable before tests");

        HANDLE transientLock =
            CreateFileW(lockedPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        success &= Check(transientLock != INVALID_HANDLE_VALUE,
                         L"transient main-file lock should be acquired");
        if (transientLock != INVALID_HANDLE_VALUE) {
            std::thread unlocker([transientLock]() {
                Sleep(200);
                CloseHandle(transientLock);
            });
            lightlaunch::AppState retriedState;
            const auto retryStart = std::chrono::steady_clock::now();
            const lightlaunch::ConfigLoadOutcome retriedOutcome =
                lockedStore.LoadDetailed(retriedState);
            const auto retryElapsed = std::chrono::steady_clock::now() - retryStart;
            unlocker.join();
            success &= Check(
                retriedOutcome.result ==
                    lightlaunch::ConfigLoadResult::LoadedPrimary,
                L"a short sharing conflict should be retried");
            success &= Check(retryElapsed >= std::chrono::milliseconds(150),
                             L"the sharing retry path should actually be exercised");
            success &= Check(StatesEqual(retriedState, newer),
                             L"retry should load the current main state");
        }

        HANDLE mainLock =
            CreateFileW(lockedPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        success &= Check(mainLock != INVALID_HANDLE_VALUE,
                         L"main-file lock should be acquired");
        if (mainLock != INVALID_HANDLE_VALUE) {
            lightlaunch::AppState backupState;
            const lightlaunch::ConfigLoadOutcome backupOutcome =
                lockedStore.LoadDetailed(backupState);
            success &= Check(
                backupOutcome.result ==
                        lightlaunch::ConfigLoadResult::LoadedBackup &&
                    backupOutcome.win32Error == ERROR_SHARING_VIOLATION,
                L"a persistently locked main file should recover from backup");
            success &= Check(StatesEqual(backupState, expected),
                             L"locked-main recovery should load the backup state");
            CloseHandle(mainLock);
            mainLock = INVALID_HANDLE_VALUE;
            success &= Check(ReadFileBytes(lockedPath) == mainBefore &&
                                 ReadFileBytes(lockedPath + L".bak") == backupBefore,
                             L"backup recovery must not rewrite locked configuration");
        }

        mainLock = CreateFileW(lockedPath.c_str(), GENERIC_READ | GENERIC_WRITE,
                               0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                               nullptr);
        HANDLE backupLock =
            CreateFileW((lockedPath + L".bak").c_str(),
                        GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
        success &= Check(mainLock != INVALID_HANDLE_VALUE &&
                             backupLock != INVALID_HANDLE_VALUE,
                         L"main and backup locks should be acquired");
        if (mainLock != INVALID_HANDLE_VALUE && backupLock != INVALID_HANDLE_VALUE) {
            lightlaunch::AppState blockedState = expected;
            const lightlaunch::ConfigLoadOutcome blockedOutcome =
                lockedStore.LoadDetailed(blockedState);
            success &= Check(
                blockedOutcome.result ==
                        lightlaunch::ConfigLoadResult::UnreadableOrInvalid &&
                    blockedOutcome.win32Error == ERROR_SHARING_VIOLATION,
                L"locked main and backup should block startup safely");
            success &= Check(StatesEqual(blockedState, expected),
                             L"a sharing failure must not mutate caller state");
        }
        if (mainLock != INVALID_HANDLE_VALUE) {
            CloseHandle(mainLock);
        }
        if (backupLock != INVALID_HANDLE_VALUE) {
            CloseHandle(backupLock);
        }
        success &= Check(ReadFileBytes(lockedPath) == mainBefore &&
                             ReadFileBytes(lockedPath + L".bak") == backupBefore &&
                             !FileExists(lockedPath + L".tmp"),
                         L"sharing failures must leave all config files unchanged");

        mainLock = CreateFileW(lockedPath.c_str(), GENERIC_READ | GENERIC_WRITE,
                               0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                               nullptr);
        success &= Check(mainLock != INVALID_HANDLE_VALUE,
                         L"main-file save lock should be acquired");
        bool saveWithLockedMain = true;
        if (mainLock != INVALID_HANDLE_VALUE) {
            saveWithLockedMain = lockedStore.Save(expected);
            CloseHandle(mainLock);
        }
        success &= Check(!saveWithLockedMain,
                         L"save must fail while the existing main file is locked");
        success &= Check(ReadFileBytes(lockedPath) == mainBefore &&
                             ReadFileBytes(lockedPath + L".bak") == backupBefore &&
                             !FileExists(lockedPath + L".tmp") &&
                             !FileExists(lockedPath + L".bak.tmp"),
                         L"failed main-file save must preserve main and backup bytes");

        backupLock = CreateFileW((lockedPath + L".bak").c_str(),
                                 GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        success &= Check(backupLock != INVALID_HANDLE_VALUE,
                         L"backup-file save lock should be acquired");
        bool saveWithLockedBackup = true;
        if (backupLock != INVALID_HANDLE_VALUE) {
            lightlaunch::AppState primaryWithLockedBackup;
            const lightlaunch::ConfigLoadOutcome primaryOutcome =
                lockedStore.LoadDetailed(primaryWithLockedBackup);
            success &= Check(
                primaryOutcome.result ==
                        lightlaunch::ConfigLoadResult::LoadedPrimary &&
                    StatesEqual(primaryWithLockedBackup, newer),
                L"a valid main file must load without touching a locked backup");
            saveWithLockedBackup = lockedStore.Save(expected);
            CloseHandle(backupLock);
        }
        success &= Check(!saveWithLockedBackup,
                         L"save must fail while the backup file is locked");
        success &= Check(ReadFileBytes(lockedPath) == mainBefore &&
                             ReadFileBytes(lockedPath + L".bak") == backupBefore &&
                             !FileExists(lockedPath + L".tmp") &&
                             !FileExists(lockedPath + L".bak.tmp"),
                         L"failed backup save must preserve main and backup bytes");

        DeleteFileW(lockedPath.c_str());
        DeleteFileW((lockedPath + L".bak").c_str());
        DeleteFileW((lockedPath + L".tmp").c_str());
        DeleteFileW((lockedPath + L".bak.tmp").c_str());
    }

    DeleteFileW(path.c_str());
    for (const std::wstring& snapshotPath : recoverySnapshots) {
        DeleteFileW(snapshotPath.c_str());
    }
    SetFileAttributesW(backupPath.c_str(), FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(backupPath.c_str());
    DeleteFileW((path + L".tmp").c_str());

    if (success) {
        std::wcout << L"All config store tests passed.\n";
        return 0;
    }
    return 1;
}
