// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "windows/Common.h"
#include "ComposeModel.h"
#include "TestImageRegistry.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"

namespace WSLCE2ETests {

using namespace wsl::shared;
using namespace wsl::windows::wslc::models;
using wsl::shared::string::WideToMultiByte;

class WSLCE2EComposeListTests
{
    WSLC_TEST_CLASS(WSLCE2EComposeListTests)

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

    WSLC_TEST_METHOD(WSLCE2E_Compose_List_ManagedProjectsOnly)
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
                WideToMultiByte(OwnedContainerName),
                WideToMultiByte(AlpineTestImage().NameAndTag())));

        auto result = RunWslc(std::format(
            L"container create --name {} --label com.docker.compose.project={} --label com.docker.compose.service=service {}",
            DockerContainerName,
            DockerProjectName,
            AlpineTestImage().NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"compose create \"{}\"", composePath.wstring()));
        VERIFY_ARE_EQUAL(0u, result.ExitCode.value_or(1));

        result = RunWslc(L"compose list --format json");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        auto projects = ParseNdjsonOutputAs<ComposeProjectInformation>(result);
        VERIFY_IS_FALSE(ContainsProject(projects, WideToMultiByte(ProjectName)));

        result = RunWslc(L"compose list --all --format json");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        projects = ParseNdjsonOutputAs<ComposeProjectInformation>(result);
        const auto project = FindProject(projects, WideToMultiByte(ProjectName));
        VERIFY_IS_TRUE(project.has_value());
        VERIFY_ARE_EQUAL(std::string{"created(1)"}, project->Status);
        VERIFY_IS_FALSE(ContainsProject(projects, WideToMultiByte(DockerProjectName)));

        result = RunWslc(std::format(L"compose start {}", ProjectName));
        VERIFY_ARE_EQUAL(0u, result.ExitCode.value_or(1));

        result = RunWslc(L"compose ls --format json");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        projects = ParseNdjsonOutputAs<ComposeProjectInformation>(result);
        const auto runningProject = FindProject(projects, WideToMultiByte(ProjectName));
        VERIFY_IS_TRUE(runningProject.has_value());
        VERIFY_ARE_EQUAL(std::string{"running(1)"}, runningProject->Status);

        result = RunWslc(L"compose list --quiet");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto quietOutput = result.GetStdoutLines();
        VERIFY_ARE_NOT_EQUAL(quietOutput.end(), std::ranges::find(quietOutput, ProjectName));

        result = RunWslc(L"compose list");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_TRUE(result.StdoutContainsSubstring(L"NAME"));
        VERIFY_IS_TRUE(result.StdoutContainsSubstring(L"STATUS"));
        VERIFY_IS_TRUE(result.StdoutContainsSubstring(ProjectName));

        result = RunWslc(std::format(L"compose stop {}", ProjectName));
        VERIFY_ARE_EQUAL(0u, result.ExitCode.value_or(1));

        result = RunWslc(L"compose list --all --format json");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        projects = ParseNdjsonOutputAs<ComposeProjectInformation>(result);
        const auto stoppedProject = FindProject(projects, WideToMultiByte(ProjectName));
        VERIFY_IS_TRUE(stoppedProject.has_value());
        VERIFY_ARE_EQUAL(std::string{"exited(1)"}, stoppedProject->Status);

        result = RunWslc(std::format(
            L"container create --name {} --label com.docker.compose.project={} --label "
            L"com.microsoft.wslc.compose.managed=true --label com.microsoft.wslc.compose.metadata-version=2 {}",
            UnsupportedContainerName,
            UnsupportedProjectName,
            AlpineTestImage().NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(L"compose list --all --format json");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        projects = ParseNdjsonOutputAs<ComposeProjectInformation>(result);
        VERIFY_IS_TRUE(ContainsProject(projects, WideToMultiByte(ProjectName)));
        VERIFY_IS_FALSE(ContainsProject(projects, WideToMultiByte(DockerProjectName)));
        VERIFY_IS_FALSE(ContainsProject(projects, WideToMultiByte(UnsupportedProjectName)));
    }

    WSLC_TEST_METHOD(WSLCE2E_Compose_Lifecycle_AcceptsFileAndProjectKey)
    {
        const auto projectDirectory = std::filesystem::current_path() / LifecycleProjectName;
        auto directoryCleanup = SetupTestDirectory(projectDirectory);
        auto resourceCleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            EnsureContainerDoesNotExist(LifecycleContainerName);
            EnsureNetworkDoesNotExist(LifecycleProjectName + L"_default");
        });
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
                WideToMultiByte(LifecycleContainerName),
                WideToMultiByte(AlpineTestImage().NameAndTag())));

        auto result = RunWslc(std::format(L"compose create \"{}\"", composePath.wstring()));
        VERIFY_ARE_EQUAL(0u, result.ExitCode.value_or(1));

        result = RunWslc(std::format(L"compose start \"{}\"", composePath.wstring()));
        VERIFY_ARE_EQUAL(0u, result.ExitCode.value_or(1));
        VerifyProjectStatus(LifecycleProjectName, L"running(1)");

        result = RunWslc(std::format(L"compose remove \"{}\"", composePath.wstring()));
        VERIFY_ARE_EQUAL(0u, result.ExitCode.value_or(1));
        VerifyProjectStatus(LifecycleProjectName, L"running(1)");

        auto attach = RunWslcInteractive(std::format(L"compose attach \"{}\"", composePath.wstring()));
        VERIFY_IS_TRUE(attach.IsRunning());
        attach.ExpectStdout("compose-lifecycle\n");

        result = RunWslc(std::format(L"compose stop \"{}\"", composePath.wstring()));
        VERIFY_ARE_EQUAL(0u, result.ExitCode.value_or(1));
        VERIFY_ARE_EQUAL(0, attach.Wait());
        VerifyProjectStatus(LifecycleProjectName, L"exited(1)");

        result = RunWslc(std::format(L"compose delete \"{}\"", composePath.wstring()));
        VERIFY_ARE_EQUAL(0u, result.ExitCode.value_or(1));
        VerifyProjectAbsent(LifecycleProjectName);

        auto up = RunWslcInteractive(std::format(L"compose up \"{}\"", composePath.wstring()));
        VERIFY_IS_TRUE(up.IsRunning());
        up.ExpectStdout("compose-lifecycle\n");

        result = RunWslc(std::format(L"compose stop \"{}\"", composePath.wstring()));
        VERIFY_ARE_EQUAL(0u, result.ExitCode.value_or(1));
        VERIFY_ARE_EQUAL(0, up.Wait());
        VerifyProjectStatus(LifecycleProjectName, L"exited(1)");

        VERIFY_IS_TRUE(std::filesystem::remove_all(projectDirectory) > 0);

        result = RunWslc(std::format(L"compose start {}", LifecycleProjectArgument));
        VERIFY_ARE_EQUAL(0u, result.ExitCode.value_or(1));
        VerifyProjectStatus(LifecycleProjectName, L"running(1)");

        auto projectAttach = RunWslcInteractive(std::format(L"compose attach {}", LifecycleProjectArgument));
        VERIFY_IS_TRUE(projectAttach.IsRunning());
        projectAttach.ExpectStdout("compose-lifecycle\n");

        result = RunWslc(std::format(L"compose stop {}", LifecycleProjectArgument));
        VERIFY_ARE_EQUAL(0u, result.ExitCode.value_or(1));
        VERIFY_ARE_EQUAL(0, projectAttach.Wait());
        VerifyProjectStatus(LifecycleProjectName, L"exited(1)");

        result = RunWslc(std::format(L"compose rm {}", LifecycleProjectArgument));
        VERIFY_ARE_EQUAL(0u, result.ExitCode.value_or(1));
        VerifyProjectAbsent(LifecycleProjectName);
    }

