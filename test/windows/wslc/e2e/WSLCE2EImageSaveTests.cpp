/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EImageSaveTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC image save.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"

namespace WSLCE2ETests {
using namespace wsl::shared;

class WSLCE2EImageSaveTests
{
    WSLC_TEST_CLASS(WSLCE2EImageSaveTests)

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        EnsureImageIsDeleted(DebianImage);
        return true;
    }

    TEST_METHOD_SETUP(MethodSetup)
    {
        EnsureImageIsLoaded(DebianImage);
        SavedArchivePath = wsl::windows::common::filesystem::GetTempFilename();
        return true;
    }

    TEST_METHOD_CLEANUP(MethodCleanup)
    {
        DeleteFileW(SavedArchivePath.c_str());
        return true;
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Save_HelpCommand)
    {
        auto result = RunWslc(L"image save --help");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Save_MissingImageName)
    {
        const auto result = RunWslc(std::format(L"image save --output \"{}\"", SavedArchivePath.wstring()));
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"Required argument not provided: 'image'\r\n", .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Save_ImageNotFound)
    {
        const auto result = RunWslc(std::format(L"image save --output \"{}\" {}", SavedArchivePath.wstring(), InvalidImage.NameAndTag()));
        result.Verify({.Stdout = L"", .Stderr = L"reference does not exist\r\nError code: E_FAIL\r\n", .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Save_Success)
    {
        const auto result = RunWslc(std::format(L"image save --output \"{}\" {}", SavedArchivePath.wstring(), DebianImage.NameAndTag()));
        result.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        // Verify the archive was written. We deliberately do NOT compare its
        // byte length against DebianImage.Path: the test data ships as an OCI
        // Image Layout (index.json + blobs/sha256/...), while podman's
        // docker-compat /images/get endpoint emits the Docker v1 image
        // format (<config>.json + <layer-id>/layer.tar). Both contain the
        // same byte-identical layer blob (SHA-256 content-addressed), but
        // different wrapper metadata yields a tar that's a few KB smaller.
        // Functional equivalence (save -> load -> run roundtrip) is covered
        // by WSLCE2E_Image_Save_Load below.
        VERIFY_IS_TRUE(std::filesystem::exists(SavedArchivePath));
        VERIFY_IS_GREATER_THAN_OR_EQUAL(std::filesystem::file_size(SavedArchivePath), static_cast<uintmax_t>(1024 * 1024));
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Save_Load)
    {
        // Save source image
        auto saveResult = RunWslc(std::format(L"image save --output \"{}\" {}", SavedArchivePath.wstring(), DebianImage.NameAndTag()));
        saveResult.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        // Delete source image
        EnsureImageIsDeleted(DebianImage);

        // Load from saved archive
        auto loadResult = RunWslc(std::format(L"image load --input \"{}\"", SavedArchivePath.wstring()));
        loadResult.Verify({.Stderr = L"", .ExitCode = 0});

        // Run a container from the loaded image to verify it works
        auto runResult = RunWslc(std::format(L"container run --rm {} echo Hello from saved image!", DebianImage.NameAndTag()));
        runResult.Verify({.Stdout = L"Hello from saved image!\n", .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Save_ToStdout_Success)
    {
        const auto result = RunWslcAndRedirectToFile(std::format(L"image save {}", DebianImage.NameAndTag()), SavedArchivePath);
        result.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        // See WSLCE2E_Image_Save_Success for why we don't compare byte sizes:
        // source tar uses OCI Image Layout, podman save emits Docker v1
        // format. Layer data is byte-identical; only wrapper metadata differs.
        VERIFY_IS_TRUE(std::filesystem::exists(SavedArchivePath));
        VERIFY_IS_GREATER_THAN_OR_EQUAL(std::filesystem::file_size(SavedArchivePath), static_cast<uintmax_t>(1024 * 1024));
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Save_ToTerminal_Fail)
    {
        // TODO: Re-enable once the test is stable in console-less pipeline environments.
        // Opening CONOUT$ may fail when the process has no console attached.
        SKIP_TEST_UNSTABLE();

        const auto result = RunWslcAndRedirectToFile(std::format(L"image save {}", DebianImage.NameAndTag()));
        result.Verify(
            {.Stderr = L"Cannot write image to terminal. Use the -o flag or redirect stdout.\r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Save_ToStdout_Load)
    {
        // Save source image
        auto saveResult = RunWslcAndRedirectToFile(std::format(L"image save {}", DebianImage.NameAndTag()), SavedArchivePath);
        saveResult.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        // Delete source image
        EnsureImageIsDeleted(DebianImage);

        // Load from saved archive
        auto loadResult = RunWslc(std::format(L"image load --input \"{}\"", SavedArchivePath.wstring()));
        loadResult.Verify({.Stderr = L"", .ExitCode = 0});

        // Run a container from the loaded image to verify it works
        auto runResult = RunWslc(std::format(L"container run --rm {} echo Hello from saved image!", DebianImage.NameAndTag()));
        runResult.Verify({.Stdout = L"Hello from saved image!\n", .Stderr = L"", .ExitCode = 0});
    }

private:
    const TestImage DebianImage = DebianTestImage();
    const TestImage& InvalidImage = InvalidTestImage();

    std::filesystem::path SavedArchivePath{};

    std::wstring GetHelpMessage() const
    {
        std::wstringstream output;
        output << GetWslcHeader()        //
               << GetDescription()       //
               << GetUsage()             //
               << GetAvailableCommands() //
               << GetAvailableOptions();
        return output.str();
    }

    std::wstring GetDescription() const
    {
        return Localization::WSLCCLI_ImageSaveLongDesc() + L"\r\n\r\n";
    }

    std::wstring GetUsage() const
    {
        return L"Usage: wslc image save [<options>] <image>\r\n\r\n";
    }

    std::wstring GetAvailableCommands() const
    {
        std::wstringstream commands;
        commands << L"The following arguments are available:\r\n" //
                 << L"  image        Image name\r\n"              //
                 << L"\r\n";
        return commands.str();
    }

    std::wstring GetAvailableOptions() const
    {
        std::wstringstream options;
        options << L"The following options are available:\r\n"                 //
                << L"  -o,--output  Path for the saved image\r\n"              //
                << L"  --session    Specify the session to use\r\n"            //
                << L"  -?,--help    Shows help about the selected command\r\n" //
                << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests