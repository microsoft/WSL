/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCSessionManagerFactory.h

Abstract:

    Contains the definitions for WSLCSessionManagerFactory.

--*/

#pragma once
#include <wil/resource.h>

namespace wsl::windows::service::wslc {

class WSLCSessionManagerFactory : public Microsoft::WRL::ClassFactory<>
{
public:
    WSLCSessionManagerFactory() = default;

    STDMETHODIMP CreateInstance(_In_ IUnknown* pUnkOuter, _In_ REFIID riid, _Out_ void** ppCreated) override;
};

void ClearWslcSessionsAndBlockNewInstances();
} // namespace wsl::windows::service::wslc