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

namespace WSLCE2ETests {

class WSLCE2EImageBuildTests
{
    WSLC_TEST_CLASS(WSLCE2EImageBuildTests)

    TEST_CLASS_SETUP(ClassSetup)
    {
        DeleteImagesWithRepositoryPrefix(c_builtImagePrefix);
        EnsureImageIsLoaded(DebianTestImage());
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        DeleteImagesWithRepositoryPrefix(c_builtImagePrefix);
        EnsureImageIsDeleted(DebianTestImage());
        return true;
    }

    // Each test owns and cleans up exactly the image(s) it builds via DeleteImageOnExit, so there is
    // no per-method sweep. DeleteImagesWithRepositoryPrefix in the class setup/cleanup above is only a
    // safety net for images left behind by a crashed run.
    static constexpr auto c_builtImagePrefix = L"wslc-e2e-build-";

    // Returns an RAII guard that best-effort deletes the given image when it goes out of scope. It is
    // deliberately non-throwing (no VERIFY) because it may run while the stack unwinds after a test
    // failure; the class-level prune is the authoritative cleanup.
    static auto DeleteImageOnExit(const TestImage& image)
    {
        return wil::scope_exit([image]() {
            try
            {
                RunWslc(std::format(L"image delete --force {}", image.NameAndTag()));
            }
            CATCH_LOG()
        });
    }

    // All secret tests build from this single shared (empty) context directory. The session never
    // releases virtiofs shares (see WSLCVirtualMachine::UnmountWindowsFolder), so every distinct
    // mounted directory permanently consumes one of a small number of share slots for the session's
    // lifetime. Giving each secret test its own context directory would exhaust that budget; reusing
    // one path keeps all secret tests to a single shared slot. The per-test Dockerfile and any secret
    // source files live under each test's own testRoot and are never mounted (the Dockerfile is
    // streamed via -f and file secrets are read client-side).
    static std::filesystem::path SharedSecretBuildContext()
    {
        auto dir = std::filesystem::current_path() / L"wslc-e2e-build-secret-context";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        THROW_HR_IF(E_FAIL, ec.value() != 0 || !std::filesystem::exists(dir));
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
        buildResult.Verify({.Stderr = L"", .ExitCode = 0});

        auto inspectData = InspectImage(BuiltImage.NameAndTag());
        VERIFY_IS_TRUE(inspectData.RepoTags.has_value());
        VERIFY_ARE_EQUAL(1u, inspectData.RepoTags.value().size());
        VERIFY_ARE_EQUAL(BuiltImage.NameAndTag(), wsl::shared::string::MultiByteToWide(inspectData.RepoTags.value()[0]));
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

    WSLC_TEST_METHOD(WSLCE2E_Image_Build_Pull_Success)
    {
        SKIP_TEST_UNSTABLE(); // TODO: Enable when a private image source is available.

        auto imageCleanup = DeleteImageOnExit(BuiltImagePull);
        auto testRoot = std::filesystem::current_path() / L"wslc-e2e-build-pull";
        auto cleanup = SetupTestDirectory(testRoot);

        auto contextDir = testRoot / L"context";
        std::error_code ec;
        std::filesystem::create_directories(contextDir, ec);
        THROW_HR_IF(E_FAIL, ec.value() != 0 || !std::filesystem::exists(contextDir));

        auto dockerfilePath = testRoot / L"Dockerfile";
        WriteTestFileContent(dockerfilePath, "FROM debian:latest\nCMD [\"echo\", \"pull-ok\"]\n");

        // Build with --pull --verbose. When --pull causes docker to resolve the base image
        // from the registry, the FROM step includes a @sha256: digest (e.g.
        // "FROM docker.io/library/debian:latest@sha256:..."). Without --pull, no digest appears.
        auto buildResult = RunWslc(std::format(
            L"build \"{}\" -f \"{}\" -t {} --pull --verbose", contextDir.wstring(), dockerfilePath.wstring(), BuiltImagePull.NameAndTag()));
        buildResult.Verify({.Stderr = L"", .ExitCode = 0});

        VERIFY_IS_TRUE(buildResult.Stdout.has_value());
        VERIFY_IS_TRUE(buildResult.Stdout->find(L"@sha256:") != std::wstring::npos);
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
        buildResult.Verify({.Stderr = L"", .ExitCode = 0});

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
        buildResult.Verify({.Stderr = L"", .ExitCode = 0});

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
        buildResult.Verify({.Stderr = L"", .ExitCode = 0});

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
        THROW_IF_WIN32_BOOL_FALSE(SetEnvironmentVariableW(envName, envValue));
        auto envCleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [envName]() { SetEnvironmentVariableW(envName, nullptr); });

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
        buildResult.Verify({.Stderr = L"", .ExitCode = 0});

        auto inspectData = InspectImage(BuiltImageSecret.NameAndTag());
        VERIFY_IS_TRUE(inspectData.RepoTags.has_value());
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Build_Secret_BareId_UsesEnvNamedById_Success)
    {
        // Docker parity: '--secret id=NAME' with no env=/src= reads the host env var named NAME.
        constexpr auto envName = L"WSLC_E2E_BARE_SECRET";
        constexpr auto envValue = L"bare-id-secret-content-67890";
        THROW_IF_WIN32_BOOL_FALSE(SetEnvironmentVariableW(envName, envValue));
        auto envCleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [envName]() { SetEnvironmentVariableW(envName, nullptr); });

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
        buildResult.Verify({.Stderr = L"", .ExitCode = 0});

