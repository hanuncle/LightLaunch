#pragma once

#include "model.h"

#include <string>

namespace lightlaunch {

class ConfigStore {
public:
    ConfigStore();
    explicit ConfigStore(std::wstring path);

    [[nodiscard]] bool Load(AppState& state) const;
    [[nodiscard]] bool Save(const AppState& state) const;
    [[nodiscard]] const std::wstring& Path() const noexcept;

    [[nodiscard]] static std::wstring DefaultPath();

private:
    [[nodiscard]] bool LoadFrom(const std::wstring& path, AppState& state) const;
    [[nodiscard]] bool WriteTo(const std::wstring& path, const AppState& state) const;

    std::wstring path_;
};

}  // namespace lightlaunch

