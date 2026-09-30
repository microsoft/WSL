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
#include "wslutil.h"

#pragma hdrstop

using wsl::windows::common::Context;
using wsl::windows::common::ExecutionContext;

constexpr auto c_processorCapabilities = "ProcessorCapabilities";
constexpr LPCWSTR c_processorCapabilitiesQuery = L"{ \"PropertyQueries\": {\"ProcessorCapabilities\" : {}}}";
constexpr LPCWSTR c_scsiResourcePath = L"VirtualMachine/Devices/Scsi/0/Attachments/";
constexpr LPCWSTR c_gpuResourcePath = L"VirtualMachine/ComputeTopology/Gpu";

std::filesystem::path wsl::windows::common::hcs::WriteVmCrashLog(
    const std::filesystem::path& Folder, std::uint32_t MaxFileCount, const GUID& VmId, HANDLE UserToken, std::wstring_view CrashLog)
{
    auto runAsUser = wil::impersonate_token(UserToken);

    std::error_code error;
    std::filesystem::create_directories(Folder, error);
    if (error.value())
    {
        THROW_WIN32_MSG(error.value(), "Failed to create folder: %ls", Folder.c_str());
    }

    constexpr auto c_extension = L".txt";
    constexpr auto c_prefix = L"kernel-panic-";
    const auto vmId = wsl::shared::string::GuidToString<wchar_t>(VmId, wsl::shared::string::GuidToStringFlags::None);
    const auto fileName = std::format(L"{}{}-{}{}", c_prefix, std::time(nullptr), vmId, c_extension);
    const auto filePath = Folder / fileName;

    auto pred = [&c_extension, &c_prefix](const auto& entry) {
        return WI_IsFlagSet(GetFileAttributes(entry.path().c_str()), FILE_ATTRIBUTE_TEMPORARY) && entry.path().has_extension() &&
               entry.path().extension() == c_extension && entry.path().has_filename() &&
               entry.path().filename().wstring().find(c_prefix) == 0;
    };

    wslutil::EnforceFileLimit(Folder.c_str(), MaxFileCount, pred);

    {
        std::wofstream outputFile(filePath.wstring());
        THROW_HR_IF(E_UNEXPECTED, !outputFile.is_open() || !(outputFile << CrashLog));
    }

    THROW_IF_WIN32_BOOL_FALSE(SetFileAttributesW(filePath.c_str(), FILE_ATTRIBUTE_TEMPORARY));
    return filePath;
}

std::filesystem::path wsl::windows::common::hcs::CreateVmSavedStateFile(const std::filesystem::path& Folder, const GUID& VmId, HANDLE UserToken)
{
    auto runAsUser = wil::impersonate_token(UserToken);
    wsl::windows::common::filesystem::EnsureDirectory(Folder.c_str());

    const auto vmId = wsl::shared::string::GuidToString<wchar_t>(VmId, wsl::shared::string::GuidToStringFlags::None);
    const auto filePath = Folder / std::format(L"saved-state-{}-{}.vmrs", std::time(nullptr), vmId);
    wil::unique_handle file{CreateFileW(filePath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr)};
    THROW_LAST_ERROR_IF(!file);
    auto removeOnFailure = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] { std::filesystem::remove(filePath); });
    GrantVmAccess(vmId.c_str(), filePath.c_str());
    removeOnFailure.release();
    return filePath;
}

void wsl::windows::common::hcs::EnforceVmSavedStateFileLimit(const std::filesystem::path& Folder, size_t MaxFileCount, HANDLE UserToken)
{
    auto runAsUser = wil::impersonate_token(UserToken);
    const auto predicate = [](const auto& entry) {
        return WI_IsFlagSet(GetFileAttributes(entry.path().c_str()), FILE_ATTRIBUTE_TEMPORARY) && entry.path().has_extension() &&
               entry.path().extension() == L".vmrs" && entry.path().has_filename() &&
               entry.path().filename().wstring().starts_with(L"saved-state-") && entry.file_size() > 0;
    };
    wsl::windows::common::wslutil::EnforceFileLimit(Folder.c_str(), MaxFileCount, predicate);
}

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

