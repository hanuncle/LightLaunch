#pragma once

#include <windows.h>

#include <optional>
#include <string>

namespace lightlaunch {

std::optional<std::wstring> PromptForText(HWND owner, HINSTANCE instance,
                                          const std::wstring& title,
                                          const std::wstring& prompt,
                                          const std::wstring& initialValue = {});

}  // namespace lightlaunch

