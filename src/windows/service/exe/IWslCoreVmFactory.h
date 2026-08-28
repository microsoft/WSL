// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "IWslCoreVm.h"

class IWslCoreVmFactory
{
public:
    virtual ~IWslCoreVmFactory() = default;

    virtual std::unique_ptr<IWslCoreVm> Create(
        _In_ const wil::shared_handle& UserToken,
        _In_ wsl::core::Config&& VmConfig,
        _In_ const GUID& VmId,
        _In_ IWslCoreVm::InitializeDrvFsCallback InitializeDrvFs) = 0;

    // May be called without the session lock and concurrently with VM teardown.
    virtual void ForceTerminate(_In_ const GUID& VmId) = 0;
};