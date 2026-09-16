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
    request.OwnerName = L"Backend test";
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
        VERIFY_ARE_EQUAL(request.OwnerName, description.OwnerName);
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

    TEST_METHOD(ReservesExactPlacementsBeforeAutomaticAndPreferredDisks)
    {
        SKIP_TEST_ARM64();
        auto request = CreateRequest();
        request.BootDisks = {CreateDisk(L"preferred"), CreateDisk(L"automatic"), CreateDisk(L"exact")};
        request.BootDisks[0].Disk.Placement = VmScsiPlacement{{0, 0}, VmPlacementPolicy::Preferred};
        request.BootDisks[2].Disk.Placement = VmScsiPlacement{{0, 0}, VmPlacementPolicy::Exact};
        const auto description = ValidateCreateRequest(request);
        VERIFY_ARE_EQUAL(UINT32{1}, description.BootDisks.at(L"preferred").GuestAddress.Lun);
        VERIFY_ARE_EQUAL(UINT32{2}, description.BootDisks.at(L"automatic").GuestAddress.Lun);
        VERIFY_ARE_EQUAL(UINT32{0}, description.BootDisks.at(L"exact").GuestAddress.Lun);

        request.BootDisks[0].Disk.Placement->Policy = VmPlacementPolicy::Exact;
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
        request.Boot.Method = VmBootMethod::LinuxFirmware;
        VERIFY_ARE_EQUAL(c_notSupported, DescribeResult(request));
    }

    TEST_METHOD(CapabilitiesDoNotAdvertiseUnimplementedOperations)
    {
        const auto capabilities = OpenVmmVirtualMachineBackend::QueryCapabilities();
        VERIFY_ARE_EQUAL(BackendKind::OpenVmm, capabilities.Backend);
        VERIFY_IS_FALSE(capabilities.Operations.contains(VmOperation::Start));
        VERIFY_IS_FALSE(capabilities.Features.contains(VmFeature::UserModeNatNetwork));
        if constexpr (!wsl::shared::Arm64)
        {
            VERIFY_IS_TRUE(capabilities.Operations.at(VmOperation::Create).Supported);
            VERIFY_IS_FALSE(capabilities.Operations.at(VmOperation::Create).RequiredDeadline);
        }
    }
};

} // namespace OpenVmmVirtualMachineBackendTests
