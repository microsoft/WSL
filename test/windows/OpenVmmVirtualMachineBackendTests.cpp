// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "Common.h"
#include "OpenVmmVirtualMachineBackend.h"
#include "VirtualMachineBackendTestHelpers.h"

using wsl::windows::common::vm::openvmm::ValidateCreateRequest;
using namespace VirtualMachineBackendTestHelpers;

namespace {

constexpr HRESULT c_notSupported = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);

VmBootDiskRequest CreateDisk(std::wstring Key)
{
    VmBootDiskRequest disk;
    disk.Key = std::move(Key);
    disk.Disk.Source = VmVirtualDiskSource{L"C:\\images\\disk.vhdx", VmDiskFormat::Vhdx};
    return disk;
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

HRESULT DescribeResult(const VmCreateRequest& Request)
{
    return wil::ResultFromException([&] { ValidateCreateRequest(Request); });
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
        request.Boot.KernelCommandLine = L"personality=caller console=hvc0 console=ttyS0 custom=value";
        request.BootDisks.push_back(CreateDisk(L"arbitrary-key"));
        request.BootDisks[0].Disk.ReadOnly = false;
        const auto description = ValidateCreateRequest(request);

        VERIFY_IS_TRUE(IsEqualGUID(request.Identity.VmId, description.Identity.VmId));
        VERIFY_ARE_EQUAL(request.Processor.Count, description.Processor.Count);
        VERIFY_ARE_EQUAL(request.Memory.SizeBytes, description.Memory.SizeBytes);
        VERIFY_ARE_EQUAL(VmBootMethod::LinuxDirect, description.Boot.Method);
        VERIFY_ARE_EQUAL(request.Boot.KernelCommandLine, description.Boot.KernelCommandLine);
        VERIFY_ARE_EQUAL(size_t{1}, description.BootDisks.size());
        const auto& disk = description.BootDisks.at(L"arbitrary-key");
        VERIFY_IS_TRUE(IsEqualGUID(request.Identity.VmId, disk.Id.Owner.VmId));
        VERIFY_ARE_EQUAL(UINT64{1}, disk.Id.Value);
        VERIFY_ARE_EQUAL(UINT32{0}, disk.GuestAddress.Lun);
        VERIFY_IS_FALSE(disk.ReadOnly);

        request.Mmio.HighWindowSizeBytes = c_mib;
        VERIFY_ARE_EQUAL(c_notSupported, DescribeResult(request));
        request.Mmio = {};
        request.Memory.SmallPageBacking = VmFeatureRequest::Required;
        VERIFY_ARE_EQUAL(c_notSupported, DescribeResult(request));
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

        auto highestPlacementRequest = CreateRequest();
        highestPlacementRequest.BootDisks.push_back(CreateDisk(L"highest"));
        highestPlacementRequest.BootDisks[0].Disk.Placement = VmScsiPlacement{{0, 253}};
        VERIFY_ARE_EQUAL(UINT32{253}, ValidateCreateRequest(highestPlacementRequest).BootDisks.at(L"highest").GuestAddress.Lun);
    }

    TEST_METHOD(DescribesCreationTimeNetworking)
    {
        SKIP_TEST_ARM64();
        auto request = CreateRequest();
        VERIFY_IS_TRUE(ValidateCreateRequest(request).NetworkAdapters.empty());
        request.NetworkAdapters.push_back(CreateNetworkRequest());
        const auto description = ValidateCreateRequest(request);
        VERIFY_ARE_EQUAL(size_t{1}, description.NetworkAdapters.size());
        const auto& adapter = description.NetworkAdapters.at(L"eth0");
        VERIFY_IS_TRUE(IsEqualGUID(request.Identity.VmId, adapter.Id.Owner.VmId));
        VERIFY_ARE_EQUAL(UINT64{1}, adapter.Id.Value);
        VERIFY_IS_TRUE(adapter.GuestInstanceId.has_value());
        VERIFY_IS_FALSE(IsEqualGUID(GUID_NULL, adapter.GuestInstanceId.value()));
        const auto& effectiveConfiguration = std::get<VmUserModeNatNetwork>(adapter.EffectiveConfiguration).Configuration;
        const auto& requestedConfiguration = std::get<VmUserModeNatNetwork>(request.NetworkAdapters[0].Configuration).Configuration;
        VERIFY_IS_TRUE(
            std::equal(
                std::begin(effectiveConfiguration.clientMac.bytes),
                std::end(effectiveConfiguration.clientMac.bytes),
                std::begin(requestedConfiguration.clientMac.bytes)));

        THROW_IF_FAILED(CoCreateGuid(&request.Identity.VmId));
        const auto other = ValidateCreateRequest(request).NetworkAdapters.at(L"eth0");
        VERIFY_ARE_EQUAL(adapter.Id.Value, other.Id.Value);
        VERIFY_IS_FALSE(IsEqualGUID(adapter.Id.Owner.VmId, other.Id.Owner.VmId));
        VERIFY_IS_FALSE(IsEqualGUID(adapter.GuestInstanceId.value(), other.GuestInstanceId.value()));
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
        THROW_IF_FAILED(CoCreateGuid(&request.Identity.VmId));
        const auto second = ValidateCreateRequest(request);
        VERIFY_ARE_EQUAL(first.BootDisks.at(L"0").Id.Value, second.BootDisks.at(L"0").Id.Value);
        VERIFY_IS_FALSE(IsEqualGUID(first.BootDisks.at(L"0").Id.Owner.VmId, second.BootDisks.at(L"0").Id.Owner.VmId));
        request.BootDisks.push_back(CreateDisk(L"overflow"));
        VERIFY_ARE_EQUAL(WSL_E_TOO_MANY_DISKS_ATTACHED, DescribeResult(request));
    }

    TEST_METHOD(ValidatesExplicitBootDiskPlacements)
    {
        SKIP_TEST_ARM64();
        auto request = CreateRequest();
        request.BootDisks.push_back(CreateDisk(L"exact"));
        auto& disk = request.BootDisks[0].Disk;
        disk.Placement = VmScsiPlacement{{0, 253}};
        VERIFY_ARE_EQUAL(UINT32{253}, ValidateCreateRequest(request).BootDisks.at(L"exact").GuestAddress.Lun);

        for (const UINT32 lun : {UINT32{254}, UINT32_MAX})
        {
            disk.Placement = VmScsiPlacement{{0, lun}};
            VERIFY_ARE_EQUAL(c_notSupported, DescribeResult(request));
        }

        for (const UINT32 lun : {UINT32{0}, UINT32{254}, UINT32_MAX})
        {
            disk.Placement = VmScsiPlacement{{1, lun}};
            VERIFY_ARE_EQUAL(c_notSupported, DescribeResult(request));
        }

        disk.Placement = VmScsiPlacement{{0, 253}};
        auto duplicate = CreateDisk(L"duplicate");
        duplicate.Disk.Placement = disk.Placement;
        request.BootDisks.push_back(std::move(duplicate));
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), DescribeResult(request));
    }

    TEST_METHOD(DescribesConsoleFamiliesIndependently)
    {
        SKIP_TEST_ARM64();
        auto request = CreateRequest();
        request.Consoles = {
            {VmConsoleRole::EarlyBoot, VmSerialConsole{0, L"\\\\.\\pipe\\early"}},
            {VmConsoleRole::KernelConsole, VmVirtioConsole{0, L"", L"\\\\.\\pipe\\console"}}};
        VERIFY_ARE_EQUAL(size_t{2}, ValidateCreateRequest(request).Boot.Consoles.size());
    }

    TEST_METHOD(BootsAndTerminates)
    {
        SKIP_TEST_ARM64();
        VerifyBootsAndTerminates(OpenVmmVirtualMachineBackend::Create(CreateRunnableRequest()));
    }

    TEST_METHOD(ManagesFileSystemNetworkAndPortResources)
    {
        SKIP_TEST_ARM64();
        auto request = CreateRunnableRequest();
        const auto networkRequest = CreateNetworkRequest();
        request.NetworkAdapters.push_back(networkRequest);

        GUID directoryId{};
        THROW_IF_FAILED(CoCreateGuid(&directoryId));
        const auto sharePath =
            wsl::windows::common::filesystem::GetTempFolderPath(GetCurrentProcessToken()) /
            (L"OpenVmmBackendTest-" + wsl::shared::string::GuidToString<wchar_t>(directoryId, wsl::shared::string::GuidToStringFlags::None));
        THROW_IF_WIN32_BOOL_FALSE(CreateDirectoryW(sharePath.c_str(), nullptr));
        auto removeShareDirectory = wil::scope_exit([&] { LOG_IF_WIN32_BOOL_FALSE(RemoveDirectoryW(sharePath.c_str())); });

        auto backend = OpenVmmVirtualMachineBackend::Create(request);
        VERIFY_ARE_EQUAL(c_notSupported, OperationResult([&] { backend->CreateFileSystemDevice({VmPlan9SocketDevice{{50000}}}); }));
        VERIFY_ARE_EQUAL(c_notSupported, OperationResult([&] { backend->CreateFileSystemDevice({VmPlan9VirtioDevice{L"plan9"}}); }));
        VmFileSystemDeviceRequest fileSystemRequest{VmVirtioFsDevice{L"test-share", VmVirtioFsLayout::Aggregate}};
        VERIFY_ARE_EQUAL(c_notSupported, OperationResult([&] { backend->CreateFileSystemDevice(fileSystemRequest); }));
        auto& fileSystemTransport = std::get<VmVirtioFsDevice>(fileSystemRequest.Transport);
        fileSystemTransport.Layout = VmVirtioFsLayout::SingleShare;
        fileSystemRequest.UserToken = request.Identity.UserToken;
        VERIFY_ARE_EQUAL(c_notSupported, OperationResult([&] { backend->CreateFileSystemDevice(fileSystemRequest); }));
        fileSystemRequest.UserToken.emplace();
        VERIFY_ARE_EQUAL(c_notSupported, OperationResult([&] { backend->CreateFileSystemDevice(fileSystemRequest); }));
        fileSystemRequest.UserToken.reset();
        const auto fileSystemDevice = backend->CreateFileSystemDevice(fileSystemRequest);
        VERIFY_ARE_EQUAL(UINT64{2}, fileSystemDevice.Id.Value);
        VERIFY_IS_TRUE(IsEqualGUID(request.Identity.VmId, fileSystemDevice.Id.Owner.VmId));
        VERIFY_ARE_EQUAL(VmFileSystemDeviceState::Prepared, fileSystemDevice.State);
        VERIFY_ARE_EQUAL(fileSystemTransport.Tag, std::get<VmVirtioFsDevice>(fileSystemDevice.Transport).Tag);
        VERIFY_ARE_EQUAL(VmVirtioFsLayout::SingleShare, std::get<VmVirtioFsDevice>(fileSystemDevice.Transport).Layout);
        VERIFY_ARE_EQUAL(
            HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), OperationResult([&] { backend->CreateFileSystemDevice(fileSystemRequest); }));

        VmFileSystemShareRequest shareRequest;
        shareRequest.HostPath = sharePath;
        shareRequest.Options = VmPlan9ShareOptions{};
        VERIFY_ARE_EQUAL(c_notSupported, OperationResult([&] { backend->AddFileSystemShare(fileSystemDevice.Id, shareRequest); }));
        shareRequest.Options = VmVirtioFsShareOptions{};
        auto& shareOptions = std::get<VmVirtioFsShareOptions>(shareRequest.Options);
        shareOptions.MountOptions.emplace(L"unsupported", L"");
        VERIFY_ARE_EQUAL(c_notSupported, OperationResult([&] { backend->AddFileSystemShare(fileSystemDevice.Id, shareRequest); }));
        shareOptions.MountOptions.clear();
        const auto share = backend->AddFileSystemShare(fileSystemDevice.Id, shareRequest);
        const auto servingDevice = backend->GetFileSystemDeviceStatus(fileSystemDevice.Id);
        VERIFY_ARE_EQUAL(VmFileSystemDeviceState::Prepared, fileSystemDevice.State);
        VERIFY_ARE_EQUAL(VmFileSystemDeviceState::Serving, servingDevice.State);
        VERIFY_ARE_EQUAL(fileSystemTransport.Tag, std::get<VmVirtioFsDevice>(servingDevice.Transport).Tag);
        VERIFY_IS_TRUE(IsEqualGUID(request.Identity.VmId, share.Id.Owner.VmId));
        VERIFY_ARE_EQUAL(fileSystemDevice.Id.Value, share.Device.Value);
        VERIFY_ARE_EQUAL(fileSystemTransport.Tag, std::get<VmVirtioFsShareAddress>(share.GuestAddress).Tag);
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), OperationResult([&] {
                             backend->AddFileSystemShare(fileSystemDevice.Id, shareRequest);
                         }));

        auto invalidShare = share.Id;
        THROW_IF_FAILED(CoCreateGuid(&invalidShare.Owner.VmId));
        VERIFY_ARE_EQUAL(E_INVALIDARG, OperationResult([&] { backend->RemoveFileSystemShare(invalidShare); }));
        backend->RemoveFileSystemShare(share.Id);
        VERIFY_ARE_EQUAL(VmFileSystemDeviceState::Prepared, backend->GetFileSystemDeviceStatus(fileSystemDevice.Id).State);
        VERIFY_ARE_EQUAL(VmFileSystemDeviceState::Serving, servingDevice.State);
        const auto replacementShare = backend->AddFileSystemShare(fileSystemDevice.Id, shareRequest);
        VERIFY_ARE_NOT_EQUAL(share.Id.Value, replacementShare.Id.Value);
        backend->RemoveFileSystemShare(replacementShare.Id);

        const auto network = backend->GetDescription().NetworkAdapters.at(networkRequest.Tag);
        VERIFY_IS_TRUE(IsEqualGUID(request.Identity.VmId, network.Id.Owner.VmId));
        VERIFY_IS_TRUE(network.GuestInstanceId.has_value());
        VERIFY_ARE_EQUAL(networkRequest.Tag, network.Tag);
        VERIFY_ARE_NOT_EQUAL(network.Id.Value, fileSystemDevice.Id.Value);
        VERIFY_ARE_EQUAL(
            std::get<VmUserModeNatNetwork>(networkRequest.Configuration).Configuration.clientIp.value,
            std::get<VmUserModeNatNetwork>(network.EffectiveConfiguration).Configuration.clientIp.value);
        VERIFY_ARE_EQUAL(c_notSupported, OperationResult([&] { backend->AddNetworkAdapter(networkRequest); }));

        // Consomme processes port requests once the guest has initialized its network queues.
        backend->Start();
        VmPortBindingRequest bindingRequest;
        bindingRequest.ListenAddress.family = IpAddressFamily_V4;
        const auto loopbackAddress = htonl(INADDR_LOOPBACK);
        std::memcpy(bindingRequest.ListenAddress.bytes, &loopbackAddress, sizeof(loopbackAddress));
        bindingRequest.HostPort = ReserveTcpPort();
        bindingRequest.GuestPort = 80;
        VERIFY_ARE_EQUAL(
            HRESULT_FROM_WIN32(ERROR_NOT_FOUND), OperationResult([&] { backend->BindPort(fileSystemDevice.Id, bindingRequest); }));
        auto invalidNetwork = network.Id;
        THROW_IF_FAILED(CoCreateGuid(&invalidNetwork.Owner.VmId));
        VERIFY_ARE_EQUAL(E_INVALIDARG, OperationResult([&] { backend->BindPort(invalidNetwork, bindingRequest); }));
        const auto binding = backend->BindPort(network.Id, bindingRequest);
        VERIFY_IS_TRUE(IsEqualGUID(request.Identity.VmId, binding.Id.Owner.VmId));
        VERIFY_ARE_EQUAL(network.Id.Value, binding.Device.Value);
        VERIFY_ARE_EQUAL(bindingRequest.HostPort, binding.EffectiveHostPort);

        auto dynamicBinding = bindingRequest;
        dynamicBinding.HostPort = 0;
        VERIFY_ARE_EQUAL(c_notSupported, OperationResult([&] { backend->BindPort(network.Id, dynamicBinding); }));
        backend->UnbindPort(binding.Id);
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), OperationResult([&] { backend->UnbindPort(binding.Id); }));

        backend->BindPort(network.Id, bindingRequest);
        VERIFY_ARE_EQUAL(E_FAIL, OperationResult([&] { backend->BindPort(network.Id, bindingRequest); }));

        backend->Terminate();
    }

    TEST_METHOD(ManagesGuestListenerLifetime)
    {
        SKIP_TEST_ARM64();
        auto backend = OpenVmmVirtualMachineBackend::Create(CreateRunnableRequest());
        const auto listener = backend->CreateGuestListener({50000});
        VERIFY_ARE_EQUAL(
            HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), OperationResult([&] { backend->CreateGuestListener(listener.Port); }));

        backend->CloseGuestListener(listener.Id);
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), OperationResult([&] { backend->CloseGuestListener(listener.Id); }));
        const auto replacement = backend->CreateGuestListener(listener.Port);
        VERIFY_ARE_NOT_EQUAL(listener.Id.Value, replacement.Id.Value);
        backend->CloseGuestListener(replacement.Id);
        backend->Terminate();
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
              VmOperation::GetFileSystemDeviceStatus,
              VmOperation::AddFileSystemShare,
              VmOperation::RemoveFileSystemShare,
              VmOperation::RemoveDevice,
              VmOperation::BindPort,
              VmOperation::UnbindPort})
        {
            expectedOperations.set(static_cast<size_t>(operation));
        }
        VERIFY_IS_TRUE(capabilities.Operations == expectedOperations);

        decltype(capabilities.Features) expectedFeatures;
        for (const auto feature :
             {VmFeature::LinuxDirectBoot,
              VmFeature::SerialConsole,
              VmFeature::VirtioConsole,
              VmFeature::Vhd,
              VmFeature::Vhdx,
              VmFeature::VirtioFsFileBacked,
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
