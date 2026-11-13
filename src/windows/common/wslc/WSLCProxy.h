// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace wsl::windows::wslc {

bool IsProxyEnvironmentVariable(std::string_view Entry);
std::string CanonicalizeProxyEnvironmentVariable(std::string_view Entry);
void AddProxyEnvironmentDefaults(std::vector<std::string>& Environment, const std::vector<std::string>& Defaults);
void ResolveProxyEnvironmentLoopback(std::vector<std::string>& Environment, const std::string& HostLoopback);
std::string ResolveProxyUrl(const std::string& Url, const std::string& HostLoopback);
bool IsLoopbackProxyUrl(const std::string& Url);

} // namespace wsl::windows::wslc