        auto inspectData = InspectImage(BuiltImageSecretBareId.NameAndTag());
        VERIFY_IS_TRUE(inspectData.RepoTags.has_value());
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Build_Secret_BareIdUnsetVar_Fails)
    {
        // Docker parity: '--secret id=NAME' with no env=/src= reads the host env var named NAME, and
        // errors when that variable is unset (unlike an explicit 'env=', which yields an empty value).
        constexpr auto envName = L"WSLC_E2E_SECRET_BARE_ID_UNSET";
        SetEnvironmentVariableW(envName, nullptr); // Ensure it's not set even if a prior run leaked.

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
        SetEnvironmentVariableW(envName, nullptr); // Ensure it's not set even if a prior run leaked.

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
        buildResult.Verify({.Stderr = L"", .ExitCode = 0});

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

        // Place the secret OUTSIDE the build context; the client reads its bytes and the server writes
        // them to a tmpfs file inside the VM, so the secret's directory is never mounted.
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
        buildResult.Verify({.Stderr = L"", .ExitCode = 0});

        auto inspectData = InspectImage(BuiltImageSecretSrc.NameAndTag());
        VERIFY_IS_TRUE(inspectData.RepoTags.has_value());
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Build_Secret_SrcSymlink_Success)
    {
        // A symlink whose target lives in a separate directory must resolve to the target's content.
        // The client reads the resolved file's bytes and the server writes them to a tmpfs file inside
        // the VM, so neither the link's nor the target's directory is ever mounted.
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
        buildResult.Verify({.Stderr = L"", .ExitCode = 0});

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

        auto missingFile = testRoot / L"does-not-exist.txt";
        auto buildResult = RunWslc(std::format(
            L"build \"{}\" -f \"{}\" --secret id=x,src=\"{}\"", contextDir.wstring(), dockerfilePath.wstring(), missingFile.wstring()));
        VERIFY_ARE_EQUAL(1u, buildResult.ExitCode.value_or(0u));
        VERIFY_IS_TRUE(buildResult.Stderr.has_value());
        VERIFY_IS_TRUE(buildResult.Stderr->find(L"source file not found or not a regular file") != std::wstring::npos);
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Build_Secret_EnvAndSrc_EnvWins_Success)
    {
        // Docker parity: when both 'env=' and 'src=' are given, the environment variable wins and
        // the file path is ignored (no error).
        constexpr auto envName = L"WSLC_E2E_ENV_WINS_VALUE";
        constexpr auto envValue = L"env-wins-content-55555";
        THROW_IF_WIN32_BOOL_FALSE(SetEnvironmentVariableW(envName, envValue));
        auto envCleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [envName]() { SetEnvironmentVariableW(envName, nullptr); });

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
        buildResult.Verify({.Stderr = L"", .ExitCode = 0});

