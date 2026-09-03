#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lightlaunch {

inline constexpr std::size_t kMaximumCategories = 256;
inline constexpr std::size_t kMaximumItemsPerCategory = 4096;

struct LaunchItem {
    std::wstring name;
    std::wstring target;
    std::wstring arguments;
    std::wstring workingDirectory;

    bool operator==(const LaunchItem&) const = default;
};

enum class FenceBackgroundMode : std::uint8_t {
    Cover = 0,
    Contain = 1,
    Stretch = 2,
};

struct FenceBackground {
    std::wstring imagePath;
    FenceBackgroundMode mode = FenceBackgroundMode::Cover;
    std::uint8_t opacityPercent = 45;
    std::uint16_t cropX = 5000;
    std::uint16_t cropY = 5000;
    // Win32 COLORREF layout (0x00BBGGRR). This is the midpoint of the
    // category Dock's light glass gradient: RGB(230, 234, 231).
    std::uint32_t backgroundColor = 0x00E7EAE6U;
    // Percentage of the complete fence surface that remains transparent.
    // Kept below 100 so a fence can never become impossible to recover.
    std::uint8_t transparencyPercent = 31;
    std::uint32_t borderColor = 0x00FFFFFFU;

    bool operator==(const FenceBackground&) const = default;
};

struct Category {
    std::wstring name;
    std::vector<LaunchItem> items;
    FenceBackground background;

    bool operator==(const Category&) const = default;
};

struct RailAppearance {
    // User-approved default Dock palette: RGB(244, 246, 244), 53%
    // transparency, and RGB(216, 221, 218) border.
    std::uint32_t backgroundColor = 0x00F4F6F4U;
    std::uint8_t transparencyPercent = 53;
    std::uint32_t borderColor = 0x00DADDD8U;

    bool operator==(const RailAppearance&) const = default;
};

struct AppState {
    std::vector<Category> categories;
    std::size_t selectedCategory = 0;
    RailAppearance railAppearance;

    bool operator==(const AppState&) const = default;
};

}  // namespace lightlaunch
