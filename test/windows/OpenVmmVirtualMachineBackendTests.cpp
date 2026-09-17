// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "Common.h"
#include "OpenVmmVirtualMachineBackend.h"

using wsl::windows::common::vm::openvmm::ValidateCreateRequest;

namespace {

constexpr UINT64 c_mib = 1024 * 1024;
constexpr HRESULT c_notSupported = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);

VmCreateRequest CreateRequest()
{
    VmCreateRequest request;
    THROW_IF_FAILED(CoCreateGuid(&request.VmId));
    request.Processor.Count = 2;
    request.Memory.SizeBytes = 512 * c_mib;
    request.Boot.KernelPath = L"C:\\images\\kernel";
    request.Boot.InitrdPath = L"C:\\images\\initrd";
    return request;
}

VmCreateRequest CreateRunnableRequest()
{
    auto request = CreateRequest();
    const auto basePath = wsl::windows::common::wslutil::GetBasePath();
    request.Boot.KernelPath = basePath / L"kernel";
    request.Boot.InitrdPath = basePath / LXSS_VM_MODE_INITRD_NAME;
    request.Boot.GuestCommandLine = L"panic=-1";
    return request;
}

VmBootDiskRequest CreateDisk(std::wstring Key)
{
    VmBootDiskRequest disk;
    disk.Key = std::move(Key);
    disk.Disk.Source = VmVirtualDiskSource{L"C:\\images\\disk.vhdx", VmDiskFormat::Vhdx};
    return disk;
}

HRESULT DescribeResult(const VmCreateRequest& Request)
{
    return wil::ResultFromException([&] { ValidateCreateRequest(Request); });
}

template <typename Callback>
HRESULT OperationResult(Callback&& Operation)
{
    return wil::ResultFromException(std::forward<Callback>(Operation));
}

