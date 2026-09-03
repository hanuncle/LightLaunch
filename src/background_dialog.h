#pragma once

#include "model.h"

#include <windows.h>

#include <optional>

namespace lightlaunch {

std::optional<FenceBackground> PromptForFenceBackground(
    HWND owner, HINSTANCE instance, const FenceBackground& initial);

}  // namespace lightlaunch
