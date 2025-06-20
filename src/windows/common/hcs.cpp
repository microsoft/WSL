/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    hcs.cpp

Abstract:

    This file contains helper function definitions for interacting with the
    host compute service.

--*/

#include "precomp.h"
#include "hcs.hpp"
#include <ComputeCore.h>

#pragma hdrstop

using wsl::windows::common::Context;
using wsl::windows::common::ExecutionContext;

constexpr auto c_processorCapabilities = "ProcessorCapabilities";
constexpr LPCWSTR c_processorCapabilitiesQuery = L"{ \"PropertyQueries\": {\"ProcessorCapabilities\" : {}}}";
constexpr LPCWSTR c_scsiResourcePath = L"VirtualMachine/Devices/Scsi/0/Attachments/";

void wsl::windows::common::hcs::AddPlan9Share(
    _In_ HCS_SYSTEM ComputeSystem, _In_ PCWSTR Name, _In_ PCWSTR AccessName, _In_ PCWSTR Path, _In_ UINT32 Port, _In_ Plan9ShareFlags Flags, _In_opt_ HANDLE UserToken)
{
    ModifySettingRequest<Plan9Share> request{};
    request.RequestType = ModifyRequestType::Add;
    request.ResourcePath = L"VirtualMachine/Devices/Plan9/Shares";
    request.Settings.Name = Name;
    request.Settings.AccessName = AccessName;
    request.Settings.Path = Path;
    request.Settings.Port = Port;
    WI_SetFlagIf(Flags, Plan9ShareFlags::UseShareRootIdentity, ARGUMENT_PRESENT(UserToken));
    request.Settings.Flags = Flags;

    ModifyComputeSystem(ComputeSystem, wsl::shared::ToJsonW(request).c_str(), UserToken);
}

void wsl::windows::common::hcs::AddVhd(_In_ HCS_SYSTEM ComputeSystem, _In_ PCWSTR VhdPath, _In_ ULONG Lun, _In_ bool ReadOnly)
{
    ModifySettingRequest<Attachment> request{};
    request.RequestType = ModifyRequestType::Add;
    request.ResourcePath = c_scsiResourcePath + std::to_wstring(Lun);
    request.Settings.Path = VhdPath;
    request.Settings.ReadOnly = ReadOnly;
    request.Settings.Type = AttachmentType::VirtualDisk;
    request.Settings.SupportCompressedVolumes = true;
    request.Settings.AlwaysAllowSparseFiles = true;
    request.Settings.SupportEncryptedFiles = true;

    ModifyComputeSystem(ComputeSystem, wsl::shared::ToJsonW(request).c_str());
}

void wsl::windows::common::hcs::AddPassThroughDisk(_In_ HCS_SYSTEM ComputeSystem, _In_ PCWSTR Disk, _In_ ULONG Lun)
{
    ModifySettingRequest<Attachment> request{};
    request.RequestType = ModifyRequestType::Add;
    request.Settings.Path = Disk;
    request.ResourcePath = c_scsiResourcePath + std::to_wstring(Lun);
    request.Settings.Type = AttachmentType::PassThru;

    ModifyComputeSystem(ComputeSystem, wsl::shared::ToJsonW(request).c_str());
}

wsl::windows::common::hcs::unique_hcs_operation wsl::windows::common::hcs::CreateOperation()
{
    unique_hcs_operation operation(::HcsCreateOperation(nullptr, nullptr));
    THROW_LAST_ERROR_IF_MSG(!operation, "HcsCreateOperation");

    return operation;
}

wsl::windows::common::hcs::unique_hcs_system wsl::windows::common::hcs::CreateComputeSystem(_In_ PCWSTR Id, _In_ PCWSTR Configuration)
{
    ExecutionContext context(Context::HCS);

    const unique_hcs_operation operation = CreateOperation();
    unique_hcs_system system{};
    THROW_IF_FAILED(::HcsCreateComputeSystem(Id, Configuration, operation.get(), nullptr, &system));

    wil::unique_cotaskmem_string resultDocument;
    const auto result = ::HcsWaitForOperationResult(operation.get(), INFINITE, &resultDocument);
    THROW_IF_FAILED_MSG(
        result,
        "HcsWaitForOperationResult for HcsCreateComputeSystem failed (%ls %ls - error string: %ls)",
        Id,
        Configuration,
        resultDocument.get());

    return system;
}

