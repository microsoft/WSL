// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include <fstream>
#include "Common.h"
#include "OpenVmmNatNetworking.h"
#include "OpenVmmVirtualMachineBackend.h"
#include "WslInstall.h"
#include "VirtualMachineBackendTestHelpers.h"
#include "WslCoreHostDnsInfo.h"
#include "vsock.hpp"

using wsl::windows::common::vm::openvmm::ValidateCreateRequest;
using namespace VirtualMachineBackendTestHelpers;

namespace {

UINT32 GetSlot(const VmDiskAttachment& Disk)
{
    return std::get<VmVirtioBlkAddress>(Disk.GuestAddress).Slot;
}

VmBootDiskRequest CreateDisk(std::wstring Key)
{
    VmBootDiskRequest disk;
    disk.Key = std::move(Key);
    disk.Disk.Source = VmVirtualDiskSource{L"C:\\images\\disk.vhdx"};
    return disk;
}

std::uint16_t ReservePort(int Protocol = IPPROTO_TCP)
{
    wil::unique_socket socket{::socket(AF_INET, Protocol == IPPROTO_TCP ? SOCK_STREAM : SOCK_DGRAM, Protocol)};
    THROW_WIN32_IF(static_cast<DWORD>(WSAGetLastError()), !socket);

    SOCKADDR_IN address{};
    address.sin_family = AF_INET;
    address.sin_addr.S_un.S_addr = htonl(INADDR_LOOPBACK);
    THROW_WIN32_IF(static_cast<DWORD>(WSAGetLastError()), bind(socket.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR);

    int addressLength = sizeof(address);
    THROW_WIN32_IF(static_cast<DWORD>(WSAGetLastError()), getsockname(socket.get(), reinterpret_cast<sockaddr*>(&address), &addressLength) == SOCKET_ERROR);
    return ntohs(address.sin_port);
}

bool CanBindPort(std::uint16_t Port, ULONG Address, int Protocol)
{
    wil::unique_socket socket{::socket(AF_INET, Protocol == IPPROTO_TCP ? SOCK_STREAM : SOCK_DGRAM, Protocol)};
    THROW_WIN32_IF(static_cast<DWORD>(WSAGetLastError()), !socket);
    BOOL exclusive = TRUE;
    THROW_WIN32_IF(
        static_cast<DWORD>(WSAGetLastError()),
        setsockopt(socket.get(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) == SOCKET_ERROR);
    SOCKADDR_IN address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(Address);
    address.sin_port = htons(Port);
    if (bind(socket.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0)
    {
        return true;
    }

    const auto error = WSAGetLastError();
    THROW_WIN32_IF(static_cast<DWORD>(error), error != WSAEADDRINUSE && error != WSAEACCES);
    return false;
}

HRESULT DescribeResult(const VmCreateRequest& Request)
{
    return wil::ResultFromException([&] { ValidateCreateRequest(Request); });
}

} // namespace

namespace OpenVmmVirtualMachineBackendTests {

class OpenVmmVirtualMachineBackendTests
{
    OPENVMM_TEST_CLASS(OpenVmmVirtualMachineBackendTests)

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

    TEST_METHOD(CreationErrorsIncludeBackend)
    {
        using namespace wsl::windows::common;
        ExecutionContext context(Context::Wsl);
        ExecutionContext serviceContext(Context::Service);
        const VmCreateRequest request;
        VERIFY_ARE_EQUAL(E_INVALIDARG, OperationResult([&] { OpenVmmVirtualMachineBackend::Create(request); }));
        VERIFY_IS_TRUE(context.ReportedError().has_value());
        VERIFY_ARE_EQUAL(
            std::wstring{L"Wsl/Service/OpenVmm/E_INVALIDARG"}, wslutil::ErrorToString(context.ReportedError().value()).Code);
        VERIFY_ARE_EQUAL(static_cast<ULONGLONG>(Context::Wsl | Context::Service), serviceContext.CurrentContext());
    }

    TEST_METHOD(OperationErrorsIncludeBackend)
    {
        SKIP_TEST_ARM64();
        using namespace wsl::windows::common;
        auto backend = OpenVmmVirtualMachineBackend::Create(CreateRunnableRequest());
        const auto verifyError = [&](auto&& Operation, HRESULT ExpectedResult, PCWSTR ExpectedCode, std::optional<Context> CallerContext = {}) {
            ExecutionContext context(Context::Wsl);
            ExecutionContext serviceContext(Context::Service);
            std::optional<ExecutionContext> callerContext;
            if (CallerContext)
            {
                callerContext.emplace(CallerContext.value());
            }

            VERIFY_ARE_EQUAL(ExpectedResult, OperationResult([&] { Operation(); }));
            VERIFY_IS_TRUE(context.ReportedError().has_value());
            VERIFY_ARE_EQUAL(std::wstring{ExpectedCode}, wslutil::ErrorToString(context.ReportedError().value()).Code);
        };

        constexpr auto invalidState = HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
        verifyError([&] { backend->DetachDisk({}); }, E_INVALIDARG, L"Wsl/Service/OpenVmm/E_INVALIDARG");
        verifyError([&] { backend->GetTerminationReason(); }, invalidState, L"Wsl/Service/OpenVmm/ERROR_INVALID_STATE");
        verifyError([&] { backend->AddGpu({}); }, c_notSupported, L"Wsl/Service/OpenVmm/ERROR_NOT_SUPPORTED");
        verifyError([&] { backend->AddSharedMemory({}); }, c_notSupported, L"Wsl/Service/OpenVmm/ERROR_NOT_SUPPORTED");
        verifyError([&] { backend->ConfigureGuestDma({}); }, c_notSupported, L"Wsl/Service/OpenVmm/ERROR_NOT_SUPPORTED");
        VmDiskRequest physicalDisk;
        physicalDisk.Source = VmPhysicalDiskSource{L"\\\\.\\PhysicalDrive0"};
        verifyError(
            [&] { backend->AttachDisk(physicalDisk); }, c_notSupported, L"Wsl/Service/AttachDisk/OpenVmm/ERROR_NOT_SUPPORTED", Context::AttachDisk);
        verifyError([&] { backend->CreateFileSystemDevice({}); }, c_notSupported, L"Wsl/Service/OpenVmm/ERROR_NOT_SUPPORTED");
        verifyError([&] { backend->GetFileSystemDeviceStatus({}); }, E_INVALIDARG, L"Wsl/Service/OpenVmm/E_INVALIDARG");
        verifyError([&] { backend->AddFileSystemShare({}, {}); }, E_INVALIDARG, L"Wsl/Service/OpenVmm/E_INVALIDARG");
        verifyError([&] { backend->RemoveFileSystemShare({}); }, E_INVALIDARG, L"Wsl/Service/OpenVmm/E_INVALIDARG");
        verifyError([&] { backend->RemoveDevice({}); }, E_INVALIDARG, L"Wsl/Service/OpenVmm/E_INVALIDARG");
        verifyError([&] { backend->CloseHostSocket({}); }, E_INVALIDARG, L"Wsl/Service/OpenVmm/E_INVALIDARG");

        // Networking callers add the ConfigureNetworking context; the backend adds its own.
        constexpr auto networking = Context::ConfigureNetworking;
        verifyError(
            [&] { backend->AddNetworkAdapter({}); },
            c_notSupported,
            L"Wsl/Service/ConfigureNetworking/OpenVmm/ERROR_NOT_SUPPORTED",
            networking);
        verifyError(
            [&] { backend->UpdateNetworkAdapter({}, {}); }, E_INVALIDARG, L"Wsl/Service/ConfigureNetworking/OpenVmm/E_INVALIDARG", networking);
        verifyError(
            [&] { backend->RemoveNetworkAdapter({}); }, E_INVALIDARG, L"Wsl/Service/ConfigureNetworking/OpenVmm/E_INVALIDARG", networking);
        verifyError([&] { backend->BindPort({}, {}); }, E_INVALIDARG, L"Wsl/Service/ConfigureNetworking/OpenVmm/E_INVALIDARG", networking);
        verifyError([&] { backend->UnbindPort({}); }, E_INVALIDARG, L"Wsl/Service/ConfigureNetworking/OpenVmm/E_INVALIDARG", networking);
        verifyError(
            [&] { backend->CreateVirtualAddress({}, {}); }, E_INVALIDARG, L"Wsl/Service/ConfigureNetworking/OpenVmm/E_INVALIDARG", networking);
        verifyError(
            [&] { backend->CreateDnsRecord({}, {}); }, E_INVALIDARG, L"Wsl/Service/ConfigureNetworking/OpenVmm/E_INVALIDARG", networking);

        backend->Terminate();
        verifyError([&] { backend->Start(); }, invalidState, L"Wsl/Service/OpenVmm/ERROR_INVALID_STATE");
        verifyError([&] { backend->CreateGuestListener({1234}); }, invalidState, L"Wsl/Service/OpenVmm/ERROR_INVALID_STATE");
        verifyError([&] { backend->ConnectGuest({1234}); }, invalidState, L"Wsl/Service/OpenVmm/ERROR_INVALID_STATE");
        verifyError([&] { backend->CreateHostSocket({1234}); }, invalidState, L"Wsl/Service/OpenVmm/ERROR_INVALID_STATE");
    }

    TEST_METHOD(NestedVirtualizationConflictIncludesUserError)
    {
        SKIP_TEST_ARM64();
        using namespace wsl::windows::common;
        for (const auto feature : {VmFeatureRequest::Preferred, VmFeatureRequest::Required})
        {
            ExecutionContext context(Context::Wsl);
            auto request = CreateRequest();
            request.Processor.NestedVirtualization = feature;
            VERIFY_ARE_EQUAL(c_notSupported, OperationResult([&] { OpenVmmVirtualMachineBackend::Create(request); }));
            VERIFY_IS_TRUE(context.ReportedError().has_value());
            const auto& error = context.ReportedError().value();
            VERIFY_IS_TRUE(error.Message.has_value());
            VERIFY_IS_TRUE(error.Message->find(L"experimental.openVmm=true") != std::wstring::npos);
            VERIFY_IS_TRUE(error.Message->find(L"wsl2.nestedVirtualization=true") != std::wstring::npos);
            VERIFY_IS_TRUE(error.Message->find(L"wsl2.nestedVirtualization=false") != std::wstring::npos);
            VERIFY_ARE_EQUAL(std::wstring{L"Wsl/OpenVmm/ERROR_NOT_SUPPORTED"}, wslutil::ErrorToString(error).Code);
        }
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
        const auto configure = [&](std::string_view Override, const std::function<void(const wsl::core::Config&)>& Inspect = {}) {
            TempFile file{GENERIC_WRITE, FILE_SHARE_READ, CREATE_ALWAYS};
            file.Handle.reset();
            WriteFileContent(file.Path, std::string{Override} + std::string{validConfig});
            return OperationResult([&] {
                wsl::core::Config config{file.Path.c_str(), token.get()};
                if (config.EnableOpenVmm)
                {
                    config.ValidateOpenVmmSupport();
                }
                if (Inspect)
                {
                    Inspect(config);
                }
            });
        };

        wsl::shared::string::MacAddress generatedMac{};
        VERIFY_ARE_EQUAL(S_OK, configure("", [&](const auto& Config) { generatedMac = Config.MacAddress; }));
        VERIFY_IS_TRUE(generatedMac != wsl::shared::string::MacAddress{});
        VERIFY_ARE_EQUAL(0, generatedMac[0] & 1);
        VERIFY_ARE_EQUAL(2, generatedMac[0] & 2);
        VERIFY_ARE_EQUAL(S_OK, configure("\n[wsl2]\nmacAddress=02:11:22:33:44:56\n", [&](const auto& Config) {
                             VERIFY_IS_TRUE((wsl::shared::string::MacAddress{0x02, 0x11, 0x22, 0x33, 0x44, 0x56}) == Config.MacAddress);
                         }));
        VERIFY_ARE_EQUAL(S_OK, configure("\n[wsl2]\nnetworkingMode=none\nfirewall=true\ndnsTunneling=true\nipv6=true\n"));
        VERIFY_ARE_EQUAL(S_OK, configure("\n[experimental]\nopenVmm=false\n[wsl2]\ngpuSupport=true\n"));
        bool openVmmEnabled = true;
        VERIFY_ARE_EQUAL(
            S_OK, configure("\n[wsl2]\nsafeMode=true\n", [&](const auto& Config) { openVmmEnabled = Config.EnableOpenVmm; }));
        VERIFY_IS_FALSE(openVmmEnabled);
        for (const auto* setting : {
                 "\n[wsl2]\ngpuSupport=true\n",
                 "\n[wsl2]\nguiApplications=true\n",
                 "\n[wsl2]\nnestedVirtualization=true\n",
                 "\n[wsl2]\nvirtio=false\n",
                 "\n[wsl2]\nvirtioFs=false\n",
                 "\n[wsl2]\nhardwarePerformanceCounters=true\n",
                 "\n[wsl2]\nnetworkingMode=mirrored\n",
                 "\n[wsl2]\nnetworkingMode=bridged\n",
                 "\n[wsl2]\nnetworkingMode=consomme\n",
                 "\n[wsl2]\nfirewall=true\n",
                 "\n[wsl2]\ndnsTunneling=true\n",
                 "\n[experimental]\nvirtioFsAggregateShares=true\n",
                 "\n[experimental]\nswiotlb=1M\n",
             })
        {
            VERIFY_ARE_EQUAL(c_notSupported, configure(setting));
        }

        // Parsing accepts a conflicting configuration, since libwsl reads and edits .wslconfig without creating a VM.
        {
            TempFile file{GENERIC_WRITE, FILE_SHARE_READ, CREATE_ALWAYS};
            file.Handle.reset();
            WriteFileContent(file.Path, std::string{"\n[wsl2]\nnetworkingMode=mirrored\n"} + std::string{validConfig});
            VERIFY_ARE_EQUAL(S_OK, OperationResult([&] { wsl::core::Config config{file.Path.c_str(), token.get()}; }));
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
        for (const auto feature : {&VmProcessorRequest::NestedVirtualization, &VmProcessorRequest::PerfmonPmu, &VmProcessorRequest::PerfmonLbr})
        {
            for (const auto value : {VmFeatureRequest::Preferred, VmFeatureRequest::Required})
            {
                auto request = CreateRequest();
                request.Processor.*feature = value;
                VERIFY_ARE_EQUAL(c_notSupported, DescribeResult(request));
            }
        }
        auto request = CreateRequest();
        VERIFY_ARE_EQUAL(S_OK, DescribeResult(request));
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
        VERIFY_ARE_EQUAL(request.Boot.KernelCommandLine, description.Boot.KernelCommandLine);
        VERIFY_ARE_EQUAL(size_t{1}, description.BootDisks.size());
        const auto& disk = description.BootDisks.at(L"arbitrary-key");
        VERIFY_IS_TRUE(IsEqualGUID(request.Identity.VmId, disk.Id.Owner.VmId));
        VERIFY_ARE_EQUAL(UINT64{1}, disk.Id.Value);
        VERIFY_ARE_EQUAL(UINT32{0}, GetSlot(disk));
        VERIFY_IS_FALSE(disk.ReadOnly);

        for (const auto* path : {L"C:\\images\\disk.img", L"C:\\images\\disk"})
        {
            request.BootDisks[0].Disk.Source = VmVirtualDiskSource{path};
            VERIFY_ARE_EQUAL(S_OK, DescribeResult(request));
        }

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

    TEST_METHOD(BootDiskAcceptsRequesterToken)
    {
        SKIP_TEST_ARM64();
        auto request = CreateRequest();
        request.BootDisks.push_back(CreateDisk(L"disk"));
        auto& token = request.BootDisks.front().Disk.UserToken;

        // The service passes a copy of the caller's token, which is never the VM owner's token instance.
        token = wil::shared_handle{GetNonElevatedToken(TokenImpersonation).release()};
        VERIFY_ARE_EQUAL(S_OK, DescribeResult(request));
        token.reset();
        VERIFY_ARE_EQUAL(S_OK, DescribeResult(request));
    }

    TEST_METHOD(RetainsOnlyCompletedCrashDumps)
    {
        SKIP_TEST_ARM64();
        GUID id{};
        THROW_IF_FAILED(CoCreateGuid(&id));
        const auto directory =
            std::filesystem::temp_directory_path() /
            (L"OpenVmmDumpTest-" + wsl::shared::string::GuidToString<wchar_t>(id, wsl::shared::string::GuidToStringFlags::None));
        THROW_IF_WIN32_BOOL_FALSE(CreateDirectoryW(directory.c_str(), nullptr));
        const auto oldDump = directory / L"saved-state-old.vmrs";
        const auto newDump = directory / L"saved-state-new.vmrs";
        const auto unrelated = directory / L"other.vmrs";
        const auto hcsDump = directory / L"saved-state-hcs.vmrs";
        auto cleanup = wil::scope_exit([&] {
            std::error_code error;
            std::filesystem::remove(oldDump, error);
            std::filesystem::remove(newDump, error);
            std::filesystem::remove(unrelated, error);
            std::filesystem::remove(hcsDump, error);
            std::filesystem::remove(directory, error);
        });
        std::ofstream{oldDump} << "old";
        std::ofstream{unrelated} << "unrelated";
        std::ofstream{hcsDump} << "hcs";
        THROW_IF_WIN32_BOOL_FALSE(SetFileAttributesW(hcsDump.c_str(), FILE_ATTRIBUTE_TEMPORARY));
        auto request = CreateRunnableRequest();
        request.CrashCapture.emplace();
        request.CrashCapture->SavedStateFolder = directory;
        request.CrashCapture->MaxSavedStateCount = 1;
        auto backend = OpenVmmVirtualMachineBackend::Create(request);
        VERIFY_IS_TRUE(std::filesystem::exists(oldDump));
        backend->Terminate();
        VERIFY_IS_TRUE(std::filesystem::exists(oldDump));

        const auto owner = GetNonElevatedToken(TokenImpersonation);
        wsl::windows::common::vm::openvmm::ApplyCrashDumpRetention(newDump, 1, owner.get());
        VERIFY_IS_TRUE(std::filesystem::exists(oldDump));

        std::ofstream{newDump};
        wsl::windows::common::vm::openvmm::ApplyCrashDumpRetention(newDump, 0, owner.get());
        VERIFY_IS_TRUE(std::filesystem::exists(oldDump));
        std::ofstream{newDump} << "new";
        const auto now = std::filesystem::file_time_type::clock::now();
        std::filesystem::last_write_time(oldDump, now - std::chrono::seconds{1});
        std::filesystem::last_write_time(newDump, now);
        wsl::windows::common::vm::openvmm::ApplyCrashDumpRetention(newDump, 2, owner.get());
        VERIFY_IS_TRUE(std::filesystem::exists(oldDump));
        wsl::windows::common::vm::openvmm::ApplyCrashDumpRetention(newDump, 1, owner.get());
        VERIFY_IS_FALSE(std::filesystem::exists(oldDump));
        VERIFY_IS_TRUE(std::filesystem::exists(newDump));
        VERIFY_IS_TRUE(std::filesystem::exists(unrelated));
        VERIFY_IS_TRUE(std::filesystem::exists(hcsDump));
        wsl::windows::common::vm::openvmm::ApplyCrashDumpRetention(newDump, 0, owner.get());
        VERIFY_IS_FALSE(std::filesystem::exists(newDump));
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

        auto second = CreateNetworkRequest();
        second.Tag = L"eth1";
        request.NetworkAdapters.push_back(second);
        VERIFY_ARE_EQUAL(size_t{2}, ValidateCreateRequest(request).NetworkAdapters.size());
        request.NetworkAdapters.back().Tag = L"eth0";
        VERIFY_ARE_EQUAL(E_INVALIDARG, DescribeResult(request));
        request.NetworkAdapters.back().Tag.clear();
        VERIFY_ARE_EQUAL(E_INVALIDARG, DescribeResult(request));
    }

    TEST_METHOD(OpenVmmNatUsesCreationTimeAdaptersAndStaticConfiguration)
    {
        SKIP_TEST_ARM64();
        constexpr wsl::shared::string::MacAddress clientMac{0x02, 0x15, 0x5d, 0x01, 0x02, 0x03};
        VERIFY_IS_TRUE(
            std::get<VmUserModeNatNetwork>(wsl::core::CreateOpenVmmNatNetworkAdapterRequest(true, clientMac).Configuration).InternalDns);
        const auto request = wsl::core::CreateOpenVmmNatNetworkAdapterRequest(false, clientMac);
        const auto& network = std::get<VmUserModeNatNetwork>(request.Configuration);
        VERIFY_IS_FALSE(network.InternalDns);
        VERIFY_ARE_EQUAL(std::wstring{L"eth0"}, request.Tag);
        VERIFY_IS_TRUE(network.ClientMacAddress() == clientMac);

        auto backendRequest = CreateRunnableRequest();
        backendRequest.NetworkAdapters.push_back(request);
        backendRequest.NetworkAdapters.push_back(wsl::core::CreateOpenVmmLoopbackNetworkAdapterRequest());
        auto backend = OpenVmmVirtualMachineBackend::Create(backendRequest);
        const auto description = backend->GetDescription();
        auto [client, server] = MakeSocketPair();
        wsl::core::OpenVmmNatNetworking networking(
            *backend,
            description.NetworkAdapters.at(L"eth0"),
            description.NetworkAdapters.at(L"loopback0"),
            wsl::core::GnsChannel(std::move(server)),
            true,
            false);
        LX_MINI_INIT_NETWORKING_CONFIGURATION configuration{};
        networking.FillInitialConfiguration(configuration);

        VERIFY_ARE_EQUAL(LxMiniInitNetworkingModeNat, configuration.NetworkingMode);
        VERIFY_IS_FALSE(configuration.DisableIpv6);
        VERIFY_IS_FALSE(configuration.EnableDhcpClient);
        VERIFY_ARE_EQUAL(LxMiniInitPortTrackerTypeMirrored, configuration.PortTrackerType);
        backend->Terminate();
    }

    TEST_METHOD(AllowsGuestAndSavedStateCrashCapture)
    {
        SKIP_TEST_ARM64();
        auto request = CreateRequest();
        request.CrashCapture = VmCrashCaptureRequest{L"C:\\crashes"};
        VERIFY_ARE_EQUAL(S_OK, DescribeResult(request));
        request.CrashCapture->SavedStateFolder = L"C:\\saved-state";
        VERIFY_ARE_EQUAL(S_OK, DescribeResult(request));
        request.CrashCapture->SavedStateFolder = std::filesystem::path{};
        VERIFY_ARE_EQUAL(E_INVALIDARG, DescribeResult(request));
    }

    TEST_METHOD(ValidatesExplicitBootDiskPlacements)
    {
        SKIP_TEST_ARM64();
        auto request = CreateRequest();
        request.BootDisks.push_back(CreateDisk(L"exact"));
        auto& disk = request.BootDisks[0].Disk;
        disk.Placement = VmDiskPlacement{VmVirtioBlkAddress{253}};
        VERIFY_ARE_EQUAL(UINT32{253}, GetSlot(ValidateCreateRequest(request).BootDisks.at(L"exact")));

        for (const UINT32 slot : {UINT32{254}, UINT32_MAX})
        {
            disk.Placement = VmDiskPlacement{VmVirtioBlkAddress{slot}};
            VERIFY_ARE_EQUAL(c_notSupported, DescribeResult(request));
        }

        // OpenVMM attaches disks over virtio-blk, so SCSI placements are rejected.
        for (const UINT32 lun : {UINT32{0}, UINT32{253}})
        {
            disk.Placement = VmDiskPlacement{VmScsiAddress{0, lun}};
            VERIFY_ARE_EQUAL(c_notSupported, DescribeResult(request));
        }

        disk.Placement = VmDiskPlacement{VmVirtioBlkAddress{253}};
        auto duplicate = CreateDisk(L"duplicate");
        duplicate.Disk.Placement = disk.Placement;
        request.BootDisks.push_back(std::move(duplicate));
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), DescribeResult(request));

        request.BootDisks = {CreateDisk(L"automatic-1"), CreateDisk(L"automatic-2"), CreateDisk(L"exact")};
        request.BootDisks[2].Disk.Placement = VmDiskPlacement{VmVirtioBlkAddress{0}};
        const auto description = ValidateCreateRequest(request);
        VERIFY_ARE_EQUAL(UINT32{1}, GetSlot(description.BootDisks.at(L"automatic-1")));
        VERIFY_ARE_EQUAL(UINT32{2}, GetSlot(description.BootDisks.at(L"automatic-2")));
        VERIFY_ARE_EQUAL(UINT32{0}, GetSlot(description.BootDisks.at(L"exact")));

        request.BootDisks.clear();
        for (UINT32 index = 0; index < 254; ++index)
        {
            request.BootDisks.push_back(CreateDisk(std::to_wstring(index)));
        }
        const auto first = ValidateCreateRequest(request);
        VERIFY_ARE_EQUAL(UINT32{253}, GetSlot(first.BootDisks.at(L"253")));
        THROW_IF_FAILED(CoCreateGuid(&request.Identity.VmId));
        const auto second = ValidateCreateRequest(request);
        VERIFY_ARE_EQUAL(first.BootDisks.at(L"0").Id.Value, second.BootDisks.at(L"0").Id.Value);
        VERIFY_IS_FALSE(IsEqualGUID(first.BootDisks.at(L"0").Id.Owner.VmId, second.BootDisks.at(L"0").Id.Owner.VmId));
        request.BootDisks.push_back(CreateDisk(L"overflow"));
        VERIFY_ARE_EQUAL(WSL_E_TOO_MANY_DISKS_ATTACHED, DescribeResult(request));
    }

    TEST_METHOD(ValidatesVirtioConsoleOrder)
    {
        SKIP_TEST_ARM64();
        auto request = CreateRequest();
        request.Consoles = {
            {VmConsoleRole::EarlyBoot, VmSerialConsole{0, L"\\\\.\\pipe\\early"}},
            {VmConsoleRole::KernelConsole, VmVirtioConsole{0, L"hvc0", L"\\\\.\\pipe\\console"}},
            {VmConsoleRole::DebugShell, VmVirtioConsole{1, L"hvc1", L"\\\\.\\pipe\\debug"}}};
        VERIFY_ARE_EQUAL(S_OK, DescribeResult(request));
        auto& console = std::get<VmVirtioConsole>(request.Consoles.back().Device);
        console.Port = 2;
        VERIFY_ARE_EQUAL(c_notSupported, DescribeResult(request));
        console.Port = 1;
        console.GuestName = L"hvc0";
        VERIFY_ARE_EQUAL(c_notSupported, DescribeResult(request));
    }

    TEST_METHOD(BootsAndTerminates)
    {
        SKIP_TEST_ARM64();
        VerifyBootsAndTerminates(OpenVmmVirtualMachineBackend::Create(CreateRunnableRequest()));
    }

    TEST_METHOD(NotifiesWhenGuestExits)
    {
        SKIP_TEST_ARM64();
        wil::unique_event callbackEvent{wil::EventOptions::ManualReset};
        std::atomic<int> notificationCount{0};
        HRESULT reentrantResult = E_FAIL;
        auto backend = OpenVmmVirtualMachineBackend::Create(CreateRunnableRequest());
        backend->RegisterTerminationCallback([&](GUID) {
            reentrantResult = OperationResult([&] { backend->Terminate(); });
            ++notificationCount;
            callbackEvent.SetEvent();
        });

        {
            auto guest = StartGuest(*backend);
        }

        VERIFY_IS_TRUE(callbackEvent.wait(30 * 1000));
        VERIFY_IS_TRUE(backend->GetTerminationEvent().is_signaled());
        VERIFY_ARE_EQUAL(S_OK, reentrantResult);
        VERIFY_ARE_EQUAL(1, notificationCount.load());
        VERIFY_ARE_EQUAL(VmState::Stopped, backend->GetState());
        VERIFY_ARE_EQUAL(VmTerminationReason::Shutdown, backend->GetTerminationReason().Reason);
    }

    TEST_METHOD(TerminatesConcurrently)
    {
        SKIP_TEST_ARM64();
        for (const bool start : {false, true})
        {
            std::atomic<int> notificationCount{0};
            wil::unique_event callbackEvent{wil::EventOptions::ManualReset};
            HRESULT reentrantResult = E_FAIL;
            bool resourcesRetained = false;
            auto backend = OpenVmmVirtualMachineBackend::Create(CreateRunnableRequest());
            std::optional<decltype(StartGuest(*backend))> guest;
            if (start)
            {
                guest.emplace(StartGuest(*backend));
            }

            const auto device = backend->CreateFileSystemDevice({VmVirtioFsDevice{L"shutdown-test", VmVirtioFsLayout::SingleShare}});
            const auto deviceId = device.Id.Value;
            backend->RegisterTerminationCallback([&, deviceId](GUID) {
                reentrantResult = OperationResult([&] { backend->Terminate(); });
                resourcesRetained = backend->GetFileSystemDevice([&](const auto& entry) { return entry.Id.Value == deviceId; }).has_value();
                ++notificationCount;
                callbackEvent.SetEvent();
            });
            wil::unique_event terminate{wil::EventOptions::ManualReset};
            std::vector<std::future<void>> terminations;
            auto releaseOnExit = wil::scope_exit([&] { terminate.SetEvent(); });
            for (int index = 0; index < 8; ++index)
            {
                terminations.emplace_back(std::async(std::launch::async, [&] {
                    terminate.wait();
                    backend->Terminate();
                }));
            }
            terminate.SetEvent();
            for (auto& termination : terminations)
            {
                termination.get();
            }
            VERIFY_IS_TRUE(callbackEvent.wait(30 * 1000));
            VERIFY_ARE_EQUAL(S_OK, reentrantResult);
            VERIFY_IS_TRUE(resourcesRetained);
            VERIFY_IS_TRUE(backend->GetTerminationEvent().is_signaled());
            VERIFY_ARE_EQUAL(VmState::Stopped, backend->GetState());
            VERIFY_ARE_EQUAL(1, notificationCount.load());
            VERIFY_ARE_EQUAL(S_OK, OperationResult([&] { backend->Terminate(); }));
            VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), OperationResult([&] { backend->Start(); }));
        }
    }

    TEST_METHOD(BootsWithVirtioBlkDiskAndReattachesIt)
    {
        SKIP_TEST_ARM64();
        using wsl::windows::common::filesystem::TempFile;
        TempFile file{GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, CREATE_ALWAYS};
        LARGE_INTEGER size{};
        size.QuadPart = 2 * c_mib;
        THROW_IF_WIN32_BOOL_FALSE(SetFilePointerEx(file.Handle.get(), size, nullptr, FILE_BEGIN));
        THROW_IF_WIN32_BOOL_FALSE(SetEndOfFile(file.Handle.get()));
        file.Handle.reset();

        auto request = CreateRunnableRequest();
        auto disk = CreateDisk(L"boot-disk");
        disk.Disk.Source = VmVirtualDiskSource{file.Path};
        disk.Disk.Placement = VmDiskPlacement{VmVirtioBlkAddress{7}};
        request.BootDisks.push_back(disk);
        auto backend = OpenVmmVirtualMachineBackend::Create(request);
        const auto bootDisk = backend->GetDescription().BootDisks.at(disk.Key);
        VERIFY_ARE_EQUAL(VmDiskTransport::VirtioBlk, GetDiskTransport(bootDisk.GuestAddress));
        VERIFY_ARE_EQUAL(UINT32{7}, GetSlot(bootDisk));
        VERIFY_ARE_EQUAL(size_t{1}, backend->GetAttachedDisks().size());

        auto guest = StartGuest(*backend);
        backend->DetachDisk(bootDisk.Id);
        VERIFY_IS_TRUE(backend->GetAttachedDisks().empty());
        const auto reattached = backend->AttachDisk(disk.Disk);
        VERIFY_ARE_EQUAL(VmDiskTransport::VirtioBlk, GetDiskTransport(reattached.GuestAddress));
        VERIFY_ARE_EQUAL(GetSlot(bootDisk), GetSlot(reattached));
        backend->DetachDisk(reattached.Id);
        VERIFY_IS_TRUE(backend->GetAttachedDisks().empty());
        backend->Terminate();
    }

    TEST_METHOD(DestructionDrainsTerminationCallback)
    {
        SKIP_TEST_ARM64();
        wil::unique_event callbackStarted{wil::EventOptions::ManualReset};
        wil::unique_event releaseCallback{wil::EventOptions::ManualReset};
        wil::unique_event destroyed{wil::EventOptions::ManualReset};
        auto backend = OpenVmmVirtualMachineBackend::Create(CreateRunnableRequest());
        std::thread destructionThread;
        auto cleanup = wil::scope_exit([&] {
            releaseCallback.SetEvent();
            if (destructionThread.joinable())
            {
                destructionThread.join();
            }
        });

        backend->RegisterTerminationCallback([&](GUID) {
            callbackStarted.SetEvent();
            releaseCallback.wait();
        });

        {
            auto guest = StartGuest(*backend);
        }

        VERIFY_IS_TRUE(callbackStarted.wait(30 * 1000));
        destructionThread = std::thread([backend = std::move(backend), &destroyed]() mutable {
            backend.reset();
            destroyed.SetEvent();
        });

        VERIFY_IS_FALSE(destroyed.wait(100));
        releaseCallback.SetEvent();
        VERIFY_IS_TRUE(destroyed.wait(30 * 1000));
    }

    TEST_METHOD(AddsPersistentMemoryInGuestOrder)
    {
        SKIP_TEST_ARM64();
        using wsl::windows::common::filesystem::TempFile;
        std::vector<TempFile> files;
        for (int index = 0; index < 2; ++index)
        {
            auto& file = files.emplace_back(GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, CREATE_ALWAYS);
            LARGE_INTEGER size{};
            size.QuadPart = 2 * c_mib;
            THROW_IF_WIN32_BOOL_FALSE(SetFilePointerEx(file.Handle.get(), size, nullptr, FILE_BEGIN));
            THROW_IF_WIN32_BOOL_FALSE(SetEndOfFile(file.Handle.get()));
            file.Handle.reset();
        }

        const auto request = CreateRunnableRequest();
        auto backend = OpenVmmVirtualMachineBackend::Create(request);
        auto guest = StartGuest(*backend);

        VmPersistentMemoryRequest firstRequest{};
        firstRequest.Path = files[0].Path;
        firstRequest.ReadOnly = true;
        const auto first = backend->AddPersistentMemory(firstRequest);

        VmPersistentMemoryRequest secondRequest{};
        secondRequest.Path = files[1].Path;
        secondRequest.ReadOnly = false;
        const auto second = backend->AddPersistentMemory(secondRequest);

        VERIFY_ARE_EQUAL(UINT32{0}, first.Index);
        VERIFY_ARE_EQUAL(UINT32{1}, second.Index);
        VERIFY_IS_TRUE(IsEqualGUID(request.Identity.VmId, first.Id.Owner.VmId));
        VERIFY_IS_TRUE(IsEqualGUID(request.Identity.VmId, second.Id.Owner.VmId));
        VERIFY_IS_TRUE(first.ReadOnly);
        VERIFY_IS_FALSE(second.ReadOnly);
        backend->Terminate();
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
        VERIFY_IS_TRUE(IsEqualGUID(request.Identity.VmId, fileSystemDevice.Id.Owner.VmId));
        VERIFY_ARE_EQUAL(VmFileSystemDeviceState::Prepared, fileSystemDevice.State);
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
        VERIFY_ARE_EQUAL(VmFileSystemDeviceState::Serving, servingDevice.State);
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
        const auto replacementShare = backend->AddFileSystemShare(fileSystemDevice.Id, shareRequest);
        VERIFY_ARE_NOT_EQUAL(share.Id.Value, replacementShare.Id.Value);
        backend->RemoveFileSystemShare(replacementShare.Id);

        const auto network = backend->GetDescription().NetworkAdapters.at(networkRequest.Tag);
        VERIFY_ARE_NOT_EQUAL(network.Id.Value, fileSystemDevice.Id.Value);
        VERIFY_ARE_EQUAL(c_notSupported, OperationResult([&] { backend->AddNetworkAdapter(networkRequest); }));

        // Consomme processes port requests once the guest has initialized its network queues.
        auto guest = StartGuest(*backend);
        VmPortBindingRequest bindingRequest;
        bindingRequest.ListenAddress.family = IpAddressFamily_V4;
        const auto loopbackAddress = htonl(INADDR_LOOPBACK);
        std::memcpy(bindingRequest.ListenAddress.bytes, &loopbackAddress, sizeof(loopbackAddress));
        bindingRequest.HostPort = ReservePort();
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

    TEST_METHOD(EnforcesDiskOwnership)
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
        VmDiskRequest disk{};
        disk.Source = VmVirtualDiskSource{file.Path};

        // The service passes a copy of the caller's token, which is never the VM owner's token instance.
        disk.UserToken = wil::shared_handle{GetNonElevatedToken(TokenImpersonation).release()};
        const auto managed = backend->AttachDisk(disk);
        VERIFY_ARE_EQUAL(managed.Id.Value, backend->AttachDisk(disk).Id.Value);
        disk.UserToken.reset();

        disk.UserDisk = true;
        for (const bool readOnly : {false, true})
        {
            disk.ReadOnly = readOnly;
            disk.Placement = VmDiskPlacement{VmVirtioBlkAddress{3}};
            VERIFY_ARE_EQUAL(WSL_E_DISK_ALREADY_ATTACHED, OperationResult([&] { backend->AttachDisk(disk); }));
        }
        backend->DetachDisk(managed.Id);
        disk.Placement.reset();
        const auto userDisk = backend->AttachDisk(disk);
        disk.UserDisk = false;
        for (const bool readOnly : {false, true})
        {
            disk.ReadOnly = readOnly;
            disk.Placement = VmDiskPlacement{VmVirtioBlkAddress{3}};
            VERIFY_ARE_EQUAL(WSL_E_USER_VHD_ALREADY_ATTACHED, OperationResult([&] { backend->AttachDisk(disk); }));
        }
        disk.Placement.reset();
        VERIFY_ARE_EQUAL(WSL_E_USER_VHD_ALREADY_ATTACHED, OperationResult([&] { backend->AttachDisk(disk); }));
        backend->DetachDisk(userDisk.Id);

        VmPersistentMemoryRequest memory{};
        memory.Path = file.Path;
        memory.UserToken = wil::shared_handle{GetNonElevatedToken(TokenImpersonation).release()};
        const auto device = backend->AddPersistentMemory(memory);
        VERIFY_ARE_EQUAL(file.Path.wstring(), device.EffectiveHostPath.wstring());
        backend->Terminate();
    }

    TEST_METHOD(ForwardsOnlySupportedLocalhostBindings)
    {
        SKIP_TEST_ARM64();
        auto request = CreateRunnableRequest();
        request.NetworkAdapters.push_back(wsl::core::CreateOpenVmmNatNetworkAdapterRequest(false, {0x02, 0x15, 0x5d, 0x01, 0x02, 0x03}));
        request.NetworkAdapters.push_back(wsl::core::CreateOpenVmmLoopbackNetworkAdapterRequest());
        auto backend = OpenVmmVirtualMachineBackend::Create(request);
        auto guest = StartGuest(*backend);
        const auto& adapters = backend->GetDescription().NetworkAdapters;
        struct PortCase
        {
            int Family;
            const char* Address;
            bool Forwarded;
        };
        constexpr PortCase cases[] = {
            {AF_INET, "0.0.0.0", true},
            {AF_INET, "127.0.0.1", true},
            {AF_INET6, "::ffff:0.0.0.0", true},
            {AF_INET6, "::ffff:127.0.0.1", true},
            {AF_INET6, "::", false},
            {AF_INET6, "::1", false},
            {AF_INET, "127.0.0.2", false},
            {AF_INET, "10.0.0.2", false},
            {AF_INET, "192.0.2.1", false},
            {AF_INET6, "::ffff:192.0.2.1", false},
            {AF_INET6, "2001:db8::1", false},
        };
        for (const bool enabled : {false, true})
        {
            wsl::core::OpenVmmNatNetworking networking{
                *backend,
                adapters.at(L"eth0"),
                enabled ? std::optional{adapters.at(L"loopback0")} : std::nullopt,
                wsl::core::GnsChannel{wil::unique_socket{}},
                enabled,
                false};
            auto [client, server] = MakeSocketPair();
            networking.StartPortTracker(std::move(server));
            wsl::shared::SocketChannel channel{std::move(client), "PortTrackerTest"};
            for (const int protocol : {IPPROTO_TCP, IPPROTO_UDP})
            {
                for (const auto& test : cases)
                {
                    LX_GNS_PORT_ALLOCATION_REQUEST notification{};
                    notification.Header.MessageType = LxGnsMessagePortMappingRequest;
                    notification.Header.MessageSize = sizeof(notification);
                    notification.Protocol = protocol;
                    notification.Port = ReservePort(protocol);
                    notification.Af = test.Family;
                    THROW_HR_IF(E_FAIL, inet_pton(test.Family, test.Address, notification.Address32) != 1);
                    notification.Allocate = true;
                    VERIFY_ARE_EQUAL(0, channel.Transaction(notification).Result);
                    VERIFY_ARE_EQUAL(!(enabled && test.Forwarded), CanBindPort(notification.Port, INADDR_LOOPBACK, protocol));
                    VERIFY_IS_TRUE(CanBindPort(notification.Port, INADDR_LOOPBACK + 1, protocol));
                    if (enabled && test.Family == AF_INET6 && test.Forwarded)
                    {
                        notification.Af = AF_INET;
                        notification.Address32[0] = notification.Address32[3];
                        VERIFY_ARE_NOT_EQUAL(0, channel.Transaction(notification).Result);
                    }
                    notification.Allocate = false;
                    VERIFY_ARE_EQUAL(0, channel.Transaction(notification).Result);
                    VERIFY_IS_TRUE(CanBindPort(notification.Port, INADDR_LOOPBACK, protocol));
                }
            }
        }

        backend->Terminate();
    }

    TEST_METHOD(RefreshesDnsOnInitialConnectivityNotification)
    {
        SKIP_TEST_ARM64();
        auto request = CreateRunnableRequest();
        request.NetworkAdapters.push_back(wsl::core::CreateOpenVmmNatNetworkAdapterRequest(false, {0x02, 0x15, 0x5d, 0x01, 0x02, 0x03}));
        auto backend = OpenVmmVirtualMachineBackend::Create(request);
        auto guest = StartGuest(*backend);
        const auto description = backend->GetDescription();
        const auto& adapter = description.NetworkAdapters.at(L"eth0");
        wsl::core::networking::DnsInfo dns{{"127.0.0.1", "127.0.0.2", "::1", "::ffff:127.0.0.1", "1.1.1.1"}, {}};
        wsl::core::MapHostLoopbackDnsServers(dns, *backend, adapter.Id);
        VERIFY_ARE_NOT_EQUAL(std::string{"127.0.0.1"}, dns.Servers[0]);
        VERIFY_ARE_NOT_EQUAL(dns.Servers[0], dns.Servers[1]);
        VERIFY_IS_TRUE(dns.Servers[2].ends_with("%eth0"));
        VERIFY_ARE_EQUAL(dns.Servers[0], dns.Servers[3]);
        VERIFY_ARE_EQUAL(std::string{"1.1.1.1"}, dns.Servers[4]);
        auto [host, guestSocket] = MakeSocketPair();
        wil::unique_event refreshed{wil::EventOptions::ManualReset};
        HRESULT receiverResult = E_FAIL;
        std::vector<LX_MESSAGE_TYPE> messages;
        std::thread receiver([&] {
            receiverResult = wil::ResultFromException([&] {
                wsl::shared::SocketChannel channel{std::move(guestSocket), "DnsRefreshTest"};
                for (int index = 0; index < 4; ++index)
                {
                    auto transaction = channel.ReceiveTransaction(30000);
                    messages.push_back(transaction.Receive<MESSAGE_HEADER>().MessageType);
                    std::vector<gsl::byte> buffer(sizeof(LX_GNS_RESULT));
                    const auto response = gslhelpers::get_struct<LX_GNS_RESULT>(gsl::make_span(buffer));
                    response->Header.MessageType = LxGnsMessageResult;
                    response->Header.MessageSize = static_cast<ULONG>(buffer.size());
                    transaction.Send<LX_GNS_RESULT>(gsl::make_span(buffer));
                }
                refreshed.SetEvent();
            });
        });
        auto join = wil::scope_exit([&] { receiver.join(); });
        {
            wsl::core::OpenVmmNatNetworking networking{*backend, adapter, std::nullopt, wsl::core::GnsChannel{std::move(host)}, false, false};
            networking.Initialize();
            VERIFY_IS_TRUE(refreshed.wait(30000));
        }
        receiver.join();
        join.release();
        VERIFY_ARE_EQUAL(S_OK, receiverResult);
        VERIFY_ARE_EQUAL(size_t{4}, messages.size());
        VERIFY_ARE_EQUAL(LxGnsMessageInterfaceConfiguration, messages.at(0));
        VERIFY_ARE_EQUAL(LxGnsMessageNotification, messages.at(1));
        VERIFY_ARE_EQUAL(LxGnsMessageNotification, messages.at(2));
        VERIFY_ARE_EQUAL(LxGnsMessageNotification, messages.at(3));
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

    TEST_METHOD(ManagesHostSocketLifetime)
    {
        SKIP_TEST_ARM64();
        const auto exists = [](const std::filesystem::path& Path) {
            return GetFileAttributesW(Path.c_str()) != INVALID_FILE_ATTRIBUTES;
        };

        auto backend = OpenVmmVirtualMachineBackend::Create(CreateRunnableRequest());
        const auto socket = backend->CreateHostSocket({50000});
        VERIFY_IS_TRUE(socket.has_value());
        VERIFY_IS_TRUE(exists(socket->Path));
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), OperationResult([&] { backend->CreateHostSocket(socket->Port); }));
        VERIFY_IS_TRUE(exists(socket->Path));

        auto foreignSocket = socket->Id;
        THROW_IF_FAILED(CoCreateGuid(&foreignSocket.Owner.VmId));
        VERIFY_ARE_EQUAL(E_INVALIDARG, OperationResult([&] { backend->CloseHostSocket(foreignSocket); }));
        backend->CloseHostSocket(socket->Id);
        VERIFY_IS_FALSE(exists(socket->Path));
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), OperationResult([&] { backend->CloseHostSocket(socket->Id); }));

