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
#include "TestImageRegistry.h"

namespace WSLCE2ETests {
using namespace wsl::shared;

class WSLCE2EImageBuildTests
{
    WSLC_TEST_CLASS(WSLCE2EImageBuildTests)

    TEST_CLASS_SETUP(ClassSetup)
    {
        DeleteImagesWithRepositoryPrefix(c_builtImagePrefix);
        TestImageRegistry::Instance().EnsureLoaded(DebianTestImage());
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        DeleteImagesWithRepositoryPrefix(c_builtImagePrefix);
        return true;
    }

    // Each test owns and cleans up exactly the image(s) it builds via DeleteImageOnExit, so there is
    // no per-method sweep. DeleteImagesWithRepositoryPrefix in the class setup/cleanup above is only a
    // safety net for images left behind by a crashed run.
    static constexpr auto c_builtImagePrefix = L"wslc-e2e-build-";

    // Port for the local registry backing the --pull test; distinct from the other test classes.
    static constexpr USHORT c_registryPort = 15005;

    // Returns an RAII guard that best-effort deletes the given image when it goes out of scope. It is
    // deliberately non-throwing (no VERIFY) because it may run while the stack unwinds after a test
    // failure; the class-level prune is the authoritative cleanup.
    static auto DeleteImageOnExit(std::wstring imageNameAndTag)
    {
        return wil::scope_exit([imageNameAndTag = std::move(imageNameAndTag)]() {
            try
            {
                RunWslc(std::format(L"image delete --force {}", imageNameAndTag));
            }
            CATCH_LOG()
        });
    }

    static auto DeleteImageOnExit(const TestImage& image)
    {
        return DeleteImageOnExit(image.NameAndTag());
    }

    // All secret tests build from this single shared (empty) context directory. Each distinct mounted
    // directory consumes a virtiofs share slot while it is mounted, so reusing a single context path
    // helps keep secret tests from exhausting the per-session share budget.
    //
    // The per-test Dockerfile is streamed via -f (never mounted); each file secret causes the server to
    // mount that secret file's parent directory read-only for the duration of that build.
    static std::filesystem::path SharedSecretBuildContext()
    {
        auto dir = std::filesystem::current_path() / L"wslc-e2e-build-secret-context";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        THROW_HR_IF(E_FAIL, ec.value() != 0 || !std::filesystem::is_directory(dir));
        return dir;
    }

