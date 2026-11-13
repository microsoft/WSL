// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <string_view>

namespace wsl::windows::common::install {

// Installer/service contract: the service holds this mutex while any workload is active.
inline constexpr auto c_serviceBusyMutexName = L"Global\\WslServiceBusyMutex";

// Best-effort snapshot; it does not prevent workloads from starting after the check.
// Throws if the status cannot be determined.
bool IsServiceBusy();

// Checks if the client performing this installation is from an explicit install command,
// such as "wsl --update" or "msiexec" (from CLI, or double-clicking the MSI).
// The client is the process that called MsiInstallProduct().
bool IsExplicitInstallClient(std::wstring_view clientProcessId);

// Checks if an object (represented by its handle) is owned by LocalSystem.
bool IsOwnedBySystem(HANDLE handle);

} // namespace wsl::windows::common::install
