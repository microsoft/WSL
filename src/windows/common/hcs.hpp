/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    hcs.hpp

Abstract:

    This file contains host compute service helper function declarations.

--*/

#pragma once
#include "hcs_schema.h"
#include "hns_schema.h"
#include <ComputeNetwork.h>
#include <ComputeCore.h>

namespace wsl::windows::common::hcs {

using unique_hcn_endpoint = wil::unique_any<HCN_ENDPOINT, decltype(&HcnCloseEndpoint), HcnCloseEndpoint>;

using unique_hcn_service_callback = wil::unique_any<HCN_CALLBACK, decltype(&HcnUnregisterServiceCallback), HcnUnregisterServiceCallback>;

using unique_hcn_guest_network_service_callback =
    wil::unique_any<HCN_CALLBACK, decltype(&HcnUnregisterGuestNetworkServiceCallback), HcnUnregisterGuestNetworkServiceCallback>;

using unique_hcn_guest_network_service =
    wil::unique_any<HCN_GUESTNETWORKSERVICE, decltype(&::HcnCloseGuestNetworkService), ::HcnCloseGuestNetworkService>;

using unique_hcn_network = wil::unique_any<HCN_NETWORK, decltype(&HcnCloseNetwork), HcnCloseNetwork>;

using unique_hcs_operation = wil::unique_any<HCS_OPERATION, decltype(&HcsCloseOperation), HcsCloseOperation>;

using unique_hcs_system = wil::unique_any<HCS_SYSTEM, decltype(&HcsCloseComputeSystem), HcsCloseComputeSystem>;

void AddPlan9Share(
    _In_ HCS_SYSTEM ComputeSystem,
    _In_ PCWSTR Name,
    _In_ PCWSTR AccessName,
    _In_ PCWSTR Path,
    _In_ UINT32 Port,
    _In_ Plan9ShareFlags Flags,
    _In_opt_ HANDLE UserToken = nullptr);

void AddVhd(_In_ HCS_SYSTEM ComputeSystem, _In_ PCWSTR VhdPath, _In_ ULONG Lun, _In_ bool ReadOnly = false);

void AddPassThroughDisk(_In_ HCS_SYSTEM ComputeSystem, _In_ PCWSTR Disk, _In_ ULONG Lun);

unique_hcs_system CreateComputeSystem(_In_ PCWSTR Id, _In_ PCWSTR Configuration);

unique_hcs_operation CreateOperation();

wsl::shared::hns::HNSEndpoint GetEndpointProperties(HCN_ENDPOINT endpoint);

const std::vector<std::string>& GetProcessorFeatures();

GUID GetRuntimeId(_In_ HCS_SYSTEM ComputeSystem);

std::pair<uint32_t, uint32_t> GetSchemaVersion();

void GrantVmAccess(_In_ PCWSTR VmId, _In_ PCWSTR FilePath);

void ModifyComputeSystem(_In_ HCS_SYSTEM ComputeSystem, _In_ PCWSTR Configuration, _In_opt_ HANDLE Identity = nullptr);

unique_hcs_system OpenComputeSystem(_In_ PCWSTR Id, _In_ DWORD RequestedAccess);

void RegisterCallback(_In_ HCS_SYSTEM ComputeSystem, _In_ HCS_EVENT_CALLBACK Callback, _In_ void* Context);

void RemoveScsiDisk(_In_ HCS_SYSTEM ComputeSystem, _In_ ULONG Lun);

void RevokeVmAccess(_In_ PCWSTR VmId, _In_ PCWSTR FilePath);

void StartComputeSystem(_In_ HCS_SYSTEM ComputeSystem, _In_ LPCWSTR Configuration);

void TerminateComputeSystem(_In_ HCS_SYSTEM ComputeSystem);

unique_hcn_service_callback RegisterServiceCallback(_In_ HCS_NOTIFICATION_CALLBACK Callback, _In_ PVOID Context);

unique_hcn_guest_network_service_callback RegisterGuestNetworkServiceCallback(
    _In_ const unique_hcn_guest_network_service& GuestNetworkService, _In_ HCS_NOTIFICATION_CALLBACK Callback, _In_ PVOID Context);

} // namespace wsl::windows::common::hcs