std::vector<std::string> wsl::windows::common::hcs::GetProcessorFeatures()
{
    ExecutionContext context(Context::HCS);

    wil::unique_cotaskmem_string result;
    THROW_IF_FAILED(::HcsGetServiceProperties(c_processorCapabilitiesQuery, &result));

    const auto properties = wsl::shared::FromJson<ServicePropertiesResponse<PropertyResponse<ProcessorCapabilitiesInfo>>>(result.get());

    const auto& response = properties.PropertyResponses.at(c_processorCapabilities);
    if (response.Error)
    {
        THROW_HR_MSG(static_cast<HRESULT>(response.Error->Error), "%hs", response.Error->ErrorMessage.c_str());
    }

    return response.Response.ProcessorFeatures;
}

wsl::shared::hns::HNSEndpoint wsl::windows::common::hcs::GetEndpointProperties(HCN_ENDPOINT Endpoint)
{
    wil::unique_cotaskmem_string propertiesString;
    wil::unique_cotaskmem_string error;

    {
        ExecutionContext context(Context::HNS);
        const auto result = HcnQueryEndpointProperties(Endpoint, nullptr, &propertiesString, &error);
        THROW_IF_FAILED_MSG(result, "HcnQueryEndpointProperties %ls", error.get());
    }

    return wsl::shared::FromJson<wsl::shared::hns::HNSEndpoint>(propertiesString.get());
}

GUID wsl::windows::common::hcs::GetRuntimeId(_In_ HCS_SYSTEM ComputeSystem)
{
    ExecutionContext context(Context::HCS);

    const unique_hcs_operation operation = CreateOperation();
    THROW_IF_FAILED(::HcsGetComputeSystemProperties(ComputeSystem, operation.get(), nullptr));

    wil::unique_cotaskmem_string resultDocument;
    const auto result = ::HcsWaitForOperationResult(operation.get(), INFINITE, &resultDocument);
    THROW_IF_FAILED_MSG(
        result, "HcsWaitForOperationResult for HcsGetComputeSystemProperties failed (error string: %ls)", resultDocument.get());

    const auto properties = wsl::shared::FromJson<Properties>(resultDocument.get());
    THROW_HR_IF(HCS_E_SYSTEM_NOT_FOUND, (properties.SystemType != SystemType::VirtualMachine));

    return properties.RuntimeId;
}

std::pair<uint32_t, uint32_t> wsl::windows::common::hcs::GetSchemaVersion()
{
    PropertyQuery query;
    query.PropertyTypes.emplace_back(PropertyType::Basic);

    ExecutionContext context(Context::HCS);
    wil::unique_cotaskmem_string result;

    THROW_IF_FAILED(::HcsGetServiceProperties(wsl::shared::ToJsonW(query).c_str(), &result));

    const auto properties = wsl::shared::FromJson<ServiceProperties<BasicInformation>>(result.get());
    THROW_HR_IF_MSG(E_UNEXPECTED, properties.Properties.empty(), "%ls", result.get());

    uint32_t majorVersion = 0;
    uint32_t minorVersion = 0;
    for (const auto& version : properties.Properties[0].SupportedSchemaVersions)
    {
        if (version.Major >= majorVersion)
        {
            if ((version.Major > majorVersion) || (version.Minor > minorVersion))
            {
                majorVersion = version.Major;
                minorVersion = version.Minor;
            }
        }
    }

    return {majorVersion, minorVersion};
}

void wsl::windows::common::hcs::GrantVmAccess(_In_ PCWSTR VmId, _In_ PCWSTR FilePath)
{
    ExecutionContext context(Context::HCS);

    THROW_IF_FAILED_MSG(::HcsGrantVmAccess(VmId, FilePath), "Path (%ws)", FilePath);
}

void wsl::windows::common::hcs::ModifyComputeSystem(_In_ HCS_SYSTEM ComputeSystem, _In_ PCWSTR Configuration, _In_opt_ HANDLE Identity)
{
    ExecutionContext context(Context::HCS);

    const unique_hcs_operation operation = CreateOperation();
    THROW_IF_FAILED_MSG(
        ::HcsModifyComputeSystem(ComputeSystem, operation.get(), Configuration, Identity), "HcsModifyComputeSystem (%ws)", Configuration);

    wil::unique_cotaskmem_string resultDocument;
    const auto result = ::HcsWaitForOperationResult(operation.get(), INFINITE, &resultDocument);
    THROW_IF_FAILED_MSG(
        result,
        "HcsWaitForOperationResult for HcsModifyComputeSystem failed (%ls - error string: %ls)",
        Configuration,
        resultDocument.get());
}

