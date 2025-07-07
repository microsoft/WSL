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

class LSWUSerSessionFactory : public Microsoft::WRL::ClassFactory<>
{
public:
    LSWUSerSessionFactory() = default;

    STDMETHODIMP CreateInstance(_In_ IUnknown* pUnkOuter, _In_ REFIID riid, _Out_ void** ppCreated) override;
};
} // namespace wsl::windows::service::lsw