        auto inspectData = InspectImage(BuiltImageSecretEnvWins.NameAndTag());
        VERIFY_IS_TRUE(inspectData.RepoTags.has_value());
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Build_Secret_TypeEnv_Success)
    {
        constexpr auto envName = L"WSLC_E2E_TYPE_ENV_VALUE";
        constexpr auto envValue = L"type-env-content-11111";
        THROW_IF_WIN32_BOOL_FALSE(SetEnvironmentVariableW(envName, envValue));
        auto envCleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [envName]() { SetEnvironmentVariableW(envName, nullptr); });

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
        buildResult.Verify({.Stderr = L"", .ExitCode = 0});

        auto inspectData = InspectImage(BuiltImageSecretTypeEnv.NameAndTag());
        VERIFY_IS_TRUE(inspectData.RepoTags.has_value());
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Build_Secret_TypeEnvSrcIsEnvName_Success)
    {
        // Docker parity: with type=env, a bare src= names the env var to read (not a file path).
        constexpr auto envName = L"WSLC_E2E_TYPE_ENV_SRC_VALUE";
        constexpr auto envValue = L"type-env-src-content-22222";
        THROW_IF_WIN32_BOOL_FALSE(SetEnvironmentVariableW(envName, envValue));
        auto envCleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [envName]() { SetEnvironmentVariableW(envName, nullptr); });

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
        buildResult.Verify({.Stderr = L"", .ExitCode = 0});

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
        buildResult.Verify({.Stderr = L"", .ExitCode = 0});

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
        buildResult.Verify({.Stderr = L"", .ExitCode = 0});

        auto inspectData = InspectImage(BuiltImageSecretBinary.NameAndTag());
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
        VERIFY_IS_TRUE(
            buildResult.Stderr->find(L"Invalid --secret value 'id=x,type=bogus': unsupported secret type 'bogus'") !=
            std::wstring::npos);
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
        WriteTestFileContent(containerfilePath, "FROM debian:latest\n");

        // Deny read access so wslc cannot open the file.
        SetPathAccess(containerfilePath, GENERIC_READ, DENY_ACCESS);

        auto restore = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [containerfilePath]() { DeleteFileW(containerfilePath.c_str()); });

        auto absoluteContainerfilePath = std::filesystem::absolute(containerfilePath);
        auto buildResult = RunWslc(std::format(L"build \"{}\"", testRoot.wstring()));
        buildResult.Verify(
            {.Stderr = std::format(
                 L"Failed to open '{}': Access is denied. \r\nError code: E_ACCESSDENIED\r\n", absoluteContainerfilePath.wstring()),
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
        firstBuild.Verify({.Stderr = L"", .ExitCode = 0});
        const auto firstId = InspectImage(BuiltImageNoCache.NameAndTag()).Id;
        VERIFY_ARE_NOT_EQUAL(std::string{}, firstId);

        // A repeated build without --no-cache should hit the cache and produce the same id.
        auto cachedBuild = RunWslc(buildCmd);
        cachedBuild.Verify({.Stderr = L"", .ExitCode = 0});
        const auto cachedId = InspectImage(BuiltImageNoCache.NameAndTag()).Id;
        VERIFY_ARE_EQUAL(firstId, cachedId, L"Repeated build without --no-cache should reuse the cached layer");

        // --no-cache must re-run the non-deterministic step, producing a new id.
        auto noCacheBuild = RunWslc(buildCmd + L" --no-cache");
        noCacheBuild.Verify({.Stderr = L"", .ExitCode = 0});
        const auto noCacheId = InspectImage(BuiltImageNoCache.NameAndTag()).Id;
        VERIFY_ARE_NOT_EQUAL(firstId, noCacheId, L"--no-cache must rebuild the non-deterministic RUN step");
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

    void BuildFromContextFile(const std::wstring& fileName, const TestImage& image)
    {
        auto testRoot = std::filesystem::current_path() / image.Name;
        auto cleanup = SetupTestDirectory(testRoot);

        WriteTestFileContent(testRoot / fileName, "FROM debian:latest\nCMD [\"echo\", \"build-ok\"]\n");

        auto buildResult = RunWslc(std::format(L"build \"{}\" -t {}", testRoot.wstring(), image.NameAndTag()));
        buildResult.Verify({.Stderr = L"", .ExitCode = 0});

        auto inspectData = InspectImage(image.NameAndTag());
        VERIFY_IS_TRUE(inspectData.RepoTags.has_value());
        VERIFY_ARE_EQUAL(1u, inspectData.RepoTags.value().size());
        VERIFY_ARE_EQUAL(image.NameAndTag(), wsl::shared::string::MultiByteToWide(inspectData.RepoTags.value()[0]));
    }
};
} // namespace WSLCE2ETests
