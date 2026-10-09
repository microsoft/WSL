// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "Common.h"
#include "HcsVirtualMachineBackend.h"
#include "VirtualMachineBackendTestHelpers.h"

using namespace VirtualMachineBackendTestHelpers;

namespace {

constexpr HRESULT c_notSupported = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
constexpr HRESULT c_invalidState = HRESULT_FROM_WIN32(ERROR_INVALID_STATE);

std::filesystem::path CreateTestDirectory()
{
    GUID id{};
    THROW_IF_FAILED(CoCreateGuid(&id));
    const auto path = std::filesystem::temp_directory_path() /
                      (L"HcsVirtualMachineBackendTests-" +
                       wsl::shared::string::GuidToString<wchar_t>(id, wsl::shared::string::GuidToStringFlags::None));
    THROW_IF_WIN32_BOOL_FALSE(CreateDirectoryW(path.c_str(), nullptr));
    return path;
}

void CreateVhd(const std::filesystem::path& Path)
{
    VIRTUAL_STORAGE_TYPE storageType{};
    storageType.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_VHDX;
    storageType.VendorId = VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT;

    CREATE_VIRTUAL_DISK_PARAMETERS parameters{};
    parameters.Version = CREATE_VIRTUAL_DISK_VERSION_2;
    parameters.Version2.BlockSizeInBytes = 1024 * 1024;
    parameters.Version2.MaximumSize = 8 * c_mib;

    wil::unique_hfile vhd;
    THROW_IF_WIN32_ERROR(CreateVirtualDisk(
        &storageType, Path.c_str(), VIRTUAL_DISK_ACCESS_NONE, nullptr, CREATE_VIRTUAL_DISK_FLAG_SUPPORT_COMPRESSED_VOLUMES, 0, &parameters, nullptr, &vhd));
}

std::wstring GetDaclAces(const std::filesystem::path& Path)
{
    PACL acl = nullptr;
    wil::unique_hlocal descriptor;
    THROW_IF_WIN32_ERROR(
        GetNamedSecurityInfoW(Path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &acl, nullptr, &descriptor));
    wil::unique_hlocal_string value;
    THROW_IF_WIN32_BOOL_FALSE(ConvertSecurityDescriptorToStringSecurityDescriptorW(
        descriptor.get(), SDDL_REVISION_1, DACL_SECURITY_INFORMATION, &value, nullptr));
    const std::wstring dacl{value.get()};
    const auto firstAce = dacl.find(L'(');
    return firstAce == std::wstring::npos ? std::wstring{} : dacl.substr(firstAce);
}

// Tests require elevation, so the test process token is the elevated counterpart of
// GetNonElevatedToken() and stands in for an administrator's DrvFs share.
wil::unique_handle GetElevatedTestToken()
{
    wil::unique_handle token;
    THROW_IF_WIN32_BOOL_FALSE(OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &token));
    wil::unique_handle impersonationToken;
    THROW_IF_WIN32_BOOL_FALSE(
        DuplicateTokenEx(token.get(), TOKEN_ALL_ACCESS, nullptr, SecurityImpersonation, TokenImpersonation, &impersonationToken));
    return impersonationToken;
}

VmDiskRequest CreateDiskRequest(const std::filesystem::path& Path, std::optional<UINT32> Lun = std::nullopt)
{
    VmDiskRequest request;
    request.Source = VmVirtualDiskSource{Path, VmDiskFormat::Vhdx};
    if (Lun)
    {
        request.Placement = VmScsiPlacement{{0, Lun.value()}};
    }

    return request;
}

VmNetworkAdapterRequest CreateNetworkRequest()
{
    VmNetworkAdapterRequest request;
    request.Tag = L"eth0";
    VmUserModeNatNetwork configuration;
    configuration.Configuration.clientIp.value = htonl(0x0a000002);
    constexpr std::array<BYTE, 6> clientMac{0x00, 0x15, 0x5d, 0x01, 0x02, 0x03};
    std::copy(clientMac.begin(), clientMac.end(), std::begin(configuration.Configuration.clientMac.bytes));
    configuration.Configuration.gatewayIp.value = htonl(0x0a000001);
    constexpr std::array<BYTE, 6> gatewayMac{0x52, 0x55, 0x0a, 0x00, 0x00, 0x01};
    std::copy(gatewayMac.begin(), gatewayMac.end(), std::begin(configuration.Configuration.gatewayMac.bytes));
    constexpr std::array<BYTE, 6> gatewayMacIpv6{0x52, 0x55, 0x0a, 0x00, 0x01, 0x02};
    std::copy(gatewayMacIpv6.begin(), gatewayMacIpv6.end(), std::begin(configuration.Configuration.gatewayMacIpv6.bytes));
    configuration.Configuration.netmask.value = htonl(0xffffff00);
    request.Configuration = configuration;
    return request;
}

std::filesystem::path ChangePathCase(const std::filesystem::path& Path)
{
    auto value = Path.native();
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t Character) {
        return static_cast<wchar_t>(std::towupper(Character));
    });
    return value;
}

} // namespace

namespace HcsVirtualMachineBackendTests {

class HcsVirtualMachineBackendTests
{
    WSL_TEST_CLASS(HcsVirtualMachineBackendTests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        WSADATA data{};
        THROW_IF_WIN32_ERROR(WSAStartup(MAKEWORD(2, 2), &data));
        return true;
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        const auto module = GetModuleHandleW(L"wsldevicehostproxystub.dll");
        if (module)
        {
            const auto canUnload = reinterpret_cast<HRESULT(STDAPICALLTYPE*)()>(GetProcAddress(module, "DllCanUnloadNow"));
            VERIFY_IS_NOT_NULL(canUnload);
            LogInfo("Device-host proxy unload status before COM cleanup: 0x%08x", canUnload());
        }
        CoFreeUnusedLibrariesEx(0, 0);
        VERIFY_IS_NULL(GetModuleHandleW(L"wsldevicehostproxystub.dll"));
        VERIFY_ARE_EQUAL(0, WSACleanup());
        return true;
    }

