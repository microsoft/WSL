// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "windows/Common.h"
#include "TestImageRegistry.h"
#include "WSLCE2EComposeHelpers.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"

namespace WSLCE2ETests {

using wsl::shared::string::WideToMultiByte;

class WSLCE2EComposeRemoveTests
{
    WSLC_TEST_CLASS(WSLCE2EComposeRemoveTests)

    TEST_CLASS_SETUP(ClassSetup)
    {
        TestImageRegistry::Instance().EnsureLoaded(AlpineTestImage());
        Cleanup();
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        Cleanup();
        return true;
    }

    WSLC_TEST_METHOD(WSLCE2E_Compose_Remove_AliasesAndRunningContainers)
    {
        const auto projectDirectory = std::filesystem::current_path() / ProjectName;
        auto directoryCleanup = SetupTestDirectory(projectDirectory);
        auto resourceCleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { Cleanup(); });
        const auto composePath = projectDirectory / L"compose.yaml";

        WriteTestFileContent(
            composePath,
            std::format(
                R"(
services:
  owned:
    name: {}
    image: {}
    command: ["/bin/sh", "-c", "while true; do sleep 1; done"]
)",
                WideToMultiByte(ContainerName),
                WideToMultiByte(AlpineTestImage().NameAndTag())));

        auto result = RunWslc(std::format(L"compose create \"{}\"", composePath.wstring()));
        VERIFY_ARE_EQUAL(0u, result.ExitCode.value_or(1));

        result = RunWslc(std::format(L"compose start \"{}\"", composePath.wstring()));
        VERIFY_ARE_EQUAL(0u, result.ExitCode.value_or(1));

        result = RunWslc(std::format(L"compose remove \"{}\"", composePath.wstring()));
        result.Verify({.Stderr = wsl::shared::Localization::WSLCCLI_ComposeNoStoppedContainers() + L"\r\n", .ExitCode = 0});
        VerifyComposeProjectStatus(ProjectName, L"running(1)");

        result = RunWslc(std::format(L"compose stop \"{}\"", composePath.wstring()));
        VERIFY_ARE_EQUAL(0u, result.ExitCode.value_or(1));

        result = RunWslc(std::format(L"compose delete \"{}\"", composePath.wstring()));
        VERIFY_ARE_EQUAL(0u, result.ExitCode.value_or(1));
        VerifyComposeProjectAbsent(ProjectName);

        result = RunWslc(std::format(L"compose create \"{}\"", composePath.wstring()));
        VERIFY_ARE_EQUAL(0u, result.ExitCode.value_or(1));
        VerifyComposeProjectStatus(ProjectName, L"created(1)");

        VERIFY_IS_TRUE(std::filesystem::remove_all(projectDirectory) > 0);

        result = RunWslc(std::format(L"compose rm {}", ProjectArgument));
        VERIFY_ARE_EQUAL(0u, result.ExitCode.value_or(1));
        VerifyComposeProjectAbsent(ProjectName);
    }

private:
    static void Cleanup()
    {
        EnsureContainerDoesNotExist(ContainerName);
        EnsureNetworkDoesNotExist(ProjectName + L"_default");
    }

    inline static const std::wstring ProjectName = L"wslc-e2e-compose-remove";
    inline static const std::wstring ProjectArgument = L"WSLC-E2E-COMPOSE-REMOVE";
    inline static const std::wstring ContainerName = L"wslc-compose-remove-owned";
};

} // namespace WSLCE2ETests
