#include "config_store.h"

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <cwctype>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lightlaunch {
namespace {

constexpr wchar_t kApplicationFolder[] = L"LightLaunch";
constexpr wchar_t kConfigFileName[] = L"config.ini";
constexpr unsigned int kSchemaVersion = 2;
constexpr unsigned int kMinimumSchemaVersion = 1;
constexpr LONGLONG kMaximumConfigBytes = 64LL * 1024 * 1024;
constexpr wchar_t kEncodedPrefix[] = L"b64:";
constexpr char kBase64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

using IniSection = std::unordered_map<std::wstring, std::wstring>;
using IniDocument = std::unordered_map<std::wstring, IniSection>;

std::wstring NormalizeIniName(std::wstring_view name) {
    std::wstring normalized(name);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](wchar_t character) { return std::towlower(character); });
    return normalized;
}

std::wstring_view Trim(std::wstring_view value) {
    while (!value.empty() && std::iswspace(value.front()) != 0) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::iswspace(value.back()) != 0) {
        value.remove_suffix(1);
    }
    return value;
}

bool ReadUnicodeText(const std::wstring& path, std::wstring& text) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 2 ||
        size.QuadPart > kMaximumConfigBytes || (size.QuadPart % 2) != 0) {
        CloseHandle(file);
        return false;
    }

    std::vector<unsigned char> bytes(static_cast<std::size_t>(size.QuadPart));
    std::size_t totalRead = 0;
    bool readResult = true;
    while (totalRead < bytes.size()) {
        DWORD bytesRead = 0;
        const DWORD remaining = static_cast<DWORD>(bytes.size() - totalRead);
        if (!ReadFile(file, bytes.data() + totalRead, remaining, &bytesRead, nullptr) ||
            bytesRead == 0) {
            readResult = false;
            break;
        }
        totalRead += bytesRead;
    }
    CloseHandle(file);
    if (!readResult || totalRead != bytes.size() ||
        bytes[0] != 0xFF || bytes[1] != 0xFE) {
        return false;
    }

    text.assign((bytes.size() - 2) / sizeof(wchar_t), L'\0');
    if (!text.empty()) {
        std::memcpy(text.data(), bytes.data() + 2, bytes.size() - 2);
    }
    return text.find(L'\0') == std::wstring::npos;
}

bool ParseIni(const std::wstring& path, IniDocument& document) {
    std::wstring text;
    if (!ReadUnicodeText(path, text)) {
        return false;
    }

    IniSection* currentSection = nullptr;
    std::size_t offset = 0;
    while (offset <= text.size()) {
        const std::size_t lineEnd = text.find(L'\n', offset);
        const std::size_t count = lineEnd == std::wstring::npos
                                      ? text.size() - offset
                                      : lineEnd - offset;
        std::wstring_view line(text.data() + offset, count);
        if (!line.empty() && line.back() == L'\r') {
            line.remove_suffix(1);
        }
        line = Trim(line);

        if (!line.empty() && line.front() != L';' && line.front() != L'#') {
            if (line.front() == L'[' && line.back() == L']') {
                const std::wstring_view sectionName = Trim(line.substr(1, line.size() - 2));
                if (sectionName.empty()) {
                    return false;
                }
                currentSection = &document[NormalizeIniName(sectionName)];
            } else {
                if (currentSection == nullptr) {
                    return false;
                }
                const std::size_t equals = line.find(L'=');
                if (equals == std::wstring_view::npos) {
                    return false;
                }
                const std::wstring_view key = Trim(line.substr(0, equals));
                const std::wstring_view value = Trim(line.substr(equals + 1));
                if (key.empty()) {
                    return false;
                }
                const auto [iterator, inserted] = currentSection->emplace(
                    NormalizeIniName(key), std::wstring(value));
                if (!inserted) {
                    return false;
                }
            }
        }

        if (lineEnd == std::wstring::npos) {
            break;
        }
        offset = lineEnd + 1;
    }
    return !document.empty();
}

const std::wstring* FindIniValue(const IniDocument& document, const std::wstring& section,
                                 const wchar_t* key) {
    const auto sectionIterator = document.find(NormalizeIniName(section));
    if (sectionIterator == document.end()) {
        return nullptr;
    }
    const auto valueIterator = sectionIterator->second.find(NormalizeIniName(key));
    return valueIterator == sectionIterator->second.end() ? nullptr : &valueIterator->second;
}

std::wstring ParentDirectory(const std::wstring& path) {
    std::vector<wchar_t> buffer(path.begin(), path.end());
    buffer.push_back(L'\0');
    if (!PathRemoveFileSpecW(buffer.data())) {
        return {};
    }
    return buffer.data();
}