    TEST_METHOD(SelectsBackendFromExperimentalFlag)
    {
        VERIFY_ARE_EQUAL(BackendKind::Hcs, SelectVirtualMachineBackendKind(false));
        VERIFY_ARE_EQUAL(BackendKind::OpenVmm, SelectVirtualMachineBackendKind(true));
    }

    TEST_METHOD(FileSystemRequestsDefaultToVirtioFs)
    {
        const VmFileSystemDeviceRequest device;
        VERIFY_IS_TRUE(std::holds_alternative<VmVirtioFsDevice>(device.Transport));
        VERIFY_ARE_EQUAL(VmVirtioFsLayout::Aggregate, std::get<VmVirtioFsDevice>(device.Transport).Layout);
        const VmFileSystemShareRequest share;
        VERIFY_IS_TRUE(std::holds_alternative<VmVirtioFsShareOptions>(share.Options));
        VERIFY_IS_TRUE(std::get<VmVirtioFsShareOptions>(share.Options).MountOptions.empty());
        VERIFY_IS_TRUE(share.ReadOnly);
        const VmFileSystemShare result;
        VERIFY_IS_TRUE(std::holds_alternative<VmVirtioFsShareAddress>(result.GuestAddress));
    }

    TEST_METHOD(PreservesCallerIdentityAndBootInputs)
    {
        SKIP_TEST_ARM64();
        auto request = CreateRunnableRequest();
        request.Boot.KernelCommandLine = L"panic=-1 console=hvc0 custom=value";
        request.Memory.SizeBytes += 123;
        request.Memory.AllowOvercommit = VmFeatureRequest::Required;
        request.Memory.DeferredCommit = VmFeatureRequest::Required;
        request.Memory.ColdDiscard = VmFeatureRequest::Required;
        request.Memory.SmallPageBacking = VmFeatureRequest::Preferred;
        request.Memory.FaultClusterSizeShift = 4;
        request.Memory.DirectMapFaultClusterSizeShift = 4;
        request.Memory.PageReportingOrder = 5;
        request.Memory.HostingProcessNameSuffix = L"WSL";
        request.Mmio.HighWindowSizeBytes = 24 * c_mib;
        request.Mmio.MaximumGuestAddressBits = 36;
        auto backend = HcsVirtualMachineBackend::Create(request);
        const auto description = backend->GetDescription();

        VERIFY_IS_NOT_NULL(backend->GetComputeSystemHandle());
        VERIFY_IS_NOT_NULL(backend->GetGuestDeviceManager().get());
        VERIFY_IS_TRUE(IsEqualGUID(request.Identity.VmId, description.Identity.VmId));
        VERIFY_ARE_EQUAL(BackendKind::Hcs, description.Backend);
        VERIFY_ARE_EQUAL(request.Processor.Count, description.Processor.Count);
        VERIFY_ARE_EQUAL((request.Memory.SizeBytes / c_mib) * c_mib, description.Memory.SizeBytes);
        VERIFY_ARE_EQUAL(wsl::windows::common::hcs::IsSmallPageMemorySupported(), description.Memory.SmallPageBacking);
        VERIFY_ARE_EQUAL(UINT32{4}, description.Memory.FaultClusterSizeShift.value());
        VERIFY_ARE_EQUAL(UINT32{4}, description.Memory.DirectMapFaultClusterSizeShift.value());
        VERIFY_ARE_EQUAL(UINT32{5}, description.Memory.PageReportingOrder.value());
        VERIFY_ARE_EQUAL(wsl::windows::common::helpers::IsVmemmSuffixSupported(), description.Memory.HostingProcessNameSuffix.has_value());
        if (description.Memory.HostingProcessNameSuffix)
        {
            VERIFY_ARE_EQUAL(std::wstring{L"WSL"}, description.Memory.HostingProcessNameSuffix.value());
        }
        VERIFY_ARE_EQUAL(UINT64{24 * c_mib}, description.Memory.HighMmioSizeBytes.value());
        VERIFY_ARE_EQUAL((UINT64{1} << 36) - (24 * c_mib), description.Memory.HighMmioBaseBytes.value());
        VERIFY_ARE_EQUAL(VmBootMethod::LinuxDirect, description.Boot.Method);
        VERIFY_ARE_EQUAL(request.Boot.KernelCommandLine, description.Boot.KernelCommandLine);
        const auto capabilities = backend->GetCapabilities();
        VERIFY_ARE_EQUAL(BackendKind::Hcs, capabilities.Backend);
        VERIFY_IS_TRUE(capabilities.Features.test(static_cast<size_t>(VmFeature::MemoryOvercommit)));
        VERIFY_IS_TRUE(capabilities.Features.test(static_cast<size_t>(VmFeature::DeferredMemoryCommit)));
        VERIFY_IS_TRUE(capabilities.Features.test(static_cast<size_t>(VmFeature::ColdDiscard)));
        VERIFY_IS_TRUE(capabilities.Features.test(static_cast<size_t>(VmFeature::HighMmio)));
        VERIFY_ARE_EQUAL(
            wsl::windows::common::helpers::IsVmemmSuffixSupported(),
            capabilities.Features.test(static_cast<size_t>(VmFeature::HostingProcessNameSuffix)));
        VERIFY_IS_TRUE(capabilities.Features.test(static_cast<size_t>(VmFeature::SerialConsole)));
        VERIFY_IS_TRUE(capabilities.Features.test(static_cast<size_t>(VmFeature::VirtioFsFileBacked)));
        VERIFY_IS_TRUE(capabilities.Features.test(static_cast<size_t>(VmFeature::UserModeNatNetwork)));
        VERIFY_IS_TRUE(capabilities.Features.test(static_cast<size_t>(VmFeature::TcpPortBinding)));
        VERIFY_IS_TRUE(capabilities.Features.test(static_cast<size_t>(VmFeature::UdpPortBinding)));
        VERIFY_IS_TRUE(capabilities.Features.test(static_cast<size_t>(VmFeature::Ipv6PortBinding)));
        backend->Terminate();
    }

