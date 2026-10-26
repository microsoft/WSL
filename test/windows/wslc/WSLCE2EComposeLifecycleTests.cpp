// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "Common.h"
#include "TestImageRegistry.h"
#include "WSLCE2EComposeHelpers.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"

namespace WSLCE2ETests {

using wsl::shared::string::WideToMultiByte;

class WSLCE2EComposeLifecycleTests
{
    WSLC_TEST_CLASS(WSLCE2EComposeLifecycleTests)

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

    WSLC_TEST_METHOD(WSLCE2E_Compose_Lifecycle_AcceptsFileAndProjectKey)
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
    command: ["/bin/sh", "-c", "while true; do echo compose-lifecycle; sleep 1; done"]
)",
                WideToMultiByte(ContainerName),
                WideToMultiByte(AlpineTestImage().NameAndTag())));

        auto result = RunWslc(std::format(L"compose create \"{}\"", composePath.wstring()));
        VERIFY_ARE_EQUAL(0u, result.ExitCode.value_or(1));

        result = RunWslc(std::format(L"compose start \"{}\"", composePath.wstring()));
        VERIFY_ARE_EQUAL(0u, result.ExitCode.value_or(1));
        VerifyComposeProjectStatus(ProjectName, L"running(1)");

        auto attach = RunWslcInteractive(std::format(L"compose attach \"{}\"", composePath.wstring()));
        VERIFY_IS_TRUE(attach.IsRunning());
        attach.ExpectStdout("compose-lifecycle\n");

        result = RunWslc(std::format(L"compose stop \"{}\"", composePath.wstring()));
        VERIFY_ARE_EQUAL(0u, result.ExitCode.value_or(1));
        VERIFY_ARE_EQUAL(0, attach.Wait());
        VerifyComposeProjectStatus(ProjectName, L"exited(1)");

        auto up = RunWslcInteractive(std::format(L"compose up \"{}\"", composePath.wstring()));
        VERIFY_IS_TRUE(up.IsRunning());
        up.ExpectStdout("compose-lifecycle\n");

        result = RunWslc(std::format(L"compose stop \"{}\"", composePath.wstring()));
        VERIFY_ARE_EQUAL(0u, result.ExitCode.value_or(1));
        VERIFY_ARE_EQUAL(0, up.Wait());
        VerifyComposeProjectStatus(ProjectName, L"exited(1)");

        VERIFY_IS_TRUE(std::filesystem::remove_all(projectDirectory) > 0);

        result = RunWslc(std::format(L"compose start {}", ProjectArgument));
        VERIFY_ARE_EQUAL(0u, result.ExitCode.value_or(1));
        VerifyComposeProjectStatus(ProjectName, L"running(1)");

        auto projectAttach = RunWslcInteractive(std::format(L"compose attach {}", ProjectArgument));
        VERIFY_IS_TRUE(projectAttach.IsRunning());
        projectAttach.ExpectStdout("compose-lifecycle\n");

        result = RunWslc(std::format(L"compose stop {}", ProjectArgument));
        VERIFY_ARE_EQUAL(0u, result.ExitCode.value_or(1));
        VERIFY_ARE_EQUAL(0, projectAttach.Wait());
        VerifyComposeProjectStatus(ProjectName, L"exited(1)");
    }

    WSLC_TEST_METHOD(WSLCE2E_Compose_Attach_SkipsInactiveContainers)
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
  active:
    name: {}
    image: {}
    command: ["/bin/sh", "-c", "while true; do echo compose-active; sleep 1; done"]
  inactive:
    name: {}
    image: {}
    command: ["/bin/sh", "-c", "sleep 300"]
)",
                WideToMultiByte(ContainerName),
                WideToMultiByte(AlpineTestImage().NameAndTag()),
                WideToMultiByte(InactiveContainerName),
                WideToMultiByte(AlpineTestImage().NameAndTag())));

        RunWslc(std::format(L"compose create \"{}\"", composePath.wstring()))
            .Verify(
                {.Stderr = std::format(
                     L"Creating Network {}_default (1/1)\r\nCreating Container {} (1/2)\r\nCreating Container {} (2/2)\r\n", ProjectName, ContainerName, InactiveContainerName),
                 .ExitCode = 0});
        RunWslc(std::format(L"compose attach {}", ProjectArgument)).Verify({.Stderr = L"", .ExitCode = 0});
        RunWslc(std::format(L"compose start {}", ProjectArgument))
            .Verify(
                {.Stderr = std::format(L"Starting Container {} (1/2)\r\nStarting Container {} (2/2)\r\n", ContainerName, InactiveContainerName),
                 .ExitCode = 0});
        RunWslc(std::format(L"container stop {} -t 0", InactiveContainerName)).Verify({.Stderr = L"", .ExitCode = 0});

        auto attach = RunWslcInteractive(std::format(L"compose attach {}", ProjectArgument));
        attach.ExpectStdout("compose-active\n");

        RunWslc(std::format(L"compose stop {}", ProjectArgument))
            .Verify(
                {.Stderr = std::format(L"Stopping Container {} (1/2)\r\nStopping Container {} (2/2)\r\n", ContainerName, InactiveContainerName),
                 .ExitCode = 0});
        VERIFY_ARE_EQUAL(0, attach.Wait());
        RunWslc(std::format(L"compose attach {}", ProjectArgument)).Verify({.Stderr = L"", .ExitCode = 0});
    }

private:
    static void Cleanup()
    {
        EnsureContainerDoesNotExist(ContainerName);
        EnsureContainerDoesNotExist(InactiveContainerName);
        EnsureNetworkDoesNotExist(ProjectName + L"_default");
    }

    inline static const std::wstring ProjectName = L"wslc-e2e-compose-lifecycle";
    inline static const std::wstring ProjectArgument = L"WSLC-E2E-COMPOSE-LIFECYCLE";
    inline static const std::wstring ContainerName = L"wslc-compose-lifecycle-owned";
    inline static const std::wstring InactiveContainerName = L"wslc-compose-lifecycle-inactive";
};

} // namespace WSLCE2ETests
