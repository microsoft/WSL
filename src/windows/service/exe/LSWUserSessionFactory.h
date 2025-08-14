/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    LSWUserSessionFactory.h

Abstract:

    TODO

--*/
#pragma once
#include <wil/resource.h>

namespace wsl::windows::service::lsw {

class LSWUserSessionFactory : public Microsoft::WRL::ClassFactory<>
{
public:
    LSWUserSessionFactory() = default;

    STDMETHODIMP CreateInstance(_In_ IUnknown* pUnkOuter, _In_ REFIID riid, _Out_ void** ppCreated) override;
};

void ClearLswSessionsAndBlockNewInstances();
} // namespace wsl::windows::service::lsw