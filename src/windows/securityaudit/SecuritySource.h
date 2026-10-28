// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <string_view>

namespace wsl::audit {
bool IsSecuritySourceInstalled();
void InstallSecuritySource(std::wstring_view messageFile);
void UninstallSecuritySource(std::wstring_view messageFile);
} // namespace wsl::audit