private:
    static void VerifyProjectStatus(const std::wstring& projectName, const std::wstring& expectedStatus)
    {
        const auto result = RunWslc(L"compose list --all --format json");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto projects = ParseNdjsonOutputAs<ComposeProjectInformation>(result);
        const auto project = FindProject(projects, WideToMultiByte(projectName));
        VERIFY_IS_TRUE(project.has_value());
        VERIFY_ARE_EQUAL(WideToMultiByte(expectedStatus), project->Status);
    }

    static void VerifyProjectAbsent(const std::wstring& projectName)
    {
        const auto result = RunWslc(L"compose list --all --format json");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto projects = ParseNdjsonOutputAs<ComposeProjectInformation>(result);
        VERIFY_IS_FALSE(ContainsProject(projects, WideToMultiByte(projectName)));
    }

    static std::optional<ComposeProjectInformation> FindProject(const std::vector<ComposeProjectInformation>& projects, const std::string& name)
    {
        const auto project = std::ranges::find(projects, name, &ComposeProjectInformation::Name);
        return project == projects.end() ? std::nullopt : std::optional{*project};
    }

    static bool ContainsProject(const std::vector<ComposeProjectInformation>& projects, const std::string& name)
    {
        return std::ranges::find(projects, name, &ComposeProjectInformation::Name) != projects.end();
    }

    static void Cleanup()
    {
        EnsureContainerDoesNotExist(OwnedContainerName);
        EnsureContainerDoesNotExist(DockerContainerName);
        EnsureContainerDoesNotExist(UnsupportedContainerName);
        EnsureNetworkDoesNotExist(ProjectName + L"_default");
        EnsureContainerDoesNotExist(LifecycleContainerName);
        EnsureNetworkDoesNotExist(LifecycleProjectName + L"_default");
    }

    inline static const std::wstring ProjectName = L"wslc-e2e-compose-list";
    inline static const std::wstring OwnedContainerName = L"wslc-compose-list-owned";
    inline static const std::wstring DockerProjectName = L"docker-compose-list";
    inline static const std::wstring DockerContainerName = L"wslc-compose-list-docker";
    inline static const std::wstring UnsupportedProjectName = L"unsupported-compose-list";
    inline static const std::wstring UnsupportedContainerName = L"wslc-compose-list-unsupported";
    inline static const std::wstring LifecycleProjectName = L"wslc-e2e-compose-lifecycle";
    inline static const std::wstring LifecycleProjectArgument = L"WSLC-E2E-COMPOSE-LIFECYCLE";
    inline static const std::wstring LifecycleContainerName = L"wslc-compose-lifecycle-owned";
};

} // namespace WSLCE2ETests