wsl::windows::common::hcs::unique_hcs_system wsl::windows::common::hcs::OpenComputeSystem(_In_ PCWSTR Id, _In_ DWORD RequestedAccess)
{
    ExecutionContext context(Context::HCS);

    unique_hcs_system system;
    THROW_IF_FAILED(::HcsOpenComputeSystem(Id, RequestedAccess, &system));

    return system;
}

void wsl::windows::common::hcs::RegisterCallback(_In_ HCS_SYSTEM ComputeSystem, _In_ HCS_EVENT_CALLBACK Callback, _In_ void* Context)
{
    ExecutionContext context(Context::HCS);

    THROW_IF_FAILED(::HcsSetComputeSystemCallback(ComputeSystem, HcsEventOptionNone, Context, Callback));
}

void wsl::windows::common::hcs::RemoveScsiDisk(_In_ HCS_SYSTEM ComputeSystem, _In_ ULONG Lun)
{
    ModifySettingRequest<void> request{};
    request.RequestType = ModifyRequestType::Remove;
    request.ResourcePath = c_scsiResourcePath + std::to_wstring(Lun);
    ModifyComputeSystem(ComputeSystem, wsl::shared::ToJsonW(request).c_str());
}

void wsl::windows::common::hcs::RevokeVmAccess(_In_ PCWSTR VmId, _In_ PCWSTR FilePath)
{
    ExecutionContext context(Context::HCS);

    THROW_IF_FAILED(::HcsRevokeVmAccess(VmId, FilePath));
}

void wsl::windows::common::hcs::StartComputeSystem(_In_ HCS_SYSTEM ComputeSystem, _In_ LPCWSTR Configuration)
{
    ExecutionContext context(Context::HCS);

    const unique_hcs_operation operation = CreateOperation();
    THROW_IF_FAILED(::HcsStartComputeSystem(ComputeSystem, operation.get(), nullptr));

    wil::unique_cotaskmem_string resultDocument;
    const auto result = ::HcsWaitForOperationResult(operation.get(), INFINITE, &resultDocument);
    THROW_IF_FAILED_MSG(
        result,
        "HcsWaitForOperationResult for HcsStartComputeSystem failed (error string: %ls, configuration: %ls)",
        resultDocument.get(),
        Configuration);
}

void wsl::windows::common::hcs::TerminateComputeSystem(_In_ HCS_SYSTEM ComputeSystem)
{
    ExecutionContext context(Context::HCS);

    const unique_hcs_operation operation = CreateOperation();
    THROW_IF_FAILED(::HcsTerminateComputeSystem(ComputeSystem, operation.get(), nullptr));

    wil::unique_cotaskmem_string resultDocument;
    const auto result = ::HcsWaitForOperationResult(operation.get(), INFINITE, &resultDocument);
    THROW_IF_FAILED_MSG(
        result, "HcsWaitForOperationResult for HcsTerminateComputeSystem failed (error string: %ls)", resultDocument.get());
}

wsl::windows::common::hcs::unique_hcn_service_callback wsl::windows::common::hcs::RegisterServiceCallback(
    _In_ HCS_NOTIFICATION_CALLBACK Callback, _In_ PVOID Context)
{
    ExecutionContext context(Context::HNS);

    unique_hcn_service_callback callbackHandle;
    THROW_IF_FAILED(::HcnRegisterServiceCallback(Callback, Context, &callbackHandle));

    return callbackHandle;
}

wsl::windows::common::hcs::unique_hcn_guest_network_service_callback wsl::windows::common::hcs::RegisterGuestNetworkServiceCallback(
    _In_ const unique_hcn_guest_network_service& GuestNetworkService, _In_ HCS_NOTIFICATION_CALLBACK Callback, _In_ PVOID Context)
{
    ExecutionContext context(Context::HNS);

    unique_hcn_guest_network_service_callback callbackHandle;
    THROW_IF_FAILED(::HcnRegisterGuestNetworkServiceCallback(GuestNetworkService.get(), Callback, Context, &callbackHandle));

    return callbackHandle;
}