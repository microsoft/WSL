// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "Common.h"
#include "hcs.hpp"

using namespace wsl::windows::common;
using namespace wsl::shared::string;

class VmBackendTests
{
    WSL_TEST_CLASS(VmBackendTests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        VERIFY_IS_TRUE(LxsstuInitialize(FALSE));
        return true;
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        LxsstuUninitialize(FALSE);
        return true;
    }

    static std::wstring BackendConfig(bool OpenVmm)
    {
        return LxssGenerateTestConfig({.vmIdleTimeout = -1, .guiApplications = false}) +
               std::format(L"\n[experimental]\nopenVmm={}\n", OpenVmm ? L"true" : L"false");
    }

    static std::wstring ReadVmId()
    {
        auto [output, warnings] = LxsstuLaunchWslAndCaptureOutput(L"--exec wslinfo --vm-id -n", 0);
        VERIFY_ARE_EQUAL(warnings, L"");
        const auto id = ToGuid(output);
        VERIFY_IS_TRUE(id.has_value());
        VERIFY_IS_FALSE(IsEqualGUID(id.value(), GUID_NULL));
        return GuidToString<wchar_t>(id.value(), GuidToStringFlags::None);
    }

    static std::filesystem::path RpcDirectory(const std::wstring& VmId)
    {
        const auto token = security::GetUserToken(TokenImpersonation);
        return filesystem::GetTempFolderPath(token.get()) / std::format(L"wsl-{}.rpc", VmId);
    }

    static void VerifyBackend(const std::wstring& VmId, bool OpenVmm)
    {
        hcs::unique_hcs_system system;
        const auto result = HcsOpenComputeSystem(VmId.c_str(), GENERIC_READ, system.put());
        VERIFY_ARE_EQUAL(result, OpenVmm ? HCS_E_SYSTEM_NOT_FOUND : S_OK);
        // Correlate the OpenVMM RPC endpoint with the guest's VM ID, not an unrelated host process.
        VERIFY_ARE_EQUAL(std::filesystem::exists(RpcDirectory(VmId) / L"s"), OpenVmm);
    }

    static void VerifyStopped(const std::wstring& VmId)
    {
        hcs::unique_hcs_system system;
        VERIFY_ARE_EQUAL(HcsOpenComputeSystem(VmId.c_str(), GENERIC_READ, system.put()), HCS_E_SYSTEM_NOT_FOUND);
        VERIFY_IS_FALSE(std::filesystem::exists(RpcDirectory(VmId)));
    }

    TEST_METHOD(SelectionConfigParsing)
    {
        GUID id{};
        VERIFY_SUCCEEDED(CoCreateGuid(&id));
        const auto path = std::filesystem::temp_directory_path() / (GuidToString<wchar_t>(id) + L".wslconfig");
        HostFileChange file(path, "");
        for (const auto& [contents, expected] :
             {std::pair{"", false},
              {"[experimental]\nopenVmm=false\n", false},
              {"[experimental]\nopenVmm=true\n", true},
              {"[experimental]\nopenVmm=NotABoolean\n", false}})
        {
            file.Update(contents);
            const wsl::core::Config config(path.c_str());
            VERIFY_ARE_EQUAL(config.EnableOpenVmm, expected);
        }
    }

    WSL2_TEST_METHOD(HcsDefaultAndExplicitSelection)
    {
        WslConfigChange config(LxssGenerateTestConfig({.vmIdleTimeout = -1, .guiApplications = false}));
        VERIFY_IS_TRUE(WslShutdown());
        for (const bool force : {false, true})
        {
            const auto id = ReadVmId();
            VerifyBackend(id, false);
            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(force ? L"--shutdown --force" : L"--shutdown"), 0u);
            VerifyStopped(id);
            config.Update(BackendConfig(false));
        }
    }

    WSL2_TEST_METHOD(OpenVmmSelectionAndRollback)
    {
        WslConfigChange config(BackendConfig(true));
        VERIFY_IS_TRUE(WslShutdown());
#if WSL_INCLUDE_OPENVMM
        for (const bool force : {false, true})
        {
            LxssWriteWslConfig(BackendConfig(true));
            const auto openVmmId = ReadVmId();
            VerifyBackend(openVmmId, true);

            // Do not use config.Update(): restarting the service would hide incorrect live-VM dispatch.
            LxssWriteWslConfig(BackendConfig(false));
            VERIFY_ARE_EQUAL(ReadVmId(), openVmmId);
            VerifyBackend(openVmmId, true);
            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(force ? L"--shutdown --force" : L"--shutdown"), 0u);
            VerifyStopped(openVmmId);

            const auto hcsId = ReadVmId();
            VERIFY_ARE_NOT_EQUAL(hcsId, openVmmId);
            VerifyBackend(hcsId, false);
            LxssWriteWslConfig(BackendConfig(true));
            VERIFY_ARE_EQUAL(ReadVmId(), hcsId);
            VerifyBackend(hcsId, false);
            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(force ? L"--shutdown --force" : L"--shutdown"), 0u);
            VerifyStopped(hcsId);
        }
#else
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            ValidateOutput(
                L"--exec true",
                FormatErrorMessage(
                    wsl::shared::Localization::MessageOpenVmmNotIncluded(), L"Wsl/Service/CreateInstance/CreateVm/E_NOTIMPL"));
        }

        LxssWriteWslConfig(BackendConfig(false));
        const auto id = ReadVmId();
        VerifyBackend(id, false);
        VERIFY_IS_TRUE(WslShutdown());
        VerifyStopped(id);
#endif
    }

#if WSL_INCLUDE_OPENVMM
    WSL2_TEST_METHOD(OpenVmmCreationFailureDoesNotFallback)
    {
        GUID id{};
        VERIFY_SUCCEEDED(CoCreateGuid(&id));
        const auto systemDistro = std::filesystem::temp_directory_path() / (GuidToString<wchar_t>(id) + L".img");
        HostFileChange image(systemDistro, std::string(4096, '\0'));
        WslConfigChange config(
            LxssGenerateTestConfig({.vmIdleTimeout = -1, .guiApplications = false, .systemDistro = systemDistro.wstring()}) +
            L"\n[experimental]\nopenVmm=true\n");
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            auto [output, warnings] = LxsstuLaunchWslAndCaptureOutput(L"--exec true", -1);
            // OpenVMM rejects .img before creating resources. HCS accepts the extension and would fail later.
            VERIFY_ARE_NOT_EQUAL(output.find(L"Wsl/Service/CreateInstance/CreateVm/WSL_E_CUSTOM_SYSTEM_DISTRO_ERROR"), std::wstring::npos);
            VERIFY_ARE_EQUAL(warnings, L"");
        }

        // Retry and rollback within the same service session, without masking state cleanup with a service restart.
        LxssWriteWslConfig(BackendConfig(true));
        const auto openVmmId = ReadVmId();
        VerifyBackend(openVmmId, true);
        VERIFY_IS_TRUE(WslShutdown());
        VerifyStopped(openVmmId);
        LxssWriteWslConfig(BackendConfig(false));
        const auto hcsId = ReadVmId();
        VerifyBackend(hcsId, false);
        VERIFY_IS_TRUE(WslShutdown());
        VerifyStopped(hcsId);
    }
#endif
};
