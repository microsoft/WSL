// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <string>
#include <vector>

namespace wsl::windows::wslc {

void AddProxyEnvironmentDefaults(std::vector<std::string>& Environment, const std::vector<std::string>& Defaults);
std::string ResolveProxyUrl(const std::string& Url, const std::string& HostLoopback);
bool IsLoopbackProxyUrl(const std::string& Url);

} // namespace wsl::windows::wslc
