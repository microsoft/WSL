// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "Common.h"
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

    TEST_METHOD(IsCopyingToContainer_DirectionFollowsTheContainerSide)
    {
        VERIFY_IS_TRUE(ContainerService::IsCopyingToContainer(L"C:\\local\\file.txt", L"mycontainer:/tmp/file.txt"));
        VERIFY_IS_FALSE(ContainerService::IsCopyingToContainer(L"mycontainer:/tmp/file.txt", L"C:\\local\\file.txt"));
    }

    TEST_METHOD(IsCopyingToContainer_RelativePathsNameTheHost)
    {
        VERIFY_IS_TRUE(ContainerService::IsCopyingToContainer(L"file.txt", L"mycontainer:/tmp/file.txt"));
        VERIFY_IS_FALSE(ContainerService::IsCopyingToContainer(L"mycontainer:/tmp/file.txt", L"subdir\\file.txt"));
        VERIFY_IS_TRUE(ContainerService::IsCopyingToContainer(L"..\\file.txt", L"mycontainer:/tmp/"));
    }

    TEST_METHOD(IsCopyingToContainer_UncPathNamesTheHost)
    {
        VERIFY_IS_TRUE(ContainerService::IsCopyingToContainer(L"\\\\server\\share\\file.txt", L"mycontainer:/tmp/file.txt"));
        VERIFY_IS_FALSE(ContainerService::IsCopyingToContainer(L"mycontainer:/tmp/file.txt", L"\\\\server\\share\\file.txt"));
    }

    TEST_METHOD(IsCopyingToContainer_DriveLetterIsNotAContainer)
    {
        VERIFY_IS_TRUE(ContainerService::IsCopyingToContainer(L"c:\\local\\file.txt", L"mycontainer:/tmp/file.txt"));

        // Two letters is a container name, not a drive.
        VERIFY_IS_TRUE(ContainerService::IsCopyingToContainer(L"Z:/local/file.txt", L"cc:/tmp/file.txt"));
    }

    TEST_METHOD(IsCopyingToContainer_StdinSourceCopiesIntoContainer)
    {
        VERIFY_IS_TRUE(ContainerService::IsCopyingToContainer(L"-", L"mycontainer:/tmp/"));
    }

    TEST_METHOD(IsCopyingToContainer_StdoutTargetCopiesOutOfContainer)
    {
        VERIFY_IS_FALSE(ContainerService::IsCopyingToContainer(L"mycontainer:/tmp/file.txt", L"-"));
    }

    TEST_METHOD(IsCopyingToContainer_RejectsCopiesThatDoNotCrossTheBoundary)
    {
        VerifyDirectionIsRejected(L"first:/tmp/file.txt", L"second:/tmp/file.txt");
        VerifyDirectionIsRejected(L"mycontainer:/tmp/a", L"mycontainer:/tmp/b");
        VerifyDirectionIsRejected(L"C:\\local\\a.txt", L"C:\\local\\b.txt");
        VerifyDirectionIsRejected(L"a.txt", L"b.txt");
        VerifyDirectionIsRejected(L"-", L"-");
        VerifyDirectionIsRejected(L"-", L"C:\\local\\file.txt");
    }

    TEST_METHOD(ParseContainerPath_SplitsAtTheFirstColon)
    {
        const auto [container, path] = ContainerService::ParseContainerPath(L"mycontainer:/tmp/file.txt");
        VERIFY_ARE_EQUAL(std::string{"mycontainer"}, container);
        VERIFY_ARE_EQUAL(std::string{"/tmp/file.txt"}, path);

        const auto [idContainer, idPath] = ContainerService::ParseContainerPath(L"3f2a9c:/var/log/a:b");
        VERIFY_ARE_EQUAL(std::string{"3f2a9c"}, idContainer);
        VERIFY_ARE_EQUAL(std::string{"/var/log/a:b"}, idPath);
    }

    TEST_METHOD(ParseContainerPath_KeepsAnEmptyPath)
    {
        const auto [container, path] = ContainerService::ParseContainerPath(L"mycontainer:");
        VERIFY_ARE_EQUAL(std::string{"mycontainer"}, container);
        VERIFY_ARE_EQUAL(std::string{}, path);
    }

    TEST_METHOD(ParseContainerPath_KeepsRelativePathsAsWritten)
    {
        const auto [container, path] = ContainerService::ParseContainerPath(L"mycontainer:tmp/file.txt");
        VERIFY_ARE_EQUAL(std::string{"mycontainer"}, container);
        VERIFY_ARE_EQUAL(std::string{"tmp/file.txt"}, path);
    }

    TEST_METHOD(ParseContainerPath_RejectsPathsThatNameNoContainer)
    {
        VerifyPathIsNotAContainerPath(L"C:\\local\\file.txt");
        VerifyPathIsNotAContainerPath(L"c:/local/file.txt");
        VerifyPathIsNotAContainerPath(L"file.txt");
        VerifyPathIsNotAContainerPath(L"\\\\server\\share\\file.txt");
        VerifyPathIsNotAContainerPath(L":/tmp/file.txt");
        VerifyPathIsNotAContainerPath(L"");
    }
};
} // namespace WSLCCLIContainerCopyPathUnitTests
