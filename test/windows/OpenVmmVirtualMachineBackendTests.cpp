// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "Common.h"
#include "OpenVmmNatNetworking.h"
#include "OpenVmmVirtualMachineBackend.h"
#include "VirtualMachineBackendTestHelpers.h"
#include "vsock.hpp"

using wsl::windows::common::vm::openvmm::ValidateCreateRequest;
using namespace VirtualMachineBackendTestHelpers;

namespace {

VmBootDiskRequest CreateDisk(std::wstring Key)
{
    VmBootDiskRequest disk;
    disk.Key = std::move(Key);
    disk.Disk.Source = VmVirtualDiskSource{L"C:\\images\\disk.vhdx"};
    return disk;
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

    TEST_CLASS_SETUP(TestClassSetup)
    {
        WSADATA data{};
        THROW_IF_WIN32_ERROR(WSAStartup(MAKEWORD(2, 2), &data));
        return true;
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        VERIFY_ARE_EQUAL(0, WSACleanup());
        return true;
    }

    TEST_METHOD(PreservesParentExecutionContextOnCreationFailure)
    {
        using namespace wsl::windows::common;
        const ExecutionContext context(Context::CreateVm);
        const VmCreateRequest request;
        VERIFY_ARE_EQUAL(E_INVALIDARG, OperationResult([&] { OpenVmmVirtualMachineBackend::Create(request); }));

        VERIFY_IS_TRUE(context.ReportedError().has_value());
        const auto& error = context.ReportedError().value();
        VERIFY_ARE_EQUAL(E_INVALIDARG, error.Code);
        VERIFY_IS_TRUE(WI_IsFlagSet(error.Context, Context::CreateVm));
        VERIFY_IS_TRUE(WI_IsFlagSet(error.Context, Context::OpenVmm));
    }

    TEST_METHOD(RejectsIncompatibleOpenVmmSettings)
    {
        using wsl::windows::common::filesystem::TempFile;
        constexpr std::string_view validConfig =
            "[wsl2]\n"
            "gpuSupport=false\n"
            "guiApplications=false\n"
            "nestedVirtualization=false\n"
            "virtio=true\n"
            "virtioFs=true\n"
            "virtio9p=false\n"
            "hardwarePerformanceCounters=false\n"
            "networkingMode=nat\n"
            "firewall=false\n"
            "dnsTunneling=false\n"
            "ipv6=false\n"
            "[experimental]\n"
            "openVmm=true\n"
            "virtioFsAggregateShares=false\n";

        auto token = GetNonElevatedToken(TokenImpersonation);
        const auto configure = [&](std::string_view Override) {
            TempFile file{GENERIC_WRITE, FILE_SHARE_READ, CREATE_ALWAYS};
            const auto content = std::string{Override} + std::string{validConfig};
            DWORD written = 0;
            THROW_IF_WIN32_BOOL_FALSE(WriteFile(file.Handle.get(), content.data(), static_cast<DWORD>(content.size()), &written, nullptr));
            VERIFY_ARE_EQUAL(content.size(), static_cast<size_t>(written));
            file.Handle.reset();
            return OperationResult([&] { wsl::core::Config config{file.Path.c_str(), token.get()}; });
        };

        VERIFY_ARE_EQUAL(S_OK, configure(""));
        VERIFY_ARE_EQUAL(S_OK, configure("\n[wsl2]\nnetworkingMode=none\nfirewall=true\ndnsTunneling=true\nipv6=true\n"));
        VERIFY_ARE_EQUAL(S_OK, configure("\n[experimental]\nopenVmm=false\n[wsl2]\ngpuSupport=true\n"));
        for (const auto* setting : {
                 "\n[wsl2]\ngpuSupport=true\n",
                 "\n[wsl2]\nguiApplications=true\n",
                 "\n[wsl2]\nnestedVirtualization=true\n",
                 "\n[wsl2]\nvirtio=false\n",
                 "\n[wsl2]\nvirtioFs=false\n",
                 "\n[wsl2]\nvirtio9p=true\n",
                 "\n[wsl2]\nhardwarePerformanceCounters=true\n",
                 "\n[wsl2]\nnetworkingMode=mirrored\n",
                 "\n[wsl2]\nnetworkingMode=bridged\n",
                 "\n[wsl2]\nnetworkingMode=consomme\n",
                 "\n[wsl2]\nfirewall=true\n",
                 "\n[wsl2]\ndnsTunneling=true\n",
                 "\n[wsl2]\nipv6=true\n",
                 "\n[experimental]\nvirtioFsAggregateShares=true\n",
                 "\n[experimental]\nswiotlb=1M\n",
             })
        {
            VERIFY_ARE_EQUAL(c_notSupported, configure(setting));
        }

        const wsl::windows::common::ExecutionContext context(wsl::windows::common::Context::CreateVm);
        VERIFY_ARE_EQUAL(c_notSupported, configure("\n[wsl2]\ngpuSupport=true\n"));
        VERIFY_IS_TRUE(context.ReportedError().has_value());
        VERIFY_IS_TRUE(context.ReportedError()->Message.has_value());
        VERIFY_IS_TRUE(context.ReportedError()->Message->find(L"wsl2.gpuSupport") != std::wstring::npos);
    }

    TEST_METHOD(RejectsUnsupportedOpenVmmProcessorFeatures)
    {
        SKIP_TEST_ARM64();
        auto request = CreateRequest();
        request.Processor.NestedVirtualization = VmFeatureRequest::Preferred;
        VERIFY_ARE_EQUAL(c_notSupported, DescribeResult(request));
        request.Processor.NestedVirtualization = VmFeatureRequest::Disabled;
        request.Processor.PerfmonPmu = VmFeatureRequest::Preferred;
        VERIFY_ARE_EQUAL(c_notSupported, DescribeResult(request));
        request.Processor.PerfmonPmu = VmFeatureRequest::Disabled;
        request.Processor.PerfmonLbr = VmFeatureRequest::Required;
        VERIFY_ARE_EQUAL(c_notSupported, DescribeResult(request));
        request.Processor.PerfmonLbr = VmFeatureRequest::Disabled;
        request.NetworkAdapters.push_back({L"host", VmHostEndpointNetwork{}});
        VERIFY_ARE_EQUAL(c_notSupported, DescribeResult(request));
    }

    TEST_METHOD(UsesUtf8ForUnixSocketPaths)
    {
        const auto address = wsl::windows::common::vsock::GetUnixSocketAddress(std::filesystem::path{L"socket-\u00e9"});
        VERIFY_ARE_EQUAL(std::string{"socket-\xc3\xa9"}, std::string{address.sun_path});

        const std::filesystem::path fitting{std::wstring(sizeof(address.sun_path) - 3, L'a') + L"\u00e9"};
        const std::filesystem::path tooLong{std::wstring(sizeof(address.sun_path) - 2, L'a') + L"\u00e9"};
        VERIFY_ARE_EQUAL(sizeof(address.sun_path) - 1, strlen(wsl::windows::common::vsock::GetUnixSocketAddress(fitting).sun_path));
        VERIFY_ARE_EQUAL(E_INVALIDARG, OperationResult([&] { wsl::windows::common::vsock::GetUnixSocketAddress(tooLong); }));
    }

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
        VERIFY_ARE_EQUAL(S_OK, DescribeResult(request));
        request.Mmio.MaximumGuestAddressBits = 20;
        VERIFY_ARE_EQUAL(c_notSupported, DescribeResult(request));
        request.Mmio.MaximumGuestAddressBits.reset();
        request.Mmio.HighWindowSizeBytes = c_mib + 1;
        VERIFY_ARE_EQUAL(E_INVALIDARG, DescribeResult(request));
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

    TEST_METHOD(LeavesVirtualDiskFileNamesToBackend)
    {
        SKIP_TEST_ARM64();
        auto request = CreateRequest();
        request.BootDisks.push_back(CreateDisk(L"modules"));
        request.BootDisks[0].Disk.Source = VmVirtualDiskSource{L"C:\\images\\modules.img"};
        VERIFY_ARE_EQUAL(std::wstring{L"C:\\images\\modules.img"}, ValidateCreateRequest(request).BootDisks.at(L"modules").Path);
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
        VERIFY_IS_TRUE(std::equal(
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
            {VmConsoleRole::KernelConsole, VmVirtioConsole{0, L"hvc0", L"\\\\.\\pipe\\console"}}};
        VERIFY_ARE_EQUAL(size_t{2}, ValidateCreateRequest(request).Boot.Consoles.size());
    }

    TEST_METHOD(BootsAndTerminates)
    {
        SKIP_TEST_ARM64();
        VerifyBootsAndTerminates(OpenVmmVirtualMachineBackend::Create(CreateRunnableRequest()));
    }

    TEST_METHOD(DrainsResourcesBeforeNotifyingTermination)
    {
        SKIP_TEST_ARM64();
        auto backend = OpenVmmVirtualMachineBackend::Create(CreateRunnableRequest());
        const auto device = backend->CreateFileSystemDevice({VmVirtioFsDevice{L"shutdown-test", VmVirtioFsLayout::SingleShare}});
        const auto terminationEvent = backend->GetTerminationEvent();
        wil::unique_event callbackEvent{wil::EventOptions::ManualReset};
        HRESULT reentrantResult = E_FAIL;
        bool resourcesReleased = false;
        backend->RegisterTerminationCallback([&](GUID) {
            reentrantResult = OperationResult([&] { backend->Terminate(); });
            resourcesReleased =
                !backend->GetFileSystemDevice([&](const auto& entry) { return entry.Id.Value == device.Id.Value; }).has_value();
            callbackEvent.SetEvent();
        });

        backend->Terminate();
        VERIFY_IS_TRUE(callbackEvent.wait(30 * 1000));
        VERIFY_ARE_EQUAL(S_OK, reentrantResult);
        VERIFY_IS_TRUE(resourcesReleased);
        VERIFY_IS_TRUE(terminationEvent.is_signaled());
        VERIFY_ARE_EQUAL(VmState::Stopped, backend->GetState());
        VERIFY_ARE_EQUAL(S_OK, OperationResult([&] { backend->Terminate(); }));
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), OperationResult([&] { backend->Start(); }));
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
        const auto ownerDevice = backend->CreateFileSystemDevice(fileSystemRequest);
        backend->RemoveDevice(ownerDevice.Id);
        fileSystemRequest.UserToken.emplace();
        VERIFY_ARE_EQUAL(E_UNEXPECTED, OperationResult([&] { backend->CreateFileSystemDevice(fileSystemRequest); }));
        fileSystemRequest.UserToken.reset();
        const auto fileSystemDevice = backend->CreateFileSystemDevice(fileSystemRequest);
        VERIFY_ARE_EQUAL(UINT64{3}, fileSystemDevice.Id.Value);
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
        shareOptions.MountOptions.emplace(L"metadata", L"");
        const auto share = backend->AddFileSystemShare(fileSystemDevice.Id, shareRequest);
        const auto servingDevice = backend->GetFileSystemDeviceStatus(fileSystemDevice.Id);
        VERIFY_ARE_EQUAL(VmFileSystemDeviceState::Prepared, fileSystemDevice.State);
        VERIFY_ARE_EQUAL(VmFileSystemDeviceState::Serving, servingDevice.State);
        VERIFY_ARE_EQUAL(fileSystemTransport.Tag, std::get<VmVirtioFsDevice>(servingDevice.Transport).Tag);
        VERIFY_IS_TRUE(IsEqualGUID(request.Identity.VmId, share.Id.Owner.VmId));
        VERIFY_ARE_EQUAL(fileSystemDevice.Id.Value, share.Device.Value);
        VERIFY_IS_TRUE(share.MountOptions.contains(L"metadata"));
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
        auto guest = StartGuest(*backend);
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

    TEST_METHOD(PersistentMemoryUsesVmOwnerToken)
    {
        SKIP_TEST_ARM64();
        using wsl::windows::common::filesystem::TempFile;
        auto request = CreateRunnableRequest();
        TempFile file{GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, CREATE_ALWAYS};
        LARGE_INTEGER size{};
        size.QuadPart = 2 * c_mib;
        THROW_IF_WIN32_BOOL_FALSE(SetFilePointerEx(file.Handle.get(), size, nullptr, FILE_BEGIN));
        THROW_IF_WIN32_BOOL_FALSE(SetEndOfFile(file.Handle.get()));
        file.Handle.reset();

        auto backend = OpenVmmVirtualMachineBackend::Create(request);
        auto guest = StartGuest(*backend);
        VmPersistentMemoryRequest memory{};
        memory.Path = file.Path;
        memory.UserToken.emplace();
        VERIFY_ARE_EQUAL(E_INVALIDARG, OperationResult([&] { backend->AddPersistentMemory(memory); }));
        memory.UserToken = wil::shared_handle{GetNonElevatedToken(TokenImpersonation).release()};
        VERIFY_ARE_EQUAL(c_notSupported, OperationResult([&] { backend->AddPersistentMemory(memory); }));
        memory.UserToken = wil::shared_handle{wsl::windows::common::wslutil::DuplicateHandle(request.Identity.UserToken.get())};
        const auto device = backend->AddPersistentMemory(memory);
        VERIFY_ARE_EQUAL(UINT32{0}, device.Index);
        VERIFY_ARE_EQUAL(file.Path.wstring(), device.EffectiveHostPath.wstring());
        backend->Terminate();
    }

    TEST_METHOD(RelaysIpv4MappedPortNotifications)
    {
        SKIP_TEST_ARM64();
        auto request = CreateRunnableRequest();
        request.NetworkAdapters.push_back(wsl::core::CreateOpenVmmNatNetworkAdapterRequest(false));
        request.NetworkAdapters.push_back(wsl::core::CreateOpenVmmLoopbackNetworkAdapterRequest());
        auto backend = OpenVmmVirtualMachineBackend::Create(request);
        auto guest = StartGuest(*backend);
        const auto& adapters = backend->GetDescription().NetworkAdapters;
        wsl::core::OpenVmmNatNetworking networking{
            *backend, adapters.at(L"eth0"), adapters.at(L"loopback0"), wsl::core::GnsChannel{wil::unique_socket{}}, true, false};

        auto [client, server] = MakeSocketPair();
        networking.StartPortTracker(std::move(server));
        wsl::shared::SocketChannel channel{std::move(client), "PortTrackerTest"};
        LX_GNS_PORT_ALLOCATION_REQUEST notification{};
        notification.Header.MessageType = LxGnsMessagePortMappingRequest;
        notification.Header.MessageSize = sizeof(notification);
        notification.Protocol = IPPROTO_TCP;
        notification.Port = ReserveTcpPort();
        notification.Af = AF_INET6;
        THROW_HR_IF(E_FAIL, inet_pton(AF_INET6, "::ffff:127.0.0.1", notification.Address32) != 1);
        notification.Allocate = true;
        VERIFY_ARE_EQUAL(0, channel.Transaction(notification).Result);

        notification.Af = AF_INET;
        std::fill(std::begin(notification.Address32), std::end(notification.Address32), 0);
        notification.Address32[0] = htonl(INADDR_LOOPBACK);
        VERIFY_ARE_NOT_EQUAL(0, channel.Transaction(notification).Result);
        notification.Allocate = false;
        VERIFY_ARE_EQUAL(0, channel.Transaction(notification).Result);
        notification.Allocate = true;
        VERIFY_ARE_EQUAL(0, channel.Transaction(notification).Result);
        notification.Af = AF_INET6;
        THROW_HR_IF(E_FAIL, inet_pton(AF_INET6, "::ffff:127.0.0.1", notification.Address32) != 1);
        notification.Allocate = false;
        VERIFY_ARE_EQUAL(0, channel.Transaction(notification).Result);

        backend->Terminate();
    }

    WSL2_TEST_METHOD(ManagesGuestListenerLifetime)
    {
        SKIP_TEST_ARM64();
        auto backend = OpenVmmVirtualMachineBackend::Create(CreateRunnableRequest());
        constexpr GuestServicePort port{50000};
        auto listener = backend->CreateGuestListener(port);
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), OperationResult([&] { backend->CreateGuestListener(port); }));

        auto moved = std::move(listener);
        VERIFY_IS_FALSE(listener.Socket.is_valid());
        VERIFY_IS_TRUE(moved.Socket.is_valid());
        listener = VmGuestListener{};
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), OperationResult([&] { backend->CreateGuestListener(port); }));

        moved = VmGuestListener{};
        auto replacement = backend->CreateGuestListener(port);
        VERIFY_IS_TRUE(replacement.Socket.is_valid());
        backend->Terminate();
        VERIFY_IS_TRUE(replacement.Socket.is_valid());
        replacement = VmGuestListener{};
    }

    TEST_METHOD(CapabilitiesReflectVmServiceProtocol)
    {
        const auto capabilities = OpenVmmVirtualMachineBackend::QueryCapabilities();
        VERIFY_ARE_EQUAL(BackendKind::OpenVmm, capabilities.Backend);

        decltype(capabilities.Features) expectedFeatures;
        for (const auto feature :
             {VmFeature::SerialConsole,
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
