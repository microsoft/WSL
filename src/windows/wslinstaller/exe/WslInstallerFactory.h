/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WslInstallerFactory.h

Abstract:

    This file contains the WslInstallerFactory class definition.

--*/

#pragma once

#include <wil/wrl.h>
#include "WslInstaller.h"

class WslInstallerFactory : public Microsoft::WRL::ClassFactory<>
{
public:
    WslInstallerFactory() = default;
    STDMETHODIMP CreateInstance(_In_ IUnknown* pUnkOuter, _In_ REFIID riid, _Out_ void** ppCreated) override;
};

void ClearSessions();