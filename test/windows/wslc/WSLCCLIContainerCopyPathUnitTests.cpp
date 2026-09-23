// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCCLITestHelpers.h"

#include "ContainerService.h"

using namespace wsl::windows::wslc;
using namespace wsl::windows::wslc::services;
using namespace WSLCTestHelpers;
using namespace WEX::Logging;
using namespace WEX::Common;
using namespace WEX::TestExecution;

namespace WSLCCLIContainerCopyPathUnitTests {

namespace {

    void VerifyDirectionIsRejected(const std::wstring& Source, const std::wstring& Target)
    {
        VERIFY_THROWS_SPECIFIC(ContainerService::IsCopyingToContainer(Source, Target), wil::ResultException, [](const wil::ResultException& e) {
            return e.GetErrorCode() == E_INVALIDARG;
        });
    }

    void VerifyPathIsNotAContainerPath(const std::wstring& Path)
    {
        VERIFY_THROWS_SPECIFIC(ContainerService::ParseContainerPath(Path), wil::ResultException, [](const wil::ResultException& e) {
            return e.GetErrorCode() == E_UNEXPECTED;
        });
    }

} // namespace

class WSLCCLIContainerCopyPathUnitTests
{
    WSLC_TEST_CLASS(WSLCCLIContainerCopyPathUnitTests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        return true;
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        return true;
    }

    // The direction is taken from which side names a container, since exactly one side may.
    TEST_METHOD(IsCopyingToContainer_DirectionFollowsTheContainerSide)
    {
        VERIFY_IS_TRUE(ContainerService::IsCopyingToContainer(L"C:\\local\\file.txt", L"mycontainer:/tmp/file.txt"));
        VERIFY_IS_FALSE(ContainerService::IsCopyingToContainer(L"mycontainer:/tmp/file.txt", L"C:\\local\\file.txt"));
    }

    // A relative path carries no colon, so it names the host wherever it appears.
    TEST_METHOD(IsCopyingToContainer_RelativePathsNameTheHost)
    {
        VERIFY_IS_TRUE(ContainerService::IsCopyingToContainer(L"file.txt", L"mycontainer:/tmp/file.txt"));
        VERIFY_IS_FALSE(ContainerService::IsCopyingToContainer(L"mycontainer:/tmp/file.txt", L"subdir\\file.txt"));
        VERIFY_IS_TRUE(ContainerService::IsCopyingToContainer(L"..\\file.txt", L"mycontainer:/tmp/"));
    }

    // A UNC path has no colon either, so it is not mistaken for a container reference.
    TEST_METHOD(IsCopyingToContainer_UncPathNamesTheHost)
    {
        VERIFY_IS_TRUE(ContainerService::IsCopyingToContainer(L"\\\\server\\share\\file.txt", L"mycontainer:/tmp/file.txt"));
        VERIFY_IS_FALSE(ContainerService::IsCopyingToContainer(L"mycontainer:/tmp/file.txt", L"\\\\server\\share\\file.txt"));
    }

    // A drive letter is a single letter before the colon, which is what separates it from a container
    // name. A name of two or more characters is a container even when it begins with a letter.
    TEST_METHOD(IsCopyingToContainer_DriveLetterIsNotAContainer)
    {
        VERIFY_IS_TRUE(ContainerService::IsCopyingToContainer(L"c:\\local\\file.txt", L"mycontainer:/tmp/file.txt"));

        // A two letter name is past the point where a drive letter could be meant, so it is a container.
        VERIFY_IS_TRUE(ContainerService::IsCopyingToContainer(L"Z:/local/file.txt", L"cc:/tmp/file.txt"));
    }

    // Standard input stands in for a local source, so it is always a copy into the container.
    TEST_METHOD(IsCopyingToContainer_StdinSourceCopiesIntoContainer)
    {
        VERIFY_IS_TRUE(ContainerService::IsCopyingToContainer(L"-", L"mycontainer:/tmp/"));
    }

    // Standard output stands in for a local destination, so it is always a copy out of the container.
    TEST_METHOD(IsCopyingToContainer_StdoutTargetCopiesOutOfContainer)
    {
        VERIFY_IS_FALSE(ContainerService::IsCopyingToContainer(L"mycontainer:/tmp/file.txt", L"-"));
    }

    // Neither side may name the container twice, and neither may name the host twice: a copy has to cross
    // the boundary, and a request that does not is a mistake rather than a local file copy.
    TEST_METHOD(IsCopyingToContainer_RejectsCopiesThatDoNotCrossTheBoundary)
    {
        VerifyDirectionIsRejected(L"first:/tmp/file.txt", L"second:/tmp/file.txt");
        VerifyDirectionIsRejected(L"mycontainer:/tmp/a", L"mycontainer:/tmp/b");
        VerifyDirectionIsRejected(L"C:\\local\\a.txt", L"C:\\local\\b.txt");
        VerifyDirectionIsRejected(L"a.txt", L"b.txt");
        VerifyDirectionIsRejected(L"-", L"-");
        VerifyDirectionIsRejected(L"-", L"C:\\local\\file.txt");
    }

    // The first colon divides the name from the path, so a path may hold colons of its own.
    TEST_METHOD(ParseContainerPath_SplitsAtTheFirstColon)
    {
        const auto [container, path] = ContainerService::ParseContainerPath(L"mycontainer:/tmp/file.txt");
        VERIFY_ARE_EQUAL(std::string{"mycontainer"}, container);
        VERIFY_ARE_EQUAL(std::string{"/tmp/file.txt"}, path);

        const auto [idContainer, idPath] = ContainerService::ParseContainerPath(L"3f2a9c:/var/log/a:b");
        VERIFY_ARE_EQUAL(std::string{"3f2a9c"}, idContainer);
        VERIFY_ARE_EQUAL(std::string{"/var/log/a:b"}, idPath);
    }

    // An empty path is preserved rather than defaulted, so the caller can reject it and report which side
    // of the argument was left out.
    TEST_METHOD(ParseContainerPath_KeepsAnEmptyPath)
    {
        const auto [container, path] = ContainerService::ParseContainerPath(L"mycontainer:");
        VERIFY_ARE_EQUAL(std::string{"mycontainer"}, container);
        VERIFY_ARE_EQUAL(std::string{}, path);
    }

    // A relative container path is not made absolute here; it is passed through for the container to
    // resolve against its own working directory.
    TEST_METHOD(ParseContainerPath_KeepsRelativePathsAsWritten)
    {
        const auto [container, path] = ContainerService::ParseContainerPath(L"mycontainer:tmp/file.txt");
        VERIFY_ARE_EQUAL(std::string{"mycontainer"}, container);
        VERIFY_ARE_EQUAL(std::string{"tmp/file.txt"}, path);
    }

    // A path with no container reference must not be split, or a drive letter would be read as a
    // container name and the copy would be sent to the wrong side.
    TEST_METHOD(ParseContainerPath_RejectsPathsThatNameNoContainer)
    {
        VerifyPathIsNotAContainerPath(L"C:\\local\\file.txt");
        VerifyPathIsNotAContainerPath(L"c:/local/file.txt");
        VerifyPathIsNotAContainerPath(L"file.txt");
        VerifyPathIsNotAContainerPath(L"\\\\server\\share\\file.txt");
        VerifyPathIsNotAContainerPath(L"");
    }
};
} // namespace WSLCCLIContainerCopyPathUnitTests
