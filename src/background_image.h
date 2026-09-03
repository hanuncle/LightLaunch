#pragma once

#include "model.h"

#include <windows.h>

#include <string>

namespace lightlaunch {

class BackgroundImage {
public:
    BackgroundImage() = default;
    ~BackgroundImage();

    BackgroundImage(const BackgroundImage&) = delete;
    BackgroundImage& operator=(const BackgroundImage&) = delete;

    bool Load(const std::wstring& path);
    bool IsLoaded() const noexcept;

    // Returns a caller-owned, top-down 32-bpp bitmap precomposed over baseColor.
    HBITMAP Render(const FenceBackground& settings, int width, int height,
                   COLORREF baseColor) const;

private:
    HBITMAP bitmap_ = nullptr;
    UINT width_ = 0;
    UINT height_ = 0;
};

}  // namespace lightlaunch