void wsl::windows::common::hcs::RemovePlan9Share(_In_ HCS_SYSTEM ComputeSystem, _In_ PCWSTR AccessName, _In_ UINT32 Port)
{
    ModifySettingRequest<Plan9Share> request{};
    request.RequestType = ModifyRequestType::Remove;
    request.ResourcePath = L"VirtualMachine/Devices/Plan9/Shares";
    request.Settings.AccessName = AccessName;
    request.Settings.Port = Port;

    ModifyComputeSystem(ComputeSystem, wsl::shared::ToJsonW(request).c_str());
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

void wsl::windows::common::hcs::AddPassThroughDisk(_In_ HCS_SYSTEM ComputeSystem, _In_ PCWSTR Disk, _In_ ULONG Lun, _In_ bool ReadOnly)
{
    ModifySettingRequest<Attachment> request{};
    request.RequestType = ModifyRequestType::Add;
    request.Settings.Path = Disk;
    request.Settings.ReadOnly = ReadOnly;
    request.ResourcePath = c_scsiResourcePath + std::to_wstring(Lun);
    request.Settings.Type = AttachmentType::PassThru;

    ModifyComputeSystem(ComputeSystem, wsl::shared::ToJsonW(request).c_str());
}

void wsl::windows::common::hcs::AddPassThroughDiskWithRetry(_In_ HCS_SYSTEM ComputeSystem, _In_ PCWSTR Disk, _In_ ULONG Lun, _In_ bool ReadOnly, _In_ size_t TimeoutMs)
{
    wsl::shared::retry::RetryWithTimeout<void>(
        std::bind(AddPassThroughDisk, ComputeSystem, Disk, Lun, ReadOnly),
        wsl::windows::common::disk::c_diskOperationRetry,
        std::chrono::milliseconds(TimeoutMs),
        []() { return wil::ResultFromCaughtException() == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION); });
}

void wsl::windows::common::hcs::AddVhdWithAccess(
    _In_ HCS_SYSTEM ComputeSystem,
    _In_ PCWSTR VmId,
    _In_ PCWSTR VhdPath,
    _In_ ULONG Lun,
    _In_ bool ReadOnly,
    _In_opt_ HANDLE UserToken,
    _Inout_ wsl::windows::common::disk::DiskStateFlags& Flags)
{
    auto grantDiskAccess = [&]() {
        auto runAsUser = wil::impersonate_token(UserToken);
        GrantVmAccess(VmId, VhdPath);
        WI_SetFlag(Flags, wsl::windows::common::disk::DiskStateFlags::AccessGranted);
    };

    // Grant the VM access to the disk.
    if (!ReadOnly)
    {
        grantDiskAccess();
    }

    const auto result = wil::ResultFromException([&]() { AddVhd(ComputeSystem, VhdPath, Lun, ReadOnly); });

    if (result == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) && WI_IsFlagClear(Flags, wsl::windows::common::disk::DiskStateFlags::AccessGranted))
    {
        grantDiskAccess();
        AddVhd(ComputeSystem, VhdPath, Lun, ReadOnly);
    }
    else
    {
        THROW_IF_FAILED(result);
    }
}

wsl::windows::common::hcs::unique_hcs_operation wsl::windows::common::hcs::CreateOperation()
{
    unique_hcs_operation operation(::HcsCreateOperation(nullptr, nullptr));
    THROW_LAST_ERROR_IF_MSG(!operation, "HcsCreateOperation");

    return operation;
}

wsl::windows::common::hcs::unique_hcs_system wsl::windows::common::hcs::CreateComputeSystem(_In_ PCWSTR Id, _In_ PCWSTR Configuration)
{
    WSL_LOG_DEBUG("HcsCreateComputeSystem", TraceLoggingValue(Id, "id"), TraceLoggingValue(Configuration, "configuration"));

    ExecutionContext context(Context::HCS);

    const unique_hcs_operation operation = CreateOperation();
    unique_hcs_system system{};
    THROW_IF_FAILED(::HcsCreateComputeSystem(Id, Configuration, operation.get(), nullptr, &system));

    wil::unique_cotaskmem_string resultDocument;
    const auto result = ::HcsWaitForOperationResult(operation.get(), INFINITE, &resultDocument);
    if (FAILED(result))
    {
        // N.B. Logging is split into two calls because the configuration and error strings can be quite long.
        LOG_HR_MSG(result, "HcsCreateComputeSystem(%ls, %ls)", Id, Configuration);
        THROW_HR_MSG(result, "HcsCreateComputeSystem failed (error string: %ls)", resultDocument.get());
    }

    return system;
}

