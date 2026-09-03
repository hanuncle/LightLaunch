#include "background_image.h"

#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace lightlaunch {
namespace {

using Microsoft::WRL::ComPtr;

constexpr UINT kMaximumDecodedEdge = 2048;

HBITMAP CreateTopDownBitmap(int width, int height, void** pixels) {
    if (pixels == nullptr || width <= 0 || height <= 0) {
        return nullptr;
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    return CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, pixels, nullptr, 0);
}

bool PixelBufferSize(UINT width, UINT height, UINT& stride, UINT& size) {
    const std::uint64_t stride64 = static_cast<std::uint64_t>(width) * 4U;
    const std::uint64_t size64 = stride64 * height;
    if (stride64 > std::numeric_limits<UINT>::max() ||
        size64 > std::numeric_limits<UINT>::max()) {
        return false;
    }
    stride = static_cast<UINT>(stride64);
    size = static_cast<UINT>(size64);
    return true;
}

int FocusOffset(int available, std::uint16_t focus) {
    if (available <= 0) {
        return 0;
    }
    return static_cast<int>(
        (static_cast<std::int64_t>(available) * std::min<std::uint16_t>(focus, 10000) +
         5000) /
        10000);
}

}  // namespace

BackgroundImage::~BackgroundImage() {
    if (bitmap_ != nullptr) {
        DeleteObject(bitmap_);
    }
}

