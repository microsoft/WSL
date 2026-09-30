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

// Tests require elevation, so the test process token is the elevated counterpart of
// GetNonElevatedToken() and stands in for an administrator's DrvFs share.
wil::unique_handle GetElevatedTestToken()
{
    wil::unique_handle token;
    THROW_IF_WIN32_BOOL_FALSE(OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &token));
    return token;
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
        VERIFY_ARE_EQUAL(std::wstring{L"WSL"}, description.Memory.HostingProcessNameSuffix.value());
        VERIFY_ARE_EQUAL(UINT64{24 * c_mib}, description.Memory.HighMmioSizeBytes.value());
        VERIFY_ARE_EQUAL((UINT64{1} << 36) - (24 * c_mib), description.Memory.HighMmioBaseBytes.value());
        VERIFY_ARE_EQUAL(VmBootMethod::LinuxDirect, description.Boot.Method);
        VERIFY_ARE_EQUAL(request.Boot.KernelCommandLine, description.Boot.KernelCommandLine);
        const auto capabilities = backend->GetCapabilities();
        VERIFY_ARE_EQUAL(BackendKind::Hcs, capabilities.Backend);
        VERIFY_IS_TRUE(capabilities.Operations.test(static_cast<size_t>(VmOperation::CreateFileSystemDevice)));
        VERIFY_IS_TRUE(capabilities.Operations.test(static_cast<size_t>(VmOperation::AddFileSystemShare)));
        VERIFY_IS_TRUE(capabilities.Operations.test(static_cast<size_t>(VmOperation::RemoveFileSystemShare)));
        VERIFY_IS_TRUE(capabilities.Operations.test(static_cast<size_t>(VmOperation::AddNetworkAdapter)));
        VERIFY_IS_TRUE(capabilities.Operations.test(static_cast<size_t>(VmOperation::UpdateNetworkAdapter)));
        VERIFY_IS_TRUE(capabilities.Operations.test(static_cast<size_t>(VmOperation::RemoveNetworkAdapter)));
        VERIFY_IS_TRUE(capabilities.Operations.test(static_cast<size_t>(VmOperation::BindPort)));
        VERIFY_IS_TRUE(capabilities.Operations.test(static_cast<size_t>(VmOperation::UnbindPort)));
        VERIFY_IS_TRUE(capabilities.Operations.test(static_cast<size_t>(VmOperation::CreateVirtualAddress)));
        VERIFY_IS_TRUE(capabilities.Operations.test(static_cast<size_t>(VmOperation::CreateDnsRecord)));
        VERIFY_IS_TRUE(capabilities.Operations.test(static_cast<size_t>(VmOperation::AddPersistentMemory)));
        VERIFY_IS_TRUE(capabilities.Operations.test(static_cast<size_t>(VmOperation::AddGpu)));
        VERIFY_IS_TRUE(capabilities.Operations.test(static_cast<size_t>(VmOperation::GetFileSystemDeviceStatus)));
        VERIFY_IS_TRUE(capabilities.Operations.test(static_cast<size_t>(VmOperation::AddSharedMemory)));
        VERIFY_IS_TRUE(capabilities.Operations.test(static_cast<size_t>(VmOperation::ConfigureGuestDma)));
        VERIFY_IS_TRUE(capabilities.Operations.test(static_cast<size_t>(VmOperation::RemoveDevice)));
        VERIFY_IS_TRUE(capabilities.Features.test(static_cast<size_t>(VmFeature::PersistentMemory)));
        VERIFY_ARE_EQUAL(
            wsl::windows::common::hcs::IsSmallPageMemorySupported(),
            capabilities.Features.test(static_cast<size_t>(VmFeature::SmallPageMemory)));
        VERIFY_IS_TRUE(capabilities.Features.test(static_cast<size_t>(VmFeature::SerialConsole)));
        VERIFY_IS_TRUE(capabilities.Features.test(static_cast<size_t>(VmFeature::VirtioConsole)));
        VERIFY_IS_TRUE(capabilities.Features.test(static_cast<size_t>(VmFeature::Plan9Socket)));
        VERIFY_IS_TRUE(capabilities.Features.test(static_cast<size_t>(VmFeature::Plan9Virtio)));
        VERIFY_IS_TRUE(capabilities.Features.test(static_cast<size_t>(VmFeature::VirtioFsFileBacked)));
        VERIFY_IS_TRUE(capabilities.Features.test(static_cast<size_t>(VmFeature::VirtioFsAggregate)));
        VERIFY_IS_TRUE(capabilities.Features.test(static_cast<size_t>(VmFeature::SectionBackedSharedMemory)));
        VERIFY_IS_TRUE(capabilities.Features.test(static_cast<size_t>(VmFeature::GuestDmaWindow)));
        VERIFY_IS_TRUE(capabilities.Features.test(static_cast<size_t>(VmFeature::MirroredGpu)));
        VERIFY_IS_TRUE(capabilities.Features.test(static_cast<size_t>(VmFeature::GpuVendorExtension)));
        VERIFY_IS_TRUE(capabilities.Features.test(static_cast<size_t>(VmFeature::HostEndpointNetwork)));
        VERIFY_IS_TRUE(capabilities.Features.test(static_cast<size_t>(VmFeature::UserModeNatNetwork)));
        VERIFY_IS_TRUE(capabilities.Features.test(static_cast<size_t>(VmFeature::DynamicHostPort)));
        VERIFY_IS_TRUE(capabilities.Features.test(static_cast<size_t>(VmFeature::VirtualHostAddress)));
        VERIFY_IS_TRUE(capabilities.Features.test(static_cast<size_t>(VmFeature::StaticDnsARecord)));
        backend->Terminate();
    }

    TEST_METHOD(BootsAndTerminates)
    {
        SKIP_TEST_ARM64();
        VerifyBootsAndTerminates(HcsVirtualMachineBackend::Create(CreateRunnableRequest()));
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
        backend->Start();
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
        firstBackend->Start();
        const auto first = firstBackend->AttachDisk(CreateDiskRequest(firstPath, 253));

        VERIFY_ARE_EQUAL(UINT32{253}, first.GuestAddress.Lun);
        VERIFY_ARE_EQUAL(c_notSupported, OperationResult([&] { firstBackend->AttachDisk(CreateDiskRequest(secondPath, 254)); }));

        firstBackend->DetachDisk(first.Id);
        firstBackend->Terminate();

        // Disk IDs are local to a VM and can be reused after another VM has terminated.
        auto secondBackend = HcsVirtualMachineBackend::Create(CreateRunnableRequest());
        secondBackend->Start();
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
        backend->Start();

        // Elevated and unelevated callers share one VM, so each elevation level gets its own device
        // just as WslCoreVm::AddDrvFsShare uses a separate Plan 9 port and virtio-fs tag for each.
        const auto userDevice = backend->CreateFileSystemDevice({VmVirtioFsDevice{L"drvfs-user", VmVirtioFsLayout::Aggregate}});
        const auto adminDevice = backend->CreateFileSystemDevice({VmVirtioFsDevice{L"drvfs-admin", VmVirtioFsLayout::Aggregate}});
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
        const auto singleShare = backend->AddFileSystemShare(singleShareDevice.Id, request);
        VERIFY_IS_FALSE(std::get<VmVirtioFsShareAddress>(singleShare.GuestAddress).ChildName.has_value());
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_BUSY), OperationResult([&] { backend->RemoveDevice(singleShareDevice.Id); }));
        backend->RemoveFileSystemShare(singleShare.Id);
        const auto replacementSingleShare = backend->AddFileSystemShare(singleShareDevice.Id, request);
        VERIFY_ARE_NOT_EQUAL(singleShare.Id.Value, replacementSingleShare.Id.Value);
        backend->RemoveFileSystemShare(replacementSingleShare.Id);
        backend->RemoveDevice(singleShareDevice.Id);
        VERIFY_ARE_EQUAL(
            HRESULT_FROM_WIN32(ERROR_NOT_FOUND), OperationResult([&] { backend->GetFileSystemDeviceStatus(singleShareDevice.Id); }));

        // A share carries its own token, so the elevated device reaches the same directory without
        // reusing the share that the unelevated device serves.
        auto adminRequest = request;
        adminRequest.UserToken = wil::shared_handle{GetElevatedTestToken().release()};
        const auto adminShare = backend->AddFileSystemShare(adminDevice.Id, adminRequest);
        VERIFY_ARE_EQUAL(adminDevice.Id.Value, adminShare.Device.Value);
        VERIFY_ARE_NOT_EQUAL(share.Id.Value, adminShare.Id.Value);
        VERIFY_ARE_EQUAL(std::wstring{L"drvfs-admin"}, std::get<VmVirtioFsShareAddress>(adminShare.GuestAddress).Tag);
        VERIFY_ARE_EQUAL(share.EffectiveHostPath.native(), adminShare.EffectiveHostPath.native());

        // Exercise the remaining file-system transports as well: Plan 9 socket and Plan 9 virtio.
        const auto createPlan9Server = [](HANDLE userToken) {
            return wsl::windows::common::wslutil::CreateComServerAsUser<p9fs::Plan9FileSystem, IPlan9FileSystem>(userToken);
        };

        VmPlan9SocketDevice socketDevice{GuestServicePort{LX_INIT_UTILITY_VM_PLAN9_PORT}, createPlan9Server};
        const auto socketDeviceResult = backend->CreateFileSystemDevice({socketDevice});
        VERIFY_ARE_EQUAL(VmFileSystemDeviceState::Serving, socketDeviceResult.State);
        VmFileSystemShareRequest socketRequest;
        socketRequest.HostPath = directory;
        socketRequest.Options = VmPlan9ShareOptions{};
        socketRequest.ReadOnly = false;
        const auto socketShare = backend->AddFileSystemShare(socketDeviceResult.Id, socketRequest);
        VERIFY_ARE_EQUAL(socketDevice.Port.Value, std::get<VmPlan9SocketShareAddress>(socketShare.GuestAddress).Port.Value);
        VERIFY_IS_FALSE(socketShare.ReadOnly);

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
        const auto virtioShare = backend->AddFileSystemShare(virtioDeviceResult.Id, virtioRequest);
        VERIFY_ARE_EQUAL(std::wstring{L"plan9-virtio"}, std::get<VmPlan9VirtioShareAddress>(virtioShare.GuestAddress).Tag);
        VERIFY_IS_FALSE(virtioShare.ReadOnly);
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
