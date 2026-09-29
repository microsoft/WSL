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

    TEST_METHOD(PreservesCallerIdentityAndBootInputs)
    {
        SKIP_TEST_ARM64();
        auto request = CreateRunnableRequest();
        request.Boot.KernelCommandLine = L"panic=-1 console=hvc0 custom=value";
        request.Memory.SizeBytes += 123;
        auto backend = HcsVirtualMachineBackend::Create(request);
        const auto description = backend->GetDescription();

        VERIFY_IS_TRUE(IsEqualGUID(request.Identity.VmId, description.Identity.VmId));
        VERIFY_ARE_EQUAL(BackendKind::Hcs, description.Backend);
        VERIFY_ARE_EQUAL(request.Processor.Count, description.Processor.Count);
        VERIFY_ARE_EQUAL((request.Memory.SizeBytes / c_mib) * c_mib, description.Memory.SizeBytes);
        VERIFY_ARE_EQUAL(VmBootMethod::LinuxDirect, description.Boot.Method);
        VERIFY_ARE_EQUAL(request.Boot.KernelCommandLine, description.Boot.KernelCommandLine);
        VERIFY_ARE_EQUAL(BackendKind::Hcs, backend->GetCapabilities().Backend);
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
        auto secondBackend = HcsVirtualMachineBackend::Create(CreateRunnableRequest());
        firstBackend->Start();
        secondBackend->Start();
        const auto first = firstBackend->AttachDisk(CreateDiskRequest(firstPath, 253));
        const auto second = secondBackend->AttachDisk(CreateDiskRequest(secondPath));

        VERIFY_ARE_EQUAL(UINT32{253}, first.GuestAddress.Lun);
        VERIFY_ARE_EQUAL(first.Id.Value, second.Id.Value);
        VERIFY_IS_FALSE(IsEqualGUID(first.Id.Owner.VmId, second.Id.Owner.VmId));
        VERIFY_ARE_EQUAL(c_notSupported, OperationResult([&] { firstBackend->AttachDisk(CreateDiskRequest(secondPath, 254)); }));

        firstBackend->DetachDisk(first.Id);
        secondBackend->DetachDisk(second.Id);
        firstBackend->Terminate();
        secondBackend->Terminate();
    }

    TEST_METHOD(RejectsInvalidIdentity)
    {
        auto request = CreateRunnableRequest();
        request.Identity.VmId = GUID_NULL;
        VERIFY_ARE_EQUAL(E_INVALIDARG, OperationResult([&] { HcsVirtualMachineBackend::Create(request); }));
    }

    TEST_METHOD(RejectsUnsupportedCreationResources)
    {
        auto request = CreateRunnableRequest();
        request.BootDisks.push_back({});
        VERIFY_ARE_EQUAL(c_notSupported, OperationResult([&] { HcsVirtualMachineBackend::Create(request); }));

        request.BootDisks.clear();
        request.Consoles.push_back({});
        VERIFY_ARE_EQUAL(c_notSupported, OperationResult([&] { HcsVirtualMachineBackend::Create(request); }));

        request.Consoles.clear();
        request.NetworkAdapters.push_back({});
        VERIFY_ARE_EQUAL(c_notSupported, OperationResult([&] { HcsVirtualMachineBackend::Create(request); }));
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

        VERIFY_ARE_EQUAL(c_notSupported, OperationResult([&] { backend->CreateFileSystemDevice({}); }));
        VERIFY_ARE_EQUAL(c_notSupported, OperationResult([&] { backend->AddFileSystemShare(device, {}); }));
        VERIFY_ARE_EQUAL(c_notSupported, OperationResult([&] { backend->RemoveFileSystemShare(share); }));
        VERIFY_ARE_EQUAL(c_notSupported, OperationResult([&] { backend->AddNetworkAdapter({}); }));
        VERIFY_ARE_EQUAL(c_notSupported, OperationResult([&] { backend->BindPort(device, {}); }));
        VERIFY_ARE_EQUAL(c_notSupported, OperationResult([&] { backend->UnbindPort(binding); }));
        backend->Terminate();
    }
};

} // namespace HcsVirtualMachineBackendTests
