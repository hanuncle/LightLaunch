#pragma once

#include "model.h"

#include <cstdint>
#include <string>

namespace lightlaunch {

enum class ConfigLoadResult {
    LoadedPrimary,
    LoadedBackup,
    NotFound,
    UnreadableOrInvalid,
};

struct ConfigLoadOutcome {
    ConfigLoadResult result = ConfigLoadResult::UnreadableOrInvalid;
    std::uint32_t win32Error = 0;
};

class ConfigStore {
public:
    ConfigStore();
    explicit ConfigStore(std::wstring path);

    [[nodiscard]] bool Load(AppState& state) const;
    [[nodiscard]] ConfigLoadOutcome LoadDetailed(AppState& state) const;
    [[nodiscard]] bool Save(const AppState& state) const;
    [[nodiscard]] bool SaveRecovered(const AppState& state,
                                     std::wstring* preservedPrimaryPath = nullptr) const;
    [[nodiscard]] const std::wstring& Path() const noexcept;

    [[nodiscard]] static std::wstring DefaultPath();

private:
    [[nodiscard]] bool LoadFrom(const std::wstring& path, AppState& state,
                                std::uint32_t* win32Error = nullptr) const;
    [[nodiscard]] bool SaveInternal(const AppState& state, bool recovery,
                                    std::wstring* preservedPrimaryPath) const;
    [[nodiscard]] bool WriteTo(const std::wstring& path, const AppState& state) const;

    std::wstring path_;
};

}  // namespace lightlaunch
