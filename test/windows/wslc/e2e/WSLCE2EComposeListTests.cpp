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
                "services:\n"
                "  owned:\n"
                "    name: {}\n"
                "    image: {}\n"
                "    command: [\"/bin/sh\", \"-c\", \"while true; do sleep 1; done\"]\n",
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

        result = RunWslc(std::format(L"compose start \"{}\"", composePath.wstring()));
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

private:
    static std::optional<ComposeProjectInformation> FindProject(const std::vector<ComposeProjectInformation>& Projects, const std::string& Name)
    {
        const auto project = std::ranges::find(Projects, Name, &ComposeProjectInformation::Name);
        return project == Projects.end() ? std::nullopt : std::optional{*project};
    }

    static bool ContainsProject(const std::vector<ComposeProjectInformation>& Projects, const std::string& Name)
    {
        return std::ranges::find(Projects, Name, &ComposeProjectInformation::Name) != Projects.end();
    }

    static void Cleanup()
    {
        EnsureContainerDoesNotExist(OwnedContainerName);
        EnsureContainerDoesNotExist(DockerContainerName);
        EnsureContainerDoesNotExist(UnsupportedContainerName);
        EnsureNetworkDoesNotExist(ProjectName + L"_default");
    }

    inline static const std::wstring ProjectName = L"wslc-e2e-compose-list";
    inline static const std::wstring OwnedContainerName = L"wslc-compose-list-owned";
    inline static const std::wstring DockerProjectName = L"docker-compose-list";
    inline static const std::wstring DockerContainerName = L"wslc-compose-list-docker";
    inline static const std::wstring UnsupportedProjectName = L"unsupported-compose-list";
    inline static const std::wstring UnsupportedContainerName = L"wslc-compose-list-unsupported";
};

} // namespace WSLCE2ETests