const std::vector<std::string>& wsl::windows::common::hcs::GetProcessorFeatures()
{
    static std::vector<std::string> g_processorFeatures;
    static std::once_flag flag;
    std::call_once(flag, []() {
        ExecutionContext context(Context::HCS);

        wil::unique_cotaskmem_string result;
        THROW_IF_FAILED(::HcsGetServiceProperties(c_processorCapabilitiesQuery, &result));

        const auto properties =
            wsl::shared::FromJson<ServicePropertiesResponse<PropertyResponse<ProcessorCapabilitiesInfo>>>(result.get());

        const auto& response = properties.PropertyResponses.at(c_processorCapabilities);
        if (response.Error)
        {
            THROW_HR_MSG(static_cast<HRESULT>(response.Error->Error), "%hs", response.Error->ErrorMessage.c_str());
        }

        g_processorFeatures = response.Response.ProcessorFeatures;
    });

    return g_processorFeatures;
}

bool wsl::windows::common::hcs::IsNestedVirtualizationSupported()
{
    if constexpr (wsl::shared::Arm64)
    {
        return false;
    }

    if (!helpers::IsWindows11OrAbove())
    {
        return false;
    }

    const auto& processorFeatures = GetProcessorFeatures();
    return std::find(processorFeatures.begin(), processorFeatures.end(), "NestedVirt") != processorFeatures.end();
}

std::pair<bool, bool> wsl::windows::common::hcs::GetPerfmonCapabilities()
{
#ifdef _AMD64_

    HV_X64_HYPERVISOR_HARDWARE_FEATURES hardwareFeatures{};
    __cpuid(reinterpret_cast<int*>(&hardwareFeatures), HvCpuIdFunctionMsHvHardwareFeatures);
    return {hardwareFeatures.ChildPerfmonPmuSupported != 0, hardwareFeatures.ChildPerfmonLbrSupported != 0};

#else

    return {};

#endif
}

wsl::shared::hns::HNSEndpoint wsl::windows::common::hcs::GetEndpointProperties(HCN_ENDPOINT Endpoint)
{
    WSL_LOG_DEBUG("HcsGetEndpointProperties");

    ExecutionContext context(Context::HNS);

    wil::unique_cotaskmem_string propertiesString;
    wil::unique_cotaskmem_string error;
    const auto result = HcnQueryEndpointProperties(Endpoint, nullptr, &propertiesString, &error);
    THROW_IF_FAILED_MSG(result, "HcnQueryEndpointProperties %ls", error.get());

    return wsl::shared::FromJson<wsl::shared::hns::HNSEndpoint>(propertiesString.get());
}

GUID wsl::windows::common::hcs::GetRuntimeId(_In_ HCS_SYSTEM ComputeSystem)
{
    ExecutionContext context(Context::HCS);

    const unique_hcs_operation operation = CreateOperation();
    THROW_IF_FAILED(::HcsGetComputeSystemProperties(ComputeSystem, operation.get(), nullptr));

    wil::unique_cotaskmem_string resultDocument;
    const auto result = ::HcsWaitForOperationResult(operation.get(), INFINITE, &resultDocument);
    THROW_IF_FAILED_MSG(result, "HcsGetComputeSystemProperties failed (error string: %ls)", resultDocument.get());

    const auto properties = wsl::shared::FromJson<Properties>(resultDocument.get());
    THROW_HR_IF(HCS_E_SYSTEM_NOT_FOUND, (properties.SystemType != SystemType::VirtualMachine));

    return properties.RuntimeId;
}

std::pair<uint32_t, uint32_t> wsl::windows::common::hcs::GetSchemaVersion()
{
    static std::pair<uint32_t, uint32_t> g_schemaVersion{};
    static std::once_flag flag;
    std::call_once(flag, []() {
        ExecutionContext context(Context::HCS);

        PropertyQuery query;
        query.PropertyTypes.emplace_back(PropertyType::Basic);
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

        g_schemaVersion = {majorVersion, minorVersion};
    });

    return g_schemaVersion;
}

void wsl::windows::common::hcs::GrantVmAccess(_In_ PCWSTR VmId, _In_ PCWSTR FilePath)
{
    WSL_LOG_DEBUG("HcsGrantVmAccess", TraceLoggingValue(VmId, "vmId"), TraceLoggingValue(FilePath, "filePath"));

    ExecutionContext context(Context::HCS);

    THROW_IF_FAILED_MSG(::HcsGrantVmAccess(VmId, FilePath), "HcsGrantVmAccess(%ls, %ls)", VmId, FilePath);
}

