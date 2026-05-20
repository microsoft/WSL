/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    install.h

Abstract:

    This file contains MSI/Wintrust install helper function declarations.

--*/

#pragma once
#include <functional>

namespace wsl::windows::common::install {

int CallMsiPackage();

void MsiMessageCallback(INSTALLMESSAGE type, LPCWSTR message);

wil::unique_hfile ValidateFileSignature(LPCWSTR Path);

// Setting CallerOwnsProcess to false will prevent this function from making process wide changes or printing output.
int UpdatePackage(bool PreRelease, bool Repair, bool CallerOwnsProcess = true);

UINT UpgradeViaMsi(_In_ LPCWSTR PackageLocation, _In_opt_ LPCWSTR ExtraArgs, _In_opt_ LPCWSTR LogFile, _In_ const std::function<void(INSTALLMESSAGE, LPCWSTR)>& callback);

UINT UninstallViaMsi(_In_opt_ LPCWSTR LogFile, _In_ const std::function<void(INSTALLMESSAGE, LPCWSTR)>& callback);

void WriteInstallLog(const std::string& Content);

// Sets a volatile (auto-cleared on reboot) registry marker indicating the MSI install
// completed but files are pending replacement until the next reboot (ERROR_SUCCESS_REBOOT_REQUIRED).
void SetRebootRequiredMarker();

// Returns true if the reboot-required marker is present (i.e. the machine has not rebooted
// since a 3010-result MSI install).
bool IsRebootRequired();

} // namespace wsl::windows::common::install