bool EnsureParentDirectory(const std::wstring& path) {
    const std::wstring parent = ParentDirectory(path);
    if (parent.empty()) {
        return false;
    }

    const int result = SHCreateDirectoryExW(nullptr, parent.c_str(), nullptr);
    return result == ERROR_SUCCESS || result == ERROR_ALREADY_EXISTS ||
           GetFileAttributesW(parent.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool WriteUnicodeText(const std::wstring& path, const std::wstring& text) {
    if (text.size() > static_cast<std::size_t>((kMaximumConfigBytes - 2) /
                                               sizeof(wchar_t))) {
        return false;
    }

    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    constexpr wchar_t bom = 0xFEFF;
    DWORD written = 0;
    bool writeResult = WriteFile(file, &bom, sizeof(bom), &written, nullptr) != FALSE &&
                       written == sizeof(bom);
    const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
    std::size_t remaining = text.size() * sizeof(wchar_t);
    while (writeResult && remaining > 0) {
        const DWORD chunk = static_cast<DWORD>(remaining);
        written = 0;
        if (!WriteFile(file, bytes, chunk, &written, nullptr) || written == 0) {
            writeResult = false;
            break;
        }
        bytes += written;
        remaining -= written;
    }
    const BOOL flushResult = FlushFileBuffers(file);
    CloseHandle(file);
    return writeResult && remaining == 0 && flushResult;
}

std::wstring EncodeValue(const std::wstring& value) {
    static_assert(sizeof(wchar_t) == 2, "LightLaunch configuration is Windows UTF-16.");

    const auto* bytes = reinterpret_cast<const unsigned char*>(value.data());
    const std::size_t byteCount = value.size() * sizeof(wchar_t);
    std::wstring encoded = kEncodedPrefix;
    encoded.reserve(4 + ((byteCount + 2) / 3) * 4);

    for (std::size_t index = 0; index < byteCount; index += 3) {
        const unsigned int first = bytes[index];
        const unsigned int second = index + 1 < byteCount ? bytes[index + 1] : 0;
        const unsigned int third = index + 2 < byteCount ? bytes[index + 2] : 0;
        const unsigned int combined = (first << 16) | (second << 8) | third;

        encoded.push_back(static_cast<wchar_t>(kBase64Alphabet[(combined >> 18) & 0x3F]));
        encoded.push_back(static_cast<wchar_t>(kBase64Alphabet[(combined >> 12) & 0x3F]));
        encoded.push_back(index + 1 < byteCount
                              ? static_cast<wchar_t>(kBase64Alphabet[(combined >> 6) & 0x3F])
                              : L'=');
        encoded.push_back(index + 2 < byteCount
                              ? static_cast<wchar_t>(kBase64Alphabet[combined & 0x3F])
                              : L'=');
    }
    return encoded;
}

int DecodeBase64Character(wchar_t character) {
    if (character >= L'A' && character <= L'Z') {
        return character - L'A';
    }
    if (character >= L'a' && character <= L'z') {
        return character - L'a' + 26;
    }
    if (character >= L'0' && character <= L'9') {
        return character - L'0' + 52;
    }
    if (character == L'+') {
        return 62;
    }
    if (character == L'/') {
        return 63;
    }
    return -1;
}

std::optional<std::wstring> DecodeValue(const std::wstring& encoded) {
    if (!encoded.starts_with(kEncodedPrefix)) {
        return encoded;
    }

    const std::wstring payload = encoded.substr(std::size(kEncodedPrefix) - 1);
    if (payload.size() % 4 != 0) {
        return std::nullopt;
    }

    std::vector<unsigned char> bytes;
    bytes.reserve((payload.size() / 4) * 3);
    for (std::size_t index = 0; index < payload.size(); index += 4) {
        const bool finalBlock = index + 4 == payload.size();
        const bool thirdIsPadding = payload[index + 2] == L'=';
        const bool fourthIsPadding = payload[index + 3] == L'=';
        if ((!finalBlock && (thirdIsPadding || fourthIsPadding)) ||
            (thirdIsPadding && !fourthIsPadding)) {
            return std::nullopt;
        }

        const int first = DecodeBase64Character(payload[index]);
        const int second = DecodeBase64Character(payload[index + 1]);
        const int third = thirdIsPadding ? 0 : DecodeBase64Character(payload[index + 2]);
        const int fourth = fourthIsPadding ? 0 : DecodeBase64Character(payload[index + 3]);
        if (first < 0 || second < 0 || third < 0 || fourth < 0) {
            return std::nullopt;
        }
        if ((thirdIsPadding && (second & 0x0F) != 0) ||
            (fourthIsPadding && !thirdIsPadding && (third & 0x03) != 0)) {
            return std::nullopt;
        }

        const unsigned int combined = (static_cast<unsigned int>(first) << 18) |
                                      (static_cast<unsigned int>(second) << 12) |
                                      (static_cast<unsigned int>(third) << 6) |
                                      static_cast<unsigned int>(fourth);
        bytes.push_back(static_cast<unsigned char>((combined >> 16) & 0xFF));
        if (!thirdIsPadding) {
            bytes.push_back(static_cast<unsigned char>((combined >> 8) & 0xFF));
        }
        if (!fourthIsPadding) {
            bytes.push_back(static_cast<unsigned char>(combined & 0xFF));
        }
    }

    if (bytes.size() % sizeof(wchar_t) != 0) {
        return std::nullopt;
    }
    std::wstring decoded(bytes.size() / sizeof(wchar_t), L'\0');
    if (!bytes.empty()) {
        std::memcpy(decoded.data(), bytes.data(), bytes.size());
    }
    return decoded;
}

bool ReadString(const IniDocument& document, const std::wstring& section,
                const wchar_t* key, bool allowEmpty, bool requireEncoding,
                std::wstring& value) {
    const std::wstring* raw = FindIniValue(document, section, key);
    if (raw == nullptr || (requireEncoding && !raw->starts_with(kEncodedPrefix))) {
        return false;
    }

    const std::optional<std::wstring> decoded = DecodeValue(*raw);
    if (!decoded.has_value() || (!allowEmpty && decoded->empty())) {
        return false;
    }
    value = *decoded;
    return true;
}

bool ReadOptionalString(const IniDocument& document, const std::wstring& section,
                        const wchar_t* key, bool allowEmpty, bool requireEncoding,
                        std::wstring& value) {
    if (FindIniValue(document, section, key) == nullptr) {
        return true;
    }
    return ReadString(document, section, key, allowEmpty, requireEncoding, value);
}

bool ReadUnsigned(const IniDocument& document, const std::wstring& section,
                  const wchar_t* key, std::size_t maximum, std::size_t& value) {
    const std::wstring* raw = FindIniValue(document, section, key);
    if (raw == nullptr || raw->empty()) {
        return false;
    }

    std::size_t parsed = 0;
    for (const wchar_t character : *raw) {
        if (character < L'0' || character > L'9') {
            return false;
        }
        const std::size_t digit = static_cast<std::size_t>(character - L'0');
        if (digit > maximum || parsed > (maximum - digit) / 10) {
            return false;
        }
        parsed = parsed * 10 + digit;
    }
    value = parsed;
    return true;
}

bool ReadOptionalUnsigned(const IniDocument& document, const std::wstring& section,
                          const wchar_t* key, std::size_t maximum,
                          std::size_t& value) {
    if (FindIniValue(document, section, key) == nullptr) {
        return true;
    }
    return ReadUnsigned(document, section, key, maximum, value);
}

void AppendSetting(std::wstring& document, const std::wstring& key,
                   const std::wstring& value) {
    document.append(key);
    document.push_back(L'=');
    document.append(value);
    document.append(L"\r\n");
}

std::wstring CategorySection(std::size_t categoryIndex) {
    return L"Category." + std::to_wstring(categoryIndex);
}

std::wstring ItemSection(std::size_t categoryIndex, std::size_t itemIndex) {
    return CategorySection(categoryIndex) + L".Item." + std::to_wstring(itemIndex);
}

}  // namespace

ConfigStore::ConfigStore() : path_(DefaultPath()) {}

ConfigStore::ConfigStore(std::wstring path) : path_(std::move(path)) {}

const std::wstring& ConfigStore::Path() const noexcept {
    return path_;
}

std::wstring ConfigStore::DefaultPath() {
    PWSTR localAppData = nullptr;
    std::wstring result;

    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr,
                                       &localAppData)) &&
        localAppData != nullptr) {
        result.assign(localAppData);
        CoTaskMemFree(localAppData);
        result += L"\\";
        result += kApplicationFolder;
        result += L"\\";
        result += kConfigFileName;
        return result;
    }

    std::array<wchar_t, MAX_PATH> executablePath{};
    const DWORD length = GetModuleFileNameW(nullptr, executablePath.data(),
                                            static_cast<DWORD>(executablePath.size()));
    if (length == 0 || length >= executablePath.size()) {
        return kConfigFileName;
    }

    result.assign(executablePath.data(), length);
    const std::wstring parent = ParentDirectory(result);
    if (parent.empty()) {
        return kConfigFileName;
    }
    return parent + L"\\" + kConfigFileName;
}

