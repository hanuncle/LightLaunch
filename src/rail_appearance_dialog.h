#pragma once

#include "model.h"

#include <windows.h>

#include <optional>

namespace lightlaunch {

std::optional<RailAppearance> PromptForRailAppearance(
    HWND owner, HINSTANCE instance, const RailAppearance& initial);

}  // namespace lightlaunch
