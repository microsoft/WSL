/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WslInstaller.h

Abstract:

    This file contains definitions for the WslInstallerService.

--*/

#pragma once
#include <wil/wrl.h>
#include <wslinstallerservice.h>

class DECLSPEC_UUID("B5AEB4C3-9541-492F-AD4D-505951F6ADA4") WslInstaller
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IWslInstaller, IFastRundown>
{
    HRESULT Install(UINT* ExitCode, LPWSTR* Error) override;
};

struct InstallContext
{
    wil::unique_handle Thread;
    HRESULT Result{};
    UINT ExitCode{};
    std::wstring Errors;
};

std::shared_ptr<InstallContext> LaunchInstall();
