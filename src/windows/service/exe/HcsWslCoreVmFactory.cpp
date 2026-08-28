// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "HcsWslCoreVmFactory.h"

#include "WslCoreVm.h"

std::unique_ptr<IWslCoreVm> HcsWslCoreVmFactory::Create(
    _In_ const wil::shared_handle& UserToken,
    _In_ wsl::core::Config&& VmConfig,
    _In_ const GUID& VmId,
    _In_ IWslCoreVm::InitializeDrvFsCallback InitializeDrvFs)
{
    return WslCoreVm::Create(UserToken, std::move(VmConfig), VmId, std::move(InitializeDrvFs));
}

void HcsWslCoreVmFactory::ForceTerminate(_In_ const GUID& VmId)
{
    const auto vmId = wsl::shared::string::GuidToString<wchar_t>(VmId, wsl::shared::string::GuidToStringFlags::Uppercase);
    auto computeSystem = wsl::windows::common::hcs::OpenComputeSystem(vmId.c_str(), GENERIC_ALL);
    wsl::windows::common::hcs::TerminateComputeSystem(computeSystem.get());
}