bool ConfigStore::Load(AppState& state) const {
    AppState loaded;
    if (LoadFrom(path_, loaded)) {
        state = std::move(loaded);
        return true;
    }

    AppState backup;
    if (LoadFrom(path_ + L".bak", backup)) {
        state = std::move(backup);
        return true;
    }

    return false;
}

bool ConfigStore::LoadFrom(const std::wstring& path, AppState& state) const {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return false;
    }

    IniDocument document;
    if (!ParseIni(path, document)) {
        return false;
    }

    std::size_t version = 0;
    if (!ReadUnsigned(document, L"General", L"SchemaVersion", kSchemaVersion, version)) {
        return false;
    }
    if (version < kMinimumSchemaVersion || version > kSchemaVersion) {
        return false;
    }
    const bool requireEncoding = version >= 2;

    std::size_t categoryCount = 0;
    std::size_t selected = 0;
    if (!ReadUnsigned(document, L"General", L"CategoryCount", kMaximumCategories,
                      categoryCount) ||
        !ReadUnsigned(document, L"General", L"SelectedCategory", kMaximumCategories,
                      selected) ||
        (categoryCount == 0 ? selected != 0 : selected >= categoryCount)) {
        return false;
    }

    AppState loaded;
    loaded.categories.reserve(categoryCount);

    for (std::size_t categoryIndex = 0; categoryIndex < categoryCount; ++categoryIndex) {
        const std::wstring section = CategorySection(categoryIndex);
        Category category;
        if (!ReadString(document, section, L"Name", false, requireEncoding,
                        category.name)) {
            return false;
        }

        if (!ReadOptionalString(document, section, L"BackgroundImage", true,
                                requireEncoding,
                                category.background.imagePath)) {
            return false;
        }

        std::size_t backgroundMode =
            static_cast<std::size_t>(category.background.mode);
        std::size_t backgroundOpacity = category.background.opacityPercent;
        std::size_t backgroundCropX = category.background.cropX;
        std::size_t backgroundCropY = category.background.cropY;
        std::size_t backgroundColor = category.background.backgroundColor;
        // Preserve the appearance of image backgrounds written by builds
        // before the independent fence-transparency setting existed.
        std::size_t fenceTransparency =
            category.background.imagePath.empty() ? 31U : 9U;
        std::size_t fenceBorderColor = category.background.borderColor;
        if (!ReadOptionalUnsigned(document, section, L"BackgroundMode",
                                  static_cast<std::size_t>(
                                      FenceBackgroundMode::Stretch),
                                  backgroundMode) ||
            !ReadOptionalUnsigned(document, section, L"BackgroundOpacity", 100,
                                  backgroundOpacity) ||
            !ReadOptionalUnsigned(document, section, L"BackgroundCropX", 10000,
                                  backgroundCropX) ||
            !ReadOptionalUnsigned(document, section, L"BackgroundCropY", 10000,
                                  backgroundCropY) ||
            !ReadOptionalUnsigned(document, section, L"BackgroundColor",
                                  0x00FFFFFFU, backgroundColor) ||
            !ReadOptionalUnsigned(document, section, L"FenceTransparency", 85,
                                  fenceTransparency) ||
            !ReadOptionalUnsigned(document, section, L"FenceBorderColor",
                                  0x00FFFFFFU, fenceBorderColor)) {
            return false;
        }
        category.background.mode =
            static_cast<FenceBackgroundMode>(backgroundMode);
        category.background.opacityPercent =
            static_cast<std::uint8_t>(backgroundOpacity);
        category.background.cropX =
            static_cast<std::uint16_t>(backgroundCropX);
        category.background.cropY =
            static_cast<std::uint16_t>(backgroundCropY);
        category.background.backgroundColor =
            static_cast<std::uint32_t>(backgroundColor);
        category.background.transparencyPercent =
            static_cast<std::uint8_t>(fenceTransparency);
        category.background.borderColor =
            static_cast<std::uint32_t>(fenceBorderColor);

        std::size_t itemCount = 0;
        if (!ReadUnsigned(document, section, L"ItemCount", kMaximumItemsPerCategory,
                          itemCount)) {
            return false;
        }
        category.items.reserve(itemCount);

        for (std::size_t itemIndex = 0; itemIndex < itemCount; ++itemIndex) {
            const std::wstring itemSection = ItemSection(categoryIndex, itemIndex);
            LaunchItem item;
            if (!ReadString(document, itemSection, L"Name", false, requireEncoding,
                            item.name) ||
                !ReadString(document, itemSection, L"Target", false, requireEncoding,
                            item.target) ||
                !ReadString(document, itemSection, L"Arguments", true, requireEncoding,
                            item.arguments) ||
                !ReadString(document, itemSection, L"WorkingDirectory", true,
                            requireEncoding, item.workingDirectory)) {
                return false;
            }
            category.items.push_back(std::move(item));
        }

        loaded.categories.push_back(std::move(category));
    }

    loaded.selectedCategory = selected;

    state = std::move(loaded);
    return true;
}