    // All --output tests build from this single shared (empty) context directory, for the same reason
    // as SharedSecretBuildContext above: the session never releases virtiofs shares (see
    // WSLCVirtualMachine::UnmountWindowsFolder), so giving each --output test its own context directory
    // would permanently consume one share slot per test and eventually exhaust the session's budget.
    // Reusing one path keeps all --output builds to a single shared slot. Each test's Dockerfile is
    // streamed via -f and its output artifacts (tarballs, extracted trees) live under its own testRoot,
    // so none of that is mounted.
    static std::filesystem::path SharedOutputBuildContext()
    {
        auto dir = std::filesystem::current_path() / L"wslc-e2e-build-output-context";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        THROW_HR_IF(E_FAIL, ec.value() != 0 || !std::filesystem::is_directory(dir));
        return dir;
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Build_EmptyContextDirectory_Success)
    {
        auto imageCleanup = DeleteImageOnExit(BuiltImage);
        auto testRoot = std::filesystem::current_path() / L"wslc-e2e-build-empty-context";
        auto cleanup = SetupTestDirectory(testRoot);

        auto contextDir = testRoot / L"context";
        std::error_code ec;
        std::filesystem::create_directories(contextDir, ec);
        THROW_HR_IF(E_FAIL, ec.value() != 0 || !std::filesystem::exists(contextDir));

        auto dockerfilePath = testRoot / L"Dockerfile";
        WriteTestFileContent(dockerfilePath, "FROM debian:latest\nCMD [\"echo\", \"wslc-e2e-build-ok\"]\n");

        auto buildResult = RunWslc(
            std::format(L"build \"{}\" -f \"{}\" -t {}", contextDir.wstring(), dockerfilePath.wstring(), BuiltImage.NameAndTag()));
        buildResult.Verify({.Stdout = L"", .ExitCode = 0});

        auto inspectData = InspectImage(BuiltImage.NameAndTag());
        VERIFY_IS_TRUE(inspectData.RepoTags.has_value());
        VERIFY_ARE_EQUAL(1u, inspectData.RepoTags.value().size());
        VERIFY_ARE_EQUAL(BuiltImage.NameAndTag(), wsl::shared::string::MultiByteToWide(inspectData.RepoTags.value()[0]));
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Build_UnicodeOutput_Success)
    {
        auto testRoot = std::filesystem::current_path() / L"wslc-e2e-build-unicode-output";
        auto cleanup = SetupTestDirectory(testRoot);

        auto dockerfilePath = testRoot / L"Dockerfile";
        WriteTestFileContent(dockerfilePath, "FROM debian:latest\nRUN echo 安装依赖\n");

        auto buildResult = RunWslc(std::format(
            L"build \"{}\" -f \"{}\" --output type=cacheonly", SharedOutputBuildContext().wstring(), dockerfilePath.wstring()));
        buildResult.Verify({.ExitCode = 0});
        VERIFY_IS_TRUE(buildResult.StderrContainsSubstring(wsl::shared::string::MultiByteToWide("安装依赖")));
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Build_BuildArgsFileAndMultipleTags_Success)
    {
        auto imageCleanup1 = DeleteImageOnExit(BuiltImageTag1);
        auto imageCleanup2 = DeleteImageOnExit(BuiltImageTag2);
        auto testRoot = std::filesystem::current_path() / L"wslc-e2e-build-args-tags";
        auto cleanup = SetupTestDirectory(testRoot);

        auto contextDir = testRoot / L"context";
        std::error_code ec;
        std::filesystem::create_directories(contextDir, ec);
        THROW_HR_IF(E_FAIL, ec.value() != 0 || !std::filesystem::exists(contextDir));

        // Create a simple file in the context directory
        auto filePath = contextDir / L"hello.txt";
        WriteTestFileContent(filePath, "hello from wslc build\n");

        auto dockerfilePath = testRoot / L"Dockerfile";
        WriteTestFileContent(
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
        buildResult.Verify({.Stdout = L"", .ExitCode = 0});

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

    WSLC_TEST_METHOD(WSLCE2E_Image_Build_Pull_Success)
    {
        // A local registry acts as the private image source that --pull re-resolves the base image from.
        TestImageRegistry::Instance().EnsureLoaded(AlpineTestImage());

        auto session = OpenDefaultElevatedSession();
        auto [registryContainer, registryAddress] = StartLocalRegistry(*session, "", "", c_registryPort);

        auto registryImage = TagImageForRegistry(AlpineTestImage().NameAndTag(), string::MultiByteToWide(registryAddress));
        auto registryImageCleanup = DeleteImageOnExit(registryImage);

        RunWslcAndVerify(std::format(L"push {}", registryImage), {.Stderr = L"", .ExitCode = 0});

        auto imageCleanup = DeleteImageOnExit(BuiltImagePull);
        auto testRoot = std::filesystem::current_path() / L"wslc-e2e-build-pull";
        auto cleanup = SetupTestDirectory(testRoot);

        auto contextDir = testRoot / L"context";
        std::error_code ec;
        std::filesystem::create_directories(contextDir, ec);
        THROW_HR_IF(E_FAIL, ec.value() != 0 || !std::filesystem::exists(contextDir));

        auto dockerfilePath = testRoot / L"Dockerfile";
        auto dockerfile = std::format("FROM {}\nCMD [\"echo\", \"pull-ok\"]\n", string::WideToMultiByte(registryImage));
        WriteTestFileContent(dockerfilePath, dockerfile);

        // The base image is already local, so only --pull can make the FROM step resolve a registry digest.
        auto buildResult = RunWslc(std::format(
            L"build \"{}\" -f \"{}\" -t {} --pull --verbose", contextDir.wstring(), dockerfilePath.wstring(), BuiltImagePull.NameAndTag()));
        buildResult.Verify({.Stdout = L"", .ExitCode = 0});

        VERIFY_IS_TRUE(buildResult.StderrContainsSubstring(std::format(L"{}@sha256:", registryImage)));
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Build_Target_Success)
    {
        auto imageCleanup = DeleteImageOnExit(BuiltImageTarget);
        auto testRoot = std::filesystem::current_path() / L"wslc-e2e-build-target";
        auto cleanup = SetupTestDirectory(testRoot);

        auto contextDir = testRoot / L"context";
        std::error_code ec;
        std::filesystem::create_directories(contextDir, ec);
        THROW_HR_IF(E_FAIL, ec.value() != 0 || !std::filesystem::exists(contextDir));

        auto dockerfilePath = testRoot / L"Dockerfile";
        WriteTestFileContent(
            dockerfilePath,
            "FROM debian:latest AS build-stage\n"
            "RUN echo build > /stage.txt\n"
            "\n"
            "FROM debian:latest AS final-stage\n"
            "COPY --from=build-stage /stage.txt /stage.txt\n"
            "CMD [\"cat\", \"/stage.txt\"]\n");

        auto buildResult = RunWslc(std::format(
            L"build \"{}\" -f \"{}\" -t {} --target build-stage", contextDir.wstring(), dockerfilePath.wstring(), BuiltImageTarget.NameAndTag()));
        buildResult.Verify({.Stdout = L"", .ExitCode = 0});

        auto inspectData = InspectImage(BuiltImageTarget.NameAndTag());
        VERIFY_IS_TRUE(inspectData.RepoTags.has_value());
        VERIFY_ARE_EQUAL(1u, inspectData.RepoTags.value().size());
        VERIFY_ARE_EQUAL(BuiltImageTarget.NameAndTag(), wsl::shared::string::MultiByteToWide(inspectData.RepoTags.value()[0]));

        // Verify that --target stopped at build-stage: the image should NOT have the CMD
        // from final-stage. If --target were ignored, the CMD would be ["cat", "/stage.txt"].
        VERIFY_IS_TRUE(inspectData.Config.has_value());
        const std::vector<std::string> finalStageCmd{"cat", "/stage.txt"};
        VERIFY_IS_TRUE(!inspectData.Config.value().Cmd.has_value() || inspectData.Config.value().Cmd.value() != finalStageCmd);
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Build_Label_Success)
    {
        auto imageCleanup = DeleteImageOnExit(BuiltImageLabel);
        auto testRoot = std::filesystem::current_path() / L"wslc-e2e-build-label";
        auto cleanup = SetupTestDirectory(testRoot);

        auto contextDir = testRoot / L"context";
        std::error_code ec;
        std::filesystem::create_directories(contextDir, ec);
        THROW_HR_IF(E_FAIL, ec.value() != 0 || !std::filesystem::exists(contextDir));

        auto dockerfilePath = testRoot / L"Dockerfile";
        WriteTestFileContent(dockerfilePath, "FROM debian:latest\nCMD [\"echo\", \"label-ok\"]\n");

        // Use both the short alias (-l) and long form (--label) to confirm both parse paths.
        auto buildResult = RunWslc(std::format(
            L"build \"{}\" -f \"{}\" -t {} -l first=one --label second=two",
            contextDir.wstring(),
            dockerfilePath.wstring(),
            BuiltImageLabel.NameAndTag()));
        buildResult.Verify({.Stdout = L"", .ExitCode = 0});

        auto inspectData = InspectImage(BuiltImageLabel.NameAndTag());
        VERIFY_IS_TRUE(inspectData.Config.has_value());
        VERIFY_IS_TRUE(inspectData.Config.value().Labels.has_value());
        const auto& labels = inspectData.Config.value().Labels.value();

        auto firstIt = labels.find("first");
        VERIFY_IS_TRUE(firstIt != labels.end());
        VERIFY_ARE_EQUAL(std::string("one"), firstIt->second);

        auto secondIt = labels.find("second");
        VERIFY_IS_TRUE(secondIt != labels.end());
        VERIFY_ARE_EQUAL(std::string("two"), secondIt->second);
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Build_LabelOverridesDockerfile_Success)
    {
        auto imageCleanup = DeleteImageOnExit(BuiltImageLabelOverride);
        auto testRoot = std::filesystem::current_path() / L"wslc-e2e-build-label-override";
        auto cleanup = SetupTestDirectory(testRoot);

        auto contextDir = testRoot / L"context";
        std::error_code ec;
        std::filesystem::create_directories(contextDir, ec);
        THROW_HR_IF(E_FAIL, ec.value() != 0 || !std::filesystem::exists(contextDir));

        auto dockerfilePath = testRoot / L"Dockerfile";
        WriteTestFileContent(
            dockerfilePath, "FROM debian:latest\nLABEL conflict=from-dockerfile\nCMD [\"echo\", \"label-override-ok\"]\n");

        auto buildResult = RunWslc(std::format(
            L"build \"{}\" -f \"{}\" -t {} --label conflict=from-cli",
            contextDir.wstring(),
            dockerfilePath.wstring(),
            BuiltImageLabelOverride.NameAndTag()));
        buildResult.Verify({.Stdout = L"", .ExitCode = 0});

        auto inspectData = InspectImage(BuiltImageLabelOverride.NameAndTag());
        VERIFY_IS_TRUE(inspectData.Config.has_value());
        VERIFY_IS_TRUE(inspectData.Config.value().Labels.has_value());
        const auto& labels = inspectData.Config.value().Labels.value();
        auto it = labels.find("conflict");
        VERIFY_IS_TRUE(it != labels.end());
        VERIFY_ARE_EQUAL(std::string("from-cli"), it->second);
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Build_Secret_Env_Success)
    {
        // Set the env var the --secret will reference; ensure cleanup so we don't leak into other tests.
        constexpr auto envName = L"WSLC_E2E_SECRET_VALUE";
        constexpr auto envValue = L"expected-secret-content-12345";
        ScopedEnvVariable envVar(envName, envValue);

        auto imageCleanup = DeleteImageOnExit(BuiltImageSecret);
        auto testRoot = std::filesystem::current_path() / L"wslc-e2e-build-secret-env";
        auto cleanup = SetupTestDirectory(testRoot);

        auto contextDir = SharedSecretBuildContext();

        // RUN with type=secret asserts the secret value matches; if mismatched, RUN exits non-zero and the build fails.
        auto dockerfilePath = testRoot / L"Dockerfile";
        WriteTestFileContent(
            dockerfilePath,
            "# syntax=docker/dockerfile:1\n"
            "FROM debian:latest\n"
            "RUN --mount=type=secret,id=mysecret "
            "[ \"$(cat /run/secrets/mysecret)\" = \"expected-secret-content-12345\" ]\n"
            "CMD [\"echo\", \"secret-ok\"]\n");

        auto buildResult = RunWslc(std::format(
            L"build \"{}\" -f \"{}\" -t {} --secret id=mysecret,env=WSLC_E2E_SECRET_VALUE",
            contextDir.wstring(),
            dockerfilePath.wstring(),
            BuiltImageSecret.NameAndTag()));
        buildResult.Verify({.ExitCode = 0});

        auto inspectData = InspectImage(BuiltImageSecret.NameAndTag());
        VERIFY_IS_TRUE(inspectData.RepoTags.has_value());
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Build_Secret_BareId_UsesEnvNamedById_Success)
    {
        // Docker parity: '--secret id=NAME' with no env=/src= reads the host env var named NAME.
        constexpr auto envName = L"WSLC_E2E_BARE_SECRET";
        constexpr auto envValue = L"bare-id-secret-content-67890";
        ScopedEnvVariable envVar(envName, envValue);

        auto imageCleanup = DeleteImageOnExit(BuiltImageSecretBareId);
        auto testRoot = std::filesystem::current_path() / L"wslc-e2e-build-secret-bare-id";
        auto cleanup = SetupTestDirectory(testRoot);

        auto contextDir = SharedSecretBuildContext();

        // The docker secret id equals the env var name, so the mount reads /run/secrets/<envName>.
        auto dockerfilePath = testRoot / L"Dockerfile";
        WriteTestFileContent(
            dockerfilePath,
            "# syntax=docker/dockerfile:1\n"
            "FROM debian:latest\n"
            "RUN --mount=type=secret,id=WSLC_E2E_BARE_SECRET "
            "[ \"$(cat /run/secrets/WSLC_E2E_BARE_SECRET)\" = \"bare-id-secret-content-67890\" ]\n"
            "CMD [\"echo\", \"secret-ok\"]\n");

        auto buildResult = RunWslc(std::format(
            L"build \"{}\" -f \"{}\" -t {} --secret id=WSLC_E2E_BARE_SECRET",
            contextDir.wstring(),
            dockerfilePath.wstring(),
            BuiltImageSecretBareId.NameAndTag()));
        buildResult.Verify({.ExitCode = 0});

        auto inspectData = InspectImage(BuiltImageSecretBareId.NameAndTag());
        VERIFY_IS_TRUE(inspectData.RepoTags.has_value());
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Build_Secret_BareIdUnsetVar_Fails)
    {
        // Docker parity: '--secret id=NAME' with no env=/src= reads the host env var named NAME, and
        // errors when that variable is unset (unlike an explicit 'env=', which yields an empty value).
        constexpr auto envName = L"WSLC_E2E_SECRET_BARE_ID_UNSET";
        ScopedEnvVariable envVar(envName); // Clears it (restoring any prior value on exit) so a leaked value can't taint the test.

        auto testRoot = std::filesystem::current_path() / L"wslc-e2e-build-secret-bare-id-unset";
        auto cleanup = SetupTestDirectory(testRoot);

        auto contextDir = testRoot / L"context";
        std::error_code ec;
        std::filesystem::create_directories(contextDir, ec);
        THROW_HR_IF(E_FAIL, ec.value() != 0 || !std::filesystem::exists(contextDir));

        auto dockerfilePath = testRoot / L"Dockerfile";
        WriteTestFileContent(dockerfilePath, "FROM debian:latest\n");

        auto buildResult = RunWslc(std::format(
            L"build \"{}\" -f \"{}\" --secret id=WSLC_E2E_SECRET_BARE_ID_UNSET", contextDir.wstring(), dockerfilePath.wstring()));
        VERIFY_ARE_EQUAL(1u, buildResult.ExitCode.value_or(0u));
        VERIFY_IS_TRUE(buildResult.Stderr.has_value());
        VERIFY_IS_FALSE(buildResult.Stderr->empty());
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Build_Secret_MissingEnvVar_EmptyValue_Success)
    {
        // Docker parity: an unset environment variable yields an empty secret value, not an error.
        constexpr auto envName = L"WSLC_E2E_SECRET_UNSET_VAR";
        ScopedEnvVariable envVar(envName); // Clears it (restoring any prior value on exit) so a leaked value can't taint the test.

        auto imageCleanup = DeleteImageOnExit(BuiltImageSecretMissingEnv);
        auto testRoot = std::filesystem::current_path() / L"wslc-e2e-build-secret-missing";
        auto cleanup = SetupTestDirectory(testRoot);

        auto contextDir = SharedSecretBuildContext();

        auto dockerfilePath = testRoot / L"Dockerfile";
        WriteTestFileContent(
            dockerfilePath,
            "# syntax=docker/dockerfile:1\n"
            "FROM debian:latest\n"
            "RUN --mount=type=secret,id=mysecret [ -z \"$(cat /run/secrets/mysecret)\" ]\n"
            "CMD [\"echo\", \"secret-empty-ok\"]\n");

        auto buildResult = RunWslc(std::format(
            L"build \"{}\" -f \"{}\" -t {} --secret id=mysecret,env=WSLC_E2E_SECRET_UNSET_VAR",
            contextDir.wstring(),
            dockerfilePath.wstring(),
            BuiltImageSecretMissingEnv.NameAndTag()));
        buildResult.Verify({.ExitCode = 0});

        auto inspectData = InspectImage(BuiltImageSecretMissingEnv.NameAndTag());
        VERIFY_IS_TRUE(inspectData.RepoTags.has_value());
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Build_Secret_Src_Success)
    {
        auto imageCleanup = DeleteImageOnExit(BuiltImageSecretSrc);
        auto testRoot = std::filesystem::current_path() / L"wslc-e2e-build-secret-src";
        auto cleanup = SetupTestDirectory(testRoot);

        auto contextDir = SharedSecretBuildContext();
        std::error_code ec;

        // Place the secret OUTSIDE the build context; the server mounts the secret file's parent
        // directory read-only and references the file in place, so its bytes are never copied.
        auto secretDir = testRoot / L"secrets";
        std::filesystem::create_directories(secretDir, ec);
        THROW_HR_IF(E_FAIL, ec.value() != 0 || !std::filesystem::exists(secretDir));
        auto secretFile = secretDir / L"token.txt";
        WriteTestFileContent(secretFile, "file-secret-content-67890");

        auto dockerfilePath = testRoot / L"Dockerfile";
        WriteTestFileContent(
            dockerfilePath,
            "# syntax=docker/dockerfile:1\n"
            "FROM debian:latest\n"
            "RUN --mount=type=secret,id=mysecret "
            "[ \"$(cat /run/secrets/mysecret)\" = \"file-secret-content-67890\" ]\n"
            "CMD [\"echo\", \"secret-src-ok\"]\n");

        auto buildResult = RunWslc(std::format(
            L"build \"{}\" -f \"{}\" -t {} --secret id=mysecret,src=\"{}\"",
            contextDir.wstring(),
            dockerfilePath.wstring(),
            BuiltImageSecretSrc.NameAndTag(),
            secretFile.wstring()));
        buildResult.Verify({.ExitCode = 0});

        auto inspectData = InspectImage(BuiltImageSecretSrc.NameAndTag());
        VERIFY_IS_TRUE(inspectData.RepoTags.has_value());
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Build_Secret_SrcSymlink_Success)
    {
        // A symlink whose target lives in a separate directory must resolve to the target's content.
        // The client canonicalizes the link to its target; the server mounts the *target's* parent
        // directory read-only and references the resolved file in place, so its bytes are never copied.
        auto imageCleanup = DeleteImageOnExit(BuiltImageSecretSrcSymlink);
        auto testRoot = std::filesystem::current_path() / L"wslc-e2e-build-secret-src-symlink";
        auto cleanup = SetupTestDirectory(testRoot);

        auto contextDir = SharedSecretBuildContext();
        std::error_code ec;

        auto targetDir = testRoot / L"target";
        std::filesystem::create_directories(targetDir, ec);
        THROW_HR_IF(E_FAIL, ec.value() != 0 || !std::filesystem::exists(targetDir));
        auto targetFile = targetDir / L"real-secret.txt";
        WriteTestFileContent(targetFile, "symlinked-secret-content-44444");

        auto linkDir = testRoot / L"links";
        std::filesystem::create_directories(linkDir, ec);
        THROW_HR_IF(E_FAIL, ec.value() != 0 || !std::filesystem::exists(linkDir));
        auto linkFile = linkDir / L"token.txt";
        std::filesystem::create_symlink(targetFile, linkFile);

        auto dockerfilePath = testRoot / L"Dockerfile";
        WriteTestFileContent(
            dockerfilePath,
            "# syntax=docker/dockerfile:1\n"
            "FROM debian:latest\n"
            "RUN --mount=type=secret,id=mysecret "
            "[ \"$(cat /run/secrets/mysecret)\" = \"symlinked-secret-content-44444\" ]\n"
            "CMD [\"echo\", \"secret-symlink-ok\"]\n");

        auto buildResult = RunWslc(std::format(
            L"build \"{}\" -f \"{}\" -t {} --secret id=mysecret,src=\"{}\"",
            contextDir.wstring(),
            dockerfilePath.wstring(),
            BuiltImageSecretSrcSymlink.NameAndTag(),
            linkFile.wstring()));
        buildResult.Verify({.ExitCode = 0});

        auto inspectData = InspectImage(BuiltImageSecretSrcSymlink.NameAndTag());
        VERIFY_IS_TRUE(inspectData.RepoTags.has_value());
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Build_Secret_SrcFileMissing_Fails)
    {
        auto testRoot = std::filesystem::current_path() / L"wslc-e2e-build-secret-src-missing";
        auto cleanup = SetupTestDirectory(testRoot);

        auto contextDir = testRoot / L"context";
        std::error_code ec;
        std::filesystem::create_directories(contextDir, ec);
        THROW_HR_IF(E_FAIL, ec.value() != 0 || !std::filesystem::exists(contextDir));

        auto dockerfilePath = testRoot / L"Dockerfile";
        WriteTestFileContent(dockerfilePath, "FROM debian:latest\n");

        // Build should fail if the src file does not exist
        auto missingFile = testRoot / L"does-not-exist.txt";
        auto buildResult = RunWslc(std::format(
            L"build \"{}\" -f \"{}\" --secret id=x,src=\"{}\"", contextDir.wstring(), dockerfilePath.wstring(), missingFile.wstring()));
        VERIFY_ARE_EQUAL(1u, buildResult.ExitCode.value_or(0u));
        VERIFY_IS_TRUE(buildResult.Stderr.has_value());
        VERIFY_IS_FALSE(buildResult.Stderr->empty());
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Build_Secret_EnvAndSrc_EnvWins_Success)
    {
        // Docker parity: when both 'env=' and 'src=' are given, the environment variable wins and
        // the file path is ignored (no error).
        constexpr auto envName = L"WSLC_E2E_ENV_WINS_VALUE";
        constexpr auto envValue = L"env-wins-content-55555";
        ScopedEnvVariable envVar(envName, envValue);

        auto imageCleanup = DeleteImageOnExit(BuiltImageSecretEnvWins);
        auto testRoot = std::filesystem::current_path() / L"wslc-e2e-build-secret-both";
        auto cleanup = SetupTestDirectory(testRoot);

        auto contextDir = SharedSecretBuildContext();

        // The src file holds different content; it must be ignored in favor of the env value.
        auto secretFile = testRoot / L"ignored.txt";
        WriteTestFileContent(secretFile, "this-file-should-be-ignored");

        auto dockerfilePath = testRoot / L"Dockerfile";
        WriteTestFileContent(
            dockerfilePath,
            "# syntax=docker/dockerfile:1\n"
            "FROM debian:latest\n"
            "RUN --mount=type=secret,id=mysecret "
            "[ \"$(cat /run/secrets/mysecret)\" = \"env-wins-content-55555\" ]\n"
            "CMD [\"echo\", \"secret-env-wins-ok\"]\n");

        auto buildResult = RunWslc(std::format(
            L"build \"{}\" -f \"{}\" -t {} --secret id=mysecret,env=WSLC_E2E_ENV_WINS_VALUE,src=\"{}\"",
            contextDir.wstring(),
            dockerfilePath.wstring(),
            BuiltImageSecretEnvWins.NameAndTag(),
            secretFile.wstring()));
        buildResult.Verify({.ExitCode = 0});

        auto inspectData = InspectImage(BuiltImageSecretEnvWins.NameAndTag());
        VERIFY_IS_TRUE(inspectData.RepoTags.has_value());
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Build_Secret_TypeEnv_Success)
    {
        constexpr auto envName = L"WSLC_E2E_TYPE_ENV_VALUE";
        constexpr auto envValue = L"type-env-content-11111";
        ScopedEnvVariable envVar(envName, envValue);

        auto imageCleanup = DeleteImageOnExit(BuiltImageSecretTypeEnv);
        auto testRoot = std::filesystem::current_path() / L"wslc-e2e-build-secret-type-env";
        auto cleanup = SetupTestDirectory(testRoot);

        auto contextDir = SharedSecretBuildContext();

        auto dockerfilePath = testRoot / L"Dockerfile";
        WriteTestFileContent(
            dockerfilePath,
            "# syntax=docker/dockerfile:1\n"
            "FROM debian:latest\n"
            "RUN --mount=type=secret,id=mysecret "
            "[ \"$(cat /run/secrets/mysecret)\" = \"type-env-content-11111\" ]\n"
            "CMD [\"echo\", \"secret-ok\"]\n");

        auto buildResult = RunWslc(std::format(
            L"build \"{}\" -f \"{}\" -t {} --secret type=env,id=mysecret,env=WSLC_E2E_TYPE_ENV_VALUE",
            contextDir.wstring(),
            dockerfilePath.wstring(),
            BuiltImageSecretTypeEnv.NameAndTag()));
        buildResult.Verify({.ExitCode = 0});

        auto inspectData = InspectImage(BuiltImageSecretTypeEnv.NameAndTag());
        VERIFY_IS_TRUE(inspectData.RepoTags.has_value());
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Build_Secret_TypeEnvSrcIsEnvName_Success)
    {
        // Docker parity: with type=env, a bare src= names the env var to read (not a file path).
        constexpr auto envName = L"WSLC_E2E_TYPE_ENV_SRC_VALUE";
        constexpr auto envValue = L"type-env-src-content-22222";
        ScopedEnvVariable envVar(envName, envValue);

        auto imageCleanup = DeleteImageOnExit(BuiltImageSecretTypeEnvSrc);
        auto testRoot = std::filesystem::current_path() / L"wslc-e2e-build-secret-type-env-src";
        auto cleanup = SetupTestDirectory(testRoot);

        auto contextDir = SharedSecretBuildContext();

        auto dockerfilePath = testRoot / L"Dockerfile";
        WriteTestFileContent(
            dockerfilePath,
            "# syntax=docker/dockerfile:1\n"
            "FROM debian:latest\n"
            "RUN --mount=type=secret,id=mysecret "
            "[ \"$(cat /run/secrets/mysecret)\" = \"type-env-src-content-22222\" ]\n"
            "CMD [\"echo\", \"secret-ok\"]\n");

        auto buildResult = RunWslc(std::format(
            L"build \"{}\" -f \"{}\" -t {} --secret type=env,id=mysecret,src=WSLC_E2E_TYPE_ENV_SRC_VALUE",
            contextDir.wstring(),
            dockerfilePath.wstring(),
            BuiltImageSecretTypeEnvSrc.NameAndTag()));
        buildResult.Verify({.ExitCode = 0});

        auto inspectData = InspectImage(BuiltImageSecretTypeEnvSrc.NameAndTag());
        VERIFY_IS_TRUE(inspectData.RepoTags.has_value());
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Build_Secret_TypeFile_Success)
    {
        auto imageCleanup = DeleteImageOnExit(BuiltImageSecretTypeFile);
        auto testRoot = std::filesystem::current_path() / L"wslc-e2e-build-secret-type-file";
        auto cleanup = SetupTestDirectory(testRoot);

        auto contextDir = SharedSecretBuildContext();

        auto secretFile = testRoot / L"token.txt";
        WriteTestFileContent(secretFile, "type-file-content-33333");

        auto dockerfilePath = testRoot / L"Dockerfile";
        WriteTestFileContent(
            dockerfilePath,
            "# syntax=docker/dockerfile:1\n"
            "FROM debian:latest\n"
            "RUN --mount=type=secret,id=mysecret "
            "[ \"$(cat /run/secrets/mysecret)\" = \"type-file-content-33333\" ]\n"
            "CMD [\"echo\", \"secret-ok\"]\n");

        auto buildResult = RunWslc(std::format(
            L"build \"{}\" -f \"{}\" -t {} --secret type=file,id=mysecret,src=\"{}\"",
            contextDir.wstring(),
            dockerfilePath.wstring(),
            BuiltImageSecretTypeFile.NameAndTag(),
            secretFile.wstring()));
        buildResult.Verify({.ExitCode = 0});

        auto inspectData = InspectImage(BuiltImageSecretTypeFile.NameAndTag());
        VERIFY_IS_TRUE(inspectData.RepoTags.has_value());
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Build_Secret_BinaryFile_Success)
    {
        // A file secret must be delivered byte-for-byte, including an embedded NUL and high bytes that
        // an environment-variable (NUL-terminated, text-only) transport could never carry. The content
        // below is 13 bytes with a NUL at offset 6; the in-container checks assert both the exact byte
        // count (proving no NUL truncation) and that the bytes on either side of the NUL survived.
        auto imageCleanup = DeleteImageOnExit(BuiltImageSecretBinary);
        auto testRoot = std::filesystem::current_path() / L"wslc-e2e-build-secret-binary";
        auto cleanup = SetupTestDirectory(testRoot);

        auto contextDir = SharedSecretBuildContext();

        auto secretFile = testRoot / L"blob.bin";
        WriteTestFileContent(secretFile, std::string("before\0after\xff", 13));

        auto dockerfilePath = testRoot / L"Dockerfile";
        WriteTestFileContent(
            dockerfilePath,
            "# syntax=docker/dockerfile:1\n"
            "FROM debian:latest\n"
            "RUN --mount=type=secret,id=mysecret "
            "[ \"$(wc -c < /run/secrets/mysecret)\" = \"13\" ] && "
            "[ \"$(tr -d '\\000' < /run/secrets/mysecret | tr -d '\\377')\" = \"beforeafter\" ]\n"
            "CMD [\"echo\", \"secret-binary-ok\"]\n");

        auto buildResult = RunWslc(std::format(
            L"build \"{}\" -f \"{}\" -t {} --secret type=file,id=mysecret,src=\"{}\"",
            contextDir.wstring(),
            dockerfilePath.wstring(),
            BuiltImageSecretBinary.NameAndTag(),
            secretFile.wstring()));
        buildResult.Verify({.ExitCode = 0});

        auto inspectData = InspectImage(BuiltImageSecretBinary.NameAndTag());
        VERIFY_IS_TRUE(inspectData.RepoTags.has_value());
    }

    // Builds a file secret of the given size (filled with a single repeated byte) and asserts, inside the
    // container, both the exact byte count and that every byte survived intact. Verifies the client->service
    // transport carries the secret byte-for-byte regardless of size.
    void RunSizedFileSecretSuccess(const TestImage& image, const std::wstring& subdir, size_t size)
    {
        auto imageCleanup = DeleteImageOnExit(image);
        auto testRoot = std::filesystem::current_path() / subdir;
        auto cleanup = SetupTestDirectory(testRoot);

        auto contextDir = SharedSecretBuildContext();

        auto secretFile = testRoot / L"secret.bin";
        WriteTestFileContent(secretFile, std::string(size, 'A'));

        auto dockerfilePath = testRoot / L"Dockerfile";
        WriteTestFileContent(
            dockerfilePath,
            std::format(
                "# syntax=docker/dockerfile:1\n"
                "FROM debian:latest\n"
                "RUN --mount=type=secret,id=mysecret "
                "[ \"$(wc -c < /run/secrets/mysecret)\" = \"{}\" ] && "
                "[ -z \"$(tr -d 'A' < /run/secrets/mysecret)\" ]\n"
                "CMD [\"echo\", \"secret-size-ok\"]\n",
                size));

        auto buildResult = RunWslc(std::format(
            L"build \"{}\" -f \"{}\" -t {} --secret id=mysecret,src=\"{}\"",
            contextDir.wstring(),
            dockerfilePath.wstring(),
            image.NameAndTag(),
            secretFile.wstring()));
        buildResult.Verify({.ExitCode = 0});

        auto inspectData = InspectImage(image.NameAndTag());
        VERIFY_IS_TRUE(inspectData.RepoTags.has_value());
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Build_Secret_EmptyFile_Success)
    {
        // A zero-byte file secret must mount as an empty (but present) file.
        auto imageCleanup = DeleteImageOnExit(BuiltImageSecretEmptyFile);
        auto testRoot = std::filesystem::current_path() / L"wslc-e2e-build-secret-empty-file";
        auto cleanup = SetupTestDirectory(testRoot);

        auto contextDir = SharedSecretBuildContext();

        auto secretFile = testRoot / L"empty.bin";
        WriteTestFileContent(secretFile, "");

        auto dockerfilePath = testRoot / L"Dockerfile";
        WriteTestFileContent(
            dockerfilePath,
            "# syntax=docker/dockerfile:1\n"
            "FROM debian:latest\n"
            "RUN --mount=type=secret,id=mysecret "
            "[ -f /run/secrets/mysecret ] && [ \"$(wc -c < /run/secrets/mysecret)\" = \"0\" ]\n"
            "CMD [\"echo\", \"secret-empty-ok\"]\n");

        auto buildResult = RunWslc(std::format(
            L"build \"{}\" -f \"{}\" -t {} --secret id=mysecret,src=\"{}\"",
            contextDir.wstring(),
            dockerfilePath.wstring(),
            BuiltImageSecretEmptyFile.NameAndTag(),
            secretFile.wstring()));
        buildResult.Verify({.ExitCode = 0});

        auto inspectData = InspectImage(BuiltImageSecretEmptyFile.NameAndTag());
        VERIFY_IS_TRUE(inspectData.RepoTags.has_value());
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Build_Secret_LargeFile_Success)
    {
        // A mid-size (256 KiB) secret is well within BuildKit's cap and exercises a multi-page transport.
        RunSizedFileSecretSuccess(BuiltImageSecretLarge, L"wslc-e2e-build-secret-large", 256 * 1024);
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Build_Secret_MaxSizeFile_Success)
    {
        // Exactly BuildKit's per-secret cap (500 KiB == 512000 bytes) must still succeed.
        RunSizedFileSecretSuccess(BuiltImageSecretMaxSize, L"wslc-e2e-build-secret-max-size", c_maxSecretSize);
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Build_Secret_OversizeFile_Fails)
    {
        // One byte over BuildKit's per-secret cap (500 KiB + 1). The file is forwarded and mounted, and
        // BuildKit enforces its MaxSecretSize limit when the secret is consumed, so the build fails.
        auto testRoot = std::filesystem::current_path() / L"wslc-e2e-build-secret-oversize";
        auto cleanup = SetupTestDirectory(testRoot);

        auto contextDir = SharedSecretBuildContext();

        auto secretFile = testRoot / L"secret.bin";
        WriteTestFileContent(secretFile, std::string(c_maxSecretSize + 1, 'A'));

        auto dockerfilePath = testRoot / L"Dockerfile";
        WriteTestFileContent(
            dockerfilePath,
            "# syntax=docker/dockerfile:1\n"
            "FROM debian:latest\n"
            "RUN --mount=type=secret,id=mysecret cat /run/secrets/mysecret > /dev/null\n"
            "CMD [\"echo\", \"secret-oversize\"]\n");

        auto buildResult = RunWslc(std::format(
            L"build \"{}\" -f \"{}\" --secret id=mysecret,src=\"{}\"", contextDir.wstring(), dockerfilePath.wstring(), secretFile.wstring()));
        VERIFY_ARE_EQUAL(1u, buildResult.ExitCode.value_or(0u));
        VERIFY_IS_TRUE(buildResult.Stderr.has_value());
        VERIFY_IS_FALSE(buildResult.Stderr->empty());
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Build_Secret_MultipleFiles_Success)
    {
        // Several file secrets in one build: two share a directory (the server mounts it once, deduped)
        // and a third lives elsewhere (a second mount). All three must be delivered with their own
        // content, exercising the multi-mount/dedup path for in-place file secrets.
        auto imageCleanup = DeleteImageOnExit(BuiltImageSecretMultiple);
        auto testRoot = std::filesystem::current_path() / L"wslc-e2e-build-secret-multi";
        auto cleanup = SetupTestDirectory(testRoot);

        auto contextDir = SharedSecretBuildContext();
        std::error_code ec;

        auto dirA = testRoot / L"a";
        auto dirB = testRoot / L"b";
        std::filesystem::create_directories(dirA, ec);
        std::filesystem::create_directories(dirB, ec);
        THROW_HR_IF(E_FAIL, !std::filesystem::exists(dirA) || !std::filesystem::exists(dirB));

        auto secret1 = dirA / L"s1.txt";
        auto secret2 = dirA / L"s2.txt";
        auto secret3 = dirB / L"s3.txt";
        WriteTestFileContent(secret1, "multi-secret-one-11111");
        WriteTestFileContent(secret2, "multi-secret-two-22222");
        WriteTestFileContent(secret3, "multi-secret-three-33333");

        auto dockerfilePath = testRoot / L"Dockerfile";
        WriteTestFileContent(
            dockerfilePath,
            "# syntax=docker/dockerfile:1\n"
            "FROM debian:latest\n"
            "RUN --mount=type=secret,id=s1 --mount=type=secret,id=s2 --mount=type=secret,id=s3 "
            "[ \"$(cat /run/secrets/s1)\" = \"multi-secret-one-11111\" ] && "
            "[ \"$(cat /run/secrets/s2)\" = \"multi-secret-two-22222\" ] && "
            "[ \"$(cat /run/secrets/s3)\" = \"multi-secret-three-33333\" ]\n"
            "CMD [\"echo\", \"secret-multi-ok\"]\n");

        auto buildResult = RunWslc(std::format(
            L"build \"{}\" -f \"{}\" -t {} --secret id=s1,src=\"{}\" --secret id=s2,src=\"{}\" --secret id=s3,src=\"{}\"",
            contextDir.wstring(),
            dockerfilePath.wstring(),
            BuiltImageSecretMultiple.NameAndTag(),
            secret1.wstring(),
            secret2.wstring(),
            secret3.wstring()));
        buildResult.Verify({.ExitCode = 0});

        auto inspectData = InspectImage(BuiltImageSecretMultiple.NameAndTag());
        VERIFY_IS_TRUE(inspectData.RepoTags.has_value());
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Build_Secret_UnknownType_Fails)
    {
        auto testRoot = std::filesystem::current_path() / L"wslc-e2e-build-secret-type-bad";
        auto cleanup = SetupTestDirectory(testRoot);

        auto contextDir = testRoot / L"context";
        std::error_code ec;
        std::filesystem::create_directories(contextDir, ec);
        THROW_HR_IF(E_FAIL, ec.value() != 0 || !std::filesystem::exists(contextDir));

        auto dockerfilePath = testRoot / L"Dockerfile";
        WriteTestFileContent(dockerfilePath, "FROM debian:latest\n");

        auto buildResult =
            RunWslc(std::format(L"build \"{}\" -f \"{}\" --secret id=x,type=bogus", contextDir.wstring(), dockerfilePath.wstring()));
        VERIFY_ARE_EQUAL(1u, buildResult.ExitCode.value_or(0u));
        VERIFY_IS_TRUE(buildResult.Stderr.has_value());
        VERIFY_IS_TRUE(buildResult.Stderr->find(L"Invalid --secret value 'id=x,type=bogus': unsupported secret type 'bogus'") != std::wstring::npos);
    }

    // An invalid --output spec is rejected client-side before any build runs. This exercises the
    // full parser through the real binary and asserts the localized "Invalid --output value" wrapper.
    WSLC_TEST_METHOD(WSLCE2E_Image_Build_Output_UnsupportedType_Fails)
    {
        auto testRoot = std::filesystem::current_path() / L"wslc-e2e-build-output-type-bad";
        auto cleanup = SetupTestDirectory(testRoot);

        auto contextDir = SharedOutputBuildContext();

        auto dockerfilePath = testRoot / L"Dockerfile";
        WriteTestFileContent(dockerfilePath, "FROM debian:latest\n");

        auto buildResult =
            RunWslc(std::format(L"build \"{}\" -f \"{}\" --output type=bogus", contextDir.wstring(), dockerfilePath.wstring()));
        VERIFY_ARE_EQUAL(1u, buildResult.ExitCode.value_or(0u));
        VERIFY_IS_TRUE(buildResult.Stderr.has_value());
        VERIFY_IS_TRUE(buildResult.Stderr->find(L"Invalid --output value 'type=bogus': unsupported output type 'bogus'") != std::wstring::npos);
    }

    // The docker exporter loads the built image into the engine's image store, so the result is
    // host-observable via inspect. The -t flag supplies the tag; the default docker builder does not
    // honor the exporter 'name=' attribute for tagging (that requires the docker-container driver), so
    // these tests deliberately tag with -t rather than name=.
    WSLC_TEST_METHOD(WSLCE2E_Image_Build_Output_TypeDockerWithTagFlag_LoadsIntoStore_Success)
    {
        auto imageCleanup = DeleteImageOnExit(BuiltImageOutputDockerTag);
        auto testRoot = std::filesystem::current_path() / L"wslc-e2e-build-output-docker-tag";
        auto cleanup = SetupTestDirectory(testRoot);

        auto contextDir = SharedOutputBuildContext();

        auto dockerfilePath = testRoot / L"Dockerfile";
        WriteTestFileContent(dockerfilePath, "FROM debian:latest\nCMD [\"echo\", \"output-docker-tag-ok\"]\n");

        auto buildResult = RunWslc(std::format(
            L"build \"{}\" -f \"{}\" -t {} --output type=docker",
            contextDir.wstring(),
            dockerfilePath.wstring(),
            BuiltImageOutputDockerTag.NameAndTag()));
        buildResult.Verify({.Stdout = L"", .ExitCode = 0});

        auto inspectData = InspectImage(BuiltImageOutputDockerTag.NameAndTag());
        VERIFY_IS_TRUE(inspectData.RepoTags.has_value());
        VERIFY_ARE_EQUAL(1u, inspectData.RepoTags.value().size());
        VERIFY_ARE_EQUAL(BuiltImageOutputDockerTag.NameAndTag(), wsl::shared::string::MultiByteToWide(inspectData.RepoTags.value()[0]));
    }

    // The docker exporter produces a complete, correct image (not just a tag). Build with a
    // distinctive CMD and verify it round-trips through inspect, proving --output built a real image.
    WSLC_TEST_METHOD(WSLCE2E_Image_Build_Output_TypeDocker_ProducesImageWithConfig_Success)
    {
        auto imageCleanup = DeleteImageOnExit(BuiltImageOutputDockerConfig);
        auto testRoot = std::filesystem::current_path() / L"wslc-e2e-build-output-docker-config";
        auto cleanup = SetupTestDirectory(testRoot);

        auto contextDir = SharedOutputBuildContext();

        auto dockerfilePath = testRoot / L"Dockerfile";
        WriteTestFileContent(dockerfilePath, "FROM debian:latest\nCMD [\"echo\", \"output-docker-config-ok\"]\n");

        auto buildResult = RunWslc(std::format(
            L"build \"{}\" -f \"{}\" -t {} --output type=docker",
            contextDir.wstring(),
            dockerfilePath.wstring(),
            BuiltImageOutputDockerConfig.NameAndTag()));
        buildResult.Verify({.Stdout = L"", .ExitCode = 0});

        auto inspectData = InspectImage(BuiltImageOutputDockerConfig.NameAndTag());
        VERIFY_IS_TRUE(inspectData.Config.has_value());
        VERIFY_IS_TRUE(inspectData.Config.value().Cmd.has_value());
        const std::vector<std::string> expectedCmd{"echo", "output-docker-config-ok"};
        VERIFY_ARE_EQUAL(expectedCmd, inspectData.Config.value().Cmd.value());
    }

    // The tar exporter streams a filesystem tarball out of the VM to a client-side file. Verify
    // the file is a valid, non-empty tar that contains the marker written by the build. Asserting the
    // file is non-empty guards the regression where the streamed tarball once came back with 0 bytes.
    WSLC_TEST_METHOD(WSLCE2E_Image_Build_Output_TypeTarToFile_ProducesValidTarball_Success)
    {
        auto testRoot = std::filesystem::current_path() / L"wslc-e2e-build-output-tar-file";
        auto cleanup = SetupTestDirectory(testRoot);

        auto contextDir = SharedOutputBuildContext();

        auto dockerfilePath = testRoot / L"Dockerfile";
        WriteTestFileContent(dockerfilePath, "FROM debian:latest\nRUN echo wslc-tar-marker > /wslc-build-marker.txt\n");

        auto tarPath = testRoot / L"out.tar";
        auto buildResult = RunWslc(std::format(
            L"build \"{}\" -f \"{}\" --output type=tar,dest=\"{}\"", contextDir.wstring(), dockerfilePath.wstring(), tarPath.wstring()));
        buildResult.Verify({.ExitCode = 0});

        VERIFY_IS_TRUE(std::filesystem::exists(tarPath));
        VERIFY_IS_TRUE(std::filesystem::file_size(tarPath) > 0, L"the streamed tarball must not be empty");
        VERIFY_IS_TRUE(ListTarEntries(tarPath).find(L"wslc-build-marker.txt") != std::wstring::npos);
    }

    // dest=- streams the tarball to the client's stdout (matching docker). This is the exact path
    // that once regressed to an empty tarball, so it redirects stdout to a file and asserts the result is
    // a non-empty tar containing the build marker.
    WSLC_TEST_METHOD(WSLCE2E_Image_Build_Output_TypeTarToStdout_ProducesValidTarball_Success)
    {
        auto testRoot = std::filesystem::current_path() / L"wslc-e2e-build-output-tar-stdout";
        auto cleanup = SetupTestDirectory(testRoot);

        auto contextDir = SharedOutputBuildContext();

        auto dockerfilePath = testRoot / L"Dockerfile";
        WriteTestFileContent(dockerfilePath, "FROM debian:latest\nRUN echo wslc-tar-marker > /wslc-build-marker.txt\n");

        auto tarPath = testRoot / L"stdout.tar";
        auto buildResult = RunWslcAndRedirectToFile(
            std::format(L"build \"{}\" -f \"{}\" --output type=tar,dest=-", contextDir.wstring(), dockerfilePath.wstring()), tarPath);
        buildResult.Verify({.ExitCode = 0});

        VERIFY_IS_TRUE(std::filesystem::exists(tarPath));
        VERIFY_IS_TRUE(std::filesystem::file_size(tarPath) > 0, L"the streamed tarball must not be empty");
        VERIFY_IS_TRUE(ListTarEntries(tarPath).find(L"wslc-build-marker.txt") != std::wstring::npos);
    }

    // The local exporter writes a Linux directory tree, which cannot be materialized faithfully on a
    // Windows destination, so it is rejected client-side before any build runs.
    WSLC_TEST_METHOD(WSLCE2E_Image_Build_Output_TypeLocalToDirectory_Rejected_Fails)
    {
        auto testRoot = std::filesystem::current_path() / L"wslc-e2e-build-output-local-dir";
        auto cleanup = SetupTestDirectory(testRoot);

        auto contextDir = SharedOutputBuildContext();

        auto dockerfilePath = testRoot / L"Dockerfile";
        WriteTestFileContent(dockerfilePath, "FROM debian:latest\nRUN echo wslc-local-marker > /wslc-build-marker.txt\n");

        auto destDir = testRoot / L"export";
        auto buildResult = RunWslc(std::format(
            L"build \"{}\" -f \"{}\" --output type=local,dest=\"{}\"", contextDir.wstring(), dockerfilePath.wstring(), destDir.wstring()));
        VERIFY_ARE_EQUAL(1u, buildResult.ExitCode.value_or(0u));
        VERIFY_IS_TRUE(buildResult.Stderr.has_value());
        VERIFY_IS_TRUE(buildResult.Stderr->find(L"directory exporters are not supported") != std::wstring::npos);
    }

    // The local exporter is a directory exporter, which is not supported, so it is rejected client-side
    // before any build runs (dest=- included).
    WSLC_TEST_METHOD(WSLCE2E_Image_Build_Output_TypeLocalToStdout_Rejected_Fails)
    {
        auto testRoot = std::filesystem::current_path() / L"wslc-e2e-build-output-local-stdout";
        auto cleanup = SetupTestDirectory(testRoot);

        auto contextDir = SharedOutputBuildContext();

        auto dockerfilePath = testRoot / L"Dockerfile";
        WriteTestFileContent(dockerfilePath, "FROM debian:latest\n");

        auto buildResult =
            RunWslc(std::format(L"build \"{}\" -f \"{}\" --output type=local,dest=-", contextDir.wstring(), dockerfilePath.wstring()));
        VERIFY_ARE_EQUAL(1u, buildResult.ExitCode.value_or(0u));
        VERIFY_IS_TRUE(buildResult.Stderr.has_value());
        VERIFY_IS_TRUE(
            buildResult.Stderr->find(L"Invalid --output value 'type=local,dest=-': directory exporters are not supported") != std::wstring::npos);
    }

    // The image exporter loads the built image into the engine's image store (like the docker
    // exporter with no dest), so the result is host-observable via inspect. Tag with -t.
    WSLC_TEST_METHOD(WSLCE2E_Image_Build_Output_TypeImage_LoadsIntoStore_Success)
    {
        auto imageCleanup = DeleteImageOnExit(BuiltImageOutputImage);
        auto testRoot = std::filesystem::current_path() / L"wslc-e2e-build-output-image";
        auto cleanup = SetupTestDirectory(testRoot);

        auto contextDir = SharedOutputBuildContext();

        auto dockerfilePath = testRoot / L"Dockerfile";
        WriteTestFileContent(dockerfilePath, "FROM debian:latest\nCMD [\"echo\", \"output-image-ok\"]\n");

        auto buildResult = RunWslc(std::format(
            L"build \"{}\" -f \"{}\" -t {} --output type=image",
            contextDir.wstring(),
            dockerfilePath.wstring(),
            BuiltImageOutputImage.NameAndTag()));
        buildResult.Verify({.Stdout = L"", .ExitCode = 0});

        auto inspectData = InspectImage(BuiltImageOutputImage.NameAndTag());
        VERIFY_IS_TRUE(inspectData.RepoTags.has_value());
        VERIFY_ARE_EQUAL(1u, inspectData.RepoTags.value().size());
        VERIFY_ARE_EQUAL(BuiltImageOutputImage.NameAndTag(), wsl::shared::string::MultiByteToWide(inspectData.RepoTags.value()[0]));
    }

    // The cacheonly exporter runs the build only to populate the build cache, producing no image
    // artifact. Verify the build succeeds and, because nothing is exported, the tag is not in the store.
    WSLC_TEST_METHOD(WSLCE2E_Image_Build_Output_TypeCacheOnly_ProducesNoImage_Success)
    {
        auto testRoot = std::filesystem::current_path() / L"wslc-e2e-build-output-cacheonly";
        auto cleanup = SetupTestDirectory(testRoot);

        auto contextDir = SharedOutputBuildContext();

        auto dockerfilePath = testRoot / L"Dockerfile";
        WriteTestFileContent(dockerfilePath, "FROM debian:latest\nCMD [\"echo\", \"cacheonly-ok\"]\n");

        // Guard against a leaked image if cacheonly ever regresses to loading into the store.
        auto imageCleanup = DeleteImageOnExit(BuiltImageOutputCacheOnly);

        auto buildResult = RunWslc(std::format(
            L"build \"{}\" -f \"{}\" -t {} --output type=cacheonly",
            contextDir.wstring(),
            dockerfilePath.wstring(),
            BuiltImageOutputCacheOnly.NameAndTag()));
        buildResult.Verify({.ExitCode = 0});

        // cacheonly exports nothing, so the tag must not resolve in the image store.
        auto inspectResult = RunWslc(std::format(L"image inspect {}", BuiltImageOutputCacheOnly.NameAndTag()));
        VERIFY_ARE_NOT_EQUAL(0u, inspectResult.ExitCode.value_or(0u), L"cacheonly must not load an image into the store");
    }

    // tar with no 'dest=' defaults to streaming a tarball to stdout ('dest=-'), matching buildx.
    // Redirect stdout to a file and assert the result is a non-empty tar containing the build marker.
    WSLC_TEST_METHOD(WSLCE2E_Image_Build_Output_TypeTarNoDest_StreamsToStdout_Success)
    {
        auto testRoot = std::filesystem::current_path() / L"wslc-e2e-build-output-tar-nodest";
        auto cleanup = SetupTestDirectory(testRoot);

        auto contextDir = SharedOutputBuildContext();

        auto dockerfilePath = testRoot / L"Dockerfile";
        WriteTestFileContent(dockerfilePath, "FROM debian:latest\nRUN echo wslc-tar-nodest-marker > /wslc-build-marker.txt\n");

        auto tarPath = testRoot / L"stdout.tar";
        auto buildResult = RunWslcAndRedirectToFile(
            std::format(L"build \"{}\" -f \"{}\" --output type=tar", contextDir.wstring(), dockerfilePath.wstring()), tarPath);
        buildResult.Verify({.ExitCode = 0});

        VERIFY_IS_TRUE(std::filesystem::exists(tarPath));
        VERIFY_IS_TRUE(std::filesystem::file_size(tarPath) > 0, L"the streamed tarball must not be empty");
        VERIFY_IS_TRUE(ListTarEntries(tarPath).find(L"wslc-build-marker.txt") != std::wstring::npos);
    }

    // A failing build step must surface as a non-zero exit with the image exporter, and the tag
    // must not be left in the store. This is the failing counterpart to TypeImage_LoadsIntoStore.
    WSLC_TEST_METHOD(WSLCE2E_Image_Build_Output_TypeImage_BuildFailure_Fails)
    {
        auto imageCleanup = DeleteImageOnExit(BuiltImageOutputImageFail);
        auto testRoot = std::filesystem::current_path() / L"wslc-e2e-build-output-image-fail";
        auto cleanup = SetupTestDirectory(testRoot);

        auto contextDir = SharedOutputBuildContext();

        auto dockerfilePath = testRoot / L"Dockerfile";
        WriteTestFileContent(dockerfilePath, "FROM debian:latest\nRUN exit 7\n");

        auto buildResult = RunWslc(std::format(
            L"build \"{}\" -f \"{}\" -t {} --output type=image",
            contextDir.wstring(),
            dockerfilePath.wstring(),
            BuiltImageOutputImageFail.NameAndTag()));
        VERIFY_ARE_EQUAL(1u, buildResult.ExitCode.value_or(0u));
        VERIFY_IS_TRUE(buildResult.StderrContainsSubstring(L"failed to solve"));

        auto inspectResult = RunWslc(std::format(L"image inspect {}", BuiltImageOutputImageFail.NameAndTag()));
        VERIFY_ARE_NOT_EQUAL(0u, inspectResult.ExitCode.value_or(0u), L"a failed build must not leave an image in the store");
    }

    // A failing build step must surface as a non-zero exit with the cacheonly exporter. This is
    // the failing counterpart to TypeCacheOnly_ProducesNoImage.
    WSLC_TEST_METHOD(WSLCE2E_Image_Build_Output_TypeCacheOnly_BuildFailure_Fails)
    {
        auto testRoot = std::filesystem::current_path() / L"wslc-e2e-build-output-cacheonly-fail";
        auto cleanup = SetupTestDirectory(testRoot);

        auto contextDir = SharedOutputBuildContext();

        auto dockerfilePath = testRoot / L"Dockerfile";
        WriteTestFileContent(dockerfilePath, "FROM debian:latest\nRUN exit 7\n");

        auto buildResult =
            RunWslc(std::format(L"build \"{}\" -f \"{}\" --output type=cacheonly", contextDir.wstring(), dockerfilePath.wstring()));
        VERIFY_ARE_EQUAL(1u, buildResult.ExitCode.value_or(0u));
        VERIFY_IS_TRUE(buildResult.StderrContainsSubstring(L"failed to solve"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Build_DockerfileInContextDir_Success)
    {
        auto imageCleanup = DeleteImageOnExit(BuiltImageDockerfile);
        BuildFromContextFile(L"Dockerfile", BuiltImageDockerfile);
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Build_ContainerfileInContextDir_Success)
    {
        auto imageCleanup = DeleteImageOnExit(BuiltImageContainerfile);
        BuildFromContextFile(L"Containerfile", BuiltImageContainerfile);
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Build_BothDockerfileAndContainerfile_Fails)
    {
        auto testRoot = std::filesystem::current_path() / L"wslc-e2e-build-both-files";
        auto cleanup = SetupTestDirectory(testRoot);

        WriteTestFileContent(testRoot / L"Dockerfile", "FROM debian:latest\n");
        WriteTestFileContent(testRoot / L"Containerfile", "FROM debian:latest\n");

        auto buildResult = RunWslc(std::format(L"build \"{}\"", testRoot.wstring()));
        buildResult.Verify(
            {.Stderr =
                 FormatErrorMessage(L"Both Dockerfile and Containerfile found. Use -f to select the file to use", L"E_INVALIDARG"),
             .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Build_NeitherDockerfileNorContainerfile_Fails)
    {
        auto testRoot = std::filesystem::current_path() / L"wslc-e2e-build-no-files";
        auto cleanup = SetupTestDirectory(testRoot);

        auto absolutePath = std::filesystem::absolute(testRoot);
        auto buildResult = RunWslc(std::format(L"build \"{}\"", testRoot.wstring()));
        buildResult.Verify(
            {.Stderr = FormatErrorMessage(
                 std::format(L"No Containerfile or Dockerfile found in '{}'", absolutePath.wstring()), L"E_INVALIDARG"),
             .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Build_ContainerfileAccessDenied_Fails)
    {
        auto testRoot = std::filesystem::current_path() / L"wslc-e2e-build-access-denied";
        auto cleanup = SetupTestDirectory(testRoot);

        auto containerfilePath = testRoot / L"Containerfile";
        WriteTestFileContent(containerfilePath, "FROM debian:latest\n");

        // Deny read access so wslc cannot open the file.
        SetPathAccess(containerfilePath, GENERIC_READ, DENY_ACCESS);

        auto restore = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [containerfilePath]() { DeleteFileW(containerfilePath.c_str()); });

        auto absoluteContainerfilePath = std::filesystem::absolute(containerfilePath);
        auto buildResult = RunWslc(std::format(L"build \"{}\"", testRoot.wstring()));
        buildResult.Verify(
            {.Stderr = FormatErrorMessage(
                 std::format(L"Failed to open '{}': Access is denied. ", absoluteContainerfilePath.wstring()), L"E_ACCESSDENIED"),
             .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Build_NoCache_Success)
    {
        auto imageCleanup = DeleteImageOnExit(BuiltImageNoCache);
        auto testRoot = std::filesystem::current_path() / L"wslc-e2e-build-no-cache";
        auto cleanup = SetupTestDirectory(testRoot);

        auto contextDir = testRoot / L"context";
        std::error_code ec;
        std::filesystem::create_directories(contextDir, ec);
        THROW_HR_IF(E_FAIL, ec.value() != 0 || !std::filesystem::exists(contextDir));

        // `RUN date +%N` produces a different output each invocation, so without caching the
        // resulting layer (and therefore the image id) changes every build.
        auto dockerfilePath = testRoot / L"Dockerfile";
        WriteTestFileContent(
            dockerfilePath,
            "FROM debian:latest\n"
            "RUN date +%N > /timestamp.txt\n");

        const auto buildCmd =
            std::format(L"build \"{}\" -f \"{}\" -t {}", contextDir.wstring(), dockerfilePath.wstring(), BuiltImageNoCache.NameAndTag());

        // Seed the cache.
        auto firstBuild = RunWslc(buildCmd);
        firstBuild.Verify({.Stdout = L"", .ExitCode = 0});
        const auto firstId = InspectImage(BuiltImageNoCache.NameAndTag()).Id;
        VERIFY_ARE_NOT_EQUAL(std::string{}, firstId);

        // A repeated build without --no-cache should hit the cache and produce the same id.
        auto cachedBuild = RunWslc(buildCmd);
        cachedBuild.Verify({.Stdout = L"", .ExitCode = 0});
        const auto cachedId = InspectImage(BuiltImageNoCache.NameAndTag()).Id;
        VERIFY_ARE_EQUAL(firstId, cachedId, L"Repeated build without --no-cache should reuse the cached layer");
        VERIFY_IS_TRUE(
            cachedBuild.StderrContainsSubstring(L"[2/2] CACHED"),
            L"A reused layer must be reported as cached in the build output");

        // --no-cache must re-run the non-deterministic step, producing a new id.
        auto noCacheBuild = RunWslc(buildCmd + L" --no-cache");
        noCacheBuild.Verify({.Stdout = L"", .ExitCode = 0});
        const auto noCacheId = InspectImage(BuiltImageNoCache.NameAndTag()).Id;
        VERIFY_ARE_NOT_EQUAL(firstId, noCacheId, L"--no-cache must rebuild the non-deterministic RUN step");
        VERIFY_IS_FALSE(
            noCacheBuild.StderrContainsSubstring(L"[2/2] CACHED"),
            L"A step re-run under --no-cache must not be reported as cached");
    }

    // --iidfile writes the built image's ID to the given host path on success, matching docker build
    // --iidfile. The file must contain the same sha256 digest the image is stored under.
    WSLC_TEST_METHOD(WSLCE2E_Image_Build_IidFile_Success)
    {
        auto imageCleanup = DeleteImageOnExit(BuiltImageIidFile);
        auto testRoot = std::filesystem::current_path() / L"wslc-e2e-build-iidfile";
        auto cleanup = SetupTestDirectory(testRoot);

        auto contextDir = SharedOutputBuildContext();

        auto dockerfilePath = testRoot / L"Dockerfile";
        WriteTestFileContent(dockerfilePath, "FROM debian:latest\nRUN echo wslc-iidfile-marker > /marker.txt\n");

        // Point --iidfile at a path that does not yet exist.
        const auto iidFilePath = testRoot / L"image.id";

        auto buildResult = RunWslc(std::format(
            L"build \"{}\" -f \"{}\" -t {} --iidfile \"{}\"",
            contextDir.wstring(),
            dockerfilePath.wstring(),
            BuiltImageIidFile.NameAndTag(),
            iidFilePath.wstring()));
        buildResult.Verify({.ExitCode = 0});

        VERIFY_IS_TRUE(std::filesystem::exists(iidFilePath));
        const auto iid = ReadFileContent(iidFilePath.wstring());
        VERIFY_IS_TRUE(iid.starts_with(L"sha256:"), L"iidfile must contain a sha256 digest");
        VERIFY_ARE_EQUAL(static_cast<size_t>(71), iid.size(), L"iidfile must contain sha256: plus a 64-char hex digest");

        // The digest written to the iidfile must match the ID the image is stored under.
        const auto inspectedId = InspectImage(BuiltImageIidFile.NameAndTag()).Id;
        VERIFY_ARE_EQUAL(inspectedId, wsl::windows::common::string::WideToMultiByte(iid));
    }

    // --iidfile must accept a path relative to the caller's current directory. The client is responsible
    // for making the path absolute before it reaches the service, which rejects non-absolute paths.
    // std::filesystem::weakly_canonical alone is not sufficient here: --iidfile names a file that does
    // not exist yet, so there is no leading element to canonicalize and the path is returned unchanged.
    WSLC_TEST_METHOD(WSLCE2E_Image_Build_IidFile_RelativePath)
    {
        auto imageCleanup = DeleteImageOnExit(BuiltImageIidFileRelative);
        auto testRoot = std::filesystem::current_path() / L"wslc-e2e-build-iidfile-relative";
        auto cleanup = SetupTestDirectory(testRoot);

        auto contextDir = SharedOutputBuildContext();

        auto dockerfilePath = testRoot / L"Dockerfile";
        WriteTestFileContent(dockerfilePath, "FROM debian:latest\nRUN echo wslc-iidfile-relative-marker > /marker.txt\n");

        // Run wslc from testRoot so the --iidfile argument below resolves against it. Declared after
        // the directory cleanup so the working directory is restored before the directory is removed.
        auto originalDirectory = std::filesystem::current_path();
        auto restoreDirectory = wil::scope_exit([&]() {
            std::error_code ec;
            std::filesystem::current_path(originalDirectory, ec);
        });
        std::filesystem::current_path(testRoot);

        const std::wstring relativeIidFile = L"image.id";
        VERIFY_IS_FALSE(std::filesystem::path(relativeIidFile).is_absolute());
        VERIFY_IS_FALSE(std::filesystem::exists(testRoot / relativeIidFile));

        auto buildResult = RunWslc(std::format(
            L"build \"{}\" -f \"{}\" -t {} --iidfile \"{}\"",
            contextDir.wstring(),
            dockerfilePath.wstring(),
            BuiltImageIidFileRelative.NameAndTag(),
            relativeIidFile));
        buildResult.Verify({.ExitCode = 0});

        VERIFY_IS_TRUE(std::filesystem::exists(testRoot / relativeIidFile), L"--iidfile must accept a relative path");
        const auto iid = ReadFileContent((testRoot / relativeIidFile).wstring());
        VERIFY_IS_TRUE(iid.starts_with(L"sha256:"), L"iidfile must contain a sha256 digest");

        // The digest written to the iidfile must match the ID the image is stored under.
        const auto inspectedId = InspectImage(BuiltImageIidFileRelative.NameAndTag()).Id;
        VERIFY_ARE_EQUAL(inspectedId, wsl::windows::common::string::WideToMultiByte(iid));
    }

    // A failing build must not write the iidfile (matching docker: the file only appears on success).
    WSLC_TEST_METHOD(WSLCE2E_Image_Build_IidFile_BuildFailure_NoFileWritten)
    {
        auto testRoot = std::filesystem::current_path() / L"wslc-e2e-build-iidfile-fail";
        auto cleanup = SetupTestDirectory(testRoot);

        auto contextDir = SharedOutputBuildContext();

        auto dockerfilePath = testRoot / L"Dockerfile";
        WriteTestFileContent(dockerfilePath, "FROM debian:latest\nRUN exit 7\n");

        const auto iidFilePath = testRoot / L"image.id";

        auto buildResult = RunWslc(std::format(
            L"build \"{}\" -f \"{}\" --iidfile \"{}\"", contextDir.wstring(), dockerfilePath.wstring(), iidFilePath.wstring()));
        VERIFY_ARE_EQUAL(1u, buildResult.ExitCode.value_or(0u));
        VERIFY_IS_FALSE(std::filesystem::exists(iidFilePath), L"a failed build must not leave an iidfile behind");
    }

    // Unlike --output, --iidfile does not create a missing parent directory (matching docker). The server
    // mounts the parent into the VM, so a missing directory must surface as a clean error, not a crash.
    WSLC_TEST_METHOD(WSLCE2E_Image_Build_IidFile_ParentDirectoryMissing_Fails)
    {
        auto testRoot = std::filesystem::current_path() / L"wslc-e2e-build-iidfile-noparent";
        auto cleanup = SetupTestDirectory(testRoot);

        auto contextDir = SharedOutputBuildContext();

        auto dockerfilePath = testRoot / L"Dockerfile";
        WriteTestFileContent(dockerfilePath, "FROM debian:latest\n");

        const auto iidFilePath = testRoot / L"does-not-exist" / L"image.id";

        auto buildResult = RunWslc(std::format(
            L"build \"{}\" -f \"{}\" --iidfile \"{}\"", contextDir.wstring(), dockerfilePath.wstring(), iidFilePath.wstring()));
        VERIFY_ARE_EQUAL(1u, buildResult.ExitCode.value_or(0u));
        VERIFY_IS_TRUE(buildResult.Stderr.has_value());
        VERIFY_IS_FALSE(buildResult.Stderr->empty());
        VERIFY_IS_FALSE(std::filesystem::exists(iidFilePath));
        VERIFY_IS_FALSE(std::filesystem::exists(iidFilePath.parent_path()), L"--iidfile must not create its parent directory");
    }

    // The iidfile's parent is mounted read-write, but the destination file itself may still be
    // unwritable. buildx must fail rather than silently reporting success.
    WSLC_TEST_METHOD(WSLCE2E_Image_Build_IidFile_NotWritable_Fails)
    {
        auto imageCleanup = DeleteImageOnExit(BuiltImageIidFileNotWritable);
        auto testRoot = std::filesystem::current_path() / L"wslc-e2e-build-iidfile-readonly";
        auto cleanup = SetupTestDirectory(testRoot);

        auto contextDir = SharedOutputBuildContext();

        auto dockerfilePath = testRoot / L"Dockerfile";
        WriteTestFileContent(dockerfilePath, "FROM debian:latest\n");

        // Pre-create the destination and deny write access so buildx cannot write the image ID to it.
        const auto iidFilePath = testRoot / L"image.id";
        WriteTestFileContent(iidFilePath, "original-content");
        SetPathAccess(iidFilePath, GENERIC_WRITE, DENY_ACCESS);

        // The deny ACE also blocks this test from reading the file back, so it must be revoked before
        // any assertion on the contents, and before cleanup can delete the file.
        auto restore = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [iidFilePath]() {
            SetPathAccess(iidFilePath, 0, REVOKE_ACCESS);
            DeleteFileW(iidFilePath.c_str());
        });

        auto buildResult = RunWslc(std::format(
            L"build \"{}\" -f \"{}\" -t {} --iidfile \"{}\"",
            contextDir.wstring(),
            dockerfilePath.wstring(),
            BuiltImageIidFileNotWritable.NameAndTag(),
            iidFilePath.wstring()));
        VERIFY_ARE_EQUAL(1u, buildResult.ExitCode.value_or(0u));
        VERIFY_IS_TRUE(buildResult.Stderr.has_value());
        VERIFY_IS_FALSE(buildResult.Stderr->empty());

        // buildx truncates the destination when it opens it, so the previous contents are not preserved.
        // What matters is that a failed write never leaves an image ID behind.
        SetPathAccess(iidFilePath, 0, REVOKE_ACCESS);
        const auto contents = ReadFileContent(iidFilePath.wstring());
        VERIFY_IS_FALSE(contents.starts_with(L"sha256:"), L"a failed iidfile write must not leave an image ID behind");
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Build_ProgressInvalid_Fails)
    {
        auto testRoot = std::filesystem::current_path() / L"wslc-e2e-build-progress-bad";
        auto cleanup = SetupTestDirectory(testRoot);

        auto contextDir = testRoot / L"context";
        std::error_code ec;
        std::filesystem::create_directories(contextDir, ec);
        THROW_HR_IF(E_FAIL, ec.value() != 0 || !std::filesystem::exists(contextDir));

        auto dockerfilePath = testRoot / L"Dockerfile";
        WriteTestFileContent(dockerfilePath, "FROM debian:latest\n");

        // Invalid --progress values are rejected client-side before any build runs.
        auto buildResult =
            RunWslc(std::format(L"build \"{}\" -f \"{}\" --progress=bogus", contextDir.wstring(), dockerfilePath.wstring()));
        VERIFY_ARE_EQUAL(1u, buildResult.ExitCode.value_or(0u));
        VERIFY_IS_TRUE(buildResult.Stderr.has_value());
        VERIFY_IS_TRUE(buildResult.Stderr->find(L"is not a recognized progress type") != std::wstring::npos);
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Build_ProgressPlain_NoEscapeSequences_Success)
    {
        auto imageCleanup = DeleteImageOnExit(BuiltImageProgressPlain);
        auto testRoot = std::filesystem::current_path() / L"wslc-e2e-build-progress-plain";
        auto cleanup = SetupTestDirectory(testRoot);

        auto contextDir = testRoot / L"context";
        std::error_code ec;
        std::filesystem::create_directories(contextDir, ec);
        THROW_HR_IF(E_FAIL, ec.value() != 0 || !std::filesystem::exists(contextDir));

        auto dockerfilePath = testRoot / L"Dockerfile";
        WriteTestFileContent(dockerfilePath, "FROM debian:latest\nCMD [\"echo\", \"plain-ok\"]\n");

        // plain emits progress text but never color/cursor VT escape sequences (ESC, 0x1b).
        auto buildResult = RunWslc(std::format(
            L"build \"{}\" -f \"{}\" -t {} --progress=plain",
            contextDir.wstring(),
            dockerfilePath.wstring(),
            BuiltImageProgressPlain.NameAndTag()));
        buildResult.Verify({.Stdout = L"", .ExitCode = 0});

        VERIFY_IS_TRUE(buildResult.Stderr.has_value());
        VERIFY_IS_TRUE(buildResult.Stderr->find(L'\x1b') == std::wstring::npos, L"plain mode must not emit VT escape sequences");
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Build_ProgressQuiet_NoProgressOutput_Success)
    {
        auto imageCleanup = DeleteImageOnExit(BuiltImageProgressQuiet);
        auto testRoot = std::filesystem::current_path() / L"wslc-e2e-build-progress-quiet";
        auto cleanup = SetupTestDirectory(testRoot);

        auto contextDir = testRoot / L"context";
        std::error_code ec;
        std::filesystem::create_directories(contextDir, ec);
        THROW_HR_IF(E_FAIL, ec.value() != 0 || !std::filesystem::exists(contextDir));

        auto dockerfilePath = testRoot / L"Dockerfile";
        WriteTestFileContent(dockerfilePath, "FROM debian:latest\nCMD [\"echo\", \"quiet-ok\"]\n");

        // quiet suppresses all progress output on a successful build.
        auto buildResult = RunWslc(std::format(
            L"build \"{}\" -f \"{}\" -t {} --progress=quiet",
            contextDir.wstring(),
            dockerfilePath.wstring(),
            BuiltImageProgressQuiet.NameAndTag()));
        buildResult.Verify({.Stdout = L"", .ExitCode = 0});

        if (buildResult.Stderr.has_value())
        {
            VERIFY_IS_TRUE(buildResult.Stderr->empty(), L"quiet mode must not emit build progress on success");
        }
    }

private:
    const TestImage BuiltImage{L"wslc-e2e-build-empty-context", L"latest", L""};
    const TestImage BuiltImageTag1{L"wslc-e2e-build-args-tags", L"v1", L""};
    const TestImage BuiltImageTag2{L"wslc-e2e-build-args-tags", L"v2", L""};
    const TestImage BuiltImagePull{L"wslc-e2e-build-pull", L"latest", L""};
    const TestImage BuiltImageTarget{L"wslc-e2e-build-target", L"latest", L""};
    const TestImage BuiltImageDockerfile{L"wslc-e2e-build-dockerfile-ctx", L"latest", L""};
    const TestImage BuiltImageContainerfile{L"wslc-e2e-build-containerfile-ctx", L"latest", L""};
    const TestImage BuiltImageNoCache{L"wslc-e2e-build-no-cache", L"latest", L""};
    const TestImage BuiltImageLabel{L"wslc-e2e-build-label", L"latest", L""};
    const TestImage BuiltImageLabelOverride{L"wslc-e2e-build-label-override", L"latest", L""};
    const TestImage BuiltImageSecret{L"wslc-e2e-build-secret-env", L"latest", L""};
    const TestImage BuiltImageSecretBareId{L"wslc-e2e-build-secret-bare-id", L"latest", L""};
    const TestImage BuiltImageSecretMissingEnv{L"wslc-e2e-build-secret-missing-env", L"latest", L""};
    const TestImage BuiltImageSecretEnvWins{L"wslc-e2e-build-secret-env-wins", L"latest", L""};
    const TestImage BuiltImageSecretTypeEnv{L"wslc-e2e-build-secret-type-env", L"latest", L""};
    const TestImage BuiltImageSecretTypeEnvSrc{L"wslc-e2e-build-secret-type-env-src", L"latest", L""};
    const TestImage BuiltImageSecretTypeFile{L"wslc-e2e-build-secret-type-file", L"latest", L""};
    const TestImage BuiltImageSecretSrc{L"wslc-e2e-build-secret-src", L"latest", L""};
    const TestImage BuiltImageSecretSrcSymlink{L"wslc-e2e-build-secret-src-symlink", L"latest", L""};
    const TestImage BuiltImageSecretBinary{L"wslc-e2e-build-secret-binary", L"latest", L""};
    const TestImage BuiltImageSecretEmptyFile{L"wslc-e2e-build-secret-empty-file", L"latest", L""};
    const TestImage BuiltImageSecretLarge{L"wslc-e2e-build-secret-large", L"latest", L""};
    const TestImage BuiltImageSecretMaxSize{L"wslc-e2e-build-secret-max-size", L"latest", L""};
    const TestImage BuiltImageSecretMultiple{L"wslc-e2e-build-secret-multi", L"latest", L""};

    // Maximum secret size allowed by BuildKit (500kb)
    static constexpr size_t c_maxSecretSize = 500 * 1024;

    const TestImage BuiltImageOutputDockerTag{L"wslc-e2e-build-output-docker-tag", L"latest", L""};
    const TestImage BuiltImageOutputDockerConfig{L"wslc-e2e-build-output-docker-config", L"latest", L""};
    const TestImage BuiltImageOutputImage{L"wslc-e2e-build-output-image", L"latest", L""};
    const TestImage BuiltImageOutputImageFail{L"wslc-e2e-build-output-image-fail", L"latest", L""};
    const TestImage BuiltImageOutputCacheOnly{L"wslc-e2e-build-output-cacheonly", L"latest", L""};
    const TestImage BuiltImageIidFile{L"wslc-e2e-build-iidfile", L"latest", L""};
    const TestImage BuiltImageIidFileNotWritable{L"wslc-e2e-build-iidfile-readonly", L"latest", L""};
    const TestImage BuiltImageProgressPlain{L"wslc-e2e-build-progress-plain", L"latest", L""};
    const TestImage BuiltImageProgressQuiet{L"wslc-e2e-build-progress-quiet", L"latest", L""};
    const TestImage BuiltImageIidFileRelative{L"wslc-e2e-build-iidfile-relative", L"latest", L""};

    // Runs `tar.exe -tf <path>` and returns the member listing so tests can assert an exporter produced a
    // valid, non-empty archive that contains an expected entry.
    static std::wstring ListTarEntries(const std::filesystem::path& tarPath)
    {
        auto cmd = std::format(L"tar.exe -tf \"{}\"", tarPath.wstring());
        wsl::windows::common::SubProcess process(nullptr, cmd.c_str());
        auto output = process.RunAndCaptureOutput();
        VERIFY_ARE_EQUAL(0u, output.ExitCode, L"tar.exe failed to list the produced archive");
        return output.Stdout;
    }

    void BuildFromContextFile(const std::wstring& fileName, const TestImage& image)
    {
        auto testRoot = std::filesystem::current_path() / image.Name;
        auto cleanup = SetupTestDirectory(testRoot);

        WriteTestFileContent(testRoot / fileName, "FROM debian:latest\nCMD [\"echo\", \"build-ok\"]\n");

        auto buildResult = RunWslc(std::format(L"build \"{}\" -t {}", testRoot.wstring(), image.NameAndTag()));
        buildResult.Verify({.Stdout = L"", .ExitCode = 0});

        auto inspectData = InspectImage(image.NameAndTag());
        VERIFY_IS_TRUE(inspectData.RepoTags.has_value());
        VERIFY_ARE_EQUAL(1u, inspectData.RepoTags.value().size());
        VERIFY_ARE_EQUAL(image.NameAndTag(), wsl::shared::string::MultiByteToWide(inspectData.RepoTags.value()[0]));
    }
};
} // namespace WSLCE2ETests
