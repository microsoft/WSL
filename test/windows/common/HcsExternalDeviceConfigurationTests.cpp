// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "Common.h"
#include "HcsVirtualMachineBackend.h"

namespace HcsExternalDeviceConfigurationTests {

class HcsExternalDeviceConfigurationTests
{
    WSL_TEST_CLASS(HcsExternalDeviceConfigurationTests)

    static nlohmann::json BuildSettings(const std::optional<HcsVirtualMachineBackend::ExternalDevice>& Device = std::nullopt)
    {
        VmCreateRequest request;
        request.Identity.VmId = {0x7f154d2e, 0x7d9f, 0x4f81, {0xa1, 0xdb, 0xb4, 0x6c, 0xe4, 0x1c, 0x6b, 0x42}};
        wil::unique_handle token;
        THROW_IF_WIN32_BOOL_FALSE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &token));
        wil::unique_handle impersonationToken;
        THROW_IF_WIN32_BOOL_FALSE(DuplicateTokenEx(
            token.get(), TOKEN_QUERY | TOKEN_IMPERSONATE, nullptr, SecurityImpersonation, TokenImpersonation, &impersonationToken));
        request.Identity.UserToken = wil::shared_handle{impersonationToken.release()};
        request.Owner = L"WSL configuration tests";
        request.Processor.Count = 1;
        request.Memory.SizeBytes = 256ull * 1024 * 1024;
        request.Boot.KernelPath = L"kernel";

        HcsVirtualMachineBackend backend;
        backend.m_configuration.Description.Identity = request.Identity;
        backend.m_configuration = backend.BuildConfiguration(request, Device);
        return nlohmann::json(backend.m_configuration.Settings);
    }

    static HcsVirtualMachineBackend::ExternalDevice Device()
    {
        return {
            {0x01234567, 0x89ab, 0x4cde, {0x80, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde}},
            {0xfedcba98, 0x7654, 0x4321, {0x80, 0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32}}};
    }

    TEST_METHOD(SerializesExternalDeviceInInitialConfiguration)
    {
        const auto settings = BuildSettings(Device());
        const auto& devices = settings.at("VirtualMachine").at("Devices").at("FlexibleIov");
        VERIFY_ARE_EQUAL(size_t{1}, devices.size());
        const auto& device = devices.at("01234567-89ab-4cde-8012-3456789abcde");
        VERIFY_IS_TRUE(device.at("EmulatorId") == "fedcba98-7654-4321-80fe-dcba98765432");
        VERIFY_IS_TRUE(device.at("HostingModel") == "External");
    }

    TEST_METHOD(EmptyOptionOmitsExternalDevices)
    {
        const auto settings = BuildSettings();
        VERIFY_IS_FALSE(settings.at("VirtualMachine").at("Devices").contains("FlexibleIov"));
        VERIFY_IS_TRUE(settings == BuildSettings(std::nullopt));
    }

    TEST_METHOD(ExternalDevicePreservesOtherInitialSettings)
    {
        auto withDevice = BuildSettings(Device());
        withDevice.at("VirtualMachine").at("Devices").erase("FlexibleIov");
        VERIFY_IS_TRUE(withDevice == BuildSettings());
    }

    TEST_METHOD(RejectsNullExternalDeviceIdentities)
    {
        const auto device = Device();
        for (const auto& invalid :
             {HcsVirtualMachineBackend::ExternalDevice{GUID_NULL, device.EmulatorId},
              HcsVirtualMachineBackend::ExternalDevice{device.InstanceId, GUID_NULL},
              HcsVirtualMachineBackend::ExternalDevice{GUID_NULL, GUID_NULL}})
        {
            VERIFY_ARE_EQUAL(E_INVALIDARG, wil::ResultFromException([&] { BuildSettings(invalid); }));
        }
    }
};

} // namespace HcsExternalDeviceConfigurationTests