bool ConfigStore::Save(const AppState& state) const {
    if (!EnsureParentDirectory(path_)) {
        return false;
    }

    const std::wstring temporaryPath = path_ + L".tmp";
    if (!WriteTo(temporaryPath, state)) {
        DeleteFileW(temporaryPath.c_str());
        return false;
    }

    AppState temporaryVerification;
    if (!LoadFrom(temporaryPath, temporaryVerification) || temporaryVerification != state) {
        DeleteFileW(temporaryPath.c_str());
        return false;
    }

    AppState existing;
    if (LoadFrom(path_, existing)) {
        if (!CopyFileW(path_.c_str(), (path_ + L".bak").c_str(), FALSE)) {
            DeleteFileW(temporaryPath.c_str());
            return false;
        }
    }

    if (!MoveFileExW(temporaryPath.c_str(), path_.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporaryPath.c_str());
        return false;
    }
    return true;
}

bool ConfigStore::WriteTo(const std::wstring& path, const AppState& state) const {
    if (state.categories.size() > kMaximumCategories ||
        (state.categories.empty() ? state.selectedCategory != 0
                                  : state.selectedCategory >= state.categories.size())) {
        return false;
    }

    std::wstring document = L"[General]\r\n";
    AppendSetting(document, L"SchemaVersion", std::to_wstring(kSchemaVersion));
    AppendSetting(document, L"CategoryCount", std::to_wstring(state.categories.size()));
    AppendSetting(document, L"SelectedCategory", std::to_wstring(state.selectedCategory));

    for (std::size_t categoryIndex = 0; categoryIndex < state.categories.size(); ++categoryIndex) {
        const Category& category = state.categories[categoryIndex];
        const auto backgroundMode =
            static_cast<unsigned int>(category.background.mode);
        if (category.name.empty() ||
            category.items.size() > kMaximumItemsPerCategory ||
            backgroundMode >
                static_cast<unsigned int>(FenceBackgroundMode::Stretch) ||
            category.background.opacityPercent > 100 ||
            category.background.cropX > 10000 ||
            category.background.cropY > 10000 ||
            category.background.backgroundColor > 0x00FFFFFFU ||
            category.background.transparencyPercent > 85 ||
            category.background.borderColor > 0x00FFFFFFU) {
            return false;
        }
        const std::wstring section = CategorySection(categoryIndex);
        document.append(L"\r\n[").append(section).append(L"]\r\n");
        AppendSetting(document, L"Name", EncodeValue(category.name));
        AppendSetting(document, L"BackgroundImage",
                      EncodeValue(category.background.imagePath));
        AppendSetting(document, L"BackgroundMode",
                      std::to_wstring(backgroundMode));
        AppendSetting(document, L"BackgroundOpacity",
                      std::to_wstring(category.background.opacityPercent));
        AppendSetting(document, L"BackgroundCropX",
                      std::to_wstring(category.background.cropX));
        AppendSetting(document, L"BackgroundCropY",
                      std::to_wstring(category.background.cropY));
        AppendSetting(document, L"BackgroundColor",
                      std::to_wstring(category.background.backgroundColor));
        AppendSetting(document, L"FenceTransparency",
                      std::to_wstring(
                          category.background.transparencyPercent));
        AppendSetting(document, L"FenceBorderColor",
                      std::to_wstring(category.background.borderColor));
        AppendSetting(document, L"ItemCount", std::to_wstring(category.items.size()));

        for (std::size_t itemIndex = 0; itemIndex < category.items.size(); ++itemIndex) {
            const LaunchItem& item = category.items[itemIndex];
            if (item.name.empty() || item.target.empty()) {
                return false;
            }
            const std::wstring itemSection = ItemSection(categoryIndex, itemIndex);
            document.append(L"\r\n[").append(itemSection).append(L"]\r\n");
            AppendSetting(document, L"Name", EncodeValue(item.name));
            AppendSetting(document, L"Target", EncodeValue(item.target));
            AppendSetting(document, L"Arguments", EncodeValue(item.arguments));
            AppendSetting(document, L"WorkingDirectory", EncodeValue(item.workingDirectory));
        }
    }
    return WriteUnicodeText(path, document);
}

}  // namespace lightlaunch
