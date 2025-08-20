/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLAUserSessionFactory.h

Abstract:

    TODO

--*/
#pragma once
#include <wil/resource.h>

namespace wsl::windows::service::wsla {

class WSLAUserSessionFactory : public Microsoft::WRL::ClassFactory<>
{
public:
    WSLAUserSessionFactory() = default;

    STDMETHODIMP CreateInstance(_In_ IUnknown* pUnkOuter, _In_ REFIID riid, _Out_ void** ppCreated) override;
};

void ClearWslaSessionsAndBlockNewInstances();
} // namespace wsl::windows::service::wsla