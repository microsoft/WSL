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
    }

    TEST_METHOD(ValidatesConsoleFamiliesIndependently)
    {
        SKIP_TEST_ARM64();
        auto request = CreateRequest();
        request.Consoles = {
            {VmConsoleRole::EarlyBoot, VmSerialConsole{0, L"\\\\.\\pipe\\early"}},
            {VmConsoleRole::KernelConsole, VmVirtioConsole{0, L"", L"\\\\.\\pipe\\console"}}};
        VERIFY_ARE_EQUAL(size_t{2}, ValidateCreateRequest(request).Boot.Consoles.size());
        std::get<VmVirtioConsole>(request.Consoles[1].Device).GuestName = L"unsupported-name";
        VERIFY_ARE_EQUAL(c_notSupported, DescribeResult(request));
    }

    TEST_METHOD(RejectsUnsupportedBootMethod)
    {
        SKIP_TEST_ARM64();
        auto request = CreateRequest();
        request.Boot.Method = VmBootMethod::Uefi;
        VERIFY_ARE_EQUAL(c_notSupported, DescribeResult(request));
    }

    TEST_METHOD(BootsAndTerminates)
    {
        SKIP_TEST_ARM64();
        auto request = CreateRequest();
        const auto basePath = wsl::windows::common::wslutil::GetBasePath();
        request.Boot.KernelPath = basePath / L"kernel";
        request.Boot.InitrdPath = basePath / LXSS_VM_MODE_INITRD_NAME;
        request.Boot.GuestCommandLine = L"panic=-1";

        auto backend = OpenVmmVirtualMachineBackend::Create(request);
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

}