// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"
#include "TestImageRegistry.h"

namespace WSLCE2ETests {
using namespace wsl::shared;

class WSLCE2EContainerCpTests
{
    WSLC_TEST_CLASS(WSLCE2EContainerCpTests)

    TEST_CLASS_SETUP(ClassSetup)
    {
        TestImageRegistry::Instance().EnsureLoaded(DebianImage);
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        return true;
    }

    TEST_METHOD_SETUP(MethodSetup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        TarPath = std::filesystem::current_path() / L"wslc-cp-test.tar";
        DeleteFileW(TarPath.c_str());
        return true;
    }

    TEST_METHOD_CLEANUP(MethodCleanup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        DeleteFileW(TarPath.c_str());
        return true;
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Cp_HelpCommand)
    {
        auto result = RunWslc(L"container cp --help");
        VERIFY_IS_TRUE(result.ExitCode.has_value());
        VERIFY_ARE_EQUAL(0u, result.ExitCode.value());
        VERIFY_IS_TRUE(result.Stdout.has_value());
        VERIFY_IS_TRUE(result.Stdout->find(L"container cp") != std::wstring::npos);
        VERIFY_IS_TRUE(result.Stdout->find(L"source") != std::wstring::npos);
        VERIFY_IS_TRUE(result.Stdout->find(L"target") != std::wstring::npos);
        VERIFY_IS_TRUE(result.Stderr.has_value());
        VERIFY_ARE_EQUAL(L"", result.Stderr.value());
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Cp_HelpListsFollowLink)
    {
        auto result = RunWslc(L"container cp --help");
        VERIFY_IS_TRUE(result.ExitCode.has_value());
        VERIFY_ARE_EQUAL(0u, result.ExitCode.value());
        VERIFY_IS_TRUE(result.Stdout.has_value());
        VERIFY_IS_TRUE(result.Stdout->find(L"--follow-link") != std::wstring::npos);
        VERIFY_IS_TRUE(result.Stdout->find(L"-L") != std::wstring::npos);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Cp_MissingBothArgs)
    {
        const auto result = RunWslc(L"container cp");
        VERIFY_IS_TRUE(result.ExitCode.has_value());
        VERIFY_ARE_EQUAL(1u, result.ExitCode.value());
        VERIFY_IS_TRUE(result.Stderr.has_value());
        VERIFY_IS_TRUE(result.Stderr->find(L"Required argument not provided: 'source'") != std::wstring::npos);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Cp_MissingTarget)
    {
        const auto result = RunWslc(L"container cp -");
        VERIFY_IS_TRUE(result.ExitCode.has_value());
        VERIFY_ARE_EQUAL(1u, result.ExitCode.value());
        VERIFY_IS_TRUE(result.Stderr.has_value());
        VERIFY_IS_TRUE(result.Stderr->find(L"Required argument not provided: 'target'") != std::wstring::npos);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Cp_StdinIsTerminal)
    {
        // Running without piped stdin should fail with a terminal error.
        // RunWslcAndRedirectToFile gives the child a real console stdout handle,
        // and since RunWslc pipes NUL to stdin, we use RunWslcAndRedirectToFile
        // with no output path to get a real console for the child.
        const auto result = RunWslcAndRedirectToFile(L"container cp - fakecontainer:/path");
        VERIFY_IS_TRUE(result.ExitCode.has_value());
        VERIFY_ARE_EQUAL(1u, result.ExitCode.value());
        VERIFY_IS_TRUE(result.Stderr.has_value());
        VERIFY_IS_TRUE(result.Stderr->find(L"Cannot read tar data from terminal") != std::wstring::npos);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Cp_SourceNotStdin)
    {
        // A local file that doesn't exist should fail with a "source not found" error.
        // Use RunWslc which pipes NUL to stdin (not a terminal).
        const auto result = RunWslc(L"container cp somefile.tar fakecontainer:/path");
        VERIFY_IS_TRUE(result.ExitCode.has_value());
        VERIFY_ARE_EQUAL(1u, result.ExitCode.value());
        VERIFY_IS_TRUE(result.Stderr.has_value());
        VERIFY_IS_TRUE(result.Stderr->find(L"Source path not found: somefile.tar") != std::wstring::npos);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Cp_InvalidTargetFormat_NoColon)
    {
        // Target must be CONTAINER:PATH — missing colon should fail.
        const auto result = RunWslc(L"container cp - fakecontainer_nopath");
        VERIFY_IS_TRUE(result.ExitCode.has_value());
        VERIFY_ARE_EQUAL(1u, result.ExitCode.value());
        VERIFY_IS_TRUE(result.Stderr.has_value());
        VERIFY_IS_TRUE(result.Stderr->find(L"Invalid copy direction") != std::wstring::npos);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Cp_InvalidTargetFormat_EmptyContainer)
    {
        // Target with empty container name (:path) should fail.
        const auto result = RunWslc(L"container cp - :/path");
        VERIFY_IS_TRUE(result.ExitCode.has_value());
        VERIFY_ARE_EQUAL(1u, result.ExitCode.value());
        VERIFY_IS_TRUE(result.Stderr.has_value());
        VERIFY_IS_TRUE(result.Stderr->find(L"Invalid copy direction") != std::wstring::npos);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Cp_InvalidTargetFormat_EmptyPath)
    {
        // Target with empty path (container:) should fail.
        const auto result = RunWslc(L"container cp - fakecontainer:");
        VERIFY_IS_TRUE(result.ExitCode.has_value());
        VERIFY_ARE_EQUAL(1u, result.ExitCode.value());
        VERIFY_IS_TRUE(result.Stderr.has_value());
        VERIFY_IS_TRUE(result.Stderr->find(L"Invalid destination format") != std::wstring::npos);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Cp_ContainerNotFound)
    {
        // Create a valid tar file to pipe in, but target a nonexistent container.
        CreateTestTarFile();

        const auto result = RunWslcWithStdinFile(std::format(L"container cp - {}:/tmp", InvalidContainerName), TarPath);
        VERIFY_IS_TRUE(result.ExitCode.has_value());
        VERIFY_ARE_EQUAL(1u, result.ExitCode.value());
        VERIFY_IS_TRUE(result.Stderr.has_value());
        VERIFY_IS_TRUE(result.Stderr->find(L"not found") != std::wstring::npos);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Cp_Success)
    {
        // Create and start a container with sleep infinity to keep it running.
        auto runResult =
            RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        runResult.Verify({.Stderr = L"", .ExitCode = 0});

        // Create a test tar file with a known file inside.
        CreateTestTarFile();

        // Cp the tar into the running container.
        const auto cpResult = RunWslcWithStdinFile(std::format(L"container cp - {}:/tmp", WslcContainerName), TarPath);
        cpResult.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        // Verify the file was copied by running a command inside the container.
        const auto execResult = RunWslc(std::format(L"container exec {} cat /tmp/testfile.txt", WslcContainerName));
        VERIFY_IS_TRUE(execResult.ExitCode.has_value());
        VERIFY_ARE_EQUAL(0u, execResult.ExitCode.value());
        VERIFY_IS_TRUE(execResult.Stdout.has_value());
        VERIFY_ARE_EQUAL(L"wslc-cp-test-content\n", execResult.Stdout.value());
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Cp_StdinFromPipe)
    {
        // Validate that cp works when stdin is a pipe (no content-length available),
        // as opposed to a file where GetFileSize can determine the length upfront.
        auto runResult =
            RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        runResult.Verify({.Stderr = L"", .ExitCode = 0});

        CreateTestTarFile();

        // Open the tar file and relay its content into a pipe on a background thread.
        wil::unique_hfile tarFile(
            CreateFileW(TarPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        THROW_LAST_ERROR_IF(!tarFile);

        auto [pipeRead, pipeWrite] = wsl::windows::common::wslutil::OpenAnonymousPipe(0, false, false);

        std::thread relayThread([&tarFile, &pipeWrite] {
            wsl::windows::common::relay::InterruptableRelay(tarFile.get(), pipeWrite.get());
            pipeWrite.reset();
        });

        // Pass the pipe read end as stdin — wslc cannot determine content-length from a pipe.
        const auto cpResult = RunWslc(std::format(L"container cp - {}:/tmp", WslcContainerName), ElevationType::Elevated, pipeRead.get());
        relayThread.join();
        cpResult.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        // Verify the file was copied.
        const auto execResult = RunWslc(std::format(L"container exec {} cat /tmp/testfile.txt", WslcContainerName));
        VERIFY_IS_TRUE(execResult.ExitCode.has_value());
        VERIFY_ARE_EQUAL(0u, execResult.ExitCode.value());
        VERIFY_IS_TRUE(execResult.Stdout.has_value());
        VERIFY_ARE_EQUAL(L"wslc-cp-test-content\n", execResult.Stdout.value());
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Cp_ToStoppedContainer)
    {
        // Create a stopped container (not started).
        auto createResult = RunWslc(std::format(L"container create --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        createResult.Verify({.Stderr = L"", .ExitCode = 0});

        // Create a test tar file.
        CreateTestTarFile();

        // Attempt to cp into the stopped container — Docker should accept this.
        const auto cpResult = RunWslcWithStdinFile(std::format(L"container cp - {}:/tmp", WslcContainerName), TarPath);

        // Docker's PUT /archive works on stopped containers too.
        cpResult.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Cp_ArchiveFlag)
    {
        // Create and start a container.
        auto runResult =
            RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        runResult.Verify({.Stderr = L"", .ExitCode = 0});

        CreateTestTarFile();

        // Cp with -a flag (archive mode preserves uid/gid).
        const auto cpResult = RunWslcWithStdinFile(std::format(L"container cp -a - {}:/tmp", WslcContainerName), TarPath);
        cpResult.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        // Verify the file was copied.
        const auto execResult = RunWslc(std::format(L"container exec {} cat /tmp/testfile.txt", WslcContainerName));
        VERIFY_IS_TRUE(execResult.ExitCode.has_value());
        VERIFY_ARE_EQUAL(0u, execResult.ExitCode.value());
        VERIFY_IS_TRUE(execResult.Stdout.has_value());
        VERIFY_ARE_EQUAL(L"wslc-cp-test-content\n", execResult.Stdout.value());
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Cp_ArchiveFlagLongForm)
    {
        // Create and start a container.
        auto runResult =
            RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        runResult.Verify({.Stderr = L"", .ExitCode = 0});

        CreateTestTarFile();

        // Cp with --archive flag (long form).
        const auto cpResult = RunWslcWithStdinFile(std::format(L"container cp --archive - {}:/tmp", WslcContainerName), TarPath);
        cpResult.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        // Verify the file was copied.
        const auto execResult = RunWslc(std::format(L"container exec {} cat /tmp/testfile.txt", WslcContainerName));
        VERIFY_IS_TRUE(execResult.ExitCode.has_value());
        VERIFY_ARE_EQUAL(0u, execResult.ExitCode.value());
        VERIFY_IS_TRUE(execResult.Stdout.has_value());
        VERIFY_ARE_EQUAL(L"wslc-cp-test-content\n", execResult.Stdout.value());
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Cp_ArchiveFlagEqualsTrue)
    {
        // Test -a=true syntax.
        auto runResult =
            RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        runResult.Verify({.Stderr = L"", .ExitCode = 0});

        CreateTestTarFile();

        const auto cpResult = RunWslcWithStdinFile(std::format(L"container cp -a=true - {}:/tmp", WslcContainerName), TarPath);
        cpResult.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        const auto execResult = RunWslc(std::format(L"container exec {} cat /tmp/testfile.txt", WslcContainerName));
        VERIFY_IS_TRUE(execResult.ExitCode.has_value());
        VERIFY_ARE_EQUAL(0u, execResult.ExitCode.value());
        VERIFY_IS_TRUE(execResult.Stdout.has_value());
        VERIFY_ARE_EQUAL(L"wslc-cp-test-content\n", execResult.Stdout.value());
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Cp_ArchiveFlagEqualsFalse)
    {
        // Test -a=false syntax (no archive mode).
        auto runResult =
            RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        runResult.Verify({.Stderr = L"", .ExitCode = 0});

        CreateTestTarFile();

        const auto cpResult = RunWslcWithStdinFile(std::format(L"container cp -a=false - {}:/tmp", WslcContainerName), TarPath);
        cpResult.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        const auto execResult = RunWslc(std::format(L"container exec {} cat /tmp/testfile.txt", WslcContainerName));
        VERIFY_IS_TRUE(execResult.ExitCode.has_value());
        VERIFY_ARE_EQUAL(0u, execResult.ExitCode.value());
        VERIFY_IS_TRUE(execResult.Stdout.has_value());
        VERIFY_ARE_EQUAL(L"wslc-cp-test-content\n", execResult.Stdout.value());
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Cp_ArchiveLongFormEqualsTrue)
    {
        // Test --archive=true syntax.
        auto runResult =
            RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        runResult.Verify({.Stderr = L"", .ExitCode = 0});

        CreateTestTarFile();

        const auto cpResult = RunWslcWithStdinFile(std::format(L"container cp --archive=true - {}:/tmp", WslcContainerName), TarPath);
        cpResult.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        const auto execResult = RunWslc(std::format(L"container exec {} cat /tmp/testfile.txt", WslcContainerName));
        VERIFY_IS_TRUE(execResult.ExitCode.has_value());
        VERIFY_ARE_EQUAL(0u, execResult.ExitCode.value());
        VERIFY_IS_TRUE(execResult.Stdout.has_value());
        VERIFY_ARE_EQUAL(L"wslc-cp-test-content\n", execResult.Stdout.value());
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Cp_ArchiveLongFormEqualsFalse)
    {
        // Test --archive=false syntax (no archive mode).
        auto runResult =
            RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        runResult.Verify({.Stderr = L"", .ExitCode = 0});

        CreateTestTarFile();

        const auto cpResult = RunWslcWithStdinFile(std::format(L"container cp --archive=false - {}:/tmp", WslcContainerName), TarPath);
        cpResult.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        const auto execResult = RunWslc(std::format(L"container exec {} cat /tmp/testfile.txt", WslcContainerName));
        VERIFY_IS_TRUE(execResult.ExitCode.has_value());
        VERIFY_ARE_EQUAL(0u, execResult.ExitCode.value());
        VERIFY_IS_TRUE(execResult.Stdout.has_value());
        VERIFY_ARE_EQUAL(L"wslc-cp-test-content\n", execResult.Stdout.value());
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Cp_ArchiveFlagInvalidValue)
    {
        // Test -a=invalid should fail with an error.
        CreateTestTarFile();

        const auto result = RunWslcWithStdinFile(std::format(L"container cp -a=invalid - {}:/tmp", WslcContainerName), TarPath);
        VERIFY_IS_TRUE(result.ExitCode.has_value());
        VERIFY_ARE_EQUAL(1u, result.ExitCode.value());
        VERIFY_IS_TRUE(result.Stderr.has_value());
        VERIFY_ARE_NOT_EQUAL(0u, result.Stderr.value().size());
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Cp_LocalFileToContainer)
    {
        // Create and start a container.
        auto runResult =
            RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        runResult.Verify({.Stderr = L"", .ExitCode = 0});

        // Create a local file to copy.
        auto localFile = std::filesystem::current_path() / L"wslc-cp-local-test.txt";
        auto cleanupLocal = wil::scope_exit([&] { DeleteFileW(localFile.c_str()); });

        {
            wil::unique_hfile file(CreateFileW(localFile.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
            THROW_LAST_ERROR_IF(!file);
            const std::string content = "local-file-content\n";
            DWORD written = 0;
            THROW_IF_WIN32_BOOL_FALSE(WriteFile(file.get(), content.data(), static_cast<DWORD>(content.size()), &written, nullptr));
        }

        // Copy local file to container.
        const auto cpResult = RunWslc(std::format(L"container cp {} {}:/tmp/", localFile.wstring(), WslcContainerName));
        cpResult.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        // Verify the file was copied.
        const auto execResult = RunWslc(std::format(L"container exec {} cat /tmp/wslc-cp-local-test.txt", WslcContainerName));
        VERIFY_IS_TRUE(execResult.ExitCode.has_value());
        VERIFY_ARE_EQUAL(0u, execResult.ExitCode.value());
        VERIFY_IS_TRUE(execResult.Stdout.has_value());
        VERIFY_ARE_EQUAL(L"local-file-content\n", execResult.Stdout.value());
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Cp_LocalToContainer_FollowLinkCopiesTargetContents)
    {
        auto runResult =
            RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        runResult.Verify({.Stderr = L"", .ExitCode = 0});

        auto targetFile = std::filesystem::current_path() / L"wslc-cp-followlink-target.txt";
        auto linkFile = std::filesystem::current_path() / L"wslc-cp-followlink-link.txt";
        auto cleanup = wil::scope_exit([&] {
            std::error_code ec;
            std::filesystem::remove(linkFile, ec);
            std::filesystem::remove(targetFile, ec);
        });

        {
            wil::unique_hfile file(CreateFileW(targetFile.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
            THROW_LAST_ERROR_IF(!file);
            const std::string content = "follow-link-local\n";
            DWORD written = 0;
            THROW_IF_WIN32_BOOL_FALSE(WriteFile(file.get(), content.data(), static_cast<DWORD>(content.size()), &written, nullptr));
        }

        THROW_LAST_ERROR_IF(!CreateSymbolicLinkW(linkFile.c_str(), targetFile.c_str(), 0));

        // --follow-link archives what the link points at, so the target's contents land in the container
        // under the name that was asked for.
        const auto cpResult = RunWslc(std::format(L"container cp --follow-link {} {}:/tmp/", linkFile.wstring(), WslcContainerName));
        cpResult.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        const auto catResult = RunWslc(std::format(L"container exec {} cat /tmp/wslc-cp-followlink-link.txt", WslcContainerName));
        catResult.Verify({.Stdout = L"follow-link-local\n", .ExitCode = 0});

        // The copy is a regular file, not a link, and the target's own name is never used as the destination.
        const auto typeCommand = std::format(
            L"container exec {} sh -c \"test -f /tmp/wslc-cp-followlink-link.txt && ! test -L /tmp/wslc-cp-followlink-link.txt "
            L"&& echo regular || echo other\"",
            WslcContainerName);
        const auto typeResult = RunWslc(typeCommand);
        typeResult.Verify({.Stdout = L"regular\n", .ExitCode = 0});

        const auto lookupCommand =
            std::format(L"container exec {} sh -c \"test -e /tmp/wslc-cp-followlink-target.txt && echo present || echo absent\"", WslcContainerName);
        const auto lookupResult = RunWslc(lookupCommand);
        lookupResult.Verify({.Stdout = L"absent\n", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Cp_LocalToContainer_FollowLinkCopiesDirectoryTree)
    {
        auto runResult =
            RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        runResult.Verify({.Stderr = L"", .ExitCode = 0});

        auto targetDir = std::filesystem::current_path() / L"wslc-cp-followlink-targetdir";
        auto linkDir = std::filesystem::current_path() / L"wslc-cp-followlink-dirlink";
        auto cleanup = wil::scope_exit([&] {
            std::error_code ec;
            std::filesystem::remove(linkDir, ec);
            std::filesystem::remove_all(targetDir, ec);
        });

        std::filesystem::create_directories(targetDir);

        {
            const auto inner = targetDir / L"inner.txt";
            wil::unique_hfile file(CreateFileW(inner.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
            THROW_LAST_ERROR_IF(!file);
            const std::string content = "follow-link-dir\n";
            DWORD written = 0;
            THROW_IF_WIN32_BOOL_FALSE(WriteFile(file.get(), content.data(), static_cast<DWORD>(content.size()), &written, nullptr));
        }

        // A link nested inside the tree, to confirm the staged copy keeps inner links as links.
        THROW_LAST_ERROR_IF(!CreateSymbolicLinkW((targetDir / L"nested.txt").c_str(), L"inner.txt", 0));

        THROW_LAST_ERROR_IF(!CreateSymbolicLinkW(linkDir.c_str(), targetDir.c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY));

        // --follow-link archives the directory the link points at, carried under the link's own name.
        const auto cpResult = RunWslc(std::format(L"container cp --follow-link {} {}:/tmp/", linkDir.wstring(), WslcContainerName));
        cpResult.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        const auto catResult = RunWslc(std::format(L"container exec {} cat /tmp/wslc-cp-followlink-dirlink/inner.txt", WslcContainerName));
        catResult.Verify({.Stdout = L"follow-link-dir\n", .ExitCode = 0});

        // The nested link survives as a link rather than being flattened into a copy of its target.
        const auto nestedCommand = std::format(
            L"container exec {} sh -c \"test -L /tmp/wslc-cp-followlink-dirlink/nested.txt && cat "
            L"/tmp/wslc-cp-followlink-dirlink/nested.txt || echo other\"",
            WslcContainerName);
        const auto nestedResult = RunWslc(nestedCommand);
        nestedResult.Verify({.Stdout = L"follow-link-dir\n", .ExitCode = 0});

        // The target directory's own name is never used as the destination.
        const auto lookupDirCommand =
            std::format(L"container exec {} sh -c \"test -e /tmp/wslc-cp-followlink-targetdir && echo present || echo absent\"", WslcContainerName);
        const auto lookupDirResult = RunWslc(lookupDirCommand);
        lookupDirResult.Verify({.Stdout = L"absent\n", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Cp_QuietFlag)
    {
        // Create and start a container.
        auto runResult =
            RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        runResult.Verify({.Stderr = L"", .ExitCode = 0});

        auto localFile = std::filesystem::current_path() / L"wslc-cp-quiet-test.txt";
        auto cleanupLocal = wil::scope_exit([&] { DeleteFileW(localFile.c_str()); });

        {
            wil::unique_hfile file(CreateFileW(localFile.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
            THROW_LAST_ERROR_IF(!file);
            const std::string content = "quiet-content\n";
            DWORD written = 0;
            THROW_IF_WIN32_BOOL_FALSE(WriteFile(file.get(), content.data(), static_cast<DWORD>(content.size()), &written, nullptr));
        }

        // wslc emits no copy progress, so -q is accepted and a quiet copy behaves exactly like an
        // unflagged one.
        const auto uploadResult = RunWslc(std::format(L"container cp -q {} {}:/tmp/", localFile.wstring(), WslcContainerName));
        uploadResult.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        const auto execResult = RunWslc(std::format(L"container exec {} cat /tmp/wslc-cp-quiet-test.txt", WslcContainerName));
        execResult.Verify({.Stdout = L"quiet-content\n", .ExitCode = 0});

        // Long form, on the container -> local direction.
        auto downloadDir = std::filesystem::current_path() / L"wslc-cp-quiet-download";
        std::filesystem::create_directories(downloadDir);
        auto cleanupDir = wil::scope_exit([&] { std::filesystem::remove_all(downloadDir); });

        const auto downloadResult =
            RunWslc(std::format(L"container cp --quiet {}:/tmp/wslc-cp-quiet-test.txt {}", WslcContainerName, downloadDir.wstring()));
        downloadResult.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        VERIFY_IS_TRUE(std::filesystem::exists(downloadDir / L"wslc-cp-quiet-test.txt"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Cp_LocalFileNotFound)
    {
        // Copying a nonexistent local file should fail.
        const auto result = RunWslc(std::format(L"container cp C:\\nonexistent_wslc_test_file.txt {}:/tmp/", WslcContainerName));
        VERIFY_IS_TRUE(result.ExitCode.has_value());
        VERIFY_ARE_EQUAL(1u, result.ExitCode.value());
        VERIFY_IS_TRUE(result.Stderr.has_value());
        VERIFY_ARE_NOT_EQUAL(0u, result.Stderr.value().size());
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Cp_ContainerToLocal)
    {
        // Create and start a container.
        auto runResult =
            RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        runResult.Verify({.Stderr = L"", .ExitCode = 0});

        // Create a file inside the container using exec.
        auto execResult =
            RunWslc(std::format(L"container exec {} sh -c \"echo container-content > /tmp/fromcontainer.txt\"", WslcContainerName));
        execResult.Verify({.ExitCode = 0});

        // Create a directory to download into.
        auto downloadDir = std::filesystem::current_path() / L"wslc-cp-download-test";
        std::filesystem::create_directories(downloadDir);
        auto cleanupDir = wil::scope_exit([&] { std::filesystem::remove_all(downloadDir); });

        // Copy from container to local.
        const auto cpResult =
            RunWslc(std::format(L"container cp {}:/tmp/fromcontainer.txt {}", WslcContainerName, downloadDir.wstring()));
        cpResult.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        // Verify the file was extracted locally.
        auto extractedFile = downloadDir / L"fromcontainer.txt";
        VERIFY_IS_TRUE(std::filesystem::exists(extractedFile));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Cp_ContainerToLocal_TrailingBackslash)
    {
        // Regression test: trailing backslash on local path should not break tar extraction.
        auto runResult =
            RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        runResult.Verify({.Stderr = L"", .ExitCode = 0});

        // Create the file inside the container using exec.
        auto execResult = RunWslc(std::format(L"container exec {} sh -c \"echo backslash-test > /tmp/bstest.txt\"", WslcContainerName));
        execResult.Verify({.ExitCode = 0});

        auto downloadDir = std::filesystem::current_path() / L"wslc-cp-backslash-test";
        std::filesystem::create_directories(downloadDir);
        auto cleanupDir = wil::scope_exit([&] { std::filesystem::remove_all(downloadDir); });

        // Copy with explicit trailing backslash in target path.
        auto targetWithBackslash = downloadDir.wstring() + L"\\";
        const auto cpResult = RunWslc(std::format(L"container cp {}:/tmp/bstest.txt {}", WslcContainerName, targetWithBackslash));
        cpResult.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        auto extractedFile = downloadDir / L"bstest.txt";
        VERIFY_IS_TRUE(std::filesystem::exists(extractedFile));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Cp_ContainerToLocal_ThroughSymlink)
    {
        auto runResult =
            RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        runResult.Verify({.Stderr = L"", .ExitCode = 0});

        auto execResult = RunWslc(std::format(L"container exec {} touch /tmp/symfile.txt", WslcContainerName));
        execResult.Verify({.ExitCode = 0});

        auto linkDir = std::filesystem::current_path() / L"wslc-cp-symlink-link";
        auto cleanup = wil::scope_exit([&] {
            std::error_code ec;
            std::filesystem::remove(linkDir, ec);
            std::filesystem::remove(L"symfile.txt", ec);
        });

        // Creating a symlink requires Administrator privileges, which the E2E tests already run with.
        THROW_LAST_ERROR_IF(!CreateSymbolicLinkW(linkDir.c_str(), std::filesystem::current_path().c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY));

        // Copy from the container into the symlink.
        const auto cpResult = RunWslc(std::format(L"container cp {}:/tmp/symfile.txt {}", WslcContainerName, linkDir.wstring()));
        cpResult.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        // The file must land in the real directory (and therefore be visible through the symlink).
        VERIFY_IS_TRUE(std::filesystem::exists(std::filesystem::current_path() / L"symfile.txt"));
        VERIFY_IS_TRUE(std::filesystem::exists(linkDir / L"symfile.txt"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Cp_ContainerToLocal_FileDestination)
    {
        // When the local target doesn't end with a separator and isn't an existing directory,
        // it should be treated as a file destination (matching docker cp semantics).
        auto runResult =
            RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        runResult.Verify({.Stderr = L"", .ExitCode = 0});

        // Create the file inside the container using exec.
        auto execResult = RunWslc(std::format(L"container exec {} sh -c \"echo file-dest-test > /tmp/srcfile.txt\"", WslcContainerName));
        execResult.Verify({.ExitCode = 0});

        auto targetFile = std::filesystem::current_path() / L"wslc-cp-file-dest-test" / L"renamed.txt";
        auto cleanupDir = wil::scope_exit([&] { std::filesystem::remove_all(targetFile.parent_path()); });

        // Copy from container to a specific file path (not a directory).
        const auto cpResult = RunWslc(std::format(L"container cp {}:/tmp/srcfile.txt {}", WslcContainerName, targetFile.wstring()));
        cpResult.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        // The file should exist at the exact target path, not inside a directory named "renamed.txt".
        VERIFY_IS_TRUE(std::filesystem::exists(targetFile));
        VERIFY_IS_TRUE(std::filesystem::is_regular_file(targetFile));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Cp_ContainerToLocal_FollowLinkCopiesTargetContents)
    {
        auto runResult =
            RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        runResult.Verify({.Stderr = L"", .ExitCode = 0});

        // A regular file plus a symbolic link pointing at it.
        auto execResult = RunWslc(
            std::format(L"container exec {} sh -c \"echo follow-link-target > /tmp/linktarget.txt; ln -s /tmp/linktarget.txt /tmp/thelink.txt\"", WslcContainerName));
        execResult.Verify({.ExitCode = 0});

        auto downloadDir = std::filesystem::current_path() / L"wslc-cp-follow-link-test";
        std::filesystem::create_directories(downloadDir);
        auto cleanupDir = wil::scope_exit([&] { std::filesystem::remove_all(downloadDir); });

        // --follow-link copies what the link points at, so the target's contents land locally under the
        // name that was asked for.
        const auto cpResult =
            RunWslc(std::format(L"container cp --follow-link {}:/tmp/thelink.txt {}", WslcContainerName, downloadDir.wstring()));
        cpResult.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        const auto copied = downloadDir / L"thelink.txt";
        VERIFY_IS_TRUE(std::filesystem::exists(copied));
        VERIFY_IS_TRUE(std::filesystem::is_regular_file(copied));
        VERIFY_IS_FALSE(std::filesystem::is_symlink(copied));
        VERIFY_ARE_EQUAL(std::wstring(L"follow-link-target\n"), ReadFileContent(copied.wstring()));

        // The target's own name is never used as the destination when the link is followed.
        VERIFY_IS_FALSE(std::filesystem::exists(downloadDir / L"linktarget.txt"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Cp_ContainerToLocal_FollowLinkResolvesRelativeTarget)
    {
        auto runResult =
            RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        runResult.Verify({.Stderr = L"", .ExitCode = 0});

        // The link target is relative, so it only resolves against the directory holding the link.
        // Read against the container root it would name a path that does not exist.
        auto execResult = RunWslc(std::format(
            L"container exec {} sh -c \"mkdir -p /tmp/linkdir; echo relative-link-target > /tmp/linkdir/linktarget.txt; ln -s "
            L"linktarget.txt /tmp/linkdir/thelink.txt\"",
            WslcContainerName));
        execResult.Verify({.ExitCode = 0});

        auto downloadDir = std::filesystem::current_path() / L"wslc-cp-follow-link-relative-test";
        std::filesystem::create_directories(downloadDir);
        auto cleanupDir = wil::scope_exit([&] { std::filesystem::remove_all(downloadDir); });

        const auto cpResult =
            RunWslc(std::format(L"container cp --follow-link {}:/tmp/linkdir/thelink.txt {}", WslcContainerName, downloadDir.wstring()));
        cpResult.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        const auto copied = downloadDir / L"thelink.txt";
        VERIFY_IS_TRUE(std::filesystem::exists(copied));
        VERIFY_IS_TRUE(std::filesystem::is_regular_file(copied));
        VERIFY_IS_FALSE(std::filesystem::is_symlink(copied));
        VERIFY_ARE_EQUAL(std::wstring(L"relative-link-target\n"), ReadFileContent(copied.wstring()));

        VERIFY_IS_FALSE(std::filesystem::exists(downloadDir / L"linktarget.txt"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Cp_ContainerToLocal_NonexistentPath)
    {
        // Regression test: DownloadArchive used to hang on 404 because the HTTP/1.1 keep-alive
        // socket never closed. The fix shuts down the socket so the read sees EOF immediately.
        auto runResult =
            RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        runResult.Verify({.Stderr = L"", .ExitCode = 0});

        auto downloadDir = std::filesystem::current_path() / L"wslc-cp-notfound-test";
        std::filesystem::create_directories(downloadDir);
        auto cleanupDir = wil::scope_exit([&] { std::filesystem::remove_all(downloadDir); });

        const auto cpResult = RunWslc(std::format(L"container cp {}:/nonexistent/file.txt {}", WslcContainerName, downloadDir.wstring()));
        VERIFY_IS_TRUE(cpResult.ExitCode.has_value());
        VERIFY_ARE_EQUAL(1u, cpResult.ExitCode.value());
        VERIFY_IS_TRUE(cpResult.Stderr.has_value());
        VERIFY_ARE_NOT_EQUAL(0u, cpResult.Stderr.value().size());
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Cp_ContainerToLocal_NonexistentDir)
    {
        // Regression test: DownloadArchive 404 for a nonexistent directory path.
        auto runResult =
            RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        runResult.Verify({.Stderr = L"", .ExitCode = 0});

        auto downloadDir = std::filesystem::current_path() / L"wslc-cp-notfound-dir-test";
        std::filesystem::create_directories(downloadDir);
        auto cleanupDir = wil::scope_exit([&] { std::filesystem::remove_all(downloadDir); });

        const auto cpResult = RunWslc(std::format(L"container cp {}:/no/such/directory/ {}", WslcContainerName, downloadDir.wstring()));
        VERIFY_IS_TRUE(cpResult.ExitCode.has_value());
        VERIFY_ARE_EQUAL(1u, cpResult.ExitCode.value());
        VERIFY_IS_TRUE(cpResult.Stderr.has_value());
        VERIFY_ARE_NOT_EQUAL(0u, cpResult.Stderr.value().size());
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Cp_Download_NonexistentContainer)
    {
        // Regression test: DownloadArchive error path when the container itself doesn't exist.
        auto downloadDir = std::filesystem::current_path() / L"wslc-cp-no-container-test";
        std::filesystem::create_directories(downloadDir);
        auto cleanupDir = wil::scope_exit([&] { std::filesystem::remove_all(downloadDir); });

        const auto cpResult = RunWslc(std::format(L"container cp {}:/tmp/file.txt {}", InvalidContainerName, downloadDir.wstring()));
        VERIFY_IS_TRUE(cpResult.ExitCode.has_value());
        VERIFY_ARE_EQUAL(1u, cpResult.ExitCode.value());
        VERIFY_IS_TRUE(cpResult.Stderr.has_value());
        VERIFY_IS_TRUE(cpResult.Stderr->find(L"not found") != std::wstring::npos);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Cp_FromStoppedContainer)
    {
        // Create a container, put a file in it, stop it, then copy out.
        auto runResult = RunWslc(std::format(
            L"container run --name {} {} sh -c \"echo stopped-content > /tmp/stopped.txt\"", WslcContainerName, DebianImage.NameAndTag()));
        runResult.Verify({.Stderr = L"", .ExitCode = 0});

        // Container has exited (ran a one-shot command). Copy from the stopped container.
        auto downloadDir = std::filesystem::current_path() / L"wslc-cp-stopped-test";
        std::filesystem::create_directories(downloadDir);
        auto cleanupDir = wil::scope_exit([&] { std::filesystem::remove_all(downloadDir); });

        const auto cpResult = RunWslc(std::format(L"container cp {}:/tmp/stopped.txt {}", WslcContainerName, downloadDir.wstring()));
        cpResult.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        auto extractedFile = downloadDir / L"stopped.txt";
        VERIFY_IS_TRUE(std::filesystem::exists(extractedFile));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Cp_InvalidDirection_LocalToLocal)
    {
        // local → local is not a valid copy direction.
        const auto result = RunWslc(L"container cp C:\\temp\\somefile.txt C:\\temp\\dest\\");
        VERIFY_IS_TRUE(result.ExitCode.has_value());
        VERIFY_ARE_EQUAL(1u, result.ExitCode.value());
        VERIFY_IS_TRUE(result.Stderr.has_value());
        VERIFY_ARE_NOT_EQUAL(0u, result.Stderr.value().size());
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Cp_StdoutIsTerminal)
    {
        // A pseudo console gives wslc a console stdout, which a copy to '-' must reject: a tar stream
        // written to a terminal would be rendered as text rather than captured.
        auto session = RunWslcInteractive(L"container cp fakecontainer:/path -", ElevationType::Elevated, PseudoConsole{200, 50});

        WaitForPseudoConsoleOutput(session, string::WideToMultiByte(Localization::WSLCCLI_CpStdoutIsTerminalError()));
        VERIFY_ARE_EQUAL(1, session.Wait());
    }

    // The container to local file destination path against one container. The rejection rules themselves
    // are covered by the ExtractSingleFileAs unit tests, so what is proven here is that a cp reaches
    // them, and that the stdout destination produces a real archive.
    WSLC_TEST_METHOD(WSLCE2E_Container_Cp_ContainerToLocal_FileDestinationBehavior)
    {
        auto runResult =
            RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        runResult.Verify({.Stderr = L"", .ExitCode = 0});

        auto execResult = RunWslc(std::format(
            L"container exec {} sh -c \"echo stdout-dest-test > /tmp/stdoutfile.txt; mkdir -p /tmp/srcdir; echo inner > "
            L"/tmp/srcdir/inner.txt\"",
            WslcContainerName));
        execResult.Verify({.ExitCode = 0});

        auto workDir = std::filesystem::current_path() / L"wslc-cp-file-dest-behavior";
        std::filesystem::create_directories(workDir);
        auto cleanupDir = wil::scope_exit([&] { std::filesystem::remove_all(workDir); });

        // '-' as the target writes the archive itself to standard output, rather than the file contents,
        // so the stream can be piped into another tar.
        const auto cpResult = RunWslcAndRedirectToFile(std::format(L"container cp {}:/tmp/stdoutfile.txt -", WslcContainerName), TarPath);
        cpResult.Verify({.Stderr = L"", .ExitCode = 0});

        VERIFY_IS_TRUE(std::filesystem::exists(TarPath));

        // The first header block names the entry and carries the ustar magic, which distinguishes an
        // archive from the file's own contents.
        const auto header = ReadFileBytes(TarPath, 512);
        VERIFY_IS_TRUE(header.size() >= 512);
        VERIFY_ARE_EQUAL(std::string{"stdoutfile.txt"}, std::string{header.data()});
        VERIFY_ARE_EQUAL(std::string{"ustar"}, std::string{header.data() + 257});

        // Extracting the archive must give back the file that was copied.
        const auto extractDir = workDir / L"extracted";
        std::filesystem::create_directories(extractDir);
        RunTar(std::format(L"tar.exe -xf \"{}\" -C \"{}\"", TarPath.wstring(), extractDir.wstring()));
        VERIFY_ARE_EQUAL(std::wstring(L"stdout-dest-test\n"), ReadFileContent((extractDir / L"stdoutfile.txt").wstring()));

        // A directory cannot be given a file path. The copy has to fail without creating the destination,
        // and without leaving staging behind in the directory that would have held it.
        const auto rejectedDir = workDir / L"rejected";
        std::filesystem::create_directories(rejectedDir);
        const auto targetFile = rejectedDir / L"target.txt";

        const auto rejectedResult = RunWslc(std::format(L"container cp {}:/tmp/srcdir {}", WslcContainerName, targetFile.wstring()));
        VERIFY_IS_TRUE(rejectedResult.ExitCode.has_value());
        VERIFY_ARE_NOT_EQUAL(0u, rejectedResult.ExitCode.value());

        VERIFY_IS_FALSE(std::filesystem::exists(targetFile));
        VERIFY_ARE_EQUAL(
            static_cast<size_t>(0),
            static_cast<size_t>(std::distance(std::filesystem::directory_iterator(rejectedDir), std::filesystem::directory_iterator{})));
    }

private:
    const std::wstring WslcContainerName = L"wslc-test-container-cp";
    const std::wstring InvalidContainerName = L"wslc-nonexistent-container-for-cp";
    const TestImage& DebianImage = DebianTestImage();

    std::filesystem::path TarPath{};

    // Reads up to Count bytes, so a binary artifact can be inspected without going through the text
    // helpers that the rest of these tests use.
    static std::string ReadFileBytes(const std::filesystem::path& Path, size_t Count)
    {
        std::ifstream file(Path, std::ios::binary);
        VERIFY_IS_TRUE(file.is_open());

        std::string bytes(Count, '\0');
        file.read(bytes.data(), static_cast<std::streamsize>(Count));
        bytes.resize(static_cast<size_t>(file.gcount()));

        return bytes;
    }

    static void RunTar(const std::wstring& CommandLine)
    {
        auto command = CommandLine;
        STARTUPINFOW si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        THROW_LAST_ERROR_IF(!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi));
        wil::unique_handle tarProcess(pi.hProcess);
        wil::unique_handle tarThread(pi.hThread);
        WaitForSingleObject(tarProcess.get(), INFINITE);

        DWORD exitCode = 0;
        GetExitCodeProcess(tarProcess.get(), &exitCode);
        THROW_HR_IF_MSG(E_FAIL, exitCode != 0, "tar.exe exited with code %u", exitCode);
    }

    // Creates a tar file containing a single text file using tar.exe.
    void CreateTestTarFile()
    {
        // Create a directory with a test file to archive.
        auto tarSrcDir = std::filesystem::current_path() / L"wslc-cp-tar-src";
        std::filesystem::create_directories(tarSrcDir);
        auto cleanupSrcDir = wil::scope_exit([&] { std::filesystem::remove_all(tarSrcDir); });

        auto testFile = tarSrcDir / L"testfile.txt";
        {
            wil::unique_hfile file(CreateFileW(testFile.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
            THROW_LAST_ERROR_IF(!file);
            const std::string content = "wslc-cp-test-content\n";
            DWORD written = 0;
            THROW_IF_WIN32_BOOL_FALSE(WriteFile(file.get(), content.data(), static_cast<DWORD>(content.size()), &written, nullptr));
        }

        // Use tar.exe to create the archive.
        RunTar(std::format(L"tar.exe -cf \"{}\" -C \"{}\" testfile.txt", TarPath.wstring(), tarSrcDir.wstring()));
    }
};
} // namespace WSLCE2ETests