void wsl::windows::common::hcs::GrantVmWorkerProcessAccessToDisk(_In_ PCWSTR VmId, _In_ PCWSTR Disk, _In_opt_ HANDLE UserToken)
{
    if (ARGUMENT_PRESENT(UserToken))
    {
        // Impersonating the user doesn't let us access a block device,
        // check for an elevated token instead.
        THROW_HR_IF(WSL_E_ELEVATION_NEEDED_TO_MOUNT_DISK, ((!wsl::windows::common::security::IsTokenElevated(UserToken))));
    }

    GrantVmAccess(VmId, Disk);
}

void wsl::windows::common::hcs::ModifyComputeSystem(_In_ HCS_SYSTEM ComputeSystem, _In_ PCWSTR Configuration, _In_opt_ HANDLE Identity)
{
    WSL_LOG_DEBUG("HcsModifyComputeSystem", TraceLoggingValue(Configuration, "configuration"));

    ExecutionContext context(Context::HCS);

    const unique_hcs_operation operation = CreateOperation();
    THROW_IF_FAILED_MSG(
        ::HcsModifyComputeSystem(ComputeSystem, operation.get(), Configuration, Identity), "HcsModifyComputeSystem (%ls)", Configuration);

    wil::unique_cotaskmem_string resultDocument;
    const auto result = ::HcsWaitForOperationResult(operation.get(), INFINITE, &resultDocument);
    if (FAILED(result))
    {
        // N.B. Logging is split into two calls because the configuration and error strings can be quite long.
        LOG_HR_MSG(result, "HcsModifyComputeSystem(%ls)", Configuration);
        THROW_HR_MSG(result, "HcsModifyComputeSystem failed (error string: %ls)", resultDocument.get());
    }
}

wsl::windows::common::hcs::unique_hcs_system wsl::windows::common::hcs::OpenComputeSystem(_In_ PCWSTR Id, _In_ DWORD RequestedAccess)
{
    WSL_LOG_DEBUG("HcsOpenComputeSystem", TraceLoggingValue(Id, "id"), TraceLoggingValue(RequestedAccess, "requestedAccess"));

    ExecutionContext context(Context::HCS);

    unique_hcs_system system;
    THROW_IF_FAILED_MSG(::HcsOpenComputeSystem(Id, RequestedAccess, &system), "HcsOpenComputeSystem(%ls)", Id);

    return system;
}

void wsl::windows::common::hcs::RegisterCallback(_In_ HCS_SYSTEM ComputeSystem, _In_ HCS_EVENT_CALLBACK Callback, _In_ void* Context)
{
    WSL_LOG_DEBUG("HcsSetComputeSystemCallback");

    ExecutionContext context(Context::HCS);

    THROW_IF_FAILED(::HcsSetComputeSystemCallback(ComputeSystem, HcsEventOptionNone, Context, Callback));
}

void wsl::windows::common::hcs::RemoveDiskWithAccess(
    _In_ HCS_SYSTEM ComputeSystem, _In_ PCWSTR VmId, _In_ PCWSTR Disk, _In_ ULONG Lun, _In_ wsl::windows::common::disk::DiskStateFlags Flags, _In_ size_t TimeoutMs)
{
    RemoveScsiDisk(ComputeSystem, Lun);
    if (WI_IsFlagSet(Flags, wsl::windows::common::disk::DiskStateFlags::AccessGranted))
    {
        RevokeVmAccess(VmId, Disk);
    }

    // If the disk was online before being attached, revert to that state.
    //
    // N.B. Failures are logged and ignored because the disk is no longer attached to the VM.
    if (WI_IsFlagSet(Flags, wsl::windows::common::disk::DiskStateFlags::Online))
    {
        try
        {
            wsl::windows::common::disk::BringOnline(Disk, TimeoutMs);
        }
        CATCH_LOG()
    }
}

void wsl::windows::common::hcs::AddMirroredGpu(_In_ HCS_SYSTEM ComputeSystem, _In_ bool AllowVendorExtension, _In_ bool DisableGdiAcceleration, _In_ bool DisablePresentation)
{
    ModifySettingRequest<GpuConfiguration> request{};
    request.ResourcePath = c_gpuResourcePath;
    request.RequestType = ModifyRequestType::Update;
    request.Settings.AssignmentMode = GpuAssignmentMode::Mirror;
    request.Settings.AllowVendorExtension = AllowVendorExtension;

    // N.B. Hosts that predate these settings reject a request that carries them.
    if (IsDisableVgpuSettingsSupported())
    {
        request.Settings.DisableGdiAcceleration = DisableGdiAcceleration;
        request.Settings.DisablePresentation = DisablePresentation;
    }

    ModifyComputeSystem(ComputeSystem, wsl::shared::ToJsonW(request).c_str());
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
    WSL_LOG_DEBUG("HcsRevokeVmAccess", TraceLoggingValue(VmId, "vmId"), TraceLoggingValue(FilePath, "filePath"));

    ExecutionContext context(Context::HCS);

    THROW_IF_FAILED_MSG(::HcsRevokeVmAccess(VmId, FilePath), "HcsRevokeVmAccess(%ls, %ls)", VmId, FilePath);
}