    TEST_METHOD(BootsAndTerminates)
    {
        SKIP_TEST_ARM64();
        VerifyBootsAndTerminates(HcsVirtualMachineBackend::Create(CreateRunnableRequest()));
    }

    TEST_METHOD(AllocatesFirstFreeBootDiskLun)
    {
        SKIP_TEST_ARM64();
        const auto directory = CreateTestDirectory();
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] { std::filesystem::remove_all(directory); });
        const auto exactPath = directory / L"exact.vhdx";
        const auto automaticPath = directory / L"automatic.vhdx";
        CreateVhd(exactPath);
        CreateVhd(automaticPath);

        auto request = CreateRunnableRequest();
        VmBootDiskRequest exact;
        exact.Key = L"exact";
        exact.Disk = CreateDiskRequest(exactPath, 253);
        VmBootDiskRequest automatic;
        automatic.Key = L"automatic";
        automatic.Disk = CreateDiskRequest(automaticPath);
        request.BootDisks = {std::move(exact), std::move(automatic)};

        auto backend = HcsVirtualMachineBackend::Create(request);
        const auto& bootDisks = backend->GetDescription().BootDisks;
        VERIFY_ARE_EQUAL(UINT32{253}, bootDisks.at(L"exact").GuestAddress.Lun);
        VERIFY_ARE_EQUAL(UINT32{0}, bootDisks.at(L"automatic").GuestAddress.Lun);
        backend->Terminate();
    }

    TEST_METHOD(RollsBackBootDiskAccessOnConfigurationFailure)
    {
        const auto directory = CreateTestDirectory();
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] { std::filesystem::remove_all(directory); });
        const auto firstPath = directory / L"first.vhdx";
        CreateVhd(firstPath);

        const auto firstDaclAces = GetDaclAces(firstPath);
        auto request = CreateRunnableRequest();
        request.Identity.UserToken = wil::shared_handle{GetElevatedTestToken().release()};
        VmBootDiskRequest first;
        first.Key = L"first";
        first.Disk = CreateDiskRequest(firstPath);
        first.GrantHostAccess = true;
        VmBootDiskRequest invalid;
        invalid.Key = L"first";
        invalid.Disk = CreateDiskRequest(firstPath);
        invalid.GrantHostAccess = true;
        request.BootDisks = {std::move(first), std::move(invalid)};

        VERIFY_ARE_NOT_EQUAL(S_OK, OperationResult([&] { HcsVirtualMachineBackend::Create(request); }));
        VERIFY_ARE_EQUAL(firstDaclAces, GetDaclAces(firstPath));
    }

    TEST_METHOD(ManagesDiskPlacementsAndLifetime)
    {
        SKIP_TEST_ARM64();
        const auto directory = CreateTestDirectory();
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] { std::filesystem::remove_all(directory); });
        const auto automaticPath = directory / L"automatic.vhdx";
        const auto exactPath = directory / L"exact.vhdx";
        const auto replacementPath = directory / L"replacement.vhdx";
        const auto invalidPath = directory / L"invalid.vhdx";
        CreateVhd(automaticPath);
        CreateVhd(exactPath);
        CreateVhd(replacementPath);
        CreateVhd(invalidPath);

        auto backend = HcsVirtualMachineBackend::Create(CreateRunnableRequest());
        auto guest = StartGuest(*backend);
        const auto automatic = backend->AttachDisk(CreateDiskRequest(automaticPath));
        const auto exact = backend->AttachDisk(CreateDiskRequest(exactPath, 2));
        const auto replacement = backend->AttachDisk(CreateDiskRequest(replacementPath));

        VERIFY_ARE_EQUAL(UINT32{0}, automatic.GuestAddress.Lun);
        VERIFY_ARE_EQUAL(UINT32{2}, exact.GuestAddress.Lun);
        VERIFY_ARE_EQUAL(UINT32{1}, replacement.GuestAddress.Lun);
        VERIFY_IS_TRUE(IsEqualGUID(backend->GetDescription().Identity.VmId, automatic.Id.Owner.VmId));

        const auto duplicate = backend->AttachDisk(CreateDiskRequest(automaticPath, 3));
        VERIFY_ARE_EQUAL(automatic.Id.Value, duplicate.Id.Value);
        VERIFY_ARE_EQUAL(automatic.GuestAddress.Lun, duplicate.GuestAddress.Lun);
        const auto differentlyCased = backend->AttachDisk(CreateDiskRequest(ChangePathCase(automaticPath)));
        VERIFY_ARE_EQUAL(automatic.Id.Value, differentlyCased.Id.Value);
        VERIFY_ARE_EQUAL(automatic.GuestAddress.Lun, differentlyCased.GuestAddress.Lun);
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), OperationResult([&] {
                             backend->AttachDisk(CreateDiskRequest(invalidPath, exact.GuestAddress.Lun));
                         }));

        auto invalidOwner = automatic.Id;
        THROW_IF_FAILED(CoCreateGuid(&invalidOwner.Owner.VmId));
        VERIFY_ARE_EQUAL(E_INVALIDARG, OperationResult([&] { backend->DetachDisk(invalidOwner); }));

        auto unknown = automatic.Id;
        unknown.Value = UINT64_MAX;
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), OperationResult([&] { backend->DetachDisk(unknown); }));

        backend->DetachDisk(replacement.Id);
        const auto reused = backend->AttachDisk(CreateDiskRequest(invalidPath, replacement.GuestAddress.Lun));
        VERIFY_ARE_EQUAL(replacement.GuestAddress.Lun, reused.GuestAddress.Lun);
        VERIFY_ARE_NOT_EQUAL(replacement.Id.Value, reused.Id.Value);

        backend->DetachDisk(reused.Id);
        backend->DetachDisk(exact.Id);
        backend->DetachDisk(automatic.Id);
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), OperationResult([&] { backend->DetachDisk(automatic.Id); }));
        backend->Terminate();
    }

    TEST_METHOD(KeepsDiskIdsVmScopedAndEnforcesPlacementBounds)
    {
        SKIP_TEST_ARM64();
        const auto directory = CreateTestDirectory();
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] { std::filesystem::remove_all(directory); });
        const auto firstPath = directory / L"first.vhdx";
        const auto secondPath = directory / L"second.vhdx";
        CreateVhd(firstPath);
        CreateVhd(secondPath);

        auto firstBackend = HcsVirtualMachineBackend::Create(CreateRunnableRequest());
        auto firstGuest = StartGuest(*firstBackend);
        const auto first = firstBackend->AttachDisk(CreateDiskRequest(firstPath, 253));

        VERIFY_ARE_EQUAL(UINT32{253}, first.GuestAddress.Lun);
        VERIFY_ARE_EQUAL(c_notSupported, OperationResult([&] { firstBackend->AttachDisk(CreateDiskRequest(secondPath, 254)); }));

        firstBackend->DetachDisk(first.Id);
        firstBackend->Terminate();

        // Disk IDs are local to a VM and can be reused after another VM has terminated.
        auto secondBackend = HcsVirtualMachineBackend::Create(CreateRunnableRequest());
        auto secondGuest = StartGuest(*secondBackend);
        const auto second = secondBackend->AttachDisk(CreateDiskRequest(secondPath));

        VERIFY_ARE_EQUAL(first.Id.Value, second.Id.Value);
        VERIFY_IS_FALSE(IsEqualGUID(first.Id.Owner.VmId, second.Id.Owner.VmId));

        secondBackend->DetachDisk(second.Id);
        secondBackend->Terminate();
    }

    TEST_METHOD(RejectsInvalidIdentity)
    {
        auto request = CreateRunnableRequest();
        request.Identity.VmId = GUID_NULL;
        VERIFY_ARE_EQUAL(E_INVALIDARG, OperationResult([&] { HcsVirtualMachineBackend::Create(request); }));
    }

    TEST_METHOD(DefersNetworkAdapterCreationUntilVmStarts)
    {
        SKIP_TEST_ARM64();
        auto request = CreateRunnableRequest();
        request.NetworkAdapters.push_back({});
        auto backend = HcsVirtualMachineBackend::Create(request);

        VERIFY_ARE_EQUAL(E_INVALIDARG, OperationResult([&] { backend->Start(); }));
        backend->Terminate();
    }

    TEST_METHOD(ReusesDuplicateNetworkAdapterRequests)
    {
        SKIP_TEST_ARM64();
        auto backend = HcsVirtualMachineBackend::Create(CreateRunnableRequest());
        auto guest = StartGuest(*backend);

        const auto request = CreateNetworkRequest();
        const auto first = backend->AddNetworkAdapter(request);
        const auto duplicate = backend->AddNetworkAdapter(request);
        VERIFY_ARE_EQUAL(first.Id.Value, duplicate.Id.Value);
        VERIFY_ARE_EQUAL(first.Tag, duplicate.Tag);
        VERIFY_IS_TRUE(first.GuestInstanceId == duplicate.GuestInstanceId);
        const auto description = backend->GetDescription();
        VERIFY_ARE_EQUAL(size_t{1}, description.NetworkAdapters.size());
        VERIFY_ARE_EQUAL(first.Id.Value, description.NetworkAdapters.at(request.Tag).Id.Value);

        backend->RemoveNetworkAdapter(first.Id);
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), OperationResult([&] { backend->RemoveNetworkAdapter(duplicate.Id); }));
        VERIFY_IS_TRUE(backend->GetDescription().NetworkAdapters.empty());
        backend->Terminate();
    }

    TEST_METHOD(KeepsOtherVmDeviceHostsAlive)
    {
        SKIP_TEST_ARM64();
        const auto directory = CreateTestDirectory();
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] { std::filesystem::remove_all(directory); });

        auto first = HcsVirtualMachineBackend::Create(CreateRunnableRequest());
        auto firstGuest = StartGuest(*first);
        first->CreateFileSystemDevice({VmVirtioFsDevice{L"first", VmVirtioFsLayout::Aggregate}});

        auto second = HcsVirtualMachineBackend::Create(CreateRunnableRequest());
        auto secondGuest = StartGuest(*second);
        const auto device = second->CreateFileSystemDevice({VmVirtioFsDevice{L"second", VmVirtioFsLayout::Aggregate}});

        first.reset();

        VmFileSystemShareRequest request;
        request.HostPath = directory;
        request.Options = VmVirtioFsShareOptions{};
        const auto share = second->AddFileSystemShare(device.Id, request);
        VERIFY_ARE_EQUAL(device.Id.Value, share.Device.Value);
        second->RemoveFileSystemShare(share.Id);
        second->Terminate();
    }

    TEST_METHOD(NotifiesTerminationCallbacksWithoutHoldingBackendLock)
    {
        SKIP_TEST_ARM64();
        auto backend = HcsVirtualMachineBackend::Create(CreateRunnableRequest());
        wil::unique_event callbackEvent{wil::EventOptions::ManualReset};
        GUID callbackVmId{};
        HRESULT callbackResult = S_OK;
        backend->RegisterTerminationCallback([&](GUID VmId) {
            callbackVmId = VmId;
            callbackResult = OperationResult([&] { backend->ConnectGuest({}); });
            callbackEvent.SetEvent();
        });

        backend->Terminate();
        VERIFY_IS_TRUE(callbackEvent.wait(30 * 1000));
        VERIFY_IS_TRUE(IsEqualGUID(backend->GetDescription().Identity.VmId, callbackVmId));
        VERIFY_ARE_EQUAL(c_invalidState, callbackResult);

        auto lateBackend = HcsVirtualMachineBackend::Create(CreateRunnableRequest());
        lateBackend->Terminate();
        wil::unique_event lateCallbackEvent{wil::EventOptions::ManualReset};
        GUID lateCallbackVmId{};
        lateBackend->RegisterTerminationCallback([&](GUID VmId) {
            lateCallbackVmId = VmId;
            lateCallbackEvent.SetEvent();
        });

        VERIFY_IS_TRUE(lateCallbackEvent.wait(30 * 1000));
        VERIFY_IS_TRUE(IsEqualGUID(lateBackend->GetDescription().Identity.VmId, lateCallbackVmId));
    }

    TEST_METHOD(RejectsUnsupportedResourceOperations)
    {
        SKIP_TEST_ARM64();
        auto backend = HcsVirtualMachineBackend::Create(CreateRunnableRequest());
        const auto identity = backend->GetDescription().Identity;
        const VmDeviceId device{identity, 1};
        const VmShareId share{identity, 1};
        const VmPortBindingId binding{identity, 1};

        VERIFY_ARE_EQUAL(E_INVALIDARG, OperationResult([&] { backend->CreateFileSystemDevice({VmPlan9SocketDevice{}}); }));
        VERIFY_ARE_EQUAL(E_INVALIDARG, OperationResult([&] { backend->AddFileSystemShare(device, {}); }));
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), OperationResult([&] {
                             VmFileSystemShareRequest request{};
                             request.HostPath = L"C:\\";
                             backend->AddFileSystemShare(device, request);
                         }));
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), OperationResult([&] { backend->RemoveFileSystemShare(share); }));
        VERIFY_ARE_EQUAL(E_INVALIDARG, OperationResult([&] { backend->AddNetworkAdapter({}); }));
        VmPortBindingRequest bindingRequest{};
        bindingRequest.ListenAddress.family = IpAddressFamily_V4;
        const auto loopbackAddress = htonl(INADDR_LOOPBACK);
        std::memcpy(bindingRequest.ListenAddress.bytes, &loopbackAddress, sizeof(loopbackAddress));
        bindingRequest.GuestPort = 80;
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), OperationResult([&] { backend->BindPort(device, bindingRequest); }));
        bindingRequest.ListenScopeId = 1;
        VERIFY_ARE_EQUAL(c_notSupported, OperationResult([&] { backend->BindPort(device, bindingRequest); }));
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), OperationResult([&] { backend->UnbindPort(binding); }));
        VERIFY_ARE_EQUAL(E_INVALIDARG, OperationResult([&] { backend->AddPersistentMemory({}); }));
        VERIFY_ARE_EQUAL(E_INVALIDARG, OperationResult([&] { backend->AddSharedMemory({}); }));
        VERIFY_ARE_EQUAL(E_INVALIDARG, OperationResult([&] { backend->ConfigureGuestDma({}); }));
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), OperationResult([&] { backend->GetFileSystemDeviceStatus(device); }));
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), OperationResult([&] { backend->RemoveDevice(device); }));
        backend->Terminate();
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), OperationResult([&] { backend->GetComputeSystemHandle(); }));
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), OperationResult([&] { backend->GetGuestDeviceManager(); }));
    }

    TEST_METHOD(SharesHostDirectoriesPerElevationLevel)
    {
        SKIP_TEST_ARM64();
        const auto directory = CreateTestDirectory();
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] { std::filesystem::remove_all(directory); });

        auto backend = HcsVirtualMachineBackend::Create(CreateRunnableRequest());
        auto guest = StartGuest(*backend);

        // Elevated and unelevated callers share one VM, so each elevation level gets its own device
        // just as WslCoreVm::AddDrvFsShare uses a separate Plan 9 port and virtio-fs tag for each.
        // An aggregate device is created with the identity it serves its children under.
        const auto userDevice = backend->CreateFileSystemDevice({VmVirtioFsDevice{L"drvfs-user", VmVirtioFsLayout::Aggregate}});
        const auto adminDevice = backend->CreateFileSystemDevice({VmVirtioFsDevice{
            L"drvfs-admin", VmVirtioFsLayout::Aggregate, VmVirtioFsShareOptions{{}, wil::shared_handle{GetElevatedTestToken().release()}}}});
        VERIFY_ARE_EQUAL(VmFileSystemDeviceState::Serving, userDevice.State);
        VERIFY_IS_TRUE(userDevice.GuestInstanceId.has_value());
        VERIFY_ARE_NOT_EQUAL(userDevice.Id.Value, adminDevice.Id.Value);
        VERIFY_ARE_EQUAL(VmFileSystemDeviceState::Serving, backend->GetFileSystemDeviceStatus(userDevice.Id).State);

        // Leaving the token unset serves the share through the identity that created the VM.
        VmFileSystemShareRequest request;
        request.HostPath = directory;
        request.Options = VmVirtioFsShareOptions{};
        request.ReadOnly = false;

        const auto share = backend->AddFileSystemShare(userDevice.Id, request);
        VERIFY_ARE_EQUAL(userDevice.Id.Value, share.Device.Value);
        VERIFY_IS_TRUE(IsEqualGUID(backend->GetDescription().Identity.VmId, share.Id.Owner.VmId));
        VERIFY_IS_FALSE(share.ReadOnly);

        const auto& address = std::get<VmVirtioFsShareAddress>(share.GuestAddress);
        VERIFY_ARE_EQUAL(std::wstring{L"drvfs-user"}, address.Tag);
        VERIFY_IS_TRUE(address.ChildName.has_value());

        // The host path is canonicalized so that requests naming the same directory resolve to a
        // single share. GetCanonicalPath removes the separator NormalizeSharePath appends before
        // canonicalization.
        VERIFY_ARE_EQUAL(std::filesystem::canonical(directory).native(), share.EffectiveHostPath.native());

        // Repeating a request reuses the share instead of adding a second child for one directory.
        const auto reused = backend->AddFileSystemShare(userDevice.Id, request);
        VERIFY_ARE_EQUAL(share.Id.Value, reused.Id.Value);

        auto namedRequest = request;
        namedRequest.Name = L"first";
        const auto firstNamedShare = backend->AddFileSystemShare(userDevice.Id, namedRequest);
        const auto& firstNamedAddress = std::get<VmVirtioFsShareAddress>(firstNamedShare.GuestAddress);
        VERIFY_IS_TRUE(firstNamedAddress.ChildName.has_value());
        VERIFY_ARE_EQUAL(std::wstring{L"first"}, firstNamedAddress.ChildName.value());
        VERIFY_ARE_EQUAL(firstNamedShare.Id.Value, backend->AddFileSystemShare(userDevice.Id, namedRequest).Id.Value);

        auto conflictingNamedRequest = namedRequest;
        conflictingNamedRequest.ReadOnly = true;
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), OperationResult([&] {
                             backend->AddFileSystemShare(userDevice.Id, conflictingNamedRequest);
                         }));
        conflictingNamedRequest = namedRequest;
        conflictingNamedRequest.HostPath = directory.parent_path();
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), OperationResult([&] {
                             backend->AddFileSystemShare(userDevice.Id, conflictingNamedRequest);
                         }));
        conflictingNamedRequest = namedRequest;
        std::get<VmVirtioFsShareOptions>(conflictingNamedRequest.Options).MountOptions[L"dax"] = L"";
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), OperationResult([&] {
                             backend->AddFileSystemShare(userDevice.Id, conflictingNamedRequest);
                         }));

        namedRequest.Name = L"second";
        const auto secondNamedShare = backend->AddFileSystemShare(userDevice.Id, namedRequest);
        const auto& secondNamedAddress = std::get<VmVirtioFsShareAddress>(secondNamedShare.GuestAddress);
        VERIFY_ARE_NOT_EQUAL(firstNamedShare.Id.Value, secondNamedShare.Id.Value);
        VERIFY_IS_TRUE(secondNamedAddress.ChildName.has_value());
        VERIFY_ARE_EQUAL(std::wstring{L"second"}, secondNamedAddress.ChildName.value());

        // Mount options are part of a share's identity, so a read-only mount is a separate share.
        auto readOnlyRequest = request;
        readOnlyRequest.ReadOnly = true;
        const auto readOnlyShare = backend->AddFileSystemShare(userDevice.Id, readOnlyRequest);
        VERIFY_ARE_NOT_EQUAL(share.Id.Value, readOnlyShare.Id.Value);
        VERIFY_IS_TRUE(readOnlyShare.ReadOnly);

        auto invalidShare = share.Id;
        THROW_IF_FAILED(CoCreateGuid(&invalidShare.Owner.VmId));
        VERIFY_ARE_EQUAL(E_INVALIDARG, OperationResult([&] { backend->RemoveFileSystemShare(invalidShare); }));

        backend->RemoveFileSystemShare(share.Id);
        const auto replacementShare = backend->AddFileSystemShare(userDevice.Id, request);
        VERIFY_ARE_NOT_EQUAL(share.Id.Value, replacementShare.Id.Value);
        backend->RemoveFileSystemShare(replacementShare.Id);
        VERIFY_ARE_EQUAL(
            HRESULT_FROM_WIN32(ERROR_NOT_FOUND), OperationResult([&] { backend->RemoveFileSystemShare(replacementShare.Id); }));

        const auto singleShareDevice = backend->CreateFileSystemDevice({VmVirtioFsDevice{L"drvfs-single", VmVirtioFsLayout::SingleShare}});
        // A single-share device inherits the options of the share that it serves, so device-level
        // options would never reach the guest.
        VERIFY_ARE_EQUAL(
            E_INVALIDARG, OperationResult([&] {
                backend->CreateFileSystemDevice({VmVirtioFsDevice{
                    L"drvfs-single-options", VmVirtioFsLayout::SingleShare, VmVirtioFsShareOptions{{{L"dax", L""}}}}});
            }));
        const auto singleShare = backend->AddFileSystemShare(singleShareDevice.Id, request);
        VERIFY_IS_FALSE(std::get<VmVirtioFsShareAddress>(singleShare.GuestAddress).ChildName.has_value());
        VERIFY_ARE_EQUAL(singleShare.Id.Value, backend->AddFileSystemShare(singleShareDevice.Id, request).Id.Value);
        auto singleShareTokenRequest = request;
        singleShareTokenRequest.UserToken = wil::shared_handle{GetElevatedTestToken().release()};
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), OperationResult([&] {
                             backend->AddFileSystemShare(singleShareDevice.Id, singleShareTokenRequest);
                         }));
        singleShareTokenRequest.UserToken.reset();
        std::get<VmVirtioFsShareOptions>(singleShareTokenRequest.Options).UserToken = wil::shared_handle{GetElevatedTestToken().release()};
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), OperationResult([&] {
                             backend->AddFileSystemShare(singleShareDevice.Id, singleShareTokenRequest);
                         }));
        auto singleShareNamedRequest = request;
        singleShareNamedRequest.Name = L"invalid";
        VERIFY_ARE_EQUAL(
            E_INVALIDARG, OperationResult([&] { backend->AddFileSystemShare(singleShareDevice.Id, singleShareNamedRequest); }));
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_BUSY), OperationResult([&] { backend->RemoveDevice(singleShareDevice.Id); }));
        backend->RemoveFileSystemShare(singleShare.Id);
        const auto replacementSingleShare = backend->AddFileSystemShare(singleShareDevice.Id, request);
        VERIFY_ARE_NOT_EQUAL(singleShare.Id.Value, replacementSingleShare.Id.Value);
        backend->RemoveFileSystemShare(replacementSingleShare.Id);
        const auto tokenShare = backend->AddFileSystemShare(singleShareDevice.Id, singleShareTokenRequest);
        VERIFY_ARE_EQUAL(tokenShare.Id.Value, backend->AddFileSystemShare(singleShareDevice.Id, singleShareTokenRequest).Id.Value);
        auto equivalentTokenRequest = request;
        equivalentTokenRequest.UserToken = std::get<VmVirtioFsShareOptions>(singleShareTokenRequest.Options).UserToken;
        VERIFY_ARE_EQUAL(tokenShare.Id.Value, backend->AddFileSystemShare(singleShareDevice.Id, equivalentTokenRequest).Id.Value);
        backend->RemoveFileSystemShare(tokenShare.Id);
        backend->RemoveDevice(singleShareDevice.Id);
        VERIFY_ARE_EQUAL(
            HRESULT_FROM_WIN32(ERROR_NOT_FOUND), OperationResult([&] { backend->GetFileSystemDeviceStatus(singleShareDevice.Id); }));

        // The elevated device reaches the same directory under its own identity without reusing the
        // share that the unelevated device serves. A child of an aggregate device cannot name a
        // second identity, so a share token is rejected rather than silently ignored.
        const auto adminShare = backend->AddFileSystemShare(adminDevice.Id, request);
        VERIFY_ARE_EQUAL(adminDevice.Id.Value, adminShare.Device.Value);
        VERIFY_ARE_NOT_EQUAL(share.Id.Value, adminShare.Id.Value);
        VERIFY_ARE_EQUAL(std::wstring{L"drvfs-admin"}, std::get<VmVirtioFsShareAddress>(adminShare.GuestAddress).Tag);
        VERIFY_ARE_EQUAL(share.EffectiveHostPath.native(), adminShare.EffectiveHostPath.native());

        auto tokenRequest = request;
        tokenRequest.UserToken = wil::shared_handle{GetElevatedTestToken().release()};
        VERIFY_ARE_EQUAL(E_INVALIDARG, OperationResult([&] { backend->AddFileSystemShare(userDevice.Id, tokenRequest); }));

        // Exercise the remaining file-system transports as well: Plan 9 socket and Plan 9 virtio.
        const auto createPlan9Server = [](HANDLE userToken) {
            return wsl::windows::common::wslutil::CreateComServerAsUser<p9fs::Plan9FileSystem, IPlan9FileSystem>(userToken);
        };

        VmPlan9SocketDevice socketDevice{GuestServicePort{LX_INIT_UTILITY_VM_PLAN9_PORT}, createPlan9Server};
        // The port is handed straight to the Plan 9 server, so an unassigned one must be rejected
        // rather than listened on.
        VERIFY_ARE_EQUAL(E_INVALIDARG, OperationResult([&] {
                             backend->CreateFileSystemDevice({VmPlan9SocketDevice{GuestServicePort{}, createPlan9Server}});
                         }));
        const auto socketDeviceResult = backend->CreateFileSystemDevice({socketDevice});
        VERIFY_ARE_EQUAL(VmFileSystemDeviceState::Serving, socketDeviceResult.State);
        VmFileSystemShareRequest socketRequest;
        socketRequest.HostPath = directory;
        socketRequest.Options = VmPlan9ShareOptions{};
        socketRequest.ReadOnly = false;
        socketRequest.Name = L"socket-share";
        const auto verifyPlan9NameCollisions = [&](VmDeviceId device, const VmFileSystemShareRequest& original) {
            auto conflicting = original;
            conflicting.HostPath = directory.parent_path();
            VERIFY_ARE_EQUAL(
                HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), OperationResult([&] { backend->AddFileSystemShare(device, conflicting); }));
            conflicting = original;
            conflicting.ReadOnly = !original.ReadOnly;
            VERIFY_ARE_EQUAL(
                HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), OperationResult([&] { backend->AddFileSystemShare(device, conflicting); }));
            for (const auto flag :
                 {&VmPlan9ShareOptions::LinuxMetadata,
                  &VmPlan9ShareOptions::CaseSensitive,
                  &VmPlan9ShareOptions::UseShareRootIdentity,
                  &VmPlan9ShareOptions::AllowOptions,
                  &VmPlan9ShareOptions::AllowSubPaths})
            {
                conflicting = original;
                auto& options = std::get<VmPlan9ShareOptions>(conflicting.Options);
                options.*flag = !(options.*flag);
                VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), OperationResult([&] {
                                     backend->AddFileSystemShare(device, conflicting);
                                 }));
            }
            conflicting = original;
            conflicting.UserToken = wil::shared_handle{GetElevatedTestToken().release()};
            VERIFY_ARE_EQUAL(
                HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), OperationResult([&] { backend->AddFileSystemShare(device, conflicting); }));
            conflicting = original;
            conflicting.Options = VmVirtioFsShareOptions{};
            VERIFY_ARE_EQUAL(E_INVALIDARG, OperationResult([&] { backend->AddFileSystemShare(device, conflicting); }));
            conflicting = original;
            conflicting.UserToken = original.UserToken ? original.UserToken : backend->GetDescription().Identity.UserToken;
            VERIFY_ARE_EQUAL(
                backend->AddFileSystemShare(device, original).Id.Value, backend->AddFileSystemShare(device, conflicting).Id.Value);
            wil::unique_handle duplicatedToken;
            THROW_IF_WIN32_BOOL_FALSE(DuplicateHandle(
                GetCurrentProcess(), conflicting.UserToken->get(), GetCurrentProcess(), duplicatedToken.put(), 0, FALSE, DUPLICATE_SAME_ACCESS));
            conflicting.UserToken = wil::shared_handle{duplicatedToken.release()};
            VERIFY_ARE_EQUAL(
                backend->AddFileSystemShare(device, original).Id.Value, backend->AddFileSystemShare(device, conflicting).Id.Value);
        };
        const auto socketShare = backend->AddFileSystemShare(socketDeviceResult.Id, socketRequest);
        VERIFY_ARE_EQUAL(socketDevice.Port.Value, std::get<VmPlan9SocketShareAddress>(socketShare.GuestAddress).Port.Value);
        VERIFY_IS_FALSE(socketShare.ReadOnly);
        const auto duplicateSocketShare = backend->AddFileSystemShare(socketDeviceResult.Id, socketRequest);
        VERIFY_ARE_EQUAL(socketShare.Id.Value, duplicateSocketShare.Id.Value);
        verifyPlan9NameCollisions(socketDeviceResult.Id, socketRequest);

        // A Plan 9 device keeps serving after one of its shares is removed, so the share can be added again.
        backend->RemoveFileSystemShare(socketShare.Id);
        VERIFY_ARE_EQUAL(
            HRESULT_FROM_WIN32(ERROR_NOT_FOUND), OperationResult([&] { backend->RemoveFileSystemShare(socketShare.Id); }));
        const auto replacementSocketShare = backend->AddFileSystemShare(socketDeviceResult.Id, socketRequest);
        VERIFY_ARE_NOT_EQUAL(socketShare.Id.Value, replacementSocketShare.Id.Value);
        backend->RemoveFileSystemShare(replacementSocketShare.Id);
        backend->RemoveDevice(socketDeviceResult.Id);

        VmPlan9HostedDevice hostedDevice{GuestServicePort{LX_INIT_UTILITY_VM_PLAN9_PORT}};
        const auto hostedDeviceResult = backend->CreateFileSystemDevice({hostedDevice});
        const auto hostedShare = backend->AddFileSystemShare(hostedDeviceResult.Id, socketRequest);
        VERIFY_ARE_EQUAL(hostedDevice.Port.Value, std::get<VmPlan9SocketShareAddress>(hostedShare.GuestAddress).Port.Value);
        verifyPlan9NameCollisions(hostedDeviceResult.Id, socketRequest);
        backend->RemoveFileSystemShare(hostedShare.Id);
        backend->RemoveDevice(hostedDeviceResult.Id);

        const auto sharedMemory = backend->AddSharedMemory({L"test-memory", L"test-memory", 8 * c_mib});
        VERIFY_ARE_EQUAL(UINT64{8 * c_mib}, sharedMemory.SizeBytes);
        VERIFY_ARE_EQUAL(std::wstring{L"test-memory"}, sharedMemory.Tag);
        VERIFY_IS_FALSE(IsEqualGUID(GUID_NULL, sharedMemory.GuestInstanceId));
        backend->RemoveDevice(sharedMemory.Id);

        VmPlan9VirtioDevice virtioDevice{L"plan9-virtio"};
        virtioDevice.FileSystemClassId = __uuidof(p9fs::Plan9FileSystem);
        virtioDevice.DeviceType = VIRTIO_PLAN9_DEVICE_ID;
        virtioDevice.ServerFactory = createPlan9Server;
        const auto virtioDeviceResult = backend->CreateFileSystemDevice({virtioDevice});
        VERIFY_ARE_EQUAL(VmFileSystemDeviceState::Serving, virtioDeviceResult.State);
        VmFileSystemShareRequest virtioRequest;
        virtioRequest.HostPath = directory;
        virtioRequest.Options = VmPlan9ShareOptions{};
        virtioRequest.ReadOnly = false;
        virtioRequest.Name = L"virtio-share";
        const auto virtioShare = backend->AddFileSystemShare(virtioDeviceResult.Id, virtioRequest);
        VERIFY_ARE_EQUAL(std::wstring{L"plan9-virtio"}, std::get<VmPlan9VirtioShareAddress>(virtioShare.GuestAddress).Tag);
        VERIFY_IS_FALSE(virtioShare.ReadOnly);
        verifyPlan9NameCollisions(virtioDeviceResult.Id, virtioRequest);
        backend->RemoveFileSystemShare(virtioShare.Id);
        const auto replacementVirtioShare = backend->AddFileSystemShare(virtioDeviceResult.Id, virtioRequest);
        VERIFY_ARE_NOT_EQUAL(virtioShare.Id.Value, replacementVirtioShare.Id.Value);
        backend->RemoveFileSystemShare(replacementVirtioShare.Id);

        // A host path is required, and the options must match the device that serves them.
        VmFileSystemShareRequest pathless;
        pathless.Options = VmVirtioFsShareOptions{};
        VERIFY_ARE_EQUAL(E_INVALIDARG, OperationResult([&] { backend->AddFileSystemShare(userDevice.Id, pathless); }));

        auto plan9Options = request;
        plan9Options.Options = VmPlan9ShareOptions{};
        VERIFY_ARE_EQUAL(E_INVALIDARG, OperationResult([&] { backend->AddFileSystemShare(userDevice.Id, plan9Options); }));

        auto unknownDevice = userDevice.Id;
        unknownDevice.Value = UINT64_MAX;
        VERIFY_ARE_EQUAL(
            HRESULT_FROM_WIN32(ERROR_NOT_FOUND), OperationResult([&] { backend->AddFileSystemShare(unknownDevice, request); }));

        auto foreignDevice = userDevice.Id;
        THROW_IF_FAILED(CoCreateGuid(&foreignDevice.Owner.VmId));
        VERIFY_ARE_EQUAL(E_INVALIDARG, OperationResult([&] { backend->AddFileSystemShare(foreignDevice, request); }));

        // A torn-down VM reports the same error as WslCoreVm::AddDrvFsShare.
        backend->Terminate();
        VERIFY_ARE_EQUAL(HCS_E_TERMINATED, OperationResult([&] { backend->AddFileSystemShare(userDevice.Id, request); }));
    }
};

} // namespace HcsVirtualMachineBackendTests