std::uint16_t ReserveTcpPort()
{
    wil::unique_socket socket{::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
    THROW_WIN32_IF(static_cast<DWORD>(WSAGetLastError()), !socket);

    SOCKADDR_IN address{};
    address.sin_family = AF_INET;
    address.sin_addr.S_un.S_addr = htonl(INADDR_LOOPBACK);
    THROW_WIN32_IF(static_cast<DWORD>(WSAGetLastError()), bind(socket.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR);

    int addressLength = sizeof(address);
    THROW_WIN32_IF(static_cast<DWORD>(WSAGetLastError()), getsockname(socket.get(), reinterpret_cast<sockaddr*>(&address), &addressLength) == SOCKET_ERROR);
    return ntohs(address.sin_port);
}

} // namespace

namespace OpenVmmVirtualMachineBackendTests {

class OpenVmmVirtualMachineBackendTests
{
    WSL_TEST_CLASS(OpenVmmVirtualMachineBackendTests)

    TEST_METHOD(PreservesCallerIdentityAndBootInputs)
    {
        SKIP_TEST_ARM64();
        auto request = CreateRequest();
        request.Boot.GuestCommandLine = L"personality=caller console=hvc0";
        request.Boot.UserCommandLine = L"console=ttyS0 custom=value";
        request.BootDisks.push_back(CreateDisk(L"arbitrary-key"));
        request.BootDisks[0].Disk.ReadOnly = false;
        const auto description = ValidateCreateRequest(request);

        VERIFY_IS_TRUE(IsEqualGUID(request.VmId, description.Identity.VmId));
        VERIFY_ARE_EQUAL(request.Processor.Count, description.Processor.Count);
        VERIFY_ARE_EQUAL(request.Memory.SizeBytes, description.Memory.SizeBytes);
        VERIFY_ARE_EQUAL(VmBootMethod::LinuxDirect, description.Boot.Method);
        VERIFY_ARE_EQUAL(request.Boot.GuestCommandLine + L" " + request.Boot.UserCommandLine, description.Boot.KernelCommandLine);
        VERIFY_ARE_EQUAL(size_t{1}, description.BootDisks.size());
        const auto& disk = description.BootDisks.at(L"arbitrary-key");
        VERIFY_IS_TRUE(IsEqualGUID(request.VmId, disk.Id.Owner.VmId));
        VERIFY_ARE_EQUAL(UINT64{1}, disk.Id.Value);
        VERIFY_ARE_EQUAL(UINT32{0}, disk.GuestAddress.Lun);
        VERIFY_IS_FALSE(disk.ReadOnly);
    }

    TEST_METHOD(ReservesExactPlacementsBeforeAutomaticDisks)
    {
        SKIP_TEST_ARM64();
        auto request = CreateRequest();
        request.BootDisks = {CreateDisk(L"automatic-1"), CreateDisk(L"automatic-2"), CreateDisk(L"exact")};
        request.BootDisks[2].Disk.Placement = VmScsiPlacement{{0, 0}};
        const auto description = ValidateCreateRequest(request);
        VERIFY_ARE_EQUAL(UINT32{1}, description.BootDisks.at(L"automatic-1").GuestAddress.Lun);
        VERIFY_ARE_EQUAL(UINT32{2}, description.BootDisks.at(L"automatic-2").GuestAddress.Lun);
        VERIFY_ARE_EQUAL(UINT32{0}, description.BootDisks.at(L"exact").GuestAddress.Lun);

        request.BootDisks[0].Disk.Placement = VmScsiPlacement{{0, 0}};
        VERIFY_ARE_EQUAL(E_INVALIDARG, DescribeResult(request));
        request.BootDisks[0].Disk.Placement.reset();
        request.BootDisks[1].Key = request.BootDisks[0].Key;
        VERIFY_ARE_EQUAL(E_INVALIDARG, DescribeResult(request));
    }

    TEST_METHOD(DoesNotCapMemoryAndRequiresGranularSizing)
    {
        SKIP_TEST_ARM64();
        auto request = CreateRequest();
        request.Memory.SizeBytes = 4096 * c_mib;
        VERIFY_ARE_EQUAL(request.Memory.SizeBytes, ValidateCreateRequest(request).Memory.SizeBytes);
        request.Memory.SizeBytes += 2 * c_mib;
        VERIFY_ARE_EQUAL(c_notSupported, DescribeResult(request));
        request.Memory.SizeBytes = 33 * c_mib;
        VERIFY_ARE_EQUAL(E_INVALIDARG, DescribeResult(request));
        request.Memory.SizeBytes = c_mib;
        VERIFY_ARE_EQUAL(c_notSupported, DescribeResult(request));
    }

    TEST_METHOD(RejectsInvalidDiskFormats)
    {
        SKIP_TEST_ARM64();
        auto request = CreateRequest();
        request.BootDisks.push_back(CreateDisk(L"disk"));
        request.BootDisks[0].Disk.Source = VmVirtualDiskSource{L"C:\\images\\disk.vhd", VmDiskFormat::Vhdx};
        VERIFY_ARE_EQUAL(E_INVALIDARG, DescribeResult(request));
    }

    TEST_METHOD(EnforcesDiskLimitsAndKeepsIdsVmScoped)
    {
        SKIP_TEST_ARM64();
        auto request = CreateRequest();
        for (UINT32 index = 0; index < 254; ++index)
        {
            request.BootDisks.push_back(CreateDisk(std::to_wstring(index)));
        }
        const auto first = ValidateCreateRequest(request);
        VERIFY_ARE_EQUAL(UINT32{253}, first.BootDisks.at(L"253").GuestAddress.Lun);
        THROW_IF_FAILED(CoCreateGuid(&request.VmId));
        const auto second = ValidateCreateRequest(request);
        VERIFY_ARE_EQUAL(first.BootDisks.at(L"0").Id.Value, second.BootDisks.at(L"0").Id.Value);
        VERIFY_IS_FALSE(IsEqualGUID(first.BootDisks.at(L"0").Id.Owner.VmId, second.BootDisks.at(L"0").Id.Owner.VmId));
        request.BootDisks.push_back(CreateDisk(L"overflow"));
        VERIFY_ARE_EQUAL(c_notSupported, DescribeResult(request));
    }

    TEST_METHOD(ValidatesConsoleFamiliesIndependently)
    {
        SKIP_TEST_ARM64();
        auto request = CreateRequest();
        request.Consoles = {
            {VmConsoleRole::EarlyBoot, VmSerialConsole{0, L"\\\\.\\pipe\\early"}},
            {VmConsoleRole::KernelConsole, VmVirtioConsole{0, L"", L"\\\\.\\pipe\\console"}}};
        VERIFY_ARE_EQUAL(size_t{2}, ValidateCreateRequest(request).Boot.Consoles.size());
        request.Consoles.push_back(request.Consoles[0]);
        VERIFY_ARE_EQUAL(E_INVALIDARG, DescribeResult(request));
        request.Consoles.pop_back();
        std::get<VmVirtioConsole>(request.Consoles[1].Device).GuestName = L"unsupported-name";
        VERIFY_ARE_EQUAL(c_notSupported, DescribeResult(request));
    }

    TEST_METHOD(RejectsInvalidIdentityAndBootPaths)
    {
        SKIP_TEST_ARM64();
        auto request = CreateRequest();
        request.VmId = GUID_NULL;
        VERIFY_ARE_EQUAL(E_INVALIDARG, DescribeResult(request));
        THROW_IF_FAILED(CoCreateGuid(&request.VmId));
        request.Boot.KernelPath = L"relative-kernel";
        VERIFY_ARE_EQUAL(E_INVALIDARG, DescribeResult(request));
        request.Boot.KernelPath = L"C:\\images\\kernel";
        request.Boot.Method = VmBootMethod::Uefi;
        VERIFY_ARE_EQUAL(c_notSupported, DescribeResult(request));
    }

    TEST_METHOD(BootsAndTerminates)
    {
        SKIP_TEST_ARM64();
        auto backend = OpenVmmVirtualMachineBackend::Create(CreateRunnableRequest());
        auto terminationEvent = backend->GetTerminationEvent();
        backend->Start();

        const auto runningResult = WaitForSingleObject(terminationEvent.get(), 100);
        VERIFY_ARE_EQUAL(static_cast<DWORD>(WAIT_TIMEOUT), runningResult);
        if (runningResult == WAIT_TIMEOUT)
        {
            backend->Terminate();
        }

        VERIFY_ARE_EQUAL(static_cast<DWORD>(WAIT_OBJECT_0), WaitForSingleObject(terminationEvent.get(), 30 * 1000));
    }

    TEST_METHOD(ManagesFileSystemNetworkAndPortResources)
    {
        SKIP_TEST_ARM64();
        auto request = CreateRunnableRequest();

        GUID directoryId{};
        THROW_IF_FAILED(CoCreateGuid(&directoryId));
        const auto sharePath =
            wsl::windows::common::filesystem::GetTempFolderPath(GetCurrentProcessToken()) /
            (L"OpenVmmBackendTest-" + wsl::shared::string::GuidToString<wchar_t>(directoryId, wsl::shared::string::GuidToStringFlags::None));
        THROW_IF_WIN32_BOOL_FALSE(CreateDirectoryW(sharePath.c_str(), nullptr));
        auto removeShareDirectory = wil::scope_exit([&] { LOG_IF_WIN32_BOOL_FALSE(RemoveDirectoryW(sharePath.c_str())); });

        auto backend = OpenVmmVirtualMachineBackend::Create(request);
        VmFileSystemDeviceRequest fileSystemRequest{{L"test-share", VmVirtioFsLayout::Aggregate}};
        VERIFY_ARE_EQUAL(c_notSupported, OperationResult([&] { backend->CreateFileSystemDevice(fileSystemRequest); }));
        fileSystemRequest.Transport.Layout = VmVirtioFsLayout::SingleShare;
        const auto fileSystemDevice = backend->CreateFileSystemDevice(fileSystemRequest);
        VERIFY_IS_TRUE(IsEqualGUID(request.VmId, fileSystemDevice.Id.Owner.VmId));
        VERIFY_ARE_EQUAL(VmFileSystemDeviceState::Prepared, fileSystemDevice.State);
        VERIFY_ARE_EQUAL(
            HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), OperationResult([&] { backend->CreateFileSystemDevice(fileSystemRequest); }));

        VmFileSystemShareRequest shareRequest;
        shareRequest.HostPath = sharePath;
        shareRequest.Options.MountOptions.emplace(L"unsupported", L"");
        VERIFY_ARE_EQUAL(c_notSupported, OperationResult([&] { backend->AddFileSystemShare(fileSystemDevice.Id, shareRequest); }));
        shareRequest.Options.MountOptions.clear();
        const auto share = backend->AddFileSystemShare(fileSystemDevice.Id, shareRequest);
        VERIFY_IS_TRUE(IsEqualGUID(request.VmId, share.Id.Owner.VmId));
        VERIFY_ARE_EQUAL(fileSystemDevice.Id.Value, share.Device.Value);
        VERIFY_ARE_EQUAL(fileSystemRequest.Transport.Tag, share.GuestAddress.Tag);
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), OperationResult([&] {
                             backend->AddFileSystemShare(fileSystemDevice.Id, shareRequest);
                         }));

        auto invalidShare = share.Id;
        THROW_IF_FAILED(CoCreateGuid(&invalidShare.Owner.VmId));
        VERIFY_ARE_EQUAL(E_INVALIDARG, OperationResult([&] { backend->RemoveFileSystemShare(invalidShare); }));
        backend->RemoveFileSystemShare(share.Id);
        const auto replacementShare = backend->AddFileSystemShare(fileSystemDevice.Id, shareRequest);
        VERIFY_ARE_NOT_EQUAL(share.Id.Value, replacementShare.Id.Value);
        backend->RemoveFileSystemShare(replacementShare.Id);

        VmNetworkAdapterRequest networkRequest;
        networkRequest.Tag = L"eth0";
        networkRequest.Configuration.ClientIpv4.Bytes = {192, 168, 127, 2};
        networkRequest.Configuration.ClientMac.Bytes = {0x00, 0x15, 0x5d, 0x01, 0x02, 0x03};
        networkRequest.Configuration.GatewayIpv4.Bytes = {192, 168, 127, 1};
        networkRequest.Configuration.Netmask.Bytes = {255, 255, 255, 0};
        const auto network = backend->AddNetworkAdapter(networkRequest);
        VERIFY_IS_TRUE(IsEqualGUID(request.VmId, network.Id.Owner.VmId));
        VERIFY_IS_TRUE(network.GuestInstanceId.has_value());
        VERIFY_ARE_EQUAL(networkRequest.Tag, network.Tag);
        VERIFY_ARE_EQUAL(
            HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), OperationResult([&] { backend->AddNetworkAdapter(networkRequest); }));

        VmPortBindingRequest bindingRequest;
        bindingRequest.Listen.Address = VmIpv4Address{{127, 0, 0, 1}};
        bindingRequest.Listen.Port = ReserveTcpPort();
        bindingRequest.GuestPort = 80;
        const auto binding = backend->BindPort(network.Id, bindingRequest);
        VERIFY_IS_TRUE(IsEqualGUID(request.VmId, binding.Id.Owner.VmId));
        VERIFY_ARE_EQUAL(network.Id.Value, binding.Device.Value);
        VERIFY_ARE_EQUAL(bindingRequest.Listen.Port, binding.EffectiveListen.Port);
        VERIFY_ARE_EQUAL(
            HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), OperationResult([&] { backend->BindPort(network.Id, bindingRequest); }));

        auto dynamicBinding = bindingRequest;
        dynamicBinding.Listen.Port = 0;
        VERIFY_ARE_EQUAL(c_notSupported, OperationResult([&] { backend->BindPort(network.Id, dynamicBinding); }));
        backend->UnbindPort(binding.Id);
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), OperationResult([&] { backend->UnbindPort(binding.Id); }));

        backend->Terminate();
    }

    TEST_METHOD(CancelPendingOperationsCancelsGuestAccept)
    {
        SKIP_TEST_ARM64();
        auto backend = OpenVmmVirtualMachineBackend::Create(CreateRunnableRequest());
        const auto listener = backend->CreateGuestListener({50000});
        VERIFY_ARE_EQUAL(
            HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), OperationResult([&] { backend->CreateGuestListener(listener.Port); }));

        std::promise<void> acceptStarted;
        auto started = acceptStarted.get_future();
        auto accept = std::async(std::launch::async, [&] {
            acceptStarted.set_value();
            return OperationResult([&] { backend->AcceptGuestConnection(listener.Id); });
        });
        started.get();

        backend->CancelPendingOperations();
        const auto acceptStatus = accept.wait_for(std::chrono::seconds{30});
        VERIFY_ARE_EQUAL(std::future_status::ready, acceptStatus);
        if (acceptStatus != std::future_status::ready)
        {
            backend->Terminate();
        }
        VERIFY_ARE_EQUAL(E_ABORT, accept.get());
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), OperationResult([&] { backend->CloseGuestListener(listener.Id); }));
        if (acceptStatus == std::future_status::ready)
        {
            backend->Terminate();
        }
    }

    TEST_METHOD(CapabilitiesReflectVmServiceProtocol)
    {
        const auto capabilities = OpenVmmVirtualMachineBackend::QueryCapabilities();
        VERIFY_ARE_EQUAL(BackendKind::OpenVmm, capabilities.Backend);

        decltype(capabilities.Operations) expectedOperations;
        for (const auto operation :
             {VmOperation::Create,
              VmOperation::Start,
              VmOperation::Terminate,
              VmOperation::CreateGuestListener,
              VmOperation::AcceptGuestConnection,
              VmOperation::ConnectGuest,
              VmOperation::CloseGuestListener,
              VmOperation::AttachDisk,
              VmOperation::DetachDisk,
              VmOperation::CreateFileSystemDevice,
              VmOperation::AddFileSystemShare,
              VmOperation::RemoveFileSystemShare,
              VmOperation::RemoveDevice,
              VmOperation::AddNetworkAdapter,
              VmOperation::UpdateNetworkAdapter,
              VmOperation::BindPort,
              VmOperation::UnbindPort})
        {
            expectedOperations.set(static_cast<size_t>(operation));
        }
        VERIFY_IS_TRUE(capabilities.Operations == expectedOperations);

        decltype(capabilities.Features) expectedFeatures;
        for (const auto feature :
             {VmFeature::LinuxDirectBoot,
              VmFeature::LinuxFirmwareBoot,
              VmFeature::MemoryOvercommit,
              VmFeature::SerialConsole,
              VmFeature::VirtioConsole,
              VmFeature::Vhd,
              VmFeature::Vhdx,
              VmFeature::VirtioFsFileBacked,
              VmFeature::SavedStateOnCrash,
              VmFeature::UserModeNatNetwork,
              VmFeature::TcpPortBinding,
              VmFeature::UdpPortBinding,
              VmFeature::Ipv6PortBinding,
              VmFeature::ScopedIpv6PortBinding})
        {
            expectedFeatures.set(static_cast<size_t>(feature));
        }
        VERIFY_IS_TRUE(capabilities.Features == expectedFeatures);
    }
};

} // namespace OpenVmmVirtualMachineBackendTests