void wsl::windows::common::hcs::StartComputeSystem(_In_ HCS_SYSTEM ComputeSystem, _In_ LPCWSTR Configuration)
{
    WSL_LOG_DEBUG("HcsStartComputeSystem", TraceLoggingValue(Configuration, "configuration"));

    ExecutionContext context(Context::HCS);

    const unique_hcs_operation operation = CreateOperation();
    THROW_IF_FAILED(::HcsStartComputeSystem(ComputeSystem, operation.get(), nullptr));

    wil::unique_cotaskmem_string resultDocument;
    const auto result = ::HcsWaitForOperationResult(operation.get(), INFINITE, &resultDocument);
    if (FAILED(result))
    {
        // N.B. Logging is split into two calls because the configuration and error strings can be quite long.
        LOG_HR_MSG(result, "HcsStartComputeSystem(%ls)", Configuration);
        THROW_HR_MSG(result, "HcsStartComputeSystem failed (error string: %ls)", resultDocument.get());
    }
}

void wsl::windows::common::hcs::TerminateComputeSystem(_In_ HCS_SYSTEM ComputeSystem)
{
    WSL_LOG_DEBUG("HcsTerminateComputeSystem");

    ExecutionContext context(Context::HCS);

    const unique_hcs_operation operation = CreateOperation();
    THROW_IF_FAILED(::HcsTerminateComputeSystem(ComputeSystem, operation.get(), nullptr));

    wil::unique_cotaskmem_string resultDocument;
    const auto result = ::HcsWaitForOperationResult(operation.get(), INFINITE, &resultDocument);
    THROW_IF_FAILED_MSG(result, "HcsTerminateComputeSystem failed (error string: %ls)", resultDocument.get());
}

wsl::windows::common::hcs::unique_hcn_service_callback wsl::windows::common::hcs::RegisterServiceCallback(
    _In_ HCS_NOTIFICATION_CALLBACK Callback, _In_ PVOID Context)
{
    WSL_LOG_DEBUG("HcsRegisterServiceCallback");

    ExecutionContext context(Context::HNS);

    unique_hcn_service_callback callbackHandle;
    THROW_IF_FAILED(::HcnRegisterServiceCallback(Callback, Context, &callbackHandle));

    return callbackHandle;
}

wsl::windows::common::hcs::unique_hcn_guest_network_service_callback wsl::windows::common::hcs::RegisterGuestNetworkServiceCallback(
    _In_ const unique_hcn_guest_network_service& GuestNetworkService, _In_ HCS_NOTIFICATION_CALLBACK Callback, _In_ PVOID Context)
{
    WSL_LOG_DEBUG("HcsRegisterGuestNetworkServiceCallback");

    ExecutionContext context(Context::HNS);

    unique_hcn_guest_network_service_callback callbackHandle;
    THROW_IF_FAILED(::HcnRegisterGuestNetworkServiceCallback(GuestNetworkService.get(), Callback, Context, &callbackHandle));

    return callbackHandle;
}

bool wsl::windows::common::hcs::IsDisableVgpuSettingsSupported()
{
    static constexpr std::pair<uint32_t, uint32_t> c_schemaVersionNickel{2, 7};

    // See if the Windows version has the required platform change.
    return ((GetSchemaVersion() >= c_schemaVersionNickel) && (wsl::windows::common::helpers::GetWindowsVersion().BuildNumber >= 22545));
}

bool wsl::windows::common::hcs::IsSmallPageMemorySupported()
{
    const auto version = wsl::windows::common::helpers::GetWindowsVersion();
    using wsl::windows::common::helpers::WindowsBuildNumbers;
    return (version.BuildNumber >= WindowsBuildNumbers::Germanium) ||
           (version.BuildNumber >= WindowsBuildNumbers::Cobalt && version.UpdateBuildRevision >= 2360) ||
           (version.BuildNumber >= WindowsBuildNumbers::Iron && version.UpdateBuildRevision >= 1970) ||
           (version.BuildNumber >= WindowsBuildNumbers::Vibranium_22H2 && version.UpdateBuildRevision >= 3393);
}