        const auto replacement = backend->CreateHostSocket(socket->Port);
        VERIFY_IS_TRUE(replacement.has_value());
        VERIFY_ARE_NOT_EQUAL(socket->Id.Value, replacement->Id.Value);

        // Host socket files outlive Terminate and are deleted when the backend is destroyed.
        backend->Terminate();
        backend.reset();
        VERIFY_IS_FALSE(exists(replacement->Path));
    }

    TEST_METHOD(ServesFileSystemShareUnderUnelevatedToken)
    {
        SKIP_TEST_ARM64();
        // A VM created by an elevated session serves an unelevated session's share under that session's token.
        auto request = CreateRunnableRequest();
        request.Identity.UserToken = wil::shared_handle{GetElevatedTestToken().release()};
        GUID directoryId{};
        THROW_IF_FAILED(CoCreateGuid(&directoryId));
        const auto sharePath =
            wsl::windows::common::filesystem::GetTempFolderPath(GetCurrentProcessToken()) /
            (L"OpenVmmBackendTest-" + wsl::shared::string::GuidToString<wchar_t>(directoryId, wsl::shared::string::GuidToStringFlags::None));
        THROW_IF_WIN32_BOOL_FALSE(CreateDirectoryW(sharePath.c_str(), nullptr));
        auto removeShareDirectory = wil::scope_exit([&] { LOG_IF_WIN32_BOOL_FALSE(RemoveDirectoryW(sharePath.c_str())); });

        auto backend = OpenVmmVirtualMachineBackend::Create(request);
        VmFileSystemDeviceRequest deviceRequest{VmVirtioFsDevice{L"unelevated-share", VmVirtioFsLayout::SingleShare}};
        deviceRequest.UserToken = wil::shared_handle{GetNonElevatedToken(TokenImpersonation).release()};
        const auto device = backend->CreateFileSystemDevice(deviceRequest);
        VERIFY_IS_FALSE(device.Elevated);

        VmFileSystemShareRequest shareRequest;
        shareRequest.HostPath = sharePath;
        shareRequest.Options = VmVirtioFsShareOptions{};
        const auto share = backend->AddFileSystemShare(device.Id, shareRequest);
        VERIFY_IS_FALSE(share.Elevated);
        VERIFY_ARE_EQUAL(VmFileSystemDeviceState::Serving, backend->GetFileSystemDeviceStatus(device.Id).State);
        backend->RemoveFileSystemShare(share.Id);
        backend->Terminate();
    }

    TEST_METHOD(CapabilitiesReflectVmServiceProtocol)
    {
        const auto capabilities = OpenVmmVirtualMachineBackend::QueryCapabilities();
        VERIFY_ARE_EQUAL(BackendKind::OpenVmm, capabilities.Backend);
        VERIFY_ARE_EQUAL(VmDiskTransport::VirtioBlk, capabilities.DiskTransport);

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

// Runs in the HCS WSL2 pass of every image, including images without the Windows Hypervisor Platform, to verify that
// OpenVMM either boots or reports that no hypervisor is available instead of failing with an unrelated error.
class OpenVmmHypervisorAvailabilityTests
{
    WSL_TEST_CLASS(OpenVmmHypervisorAvailabilityTests)

    TEST_METHOD(BootsOrReportsMissingHypervisor)
    {
        SKIP_TEST_ARM64();
        if (!LxsstuVmMode())
        {
            LogSkipped("OpenVMM requires WSL2");
            return;
        }
        using namespace wsl::windows::common;

        std::optional<bool> whpInstalled;
        try
        {
            whpInstalled = WslInstall::IsOptionalComponentInstalled(L"HypervisorPlatform");
        }
        CATCH_LOG()

        ExecutionContext context(Context::Wsl);
        std::unique_ptr<OpenVmmVirtualMachineBackend> backend;
        const auto result = wil::ResultFromException([&] {
            backend = OpenVmmVirtualMachineBackend::Create(CreateRunnableRequest());
            auto guest = StartGuest(*backend);
        });

        const auto error = context.ReportedError();
        LogInfo(
            "HypervisorPlatform installed: %ls, OpenVMM boot result: 0x%08x, error: %ls, message: %ls",
            whpInstalled ? (*whpInstalled ? L"yes" : L"no") : L"unknown",
            result,
            error ? wslutil::ErrorToString(*error).Code.c_str() : L"none",
            error && error->Message ? error->Message->c_str() : L"none");

        if (backend)
        {
            backend->Terminate();
        }

        if (FAILED(result))
        {
            VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_HV_NOT_PRESENT), result);
        }
    }
};

} // namespace OpenVmmVirtualMachineBackendTests
