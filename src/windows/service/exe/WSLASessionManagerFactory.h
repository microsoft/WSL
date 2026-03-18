/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLASessionManagerFactory.h

Abstract:

    Contains the definitions for WSLASessionManagerFactory.

--*/

#pragma once
#include <wil/resource.h>

namespace wsl::windows::service::wsla {

class WSLASessionManagerFactory : public Microsoft::WRL::ClassFactory<>
{
public:
    WSLASessionManagerFactory() = default;

    STDMETHODIMP CreateInstance(_In_ IUnknown* pUnkOuter, _In_ REFIID riid, _Out_ void** ppCreated) override;
};

void ClearWslaSessionsAndBlockNewInstances();
} // namespace wsl::windows::service::wsla