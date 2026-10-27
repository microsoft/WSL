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

    WSLC_TEST_METHOD(WSLCE2E_Compose_ReportsServiceValidationError)
    {
        const auto projectDirectory = std::filesystem::current_path() / ProjectName;
        auto directoryCleanup = SetupTestDirectory(projectDirectory);
        const auto composePath = projectDirectory / L"compose.yaml";
        WriteTestFileContent(composePath, "services:\n  bad name:\n    image: alpine:latest\n");

        const auto result = RunWslc(std::format(L"compose create \"{}\"", composePath.wstring()));
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(
            result.StderrContainsSubstring(wsl::shared::Localization::MessageWslcComposeServiceNameInvalid("bad name")));
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
        VERIFY_ARE_EQUAL(0, attach.Wait(120'000));
        VerifyComposeProjectStatus(ProjectName, L"exited(1)");

        auto up = RunWslcInteractive(std::format(L"compose up \"{}\"", composePath.wstring()));
        VERIFY_IS_TRUE(up.IsRunning());
        up.ExpectStdout("compose-lifecycle\n");

        result = RunWslc(std::format(L"compose stop \"{}\"", composePath.wstring()));
        VERIFY_ARE_EQUAL(0u, result.ExitCode.value_or(1));
        VERIFY_ARE_EQUAL(0, up.Wait(120'000));
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
        VERIFY_ARE_EQUAL(0, projectAttach.Wait(120'000));
        VerifyComposeProjectStatus(ProjectName, L"exited(1)");
    }

    WSLC_TEST_METHOD(WSLCE2E_Compose_SymlinkUsesTargetProjectAndVolumes)
    {
        const auto projectDirectory = std::filesystem::current_path() / ProjectName;
        auto directoryCleanup = SetupTestDirectory(projectDirectory);
        auto resourceCleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { Cleanup(); });
        const auto composePath = projectDirectory / L"compose.yaml";
        const auto linkDirectory = projectDirectory / L"links";
        const auto linkPath = linkDirectory / L"compose.yaml";
        VERIFY_IS_TRUE(std::filesystem::create_directory(linkDirectory));
        VERIFY_IS_TRUE(std::filesystem::create_directory(projectDirectory / L"data"));
        VERIFY_IS_TRUE(std::filesystem::create_directory(linkDirectory / L"data"));
        WriteTestFileContent(projectDirectory / L"data" / L"marker.txt", "target-directory\n");
        WriteTestFileContent(linkDirectory / L"data" / L"marker.txt", "link-directory\n");
        WriteTestFileContent(
            composePath,
            std::format(
                R"(
services:
 owned:
   name: {}
   image: {}
   command: ["/bin/sleep", "99999"]
   volumes:
     - ./data:/work:ro
)",
                WideToMultiByte(ContainerName),
                WideToMultiByte(AlpineTestImage().NameAndTag())));
        VERIFY_WIN32_BOOL_SUCCEEDED(CreateSymbolicLinkW(linkPath.c_str(), composePath.c_str(), 0));

        auto result = RunWslc(std::format(L"compose create \"{}\"", linkPath.wstring()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_ARE_EQUAL(WideToMultiByte(ProjectName), InspectContainer(ContainerName).Config.Labels.at("com.microsoft.wslc.compose.project"));

        result = RunWslc(std::format(L"compose start \"{}\"", composePath.wstring()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        result = RunWslc(std::format(L"container exec {} cat /work/marker.txt", ContainerName));
        result.Verify({.Stdout = L"target-directory\n", .Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"compose stop \"{}\"", linkPath.wstring()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_FALSE(InspectContainer(ContainerName).State.Running);
    }

private:
    static void Cleanup()
    {
        EnsureContainerDoesNotExist(ContainerName);
        EnsureNetworkDoesNotExist(ProjectName + L"_default");
    }

    inline static const std::wstring ProjectName = L"wslc-e2e-compose-lifecycle";
    inline static const std::wstring ProjectArgument = L"WSLC-E2E-COMPOSE-LIFECYCLE";
    inline static const std::wstring ContainerName = L"wslc-compose-lifecycle-owned";
};

} // namespace WSLCE2ETests
