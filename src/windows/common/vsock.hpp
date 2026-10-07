// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <winsock2.h>
#include <afunix.h>
#include <filesystem>
#include <wil/resource.h>

namespace wsl::windows::common::vsock {

SOCKADDR_UN GetUnixSocketAddress(_In_ const std::filesystem::path& Path);

wil::unique_socket Connect(_In_ const std::filesystem::path& VsockPath, _In_ unsigned long Port, _In_opt_ HANDLE ExitHandle = nullptr);

} // namespace wsl::windows::common::vsock