bool BackgroundImage::Load(const std::wstring& path) {
    if (bitmap_ != nullptr) {
        DeleteObject(bitmap_);
        bitmap_ = nullptr;
    }
    width_ = 0;
    height_ = 0;
    if (path.empty()) {
        return false;
    }

    ComPtr<IWICImagingFactory> factory;
    HRESULT result = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                      CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&factory));
    if (FAILED(result)) {
        return false;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    result = factory->CreateDecoderFromFilename(
        path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad,
        &decoder);
    if (FAILED(result)) {
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    result = decoder->GetFrame(0, &frame);
    if (FAILED(result)) {
        return false;
    }

    UINT sourceWidth = 0;
    UINT sourceHeight = 0;
    result = frame->GetSize(&sourceWidth, &sourceHeight);
    if (FAILED(result) || sourceWidth == 0 || sourceHeight == 0) {
        return false;
    }

    UINT decodedWidth = sourceWidth;
    UINT decodedHeight = sourceHeight;
    ComPtr<IWICBitmapScaler> scaler;
    IWICBitmapSource* source = frame.Get();
    const UINT longestEdge = std::max(sourceWidth, sourceHeight);
    if (longestEdge > kMaximumDecodedEdge) {
        const double scale = static_cast<double>(kMaximumDecodedEdge) /
                             static_cast<double>(longestEdge);
        decodedWidth = std::max<UINT>(
            1, static_cast<UINT>(std::lround(sourceWidth * scale)));
        decodedHeight = std::max<UINT>(
            1, static_cast<UINT>(std::lround(sourceHeight * scale)));
        result = factory->CreateBitmapScaler(&scaler);
        if (FAILED(result)) {
            return false;
        }
        result = scaler->Initialize(frame.Get(), decodedWidth, decodedHeight,
                                    WICBitmapInterpolationModeFant);
        if (FAILED(result)) {
            return false;
        }
        source = scaler.Get();
    }

    ComPtr<IWICFormatConverter> converter;
    result = factory->CreateFormatConverter(&converter);
    if (FAILED(result)) {
        return false;
    }
    result = converter->Initialize(
        source, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
        nullptr, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(result)) {
        return false;
    }

    UINT stride = 0;
    UINT bufferSize = 0;
    if (!PixelBufferSize(decodedWidth, decodedHeight, stride, bufferSize)) {
        return false;
    }
    void* pixels = nullptr;
    HBITMAP decoded = CreateTopDownBitmap(static_cast<int>(decodedWidth),
                                          static_cast<int>(decodedHeight),
                                          &pixels);
    if (decoded == nullptr || pixels == nullptr) {
        if (decoded != nullptr) {
            DeleteObject(decoded);
        }
        return false;
    }
    result = converter->CopyPixels(nullptr, stride, bufferSize,
                                   static_cast<BYTE*>(pixels));
    if (FAILED(result)) {
        DeleteObject(decoded);
        return false;
    }

    bitmap_ = decoded;
    width_ = decodedWidth;
    height_ = decodedHeight;
    return true;
}

bool BackgroundImage::IsLoaded() const noexcept {
    return bitmap_ != nullptr && width_ != 0 && height_ != 0;
}

HBITMAP BackgroundImage::Render(const FenceBackground& settings, int width,
                                int height, COLORREF baseColor) const {
    if (!IsLoaded() || width <= 0 || height <= 0) {
        return nullptr;
    }

    void* destinationPixels = nullptr;
    HBITMAP destination =
        CreateTopDownBitmap(width, height, &destinationPixels);
    if (destination == nullptr || destinationPixels == nullptr) {
        if (destination != nullptr) {
            DeleteObject(destination);
        }
        return nullptr;
    }

    const BYTE red = GetRValue(baseColor);
    const BYTE green = GetGValue(baseColor);
    const BYTE blue = GetBValue(baseColor);
    auto* pixels = static_cast<BYTE*>(destinationPixels);
    const std::size_t pixelCount = static_cast<std::size_t>(width) * height;
    for (std::size_t index = 0; index < pixelCount; ++index) {
        pixels[index * 4 + 0] = blue;
        pixels[index * 4 + 1] = green;
        pixels[index * 4 + 2] = red;
        pixels[index * 4 + 3] = 255;
    }

    const BYTE opacity = static_cast<BYTE>(
        (static_cast<unsigned int>(
             std::min<std::uint8_t>(settings.opacityPercent, 100)) *
             255U +
         50U) /
        100U);
    if (opacity == 0) {
        return destination;
    }

    int sourceX = 0;
    int sourceY = 0;
    int sourceWidth = static_cast<int>(width_);
    int sourceHeight = static_cast<int>(height_);
    int destinationX = 0;
    int destinationY = 0;
    int destinationWidth = width;
    int destinationHeight = height;

    if (settings.mode == FenceBackgroundMode::Cover) {
        const double sourceAspect = static_cast<double>(width_) / height_;
        const double destinationAspect = static_cast<double>(width) / height;
        if (sourceAspect > destinationAspect) {
            sourceWidth = std::max(
                1, static_cast<int>(std::lround(height_ * destinationAspect)));
            sourceX = FocusOffset(static_cast<int>(width_) - sourceWidth,
                                  settings.cropX);
        } else if (sourceAspect < destinationAspect) {
            sourceHeight = std::max(
                1, static_cast<int>(std::lround(width_ / destinationAspect)));
            sourceY = FocusOffset(static_cast<int>(height_) - sourceHeight,
                                  settings.cropY);
        }
    } else if (settings.mode == FenceBackgroundMode::Contain) {
        const double scale = std::min(
            static_cast<double>(width) / width_,
            static_cast<double>(height) / height_);
        destinationWidth = std::max(
            1, static_cast<int>(std::lround(width_ * scale)));
        destinationHeight = std::max(
            1, static_cast<int>(std::lround(height_ * scale)));
        destinationX = (width - destinationWidth) / 2;
        destinationY = (height - destinationHeight) / 2;
    }

    HDC sourceDc = CreateCompatibleDC(nullptr);
    HDC destinationDc = CreateCompatibleDC(nullptr);
    if (sourceDc == nullptr || destinationDc == nullptr) {
        if (sourceDc != nullptr) DeleteDC(sourceDc);
        if (destinationDc != nullptr) DeleteDC(destinationDc);
        DeleteObject(destination);
        return nullptr;
    }
    HGDIOBJ oldSource = SelectObject(sourceDc, bitmap_);
    HGDIOBJ oldDestination = SelectObject(destinationDc, destination);
    SetStretchBltMode(destinationDc, HALFTONE);
    SetBrushOrgEx(destinationDc, 0, 0, nullptr);
    const BLENDFUNCTION blend{AC_SRC_OVER, 0, opacity, AC_SRC_ALPHA};
    const BOOL blended = AlphaBlend(
        destinationDc, destinationX, destinationY, destinationWidth,
        destinationHeight, sourceDc, sourceX, sourceY, sourceWidth,
        sourceHeight, blend);
    SelectObject(destinationDc, oldDestination);
    SelectObject(sourceDc, oldSource);
    DeleteDC(destinationDc);
    DeleteDC(sourceDc);
    if (!blended) {
        DeleteObject(destination);
        return nullptr;
    }
    return destination;
}

}  // namespace lightlaunch
