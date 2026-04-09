/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EImageBuildTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC image build.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"
#include <fstream>

namespace WSLCE2ETests {

class WSLCE2EImageBuildTests
{
    WSLC_TEST_CLASS(WSLCE2EImageBuildTests)

    TEST_CLASS_SETUP(ClassSetup)
    {
        DeleteAllBuiltImages();
        EnsureImageIsLoaded(DebianTestImage());
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        DeleteAllBuiltImages();
        EnsureImageIsDeleted(DebianTestImage());
        return true;
    }

    TEST_METHOD_SETUP(MethodSetup)
    {
        DeleteAllBuiltImages();
        return true;
    }

    TEST_METHOD_CLEANUP(MethodCleanup)
    {
        DeleteAllBuiltImages();
        return true;
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Build_EmptyContextDirectory_Success)
    {
        auto testRoot = std::filesystem::current_path() / L"wslc-e2e-build-empty-context";
        auto cleanup = SetupTestDirectory(testRoot);

        auto contextDir = testRoot / L"context";
        std::error_code ec;
        std::filesystem::create_directories(contextDir, ec);
        THROW_HR_IF(E_FAIL, ec.value() != 0 || !std::filesystem::exists(contextDir));

        auto dockerfilePath = testRoot / L"Dockerfile";
        WriteTestFile(dockerfilePath, "FROM debian:latest\nCMD [\"echo\", \"wslc-e2e-build-ok\"]\n");

        auto buildResult = RunWslc(
            std::format(L"build \"{}\" -f \"{}\" -t {}", contextDir.wstring(), dockerfilePath.wstring(), BuiltImage.NameAndTag()));
        buildResult.Verify({.Stderr = L"", .ExitCode = 0});

        auto inspectData = InspectImage(BuiltImage.NameAndTag());
        VERIFY_IS_TRUE(inspectData.RepoTags.has_value());
        VERIFY_ARE_EQUAL(1u, inspectData.RepoTags.value().size());
        VERIFY_ARE_EQUAL(BuiltImage.NameAndTag(), wsl::shared::string::MultiByteToWide(inspectData.RepoTags.value()[0]));
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Build_BuildArgsFileAndMultipleTags_Success)
    {
        auto testRoot = std::filesystem::current_path() / L"wslc-e2e-build-args-tags";
        auto cleanup = SetupTestDirectory(testRoot);

        auto contextDir = testRoot / L"context";
        std::error_code ec;
        std::filesystem::create_directories(contextDir, ec);
        THROW_HR_IF(E_FAIL, ec.value() != 0 || !std::filesystem::exists(contextDir));

        // Create a simple file in the context directory
        auto filePath = contextDir / L"hello.txt";
        WriteTestFile(filePath, "hello from wslc build\n");

        auto dockerfilePath = testRoot / L"Dockerfile";
        WriteTestFile(
            dockerfilePath,
            "FROM debian:latest\n"
            "ARG TEST_LABEL=default_value\n"
            "LABEL test_label=$TEST_LABEL\n"
            "COPY hello.txt /hello.txt\n"
            "CMD [\"cat\", \"/hello.txt\"]\n");

        auto buildResult = RunWslc(std::format(
            L"build \"{}\" -f \"{}\" -t {} -t {} --build-arg TEST_LABEL=wslc_e2e_test",
            contextDir.wstring(),
            dockerfilePath.wstring(),
            BuiltImageTag1.NameAndTag(),
            BuiltImageTag2.NameAndTag()));
        buildResult.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify both tags are present by inspecting each one
        auto inspectData1 = InspectImage(BuiltImageTag1.NameAndTag());
        VERIFY_IS_TRUE(inspectData1.RepoTags.has_value());

        auto inspectData2 = InspectImage(BuiltImageTag2.NameAndTag());

        // Both tags refer to the same image
        VERIFY_ARE_EQUAL(inspectData1.Id, inspectData2.Id);

        // Verify the build arg was applied as a label
        VERIFY_IS_TRUE(inspectData1.Config.has_value());
        VERIFY_IS_TRUE(inspectData1.Config.value().Labels.has_value());
        const auto& labels = inspectData1.Config.value().Labels.value();
        auto it = labels.find("test_label");
        VERIFY_IS_TRUE(it != labels.end());
        VERIFY_ARE_EQUAL(std::string("wslc_e2e_test"), it->second);
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Build_DockerfileInContextDir_Success)
    {
        BuildFromContextFile(L"Dockerfile", BuiltImageDockerfile);
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Build_ContainerfileInContextDir_Success)
    {
        BuildFromContextFile(L"Containerfile", BuiltImageContainerfile);
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Build_BothDockerfileAndContainerfile_Fails)
    {
        auto testRoot = std::filesystem::current_path() / L"wslc-e2e-build-both-files";
        auto cleanup = SetupTestDirectory(testRoot);

        WriteTestFile(testRoot / L"Dockerfile", "FROM debian:latest\n");
        WriteTestFile(testRoot / L"Containerfile", "FROM debian:latest\n");

        auto buildResult = RunWslc(std::format(L"build \"{}\"", testRoot.wstring()));
        buildResult.Verify(
            {.Stderr =
                 L"Both Dockerfile and Containerfile found. Use -f to select the file to use\r\nError code: E_INVALIDARG\r\n",
             .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Build_NeitherDockerfileNorContainerfile_Fails)
    {
        auto testRoot = std::filesystem::current_path() / L"wslc-e2e-build-no-files";
        auto cleanup = SetupTestDirectory(testRoot);

        auto absolutePath = std::filesystem::absolute(testRoot);
        auto buildResult = RunWslc(std::format(L"build \"{}\"", testRoot.wstring()));
        buildResult.Verify(
            {.Stderr = std::format(L"No Containerfile or Dockerfile found in '{}'\r\nError code: E_INVALIDARG\r\n", absolutePath.wstring()),
             .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Build_ContainerfileAccessDenied_Fails)
    {
        auto testRoot = std::filesystem::current_path() / L"wslc-e2e-build-access-denied";
        auto cleanup = SetupTestDirectory(testRoot);

        auto containerfilePath = testRoot / L"Containerfile";
        WriteTestFile(containerfilePath, "FROM debian:latest\n");

        // Deny read access so wslc cannot open the file.
        SetReadAccess(containerfilePath, DENY_ACCESS);

        auto restore =
            wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [containerfilePath]() { SetReadAccess(containerfilePath, GRANT_ACCESS); });

        auto absoluteContainerfilePath = std::filesystem::absolute(containerfilePath);
        auto buildResult = RunWslc(std::format(L"build \"{}\"", testRoot.wstring()));
        buildResult.Verify(
            {.Stderr = std::format(
                 L"Failed to open '{}': Access is denied. \r\nError code: E_ACCESSDENIED\r\n", absoluteContainerfilePath.wstring()),
             .ExitCode = 1});
    }

private:
    const TestImage BuiltImage{L"wslc-e2e-build-empty-context", L"latest", L""};
    const TestImage BuiltImageTag1{L"wslc-e2e-build-args-tags", L"v1", L""};
    const TestImage BuiltImageTag2{L"wslc-e2e-build-args-tags", L"v2", L""};
    const TestImage BuiltImageDockerfile{L"wslc-e2e-build-dockerfile-ctx", L"latest", L""};
    const TestImage BuiltImageContainerfile{L"wslc-e2e-build-containerfile-ctx", L"latest", L""};

    void BuildFromContextFile(const std::wstring& fileName, const TestImage& image)
    {
        auto testRoot = std::filesystem::current_path() / image.Name;
        auto cleanup = SetupTestDirectory(testRoot);

        WriteTestFile(testRoot / fileName, "FROM debian:latest\nCMD [\"echo\", \"build-ok\"]\n");

        auto buildResult = RunWslc(std::format(L"build \"{}\" -t {}", testRoot.wstring(), image.NameAndTag()));
        buildResult.Verify({.Stderr = L"", .ExitCode = 0});

        auto inspectData = InspectImage(image.NameAndTag());
        VERIFY_IS_TRUE(inspectData.RepoTags.has_value());
        VERIFY_ARE_EQUAL(1u, inspectData.RepoTags.value().size());
        VERIFY_ARE_EQUAL(image.NameAndTag(), wsl::shared::string::MultiByteToWide(inspectData.RepoTags.value()[0]));
    }

    void DeleteAllBuiltImages()
    {
        EnsureImageIsDeleted(BuiltImage);
        EnsureImageIsDeleted(BuiltImageTag1);
        EnsureImageIsDeleted(BuiltImageTag2);
        EnsureImageIsDeleted(BuiltImageDockerfile);
        EnsureImageIsDeleted(BuiltImageContainerfile);
    }

    static auto SetupTestDirectory(const std::filesystem::path& testRoot)
    {
        std::error_code ec;
        std::filesystem::remove_all(testRoot, ec);
        THROW_HR_IF_MSG(E_FAIL, ec.value() != 0 && std::filesystem::exists(testRoot), "%hs", ec.message().c_str());

        std::filesystem::create_directories(testRoot, ec);
        THROW_HR_IF_MSG(E_FAIL, ec.value() != 0 || !std::filesystem::exists(testRoot), "%hs", ec.message().c_str());

        return wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [testRoot]() {
            std::error_code removeError;
            std::filesystem::remove_all(testRoot, removeError);
        });
    }

    static void WriteTestFile(const std::filesystem::path& path, const std::string& content)
    {
        std::ofstream file(path);
        THROW_HR_IF(E_FAIL, !file.is_open());
        file << content;
        THROW_HR_IF(E_FAIL, !file.good());
        file.close();
    }

    static void SetReadAccess(const std::filesystem::path& path, ACCESS_MODE Mode)
    {
        auto [everyoneSid, everyoneSidBuffer] = wsl::windows::common::security::CreateSid(SECURITY_WORLD_SID_AUTHORITY, SECURITY_WORLD_RID);

        EXPLICIT_ACCESSW ea{};
        ea.grfAccessPermissions = FILE_GENERIC_READ;
        ea.grfAccessMode = Mode;
        ea.grfInheritance = NO_INHERITANCE;
        ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        ea.Trustee.ptstrName = static_cast<LPWSTR>(everyoneSid);

        PACL acl = nullptr;
        wil::unique_hlocal descriptor;
        THROW_IF_WIN32_ERROR(GetNamedSecurityInfoW(
            path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &acl, nullptr, &descriptor));

        wsl::windows::common::security::unique_acl newAcl;
        THROW_IF_WIN32_ERROR(SetEntriesInAclW(1, &ea, acl, &newAcl));

        THROW_IF_WIN32_ERROR(SetNamedSecurityInfoW(
            const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, newAcl.get(), nullptr));
    }
};
} // namespace WSLCE2ETests
