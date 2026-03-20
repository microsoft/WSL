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

int UpdatePackage(bool PreRelease, bool Repair);

UINT UpgradeViaMsi(_In_ LPCWSTR PackageLocation, _In_opt_ LPCWSTR ExtraArgs, _In_opt_ LPCWSTR LogFile, _In_ const std::function<void(INSTALLMESSAGE, LPCWSTR)>& callback);

UINT UninstallViaMsi(_In_opt_ LPCWSTR LogFile, _In_ const std::function<void(INSTALLMESSAGE, LPCWSTR)>& callback);

void WriteInstallLog(const std::string& Content);

} // namespace wsl::windows::common::install
