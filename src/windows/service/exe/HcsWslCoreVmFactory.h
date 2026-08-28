// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include "IWslCoreVmFactory.h"

class HcsWslCoreVmFactory final : public IWslCoreVmFactory
{
public:
    std::unique_ptr<IWslCoreVm> Create(
        _In_ const wil::shared_handle& UserToken,
        _In_ wsl::core::Config&& VmConfig,
        _In_ const GUID& VmId,
        _In_ IWslCoreVm::InitializeDrvFsCallback InitializeDrvFs) override;

    void ForceTerminate(_In_ const GUID& VmId) override;
};